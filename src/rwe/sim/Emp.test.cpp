#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/cob/CobOpCode.h>
#include <rwe/sim/cob.h>
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
        struct EmpFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0};
            PlayerId human{0};
            PlayerId computer{1};

            EmpFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_emp_test.log", false));
                sim.lineOfSightEnabled = false;
                for (unsigned int i = 0; i < 2; ++i)
                {
                    sim.addPlayer(GamePlayerInfo{std::nullopt,
                        i == 0 ? GamePlayerType::Human : GamePlayerType::Computer,
                        PlayerColorIndex(i), GamePlayerStatus::Alive, i == 0 ? "ARM" : "CORE",
                        std::nullopt, Metal(100000), Energy(100000), Metal(100000), Energy(100000), Metal(100000), Energy(100000)});
                }
                for (const auto* name : {"MOBILE", "FACTORY", "IMMUNE", "AIR"})
                {
                    UnitDefinition def{};
                    def.unitName = name;
                    def.objectName = name;
                    def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 255, 255, 0, 0};
                    def.maxHitPoints = 1000;
                    def.buildTime = 1000;
                    def.workerTimePerTick = 10;
                    def.builder = true;
                    def.buildDistance = 300_ss;
                    def.canMove = true;
                    def.canAttack = true;
                    def.standingFireOrder = UnitFireOrders::FireAtWill;
                    def.isMobile = true;
                    def.maxVelocity = 2_ss;
                    def.acceleration = 1_ss;
                    def.brakeRate = 1_ss;
                    def.turnRate = 1024_ss;
                    def.sightDistance = 500_ss;
                    def.energyStorage = Energy(100000);
                    def.metalStorage = Metal(100000);
                    if (std::string(name) == "FACTORY")
                    {
                        def.isMobile = false;
                        def.canMove = false;
                        def.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Passable);
                    }
                    def.immuneToParalyzer = std::string(name) == "IMMUNE";
                    def.canFly = std::string(name) == "AIR";
                    def.cruiseAltitude = 50_ss;
                    sim.unitDefinitions.emplace(name, def);
                    sim.unitModelDefinitions.emplace(name, createUnitModelDefinition(20_ss, {UnitPieceDefinition{"flare", SimVector(0_ss, 10_ss, 0_ss)}}));
                    sim.unitScriptDefinitions.emplace(name, CobScript{{}, {"flare"}, {}, 0});
                }
                for (const auto* name : {"GUN", "EMP"})
                {
                    WeaponDefinition def{};
                    def.paralyzer = std::string(name) == "EMP";
                    def.maxRange = 500_ss;
                    def.velocity = 10_ss;
                    def.damageRadius = 40_ss;
                    def.damage = {{"DEFAULT", 90}};
                    def.reloadTime = 1_ss;
                    def.burst = 1;
                    sim.weaponDefinitions.emplace(name, def);
                }
            }

            UnitId spawn(const std::string& type = "MOBILE", PlayerId owner = PlayerId(0), int x = 8, int z = 8)
            {
                auto position = sim.terrain.heightmapIndexToWorldCenter(x, z);
                position.y = sim.terrain.getHeightAt(position.x, position.z);
                auto id = sim.trySpawnUnit(type, owner, position, std::nullopt);
                REQUIRE(id);
                sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(type));
                return *id;
            }

            void emp(UnitId id, unsigned int credit)
            {
                sim.applyDamage(id, credit, std::nullopt, false, true);
            }

            void ticks(unsigned int n)
            {
                for (unsigned int i = 0; i < n; ++i) { sim.tick(); }
            }

            GameParameters params() const
            {
                GameParameters p("EMP_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(100000), Energy(100000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(100000), Energy(100000)};
                return p;
            }
        };
    }

    TEST_CASE("deferred and cancelled aim results retain a saveable thread identity", "[mission-regression][save]")
    {
        EmpFixture f;
        f.sim.unitScriptDefinitions["MOBILE"] = CobScript{
            {static_cast<uint32_t>(OpCode::PUSH_CONSTANT), 1, static_cast<uint32_t>(OpCode::RETURN)},
            {"flare"}, {{"AimPrimary", 0}}, 0};
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        auto thread = u.cobEnvironment->createThread("AimPrimary");
        REQUIRE(thread);
        u.weapons[0] = UnitWeapon{"GUN"};
        UnitWeaponStateAttacking attack(SimVector(0_ss, 0_ss, 0_ss));
        attack.attackInfo = UnitWeaponStateAttacking::AimInfo{*thread, SimAngle(0), SimAngle(0)};
        u.weapons[0]->state = attack;
        int expected = 1;
        SECTION("finished result survives a deferred weapon pass")
        {
            runUnitCobScripts(f.sim, id);
            runUnitCobScripts(f.sim, id);
        }
        SECTION("signal cancellation remains observable by the aim consumer")
        {
            u.cobEnvironment->threads.back()->signalMask = 2;
            u.cobEnvironment->sendSignal(2);
            runUnitCobScripts(f.sim, id);
            expected = 0;
        }
        REQUIRE(u.cobEnvironment->tryReapThread(*thread) == expected);
        REQUIRE_NOTHROW(encodeSavedGame(f.sim, f.params(), nullptr, "AIM-EMP", 0));
        auto save = decodeSavedGame(encodeSavedGame(f.sim, f.params(), nullptr, "AIM-EMP", 0));
        EmpFixture restored;
        restored.sim.unitScriptDefinitions["MOBILE"] = f.sim.unitScriptDefinitions.at("MOBILE");
        applySavedGame(restored.sim, save, nullptr);
        REQUIRE(f.sim.computeHash() == restored.sim.computeHash());
        const auto& restoredUnit = restored.sim.getUnitState(id);
        const auto& restoredAttack = std::get<UnitWeaponStateAttacking>(restoredUnit.weapons[0]->state);
        const auto& restoredAim = std::get<UnitWeaponStateAttacking::AimInfo>(restoredAttack.attackInfo);
        REQUIRE(restoredUnit.cobEnvironment->tryReapThread(restoredAim.thread) == expected);
        REQUIRE(encodeSavedGame(f.sim, f.params(), nullptr, "AIM-EMP", 0)
            == encodeSavedGame(restored.sim, restored.params(), nullptr, "AIM-EMP", 0));
        for (unsigned int i = 0; i < 100; ++i)
        {
            f.sim.tick(); restored.sim.tick();
            REQUIRE(f.sim.computeHash() == restored.sim.computeHash());
        }
    }

    TEST_CASE("EMP flags propagate from TDF and FBI into simulation definitions", "[emp]")
    {
        auto w = parseWeaponBlock(parseTdfFromString("[W] { paralyzer=1; damage=0; }").findBlock("W")->get());
        REQUIRE(parseWeaponDefinition(w).paralyzer);
        auto fbi = parseUnitFbi(parseTdfFromString("[UNITINFO] { UnitName=X; ObjectName=X; SoundCategory=X; ImmuneToParalyzer=1; }"));
        MovementClassDatabase mcd;
        REQUIRE(fbi.immuneToParalyzer);
        REQUIRE(parseUnitDefinition(fbi, mcd).immuneToParalyzer);
        REQUIRE_FALSE(WeaponDefinition{}.paralyzer);
        REQUIRE_FALSE(UnitDefinition{}.immuneToParalyzer);
    }

    TEST_CASE("EMP projectile impacts separate paralysis from ordinary HP damage", "[emp]")
    {
        for (bool emp : {false, true})
        {
            for (const auto* type : {"MOBILE", "AIR"})
            {
                EmpFixture f;
                auto id = f.spawn(type, f.computer);
                auto& u = f.sim.getUnitState(id);
                if (std::string(type) == "AIR")
                {
                    u.position.y += 50_ss;
                    u.physics = UnitPhysicsInfoAir{};
                    f.sim.rebuildOccupancyAfterLoad();
                }
                auto p = f.sim.createProjectileFromWeapon(f.human, emp ? "EMP" : "GUN", u.position,
                    SimVector(1_ss, 0_ss, 0_ss), 100_ss, id);
                f.sim.applyDamageInRadius(u.position, p.damageRadius, p);
                REQUIRE(u.hitPoints == (emp ? 1000 : 910));
                REQUIRE((!u.orders.empty() && std::holds_alternative<ParalyzeOrder>(u.orders.front())) == emp);
            }
        }
        SECTION("noexplode direct collisions also preserve HP")
        {
            EmpFixture f;
            auto id = f.spawn("MOBILE", f.computer);
            auto& u = f.sim.getUnitState(id);
            f.sim.weaponDefinitions.at("EMP").noExplode = true;
            auto p = f.sim.createProjectileFromWeapon(f.human, "EMP", u.position + SimVector(-5_ss, 10_ss, 0_ss),
                SimVector(1_ss, 0_ss, 0_ss), 100_ss, id);
            f.sim.projectiles.emplace(std::move(p));
            f.sim.updateProjectiles();
            REQUIRE(u.hitPoints == 1000);
            REQUIRE_FALSE(u.orders.empty());
            REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).credit == 90);
        }
    }

    TEST_CASE("EMP eligibility includes buildings and AI and excludes immune cargo and dead units", "[emp]")
    {
        for (const auto* type : {"MOBILE", "FACTORY", "AIR", "IMMUNE"})
        {
            for (auto owner : {PlayerId(0), PlayerId(1)})
            {
                EmpFixture f;
                auto id = f.spawn(type, owner);
                f.emp(id, 90);
                f.ticks(1);
                REQUIRE(f.sim.getUnitState(id).isParalyzed() == (std::string(type) != "IMMUNE"));
                REQUIRE(f.sim.getUnitState(id).hitPoints == 1000);
            }
        }
        EmpFixture f;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        u.missionState = MissionUnitState{};
        u.missionState->immunity = true;
        f.emp(id, 90);
        REQUIRE(u.orders.empty());
        u.missionState->immunity = false;
        u.carriedBy = UnitId(99);
        f.emp(id, 90);
        REQUIRE(u.orders.empty());
        u.carriedBy.reset();
        f.sim.quietlyKillUnit(id);
        f.emp(id, 90);
        REQUIRE(u.orders.empty());
    }

    TEST_CASE("EMP uses credited ticks with a 1800 tick per-visit cap and delayed repeated-hit credit", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        u.addOrder(WaitOrder(20));
        f.emp(id, 90);
        f.ticks(1);
        REQUIRE(u.isParalyzed());
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).resumeAt == GameTime(91));
        f.ticks(30);
        f.emp(id, 3000);
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).resumeAt == GameTime(91));
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).credit == 3000);
        f.ticks(60);
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).resumeAt == GameTime(1891));
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).credit == 0);
        f.ticks(1799);
        REQUIRE(u.isParalyzed());
        f.ticks(1);
        REQUIRE_FALSE(u.isParalyzed());
        REQUIRE(std::get<WaitOrder>(u.orders.front()).ticksRemaining == 19);
        REQUIRE(u.hitPoints == 1000);
    }

    TEST_CASE("EMP zero credit and armored scaling preserve retail packet arithmetic", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        f.emp(id, 0);
        f.ticks(1);
        REQUIRE_FALSE(u.isParalyzed());
        REQUIRE(u.orders.empty());
        u.armored = true;
        f.sim.unitDefinitions.at("MOBILE").damageModifier = 0x8000;
        f.emp(id, 100);
        f.ticks(1);
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).resumeAt == GameTime(52));
        f.emp(id, 20);
        f.emp(id, 40);
        REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).credit == 30);
        REQUIRE(u.hitPoints == 1000);
    }

    TEST_CASE("EMP interrupt survives stop replacement and marker removal", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        u.addOrder(WaitOrder(50));
        f.emp(id, 5);
        f.ticks(1);
        u.clearOrders();
        REQUIRE(u.orders.size() == 1);
        REQUIRE(u.isParalyzed());
        u.removeOrderAt(0);
        REQUIRE(u.orders.size() == 1);
        u.replaceOrders({WaitOrder(7)});
        REQUIRE(u.orders.size() == 2);
        u.navigationState.desiredDestination = SimVector(100_ss, 64_ss, 100_ss);
        u.removeOrderAt(1);
        REQUIRE_FALSE(u.navigationState.desiredDestination);
        REQUIRE(u.isParalyzed());
        u.addOrder(WaitOrder(7));
        f.ticks(5);
        REQUIRE_FALSE(u.isParalyzed());
        REQUIRE(std::get<WaitOrder>(u.orders.front()).ticksRemaining == 6);
    }

    TEST_CASE("EMP blocks new movement orders while preserving an already bound mover", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        auto goal = f.sim.terrain.heightmapIndexToWorldCenter(30, 8);
        u.addOrder(MoveOrder(goal));
        f.ticks(10);
        auto before = u.position;
        f.emp(id, 30);
        f.ticks(10);
        REQUIRE(u.isParalyzed());
        REQUIRE((u.position != before));
        REQUIRE(u.orders.size() == 2);
        REQUIRE(std::holds_alternative<MoveOrder>(u.orders[1]));
        // Explicit replacement destroys the previous goal, but not EMP.
        u.clearOrders();
        u.addOrder(MoveOrder(goal));
        f.ticks(4); // existing ground momentum settles normally
        before = u.position;
        f.ticks(5);
        REQUIRE((u.position == before));
        REQUIRE(u.isParalyzed());
        f.ticks(30);
        REQUIRE_FALSE(u.isParalyzed());
        REQUIRE((u.position != before));
    }

    TEST_CASE("EMP clears weapon targets and blocks acquisition until the primary wait recovers", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto enemy = f.spawn("MOBILE", f.computer, 12, 8);
        auto& u = f.sim.getUnitState(id);
        u.weapons[0] = UnitWeapon{"GUN"};
        u.weapons[0]->state = UnitWeaponStateAttacking(enemy);
        u.addOrder(AttackOrder(enemy));
        f.emp(id, 5);
        f.ticks(1);
        REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(u.weapons[0]->state));
        UnitBehaviorService(&f.sim).update(id);
        REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(u.weapons[0]->state));
        f.ticks(4);
        REQUIRE(u.isParalyzed());
        f.ticks(1);
        REQUIRE_FALSE(u.isParalyzed());
        REQUIRE(std::holds_alternative<UnitWeaponStateAttacking>(u.weapons[0]->state));
    }

    TEST_CASE("EMP auto targeting skips active stun but ordinary and explicit attacks retain eligibility", "[emp]")
    {
        EmpFixture f;
        auto id = f.spawn();
        auto enemy = f.spawn("MOBILE", f.computer, 12, 8);
        auto& u = f.sim.getUnitState(id);
        f.emp(enemy, 90);
        f.ticks(1);
        u.weapons[0] = UnitWeapon{"EMP"};
        UnitBehaviorService(&f.sim).update(id);
        REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(u.weapons[0]->state));
        u.weapons[0]->state = UnitWeaponStateAttacking(enemy);
        UnitBehaviorService(&f.sim).update(id);
        REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(u.weapons[0]->state));
        u.weapons[0] = UnitWeapon{"GUN"};
        UnitBehaviorService(&f.sim).update(id);
        REQUIRE(std::holds_alternative<UnitWeaponStateAttacking>(u.weapons[0]->state));
        u.weapons[0] = UnitWeapon{"EMP"};
        u.addOrder(AttackOrder(enemy));
        UnitBehaviorService(&f.sim).update(id);
        REQUIRE(std::holds_alternative<UnitWeaponStateAttacking>(u.weapons[0]->state));
    }

    TEST_CASE("EMP pauses constructor repair and factory production without deleting their work", "[emp]")
    {
        SECTION("constructor resumes repair")
        {
            EmpFixture f;
            auto id = f.spawn();
            auto target = f.spawn("MOBILE", f.human, 12, 8);
            auto& u = f.sim.getUnitState(id);
            auto& t = f.sim.getUnitState(target);
            t.hitPoints = 100;
            u.addOrder(RepairOrder(target));
            f.ticks(1);
            u.inBuildStance = true;
            f.ticks(1);
            REQUIRE(t.hitPoints > 100);
            auto hp = t.hitPoints;
            f.emp(id, 10);
            f.ticks(10);
            REQUIRE(t.hitPoints == hp);
            REQUIRE_FALSE(u.activeNanolatheTarget);
            f.ticks(1);
            REQUIRE(t.hitPoints > hp);
        }
        SECTION("factory resumes the same product")
        {
            EmpFixture f;
            auto id = f.spawn("FACTORY", f.human, 30, 30);
            auto& u = f.sim.getUnitState(id);
            u.inBuildStance = true;
            u.addOrder(MissionFactoryBuildOrder("MOBILE", 1));
            for (int i = 0; i < 30 && !u.activeNanolatheTarget; ++i) { f.ticks(1); }
            REQUIRE(u.activeNanolatheTarget);
            auto state = std::get<FactoryBehaviorStateBuilding>(u.factoryState);
            REQUIRE(state.targetUnit);
            auto product = state.targetUnit->first;
            auto progress = f.sim.getUnitState(product).buildTimeCompleted;
            f.emp(id, 10);
            f.ticks(10);
            REQUIRE(f.sim.getUnitState(product).buildTimeCompleted == progress);
            REQUIRE_FALSE(u.activeNanolatheTarget);
            f.ticks(1);
            REQUIRE(f.sim.getUnitState(product).buildTimeCompleted > progress);
        }
    }

    TEST_CASE("EMP leaves economy cloak secondary stockpile and incoming ordinary damage active", "[emp]")
    {
        EmpFixture f;
        auto& def = f.sim.unitDefinitions.at("MOBILE");
        def.energyMake = Energy(100);
        def.cloakCost = 10;
        def.cloakCostMoving = 10;
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        u.cloakRequested = true;
        u.activated = true;
        auto& weapon = f.sim.weaponDefinitions.at("GUN");
        weapon.stockpile = true;
        weapon.reloadTime = 1_ss;
        u.weapons[0] = UnitWeapon{"GUN"};
        u.backgroundOrders.emplace_back(BuildWeaponOrder{1});
        f.emp(id, 90);
        f.ticks(30);
        REQUIRE(u.isParalyzed());
        REQUIRE(u.activated);
        REQUIRE(u.cloaked);
        REQUIRE(u.energyProductionBuffer == Energy(100));
        REQUIRE(u.weapons[0]->stockpileCount == 1);
        f.sim.applyDamage(id, 50);
        REQUIRE(u.hitPoints == 950);
        REQUIRE(u.isParalyzed());
        f.sim.applyDamage(id, 1000);
        f.ticks(1);
        REQUIRE_FALSE(f.sim.tryGetUnitState(id));
    }

    TEST_CASE("EMP save restores pending and active repeated-hit waits with deterministic continuation", "[emp][save]")
    {
        const auto saveVersion = GENERATE(14u, 15u, SaveGameFormatVersion);
        for (bool activate : {false, true})
        {
            EmpFixture a;
            auto id = a.spawn();
            a.sim.getUnitState(id).addOrder(WaitOrder(100));
            a.emp(id, 10);
            if (activate) { a.ticks(4); a.emp(id, 12); }
            auto bytes = encodeSavedGame(a.sim, a.params(), nullptr, "EMP", 0, saveVersion);
            auto saved = decodeSavedGame(bytes);
            REQUIRE(saved.version == saveVersion);
            EmpFixture b;
            applySavedGame(b.sim, saved, nullptr);
            REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            auto originalHash = b.sim.computeHash();
            std::get<ParalyzeOrder>(b.sim.getUnitState(id).orders.front()).credit += 1;
            REQUIRE(b.sim.computeHash() != originalHash);
            applySavedGame(b.sim, saved, nullptr);
            auto& restoredOrder = std::get<ParalyzeOrder>(b.sim.getUnitState(id).orders.front());
            restoredOrder.resumeAt = restoredOrder.resumeAt.value_or(GameTime(0)) + GameTime(1);
            REQUIRE(b.sim.computeHash() != originalHash);
            applySavedGame(b.sim, saved, nullptr);
            for (unsigned int i = 0; i < 160; ++i)
            {
                a.ticks(1); b.ticks(1);
                REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            }
            REQUIRE_FALSE(a.sim.getUnitState(id).isParalyzed());
        }
    }

    TEST_CASE("EMP format evolution still reads v1 through v8 and rejects lossy downgrade", "[emp][save]")
    {
        EmpFixture a;
        auto id = a.spawn();
        for (std::uint32_t v = 1; v <= 8; ++v)
        {
            auto bytes = encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, std::max(v, 4u));
            if (v < 4)
            {
                // Match the established legacy fixture contract: the writer
                // emits genuine v4+ only. Idle v1-v3 lack the meteor tail;
                // v1 additionally lacks the empty campaign-AI count.
                std::uint32_t encodedVersion = 0;
                auto payload = decodeSaveFile(bytes, encodedVersion);
                payload.resize(payload.size() - (v == 1 ? 34 : 30));
                bytes = encodeSaveFile(v, payload);
            }
            auto old = decodeSavedGame(bytes);
            EmpFixture b;
            applySavedGame(b.sim, old, nullptr);
            REQUIRE_FALSE(b.sim.getUnitState(id).isParalyzed());
            REQUIRE(b.sim.getUnitState(id).orders.empty());
            b.emp(id, 1);
            b.ticks(1);
            REQUIRE(b.sim.getUnitState(id).isParalyzed());
            b.ticks(1);
            REQUIRE_FALSE(b.sim.getUnitState(id).isParalyzed());
        }
        a.emp(id, 60);
        REQUIRE_THROWS_AS(encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, 8), SaveFormatException);
    }

    TEST_CASE("EMP allows COB and piece animation to continue without deactivation", "[emp]")
    {
        EmpFixture f;
        f.sim.unitScriptDefinitions.at("MOBILE") = CobScript{
            {static_cast<std::uint32_t>(OpCode::PUSH_CONSTANT), 42,
                static_cast<std::uint32_t>(OpCode::POP_STATIC), 0,
                static_cast<std::uint32_t>(OpCode::PUSH_CONSTANT), 0,
                static_cast<std::uint32_t>(OpCode::RETURN)},
            {"flare"}, {{"Probe", 0}}, 1};
        auto id = f.spawn();
        auto& u = f.sim.getUnitState(id);
        u.activated = true;
        REQUIRE(u.cobEnvironment->createThread("Probe"));
        f.sim.moveObject(id, "flare", SimAxis::X, 10_ss, 30_ss);
        f.emp(id, 90);
        f.ticks(1);
        REQUIRE(u.isParalyzed());
        REQUIRE(u.activated);
        REQUIRE(u.cobEnvironment->_statics[0] == 42);
        REQUIRE(u.pieces[0].offset.x > 0_ss);
    }

    TEST_CASE("EMP mixed-owner lockstep repeats identical inputs and hashes from fresh worlds", "[emp][determinism]")
    {
        EmpFixture a;
        EmpFixture b;
        for (auto* f : {&a, &b})
        {
            auto left = f->spawn();
            auto right = f->spawn("MOBILE", f->computer, 24, 24);
            f->sim.getUnitState(left).addOrder(WaitOrder(300));
            f->sim.getUnitState(right).addOrder(WaitOrder(300));
        }
        for (unsigned int tick = 0; tick < 240; ++tick)
        {
            for (auto* f : {&a, &b})
            {
                for (auto& [id, unit] : f->sim.units)
                {
                    if (tick == 0 || tick == 11 || tick == 42) { f->emp(id, 40 + unit.owner.value); }
                    if (tick == 30) { f->sim.applyDamage(id, 7); }
                    if (tick == 60) { unit.clearOrders(); unit.addOrder(WaitOrder(100)); }
                }
                f->ticks(1);
            }
            REQUIRE(a.sim.computeHash() == b.sim.computeHash());
        }
        for (const auto& [id, unit] : a.sim.units)
        {
            REQUIRE_FALSE(unit.isParalyzed());
            REQUIRE(unit.hitPoints == 993);
        }
    }

    TEST_CASE("EMP retail weapon inventory reaches authoritative damage and recovery", "[emp][retail]")
    {
        const auto* root = std::getenv("RWE_DATA_PATH");
        if (!root || !std::filesystem::exists(root)) { SKIP("RWE_DATA_PATH required for retail EMP integration"); }
        auto vfs = constructVfs(root);
        for (const auto* unitName : {"ARMCOM", "CORCOM"})
        {
            auto bytes = vfs.readFile(std::string("units/") + unitName + ".fbi");
            REQUIRE(bytes);
            MovementClassDatabase movement;
            REQUIRE(parseUnitDefinition(parseUnitFbi(parseTdfFromBytes(*bytes)), movement).immuneToParalyzer);
        }
        unsigned int found = 0;
        for (const auto& file : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + file);
            REQUIRE(bytes);
            for (const auto& [name, tdf] : parseWeaponTdf(parseTdfFromBytes(*bytes)))
            {
                if (!tdf.paralyzer) { continue; }
                ++found;
                EmpFixture f;
                auto id = f.spawn();
                auto& u = f.sim.getUnitState(id);
                auto def = parseWeaponDefinition(tdf);
                REQUIRE(def.paralyzer);
                if (toUpper(name) == "ARMEMP_WEAPON")
                {
                    REQUIRE(def.damage.at("DEFAULT") == 1);
                    REQUIRE(def.damage.at("CORAK") == 1800);
                    REQUIRE(def.damage.at("CORCOM") == 100);
                }
                auto key = toUpper(name);
                f.sim.weaponDefinitions.insert_or_assign(key, def);
                auto p = f.sim.createProjectileFromWeapon(f.computer, key, u.position, SimVector(1_ss, 0_ss, 0_ss), 100_ss, id);
                f.sim.applyDamageInRadius(u.position, p.damageRadius, p);
                auto credit = std::get<ParalyzeOrder>(u.orders.front()).credit;
                REQUIRE(credit == p.getDamage(u.unitType));
                REQUIRE(u.hitPoints == 1000);
                f.ticks(1);
                REQUIRE(u.isParalyzed());
                REQUIRE(std::get<ParalyzeOrder>(u.orders.front()).resumeAt == GameTime(1 + std::min(credit, 1800u)));
                f.ticks(std::min(credit, 1800u));
                REQUIRE_FALSE(u.isParalyzed());
            }
        }
        REQUIRE(found > 0);
    }
}
