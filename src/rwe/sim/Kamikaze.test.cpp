#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <cstdlib>
#include <filesystem>

namespace rwe
{
    namespace
    {
        struct BombFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0};
            BombFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_kamikaze_test.log", false));
                sim.lineOfSightEnabled = false;
                sim.maxUnitsPerPlayer = 2000;
                for (unsigned int i = 0; i < 2; ++i)
                {
                    sim.addPlayer(GamePlayerInfo{std::nullopt, i == 0 ? GamePlayerType::Human : GamePlayerType::Computer,
                        PlayerColorIndex(i), GamePlayerStatus::Alive, i == 0 ? "ARM" : "CORE", std::nullopt,
                        Metal(100000), Energy(100000), Metal(100000), Energy(100000), Metal(100000), Energy(100000)});
                }
                for (const auto* name : {"SMALL", "LARGE", "GUN"})
                {
                    WeaponDefinition w{};
                    w.maxRange = 100_ss; w.velocity = 10_ss;
                    w.damageRadius = std::string(name) == "LARGE" ? 128_ss : 80_ss;
                    w.damage = {{"DEFAULT", std::string(name) == "LARGE" ? 200u : 20u}};
                    w.reloadTime = 1_ss; w.burst = 1;
                    sim.weaponDefinitions.emplace(name, w);
                }
                for (const auto* name : {"BOMB", "TARGET", "MINE", "ARMED", "BUILDING", "AIR"})
                {
                    UnitDefinition d{};
                    d.unitName = d.objectName = name;
                    d.movementCollisionInfo = UnitDefinition::AdHocMovementClass{1, 1, 255, 255, 0, 255};
                    d.maxHitPoints = 1000; d.buildTime = 100;
                    d.isMobile = d.canMove = d.canAttack = true;
                    d.shootMe = true; d.standingFireOrder = UnitFireOrders::HoldFire;
                    d.maxVelocity = 2_ss; d.acceleration = 1_ss; d.brakeRate = 1_ss; d.turnRate = 2048_ss;
                    d.sightDistance = 100_ss;
                    if (std::string(name) == "BOMB" || std::string(name) == "MINE" || std::string(name) == "ARMED")
                    {
                        d.kamikaze = true; d.kamikazeDistance = 40_ss; d.maxHitPoints = 100;
                        d.explodeAs = "SMALL"; d.selfDestructAs = "LARGE";
                    }
                    if (std::string(name) == "MINE" || std::string(name) == "BUILDING")
                    {
                        d.isMobile = d.canMove = d.canAttack = false;
                        d.yardMap = Grid<YardMapCell>(1, 1, YardMapCell::GroundPassable);
                    }
                    if (std::string(name) == "MINE") { d.standbyMine = true; d.standingFireOrder = UnitFireOrders::FireAtWill; }
                    if (std::string(name) == "ARMED") { d.weapon1 = "GUN"; }
                    if (std::string(name) == "AIR") { d.canFly = true; d.cruiseAltitude = 60_ss; d.categories.insert("VTOL"); }
                    sim.unitDefinitions.emplace(name, d);
                    sim.unitModelDefinitions.emplace(name, createUnitModelDefinition(16_ss, {UnitPieceDefinition{"base", SimVector(0_ss, 8_ss, 0_ss)}}));
                    sim.unitScriptDefinitions.emplace(name, CobScript{{}, {"base"}, {}, 0});
                }
            }
            UnitId spawn(const std::string& type = "BOMB", PlayerId owner = PlayerId(0), int x = 8, int z = 8)
            {
                auto p = sim.terrain.heightmapIndexToWorldCenter(x, z);
                p.y = sim.terrain.getHeightAt(p.x, p.z);
                auto id = sim.trySpawnUnit(type, owner, p, std::nullopt);
                REQUIRE(id);
                sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(type));
                return *id;
            }
            void ticks(unsigned int n) { for (unsigned int i = 0; i < n; ++i) { sim.tick(); } }
            GameParameters params() const
            {
                GameParameters p("BOMB_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(100000), Energy(100000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(100000), Energy(100000)};
                return p;
            }
            unsigned int deaths(UnitId id) const
            {
                unsigned int count = 0;
                for (const auto& e : sim.events) { if (auto d = std::get_if<UnitDiedEvent>(&e); d && d->unitId == id) { ++count; } }
                return count;
            }
            std::string payload(UnitId id) const
            {
                for (const auto& e : sim.events) { if (auto d = std::get_if<UnitDiedEvent>(&e); d && d->unitId == id) { return d->explosionWeapon; } }
                return {};
            }
        };
    }

    TEST_CASE("kamikaze objective death and protection outcomes survive pending-result saves", "[kamikaze][mission-regression][save]")
    {
        for (bool protect : {false, true})
        {
            BombFixture f;
            auto bomb = f.spawn("BOMB", protect ? PlayerId(1) : PlayerId(0));
            auto target = f.spawn("TARGET", protect ? PlayerId(0) : PlayerId(1), 11);
            f.spawn("BUILDING", PlayerId(0), 30, 30);
            f.sim.getUnitState(target).hitPoints = 50;
            MissionInfo info;
            info.localPlayerId = PlayerId(0);
            info.objectives = protect
                ? std::vector<MissionObjective>{{"KillUnitType", {"BUILDING", "1"}}, {"UnitTypeKilled", {"TARGET", "1"}}}
                : std::vector<MissionObjective>{{"KillUnitType", {"TARGET", "1"}}, {"AllUnitsKilled", {"1"}}};
            f.sim.missionInfo = info;
            f.sim.initMissionObjectives();
            f.sim.getUnitState(bomb).addOrder(AttackOrder(target));
            auto before = decodeSavedGame(encodeSavedGame(f.sim, f.params(), nullptr, "BEFORE-BLAST", 0));
            BombFixture restored; applySavedGame(restored.sim, before, nullptr);
            bool savedLatch = false;
            for (unsigned int i = 0; i < 300; ++i)
            {
                f.sim.tick(); restored.sim.tick();
                REQUIRE(f.sim.computeHash() == restored.sim.computeHash());
                REQUIRE(f.sim.getMissionResult() == restored.sim.getMissionResult());
                if (!savedLatch && !f.sim.tryGetUnitState(target))
                {
                    savedLatch = true;
                    REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
                    auto after = decodeSavedGame(encodeSavedGame(f.sim, f.params(), nullptr, "AFTER-BLAST", 0));
                    applySavedGame(restored.sim, after, nullptr);
                }
            }
            REQUIRE(savedLatch);
            REQUIRE(f.deaths(target) == 1);
            REQUIRE(f.sim.getMissionResult() == (protect ? MissionResult::Defeat : MissionResult::Victory));
        }
    }

    TEST_CASE("kamikaze FBI fields retain independent death and mine contracts", "[kamikaze]")
    {
        auto f = parseUnitFbi(parseTdfFromString("[UNITINFO]{UnitName=X;ObjectName=X;SoundCategory=X;Kamikaze=1;KamikazeDistance=65537;ExplodeAs=A;SelfDestructAs=B;DefaultMissionType=Standby_Mine;ShootMe=1;NoChaseCategory=VTOL NOTSUB;}"));
        MovementClassDatabase db;
        auto d = parseUnitDefinition(f, db);
        REQUIRE(d.kamikaze); REQUIRE(d.kamikazeDistance == 1_ss);
        REQUIRE(d.standbyMine); REQUIRE(d.shootMe);
        REQUIRE(d.explodeAs == "A"); REQUIRE(d.selfDestructAs == "B");
        REQUIRE(d.noChaseCategories.count("VTOL") == 1);
        auto empty = parseUnitDefinition(parseUnitFbi(parseTdfFromString("[UNITINFO]{UnitName=Y;ObjectName=Y;SoundCategory=Y;}")), db);
        REQUIRE_FALSE(empty.kamikaze); REQUIRE_FALSE(empty.standbyMine); REQUIRE(empty.selfDestructAs.empty());
    }

    TEST_CASE("normal death and manual or scripted self destruction select distinct payloads exactly once", "[kamikaze][explosion]")
    {
        for (int mode = 0; mode < 3; ++mode)
        {
            BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 11);
            if (mode == 0) { f.sim.applyDamage(b, 100, t); }
            else
            {
                SelfDestructOrder order{0}; order.fuseTicks = 0;
                if (mode == 1) { f.sim.getUnitState(b).selfDestruct = order; }
                else { f.sim.getUnitState(b).addOrder(order); }
                UnitBehaviorService(&f.sim).update(b);
            }
            REQUIRE(f.sim.getUnitState(b).isDead());
            REQUIRE(f.payload(b) == (mode == 0 ? "SMALL" : "LARGE"));
            REQUIRE(f.sim.getUnitState(t).hitPoints < 1000);
            auto hp = f.sim.getUnitState(t).hitPoints;
            f.sim.killUnit(b); f.sim.selfDestructUnit(b); f.sim.applyDamage(b, 30000, t);
            UnitBehaviorService(&f.sim).update(b);
            REQUIRE(f.deaths(b) == 1); REQUIRE(f.sim.getUnitState(t).hitPoints == hp);
            REQUIRE(f.sim.players[0].unitsLost == 1);
            REQUIRE(f.sim.players[1].unitsKilled == (mode == 0 ? 1 : 0));
        }
    }

    TEST_CASE("kamikaze approach uses navigation and the strict authored planar arrival radius", "[kamikaze]")
    {
        for (int radius : {0, 40, 80})
        {
            BombFixture f; f.sim.unitDefinitions.at("BOMB").kamikazeDistance = SimScalar(radius);
            auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 20);
            auto& bomb = f.sim.getUnitState(b);
            bomb.addOrder(AttackOrder(t));
            f.ticks(5);
            REQUIRE(f.sim.tryGetUnitState(b)); REQUIRE(bomb.navigationState.desiredDestination);
            auto distance = std::max(16, radius);
            auto& target = f.sim.getUnitState(t);
            target.position = bomb.position + SimVector(SimScalar(distance), 200_ss, 0_ss);
            f.sim.rebuildOccupancyAfterLoad();
            UnitBehaviorService(&f.sim).update(b);
            REQUIRE_FALSE(bomb.isDead());
            target.position = bomb.position + SimVector(SimScalar(distance) - 0.5_ssf, 200_ss, 0_ss);
            f.sim.rebuildOccupancyAfterLoad();
            UnitBehaviorService(&f.sim).update(b);
            REQUIRE(bomb.isDead()); REQUIRE(f.payload(b) == "LARGE"); REQUIRE(f.deaths(b) == 1);
        }
    }

    TEST_CASE("kamikaze physically approaches a distant attack target and detonates", "[kamikaze]")
    {
        BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 20);
        auto start = f.sim.getUnitState(b).position;
        f.sim.getUnitState(b).addOrder(AttackOrder(t)); f.ticks(30);
        REQUIRE(f.sim.tryGetUnitState(b)); REQUIRE((f.sim.getUnitState(b).position != start));
        for (unsigned int i = 0; i < 500 && f.sim.tryGetUnitState(b); ++i) { f.ticks(1); }
        REQUIRE_FALSE(f.sim.tryGetUnitState(b)); REQUIRE(f.deaths(b) == 1); REQUIRE(f.payload(b) == "LARGE");
    }

    TEST_CASE("kamikaze target removal and exhausted paths never trigger a blast", "[kamikaze]")
    {
        BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 30);
        auto& u = f.sim.getUnitState(b); u.addOrder(AttackOrder(t)); f.ticks(2);
        SECTION("target is removed") { f.sim.quietlyKillUnit(t); f.sim.deleteDeadUnits(); }
        SECTION("target is already dead this tick") { f.sim.killUnit(t); }
        SECTION("path cannot reach trigger distance")
        {
            auto& nav = std::get<NavigationStateMoving>(u.navigationState.state);
            nav.pathExhausted = true; nav.pathExhaustedAt = f.sim.gameTime;
        }
        UnitBehaviorService(&f.sim).update(b);
        REQUIRE(u.isAlive()); REQUIRE(u.orders.empty()); REQUIRE(f.deaths(b) == 0);
        f.ticks(3); REQUIRE(u.navigationState.desiredDestination == std::nullopt);
    }

    TEST_CASE("kamikaze standby acquires only eligible detected enemies and respects standing fire", "[kamikaze]")
    {
        for (const auto mode : {UnitFireOrders::HoldFire, UnitFireOrders::ReturnFire, UnitFireOrders::FireAtWill})
        {
            BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 13);
            f.sim.getUnitState(b).fireOrders = mode; f.ticks(2);
            REQUIRE(f.sim.getUnitState(b).orders.empty() == (mode != UnitFireOrders::FireAtWill));
            if (mode == UnitFireOrders::ReturnFire)
            {
                f.sim.applyDamage(b, 1, t); f.ticks(180);
                REQUIRE_FALSE(f.sim.tryGetUnitState(b)); REQUIRE(f.payload(b) == "LARGE");
            }
        }
        for (int exclusion = 0; exclusion < 6; ++exclusion)
        {
            BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 13);
            f.sim.getUnitState(b).fireOrders = UnitFireOrders::FireAtWill;
            auto& target = f.sim.getUnitState(t);
            if (exclusion == 0) { target.owner = PlayerId(0); }
            if (exclusion == 1) { f.sim.players[0].allyGroup = 1; f.sim.players[1].allyGroup = 1; }
            if (exclusion == 2) { target.missionState = MissionUnitState{}; target.missionState->immunity = true; }
            if (exclusion == 3) { target.carriedBy = b; }
            if (exclusion == 4) { f.sim.unitDefinitions.at("TARGET").shootMe = false; }
            if (exclusion == 5) { f.sim.lineOfSightEnabled = true; f.sim.unitDefinitions.at("BOMB").sightDistance = 0_ss; }
            f.ticks(5); REQUIRE(f.sim.getUnitState(b).orders.empty()); REQUIRE(f.deaths(b) == 0);
        }
    }

    TEST_CASE("kamikaze standby never replaces a move or wait and armed units use weapons", "[kamikaze]")
    {
        for (bool armed : {false, true})
        {
            BombFixture f; auto b = f.spawn(armed ? "ARMED" : "BOMB"); auto t = f.spawn("TARGET", PlayerId(1), 10);
            auto& u = f.sim.getUnitState(b); u.fireOrders = UnitFireOrders::FireAtWill;
            u.addOrder(WaitOrder(30)); f.ticks(3);
            REQUIRE(u.isAlive()); REQUIRE(std::holds_alternative<WaitOrder>(u.orders.front()));
            u.replaceOrders({AttackOrder(t)}); f.ticks(1);
            REQUIRE(f.deaths(b) == (armed ? 0 : 1));
        }
    }

    TEST_CASE("mine standby uses sight radius grounded targets and its own authored activation", "[kamikaze][mine]")
    {
        for (int kind = 0; kind < 6; ++kind)
        {
            BombFixture f; auto m = f.spawn("MINE");
            auto t = f.spawn(kind == 1 ? "BUILDING" : kind == 2 ? "AIR" : "TARGET", PlayerId(1), kind == 3 ? 18 : 12);
            if (kind == 2)
            {
                f.sim.getUnitState(t).physics = UnitPhysicsInfoAir{AirMovementStateFlying{}};
                f.sim.rebuildOccupancyAfterLoad();
            }
            if (kind == 4) { f.sim.getUnitState(m).fireOrders = UnitFireOrders::HoldFire; }
            if (kind == 5) { f.sim.unitDefinitions.at("MINE").standbyMine = false; }
            f.ticks(3);
            REQUIRE(f.deaths(m) == (kind == 0 ? 1 : 0));
            if (kind == 0) { REQUIRE(f.payload(m) == "LARGE"); }
        }
    }

    TEST_CASE("explosions damage friends enemies airborne units and passable mines once", "[kamikaze][explosion]")
    {
        BombFixture f; auto b = f.spawn(); auto ally = f.spawn("TARGET", PlayerId(0), 11);
        auto enemy = f.spawn("TARGET", PlayerId(1), 8, 11);
        auto mine = f.spawn("MINE", PlayerId(1), 5);
        f.sim.unitDefinitions.at("MINE").maxHitPoints = 1000;
        f.sim.getUnitState(mine).hitPoints = 1000;
        auto air = f.spawn("AIR", PlayerId(1), 8, 5);
        f.sim.getUnitState(air).physics = UnitPhysicsInfoAir{AirMovementStateFlying{}};
        f.sim.rebuildOccupancyAfterLoad();
        f.sim.selfDestructUnit(b);
        auto hp = f.sim.getUnitState(ally).hitPoints;
        REQUIRE(hp < 1000); REQUIRE(hp > 800);
        REQUIRE(f.sim.getUnitState(enemy).hitPoints == hp);
        REQUIRE(f.sim.getUnitState(mine).hitPoints == hp);
        REQUIRE(f.sim.getUnitState(air).hitPoints == hp);
        REQUIRE(f.sim.players[0].unitsKilled == 0); REQUIRE(f.sim.players[1].unitsKilled == 0);
    }

    TEST_CASE("death explosion victims never credit the detonator or a stale attacker", "[kamikaze][explosion]")
    {
        BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 10);
        auto attacker = f.spawn("TARGET", PlayerId(0), 40);
        f.sim.applyDamage(t, 950, attacker); f.sim.applyDamage(b, 1, t);
        f.sim.selfDestructUnit(b);
        REQUIRE(f.sim.getUnitState(t).isDead()); REQUIRE(f.sim.players[0].unitsKilled == 0);
        REQUIRE(f.sim.players[1].unitsKilled == 0);
        REQUIRE(f.sim.players[0].unitsLost == 1); REQUIRE(f.sim.players[1].unitsLost == 1);
    }

    TEST_CASE("chain reactions latch each death before its blast and drain without repeated damage", "[kamikaze][explosion]")
    {
        BombFixture f;
        f.sim.weaponDefinitions.at("SMALL").damage["DEFAULT"] = 10000;
        f.sim.weaponDefinitions.at("SMALL").damageRadius = 50_ss;
        std::vector<UnitId> bombs;
        for (int z = 4; z < 44; ++z) { for (int x = 4; x < 44; ++x) { bombs.push_back(f.spawn("BOMB", PlayerId(0), x, z)); } }
        f.sim.killUnit(bombs.front());
        for (auto id : bombs) { REQUIRE(f.sim.getUnitState(id).isDead()); REQUIRE(f.deaths(id) == 1); }
        REQUIRE(f.sim.players[0].unitsLost == bombs.size()); REQUIRE(f.sim.deathExplosions.empty());
        REQUIRE_FALSE(f.sim.drainingDeathExplosions);
    }

    TEST_CASE("EMP blocks automatic detonation and approach orders while manual fuse stays independent", "[kamikaze][emp]")
    {
        for (bool manual : {false, true})
        {
            BombFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 10);
            auto& u = f.sim.getUnitState(b); u.addOrder(AttackOrder(t));
            f.sim.applyDamage(b, 10, std::nullopt, false, true);
            if (manual) { SelfDestructOrder o{0}; o.fuseTicks = 0; u.selfDestruct = o; }
            f.ticks(1); REQUIRE(f.deaths(b) == (manual ? 1 : 0));
            if (!manual)
            {
                REQUIRE(u.isParalyzed()); f.ticks(9); REQUIRE(f.deaths(b) == 0);
                f.ticks(1); REQUIRE(f.deaths(b) == 1);
            }
            REQUIRE(f.payload(b) == "LARGE");
        }
    }

    TEST_CASE("kamikaze save continuation before activation during approach before detonation and after death", "[kamikaze][save]")
    {
        const auto saveVersion = GENERATE(14u, 15u, SaveGameFormatVersion);
        for (int phase = 0; phase < 5; ++phase)
        {
            BombFixture a; auto b = a.spawn(); auto t = a.spawn("TARGET", PlayerId(1), phase == 3 ? 10 : 20);
            auto& u = a.sim.getUnitState(b);
            if (phase == 1) { u.fireOrders = UnitFireOrders::FireAtWill; a.ticks(3); REQUIRE(u.nextKamikazeScan); }
            if (phase == 2) { u.addOrder(AttackOrder(t)); a.ticks(15); }
            if (phase == 3) { u.addOrder(AttackOrder(t)); }
            if (phase == 4) { a.sim.selfDestructUnit(b); }
            auto saved = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "BOMB", 0, saveVersion));
            REQUIRE(saved.version == saveVersion);
            BombFixture copy; applySavedGame(copy.sim, saved, nullptr);
            REQUIRE(a.sim.computeHash() == copy.sim.computeHash());
            a.sim.events.clear(); copy.sim.events.clear();
            for (int i = 0; i < 160; ++i)
            {
                a.ticks(1); copy.ticks(1);
                REQUIRE(a.sim.computeHash() == copy.sim.computeHash()); REQUIRE(a.deaths(b) == copy.deaths(b));
            }
            REQUIRE(copy.deaths(b) <= 1);
        }
    }

    TEST_CASE("kamikaze scan hash includes deadline presence and older saves start fresh", "[kamikaze][save]")
    {
        BombFixture f; auto b = f.spawn(); auto original = f.sim.computeHash();
        f.sim.getUnitState(b).nextKamikazeScan = GameTime(0); REQUIRE(f.sim.computeHash() != original);
        auto zero = f.sim.computeHash(); f.sim.getUnitState(b).nextKamikazeScan = GameTime(1); REQUIRE(f.sim.computeHash() != zero);
        REQUIRE_THROWS_AS(encodeSavedGame(f.sim, f.params(), nullptr, "OLD", 0, 9), SaveFormatException);
        f.sim.getUnitState(b).nextKamikazeScan = std::nullopt;
        for (std::uint32_t v = 1; v <= 9; ++v)
        {
            auto bytes = encodeSavedGame(f.sim, f.params(), nullptr, "OLD", 0, std::max(v, 4u));
            if (v < 4)
            {
                std::uint32_t encoded = 0; auto payload = decodeSaveFile(bytes, encoded);
                payload.resize(payload.size() - (v == 1 ? 34 : 30)); bytes = encodeSaveFile(v, payload);
            }
            BombFixture copy; applySavedGame(copy.sim, decodeSavedGame(bytes), nullptr);
            REQUIRE_FALSE(copy.sim.getUnitState(b).nextKamikazeScan);
            copy.ticks(1); REQUIRE(copy.sim.getUnitState(b).nextKamikazeScan == GameTime(2));
        }
    }

    TEST_CASE("human and AI kamikaze replays are deterministic through scans commands and destruction", "[kamikaze][determinism]")
    {
        BombFixture a; BombFixture b;
        for (auto* f : {&a, &b})
        {
            auto human = f->spawn("BOMB", PlayerId(0), 8, 8);
            auto ai = f->spawn("BOMB", PlayerId(1), 13, 8);
            f->sim.getUnitState(human).fireOrders = UnitFireOrders::FireAtWill;
            f->sim.getUnitState(ai).fireOrders = UnitFireOrders::FireAtWill;
        }
        for (int tick = 0; tick < 240; ++tick)
        {
            a.ticks(1); b.ticks(1); REQUIRE(a.sim.computeHash() == b.sim.computeHash());
        }
        REQUIRE(a.sim.players[0].unitsLost == 1); REQUIRE(a.sim.players[1].unitsLost == 1);
    }

    TEST_CASE("non kamikaze missing payload zero radius and unfinished units remain safe", "[kamikaze][explosion]")
    {
        BombFixture f; auto normal = f.spawn("TARGET"); auto enemy = f.spawn("TARGET", PlayerId(1), 10);
        f.sim.getUnitState(normal).addOrder(AttackOrder(enemy)); f.ticks(2); REQUIRE(f.deaths(normal) == 0);
        auto bomb = f.spawn("BOMB", PlayerId(0), 20);
        SECTION("absent self payload does not fall back to ExplodeAs") { f.sim.unitDefinitions.at("BOMB").selfDestructAs = "MISSING"; }
        SECTION("zero radius is harmless") { f.sim.weaponDefinitions.at("LARGE").damageRadius = 0_ss; }
        SECTION("unfinished units die quietly") { f.sim.getUnitState(bomb).buildTimeCompleted = 0; }
        auto hp = f.sim.getUnitState(enemy).hitPoints; f.sim.selfDestructUnit(bomb);
        REQUIRE(f.sim.getUnitState(bomb).isDead()); REQUIRE(f.sim.getUnitState(enemy).hitPoints == hp);
    }

    TEST_CASE("carrier self destruction propagates its cause to cargo and normal death stays normal", "[kamikaze][transport]")
    {
        for (bool self : {false, true})
        {
            BombFixture f; auto carrier = f.spawn(); auto cargo = f.spawn("BOMB", PlayerId(0), 30);
            f.sim.getUnitState(cargo).carriedBy = carrier; f.sim.getUnitState(carrier).passengers.push_back(cargo);
            if (self) { f.sim.selfDestructUnit(carrier); } else { f.sim.killUnit(carrier); }
            REQUIRE(f.payload(carrier) == (self ? "LARGE" : "SMALL")); REQUIRE(f.payload(cargo) == f.payload(carrier));
            REQUIRE(f.deaths(cargo) == 1); REQUIRE(f.sim.players[0].unitsLost == 2);
        }
    }

    TEST_CASE("mounted retail crawling bombs and twelve mine definitions preserve their distinct contracts", "[kamikaze][retail]")
    {
        auto path = std::getenv("RWE_DATA_PATH");
        if (!path || !std::filesystem::exists(path)) { SKIP("RWE_DATA_PATH required for retail data gate"); }
        auto vfs = constructVfs(path); MovementClassDatabase db;
        unsigned int bombs = 0, mines = 0;
        for (const auto& filename : vfs.getFileNames("units", ".fbi"))
        {
            auto bytes = vfs.readFile("units/" + filename); REQUIRE(bytes);
            auto fbi = parseUnitFbi(parseTdfFromString(std::string(bytes->data(), bytes->size())));
            auto d = parseUnitDefinition(fbi, db);
            if (!d.kamikaze) { continue; }
            REQUIRE(d.weapon1.empty()); REQUIRE(d.weapon2.empty()); REQUIRE(d.weapon3.empty());
            if (d.standbyMine)
            {
                ++mines; REQUIRE_FALSE(d.isMobile); REQUIRE_FALSE(d.canAttack); REQUIRE(d.initCloaked);
                REQUIRE(d.explodeAs == d.selfDestructAs); REQUIRE(d.kamikazeDistance == 0_ss);
                bool manual = toUpper(fbi.unitName) == "ARMMINE6" || toUpper(fbi.unitName) == "CORMINE6";
                REQUIRE(d.standingFireOrder == (manual ? UnitFireOrders::HoldFire : UnitFireOrders::FireAtWill));
            }
            else
            {
                ++bombs; REQUIRE(d.isMobile); REQUIRE(d.canAttack); REQUIRE(d.standingFireOrder == UnitFireOrders::HoldFire);
                REQUIRE(d.explodeAs == "CRAWL_BLASTSML"); REQUIRE(d.selfDestructAs == "CRAWL_BLAST");
                REQUIRE(d.kamikazeDistance == (toUpper(fbi.unitName) == "CORROACH" ? 40_ss : 80_ss));
            }
        }
        REQUIRE(bombs == 2); REQUIRE(mines == 12);
        for (const auto& filename : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + filename); REQUIRE(bytes);
            for (const auto& [name, tdf] : parseWeaponTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
            {
                if (toUpper(name) != "CRAWL_BLAST" && toUpper(name) != "CRAWL_BLASTSML") { continue; }
                auto w = parseWeaponDefinition(tdf); bool large = toUpper(name) == "CRAWL_BLAST";
                REQUIRE(w.damage.at("DEFAULT") == (large ? 2500 : 1200));
                REQUIRE(w.damageRadius == (large ? 278_ss : 137.5_ssf));
            }
        }
    }
    TEST_CASE("carried bomb keeps an independent manual fuse and cannot auto acquire", "[kamikaze][transport][save]")
    {
        BombFixture f; auto carrier = f.spawn("TARGET"); auto cargo = f.spawn("BOMB", PlayerId(0), 30);
        auto enemy = f.spawn("TARGET", PlayerId(1), 10);
        auto& u = f.sim.getUnitState(cargo); u.carriedBy = carrier;
        f.sim.getUnitState(carrier).passengers.push_back(cargo); u.fireOrders = UnitFireOrders::FireAtWill;
        f.ticks(3); REQUIRE(f.deaths(cargo) == 0); REQUIRE(u.orders.empty());
        f.sim.applyDamage(cargo, 30000, enemy); REQUIRE(u.isAlive());
        u.selfDestruct = SelfDestructOrder{2}; u.selfDestruct->fuseTicks = 0;
        auto save = decodeSavedGame(encodeSavedGame(f.sim, f.params(), nullptr, "CARGO", 0));
        BombFixture restored; applySavedGame(restored.sim, save, nullptr);
        f.ticks(1); restored.ticks(1);
        REQUIRE(f.sim.computeHash() == restored.sim.computeHash());
        REQUIRE_FALSE(f.sim.tryGetUnitState(cargo)); REQUIRE(f.payload(cargo) == "LARGE");
        REQUIRE(f.sim.getUnitState(carrier).passengers.empty()); REQUIRE(f.sim.getUnitState(carrier).hitPoints < 1000);
    }

    TEST_CASE("retail explosive definitions execute through native behavior and damage", "[kamikaze][retail]")
    {
        const auto* path = std::getenv("RWE_DATA_PATH");
        if (!path || !std::filesystem::exists(path)) { SKIP("RWE_DATA_PATH required for retail gate"); }
        auto vfs = constructVfs(path); MovementClassDatabase db;
        std::unordered_map<std::string, WeaponDefinition> weapons;
        for (const auto& filename : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + filename); REQUIRE(bytes);
            for (const auto& [name, tdf] : parseWeaponTdf(parseTdfFromString(std::string(bytes->begin(), bytes->end()))))
            { weapons.emplace(toUpper(name), parseWeaponDefinition(tdf)); }
        }
        unsigned int ran = 0;
        for (const auto& filename : vfs.getFileNames("units", ".fbi"))
        {
            auto bytes = vfs.readFile("units/" + filename); REQUIRE(bytes);
            auto d = parseUnitDefinition(parseUnitFbi(parseTdfFromString(std::string(bytes->begin(), bytes->end()))), db);
            if (!d.kamikaze) { continue; }
            INFO(filename);
            BombFixture f;
            // Real FBI/weapon contracts, synthetic flat map/model/script. Full
            // retail model/COB initialization is covered by the mission gate.
            d.objectName = "BOMB";
            f.sim.unitDefinitions.insert_or_assign("BOMB", d); f.sim.weaponDefinitions = weapons;
            auto b = f.spawn(); auto target = f.spawn("TARGET", PlayerId(1), 10);
            if (!d.standbyMine) { f.sim.getUnitState(b).addOrder(AttackOrder(target)); }
            f.ticks(3);
            if (d.standbyMine && d.standingFireOrder == UnitFireOrders::HoldFire)
            {
                REQUIRE(f.deaths(b) == 0); f.sim.selfDestructUnit(b);
            }
            REQUIRE(f.deaths(b) == 1); REQUIRE(f.payload(b) == toUpper(d.selfDestructAs));
            ++ran;
        }
        REQUIRE(ran == 14);
    }

    TEST_CASE("kamikaze respects no chase categories and computer opportunity targeting", "[kamikaze]")
    {
        BombFixture f; auto bomb = f.spawn(); auto target = f.spawn("TARGET", PlayerId(1), 13);
        f.sim.unitDefinitions.at("BOMB").noChaseCategories.insert("AVOID");
        f.sim.unitDefinitions.at("TARGET").categories.insert("AVOID");
        auto& u = f.sim.getUnitState(bomb); u.fireOrders = UnitFireOrders::FireAtWill;
        f.ticks(3); REQUIRE(u.orders.empty());
        f.sim.unitDefinitions.at("BOMB").noChaseCategories.clear();
        f.sim.unitDefinitions.at("TARGET").shootMe = false;
        f.sim.players[0].type = GamePlayerType::Computer;
        u.nextKamikazeScan = f.sim.gameTime;
        f.ticks(1); REQUIRE_FALSE(u.orders.empty());
        REQUIRE((std::get<AttackOrder>(u.orders.front()).target == AttackTarget(target)));
    }

    TEST_CASE("kamikaze follows a moving target without discarding its live path each tick", "[kamikaze]")
    {
        BombFixture f;
        f.sim.unitDefinitions.at("TARGET").maxVelocity = 0.5_ssf;
        auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 16);
        auto start = f.sim.getUnitState(b).position;
        auto targetStart = f.sim.getUnitState(t).position;
        f.sim.getUnitState(b).addOrder(AttackOrder(t));
        f.sim.getUnitState(t).addOrder(MoveOrder(targetStart + SimVector(160_ss, 0_ss, 160_ss)));
        f.ticks(30);
        REQUIRE(f.sim.tryGetUnitState(b));
        REQUIRE((f.sim.getUnitState(b).position != start));
        REQUIRE((f.sim.getUnitState(t).position != targetStart));
        for (unsigned int i = 0; i < 300 && f.sim.tryGetUnitState(b); ++i) { f.ticks(1); }
        REQUIRE_FALSE(f.sim.tryGetUnitState(b)); REQUIRE(f.deaths(b) == 1); REQUIRE(f.payload(b) == "LARGE");
    }

}
