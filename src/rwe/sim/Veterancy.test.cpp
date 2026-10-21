#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <rwe/LoadingScene_util.h>
#include <rwe/cob/CobOpCode.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/sim/UnitBehaviorService_util.h>
#include <rwe/sim/Veterancy.h>
#include <rwe/sim/cob.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    namespace
    {
        struct VeteranFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0};
            PlayerId human{0};
            PlayerId computer{1};

            VeteranFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_veterancy_test.log", false));
                sim.lineOfSightEnabled = false;
                for (unsigned int i = 0; i < 2; ++i)
                {
                    sim.addPlayer(GamePlayerInfo{std::nullopt,
                        i == 0 ? GamePlayerType::Human : GamePlayerType::Computer,
                        PlayerColorIndex(i),
                        GamePlayerStatus::Alive,
                        i == 0 ? "ARM" : "CORE",
                        std::nullopt,
                        Metal(100000),
                        Energy(100000),
                        Metal(100000),
                        Energy(100000),
                        Metal(100000),
                        Energy(100000)});
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
                REQUIRE((id));
                sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(type));
                return *id;
            }

            void emp(UnitId id, unsigned int credit)
            {
                sim.applyDamage(id, credit, std::nullopt, false, true);
            }

            void ticks(unsigned int n)
            {
                for (unsigned int i = 0; i < n; ++i)
                {
                    sim.tick();
                }
            }

            GameParameters params() const
            {
                GameParameters p("VETERANCY_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(100000), Energy(100000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(100000), Energy(100000)};
                return p;
            }
        };
    }

    namespace
    {
        void fire(VeteranFixture& f, UnitId shooter, UnitId target, const std::string& weapon = "GUN")
        {
            auto& u = f.sim.getUnitState(shooter);
            u.weapons[0] = UnitWeapon{weapon};
            auto point = f.sim.getUnitState(target).position;
            UnitWeaponStateAttacking state(target);
            auto angles = computeLineOfSightHeadingAndPitch(u.rotation, u.position, point);
            state.attackInfo = UnitWeaponStateAttacking::FireInfo{angles.first, angles.second, point, 0, 0, GameTime(0)};
            u.weapons[0]->state = state;
            if (std::holds_alternative<ProjectilePhysicsTypeDropped>(f.sim.weaponDefinitions.at(weapon).physicsType))
            {
                u.orders.emplace_back(AttackOrder(target));
                u.bomberState = std::make_unique<BomberState>();
                u.bomberState->target = target;
                u.bomberState->phase = BomberState::Phase::Overfly;
                u.bomberState->releaseTarget = point;
                u.bomberState->goal = u.position + SimVector(0_ss, 0_ss, 1080_ss);
            }
            UnitBehaviorService(&f.sim).update(shooter);
        }
    }

    TEST_CASE("veterancy begins at zero and tiers use the retail unsigned kill word", "[veterancy]")
    {
        VeteranFixture f;
        auto id = f.spawn();
        REQUIRE((f.sim.getUnitState(id).kills == 0));
        // Literal boundaries from the retail quotient and cap, including the
        // adjacent pre-promotion values and maximum stored word.
        for (auto [kills, tier] : {std::pair{0u, 0u}, {4u, 0u}, {5u, 1u}, {9u, 1u}, {10u, 2u}, {14u, 2u}, {15u, 3u}, {19u, 3u}, {20u, 4u}, {24u, 4u}, {25u, 5u}, {29u, 5u}, {65535u, 5u}})
        {
            REQUIRE(veteranTier(kills) == tier);
        }
    }

    TEST_CASE("retail mission placement retains OTA Kills metadata without granting experience", "[veterancy][veterancy-regression]")
    {
        // TotalA.exe 3.1: the complete 0x24-byte mission placement parser
        // (0x436c30) has no Kills reader. Spawn (0x488310 -> 0x485f50)
        // uses the normal zero-kill initializer (0x485a40, store 0x485c76).
        for (unsigned int player : {1u, 2u})
        {
            for (auto authored : {std::optional<unsigned int>{}, std::optional<unsigned int>{0},
                     std::optional<unsigned int>{1}, std::optional<unsigned int>{34}, std::optional<unsigned int>{65535}})
            {
                VeteranFixture f;
                OtaUnit placement{};
                placement.unitName = "MOBILE"; placement.player = player;
                placement.xPos = placement.zPos = 128;
                placement.kills = authored; placement.healthPercentage = 12;
                auto stats = spawnMissionUnits(f.sim, {placement}, {f.human, f.computer});
                REQUIRE(stats.failures.empty()); REQUIRE(stats.spawned == 1);
                const auto& unit = f.sim.units.begin()->second;
                REQUIRE(unit.kills == 0);
                REQUIRE(unit.missionState->kills == authored);
                REQUIRE(unit.hitPoints == 120);
                REQUIRE(unit.owner == PlayerId(player - 1));
            }
        }
    }

    TEST_CASE("mission units earn their first promotion from combat rather than OTA metadata", "[veterancy][veterancy-regression]")
    {
        VeteranFixture f;
        OtaUnit placement{};
        placement.unitName = "MOBILE"; placement.player = 1;
        placement.xPos = placement.zPos = 128; placement.kills = 34;
        REQUIRE(spawnMissionUnits(f.sim, {placement}, {f.human, f.computer}).spawned == 1);
        auto id = f.sim.units.begin()->first;
        REQUIRE(f.sim.getUnitState(id).kills == 0);
        // Authored 34 is metadata: neither the 130% attack nor 80% defence
        // factor may be granted before actual combat earns those kills.
        auto probe = f.spawn("MOBILE", f.computer, 17);
        f.sim.applyDamage(probe, 100, id);
        REQUIRE(f.sim.getUnitState(probe).hitPoints == 900);
        f.sim.applyDamage(id, 100, probe);
        REQUIRE(f.sim.getUnitState(id).hitPoints == 900);
        REQUIRE(f.sim.getUnitState(id).kills == 0);
        for (unsigned int kill = 1; kill <= 5; ++kill)
        {
            auto victim = f.spawn("MOBILE", f.computer, 20 + kill * 3);
            f.sim.applyDamage(victim, 10000, id);
            f.sim.applyDamage(victim, 10000, id);
            REQUIRE(f.sim.getUnitState(id).kills == kill);
            REQUIRE(veteranTier(f.sim.getUnitState(id).kills) == (kill == 5 ? 1 : 0));
        }
        REQUIRE(f.sim.getUnitState(id).missionState->kills == 34);
    }

    TEST_CASE("v14 restores the live kill counter independently of mission Kills metadata", "[veterancy][veterancy-regression][save]")
    {
        // Includes a Phase-6-produced save: the loader must retain its stored
        // live counter, not guess which historical kills should be subtracted.
        for (uint16_t liveKills : {0, 4, 5, 34, 35})
        {
            VeteranFixture a;
            auto id = a.spawn();
            auto& unit = a.sim.getUnitState(id);
            unit.missionState = MissionUnitState{};
            unit.missionState->kills = 34;
            unit.kills = liveKills;
            auto saved = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "LIVE_KILLS", 0, 14));
            REQUIRE(saved.version == 14);
            VeteranFixture b;
            applySavedGame(b.sim, saved, nullptr);
            REQUIRE(b.sim.getUnitState(id).kills == liveKills);
            REQUIRE(b.sim.getUnitState(id).missionState->kills == 34);
            REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            for (int tick = 0; tick < 20; ++tick)
            {
                a.sim.tick(); b.sim.tick();
                REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            }
        }
    }
    TEST_CASE("veterancy parses the authored turret accuracy without changing spray", "[veterancy]")
    {
        auto w = parseWeaponDefinition(parseWeaponBlock(parseTdfFromString("[W]{turret=1;accuracy=777;sprayangle=123;}").findBlock("W")->get()));
        REQUIRE((w.turret));
        REQUIRE((w.accuracy == 777));
        REQUIRE((w.sprayAngle == SimAngle(123)));
    }
    TEST_CASE("damage alone and EMP assistance award no experience", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.applyDamage(b, 100, a);
        f.sim.applyDamage(b, 500, a, false, true);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
        REQUIRE((f.sim.getUnitState(b).hitPoints == 900));
        f.sim.applyDamage(b, 1000);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
    }
    TEST_CASE("completed enemy kills increment once and promote at five", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        for (unsigned int i = 0; i < 6; ++i)
        {
            auto b = f.spawn("MOBILE", f.computer, 20 + i * 3);
            f.sim.applyDamage(b, 10000, a);
            f.sim.applyDamage(b, 10000, a);
            f.sim.killUnit(b);
            REQUIRE((f.sim.getUnitState(a).kills == i + 1));
            REQUIRE((f.sim.players[0].unitsKilled == i + 1));
        }
        REQUIRE((veteranTier(f.sim.getUnitState(a).kills) == 1));
    }
    TEST_CASE("friendly and self kills do not increment unit experience", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.human, 20);
        f.sim.applyDamage(b, 10000, a);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
        f.sim.applyDamage(a, 10000, a);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
    }
    TEST_CASE("nanoframes do not award unit experience", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.getUnitState(b).buildTimeCompleted = 1;
        f.sim.applyDamage(b, 10000, a);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
    }
    TEST_CASE("the lethal attacker owns credit rather than the largest damage source", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto c = f.spawn("MOBILE", f.human, 12);
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.applyDamage(b, 999, a);
        f.sim.applyDamage(b, 1, c);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
        REQUIRE((f.sim.getUnitState(c).kills == 1));
    }
    TEST_CASE("kill words wrap and do not permanently clamp at the top tier", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.getUnitState(a).kills = 65535;
        f.sim.getUnitState(b).kills = 25;
        f.sim.applyDamage(b, 10000, a);
        REQUIRE(f.sim.getUnitState(b).isDead());
        REQUIRE(f.sim.getUnitState(b).kills == 25);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
        REQUIRE((veteranTier(f.sim.getUnitState(a).kills) == 0));
    }
    TEST_CASE("veterancy attack and defence use their distinct bounded percentages", "[veterancy]")
    {
        for (unsigned int tier = 0; tier < 7; ++tier)
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 20);
            auto& u = f.sim.getUnitState(a);
            auto& v = f.sim.getUnitState(b);
            u.kills = tier * 5;
            f.sim.applyDamage(b, 100, a);
            REQUIRE((v.hitPoints == 1000 - (100 + 6 * std::min(tier, 5u))));
            v.hitPoints = 1000;
            v.kills = tier * 5;
            u.kills = 0;
            f.sim.applyDamage(b, 100, a);
            REQUIRE((v.hitPoints == 1000 - (100 - 4 * std::min(tier, 5u))));
        }
        REQUIRE((veteranAttackDamage(101, 5) == 107));
        REQUIRE((veteranDefenceDamage(101, 5) == 96));
    }
    TEST_CASE("armored state sits between attacker and defender veterancy", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.getUnitState(a).kills = 25;
        auto& v = f.sim.getUnitState(b);
        v.kills = 25;
        v.armored = true;
        f.sim.unitDefinitions.at("MOBILE").damageModifier = 32768;
        f.sim.applyDamage(b, 101, a);
        REQUIRE((v.hitPoints == 948)); // 131 -> 65 -> 52
    }
    TEST_CASE("area attacks credit distinct victims exactly once", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto c = f.spawn("MOBILE", f.computer, 23);
        f.sim.weaponDefinitions.at("GUN").damage = {{"DEFAULT", 10000}};
        auto p = f.sim.createProjectileFromWeapon(f.human, "GUN", f.sim.getUnitState(b).position, SimVector(1_ss, 0_ss, 0_ss), 100_ss, b);
        p.firedBy = a;
        p.damageRadius = 100_ss;
        f.sim.applyDamageInRadius(p.position, p.damageRadius, p);
        f.sim.applyDamageInRadius(p.position, p.damageRadius, p);
        REQUIRE((f.sim.getUnitState(b).isDead()));
        REQUIRE((f.sim.getUnitState(c).isDead()));
        REQUIRE((f.sim.getUnitState(a).kills == 2));
    }
    TEST_CASE("death blast chain reactions do not inherit the triggering shooter", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto c = f.spawn("MOBILE", f.computer, 23);
        auto& w = f.sim.weaponDefinitions.at("GUN");
        w.damage = {{"DEFAULT", 10000}};
        w.damageRadius = 100_ss;
        f.sim.unitDefinitions.at("MOBILE").explodeAs = "GUN";
        f.sim.applyDamage(b, 10000, a);
        REQUIRE((f.sim.getUnitState(c).isDead()));
        REQUIRE((f.sim.getUnitState(a).kills == 1));
    }
    TEST_CASE("manual self destruction uses defender reduction without attacker amplification", "[veterancy]")
    {
        VeteranFixture f;
        f.sim.unitDefinitions.at("MOBILE").maxHitPoints = 25000;
        auto a = f.spawn();
        auto& u = f.sim.getUnitState(a);
        u.kills = 25;
        f.sim.selfDestructUnit(a);
        REQUIRE((u.hitPoints == 1000));
        REQUIRE((u.kills == 25)); // 30000 * .8, not *1.3 first
    }
    TEST_CASE("EMP credit receives both veterancy factors without HP damage", "[veterancy][emp]")
    {
        for (unsigned int k : {0u, 25u})
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 20);
            auto& v = f.sim.getUnitState(b);
            v.kills = k;
            f.sim.applyDamage(b, 100, a, false, true);
            REQUIRE((v.hitPoints == 1000));
            REQUIRE((std::get<ParalyzeOrder>(v.orders.front()).credit == (k == 0 ? 100u : 80u)));
            f.ticks(1);
            REQUIRE((v.isParalyzed()));
            f.ticks(k == 0 ? 99 : 79);
            REQUIRE((v.isParalyzed()));
            f.ticks(1);
            REQUIRE_FALSE(v.isParalyzed());
        }
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.getUnitState(a).kills = 25;
        f.sim.getUnitState(b).kills = 25;
        f.sim.applyDamage(b, 100, a, false, true);
        REQUIRE((std::get<ParalyzeOrder>(f.sim.getUnitState(b).orders.front()).credit == 104));
    }
    TEST_CASE("ordinary reload gains stop at twenty five kills and do not change burst spacing", "[veterancy]")
    {
        for (unsigned int k : {0u, 4u, 5u, 10u, 15u, 20u, 25u, 100u})
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 20);
            f.sim.getUnitState(a).kills = k;
            auto& w = f.sim.weaponDefinitions.at("GUN");
            w.reloadTime = 10_ss;
            w.burst = 3;
            w.burstInterval = 1_ss;
            fire(f, a, b);
            auto& u = f.sim.getUnitState(a);
            REQUIRE((u.weapons[0]->readyTime == GameTime(300 - 18 * std::min(k / 5, 5u))));
            REQUIRE((std::get<UnitWeaponStateAttacking::FireInfo>(std::get<UnitWeaponStateAttacking>(u.weapons[0]->state).attackInfo).readyTime == GameTime(30)));
        }
    }
    TEST_CASE("stockpile assembly and launch cadence receive no veteran reload bonus", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto& u = f.sim.getUnitState(a);
        u.kills = 25;
        u.fireOrders = UnitFireOrders::HoldFire;
        f.sim.weaponDefinitions.at("GUN").stockpile = true;
        u.weapons[0] = UnitWeapon{"GUN"};
        u.backgroundOrders.emplace_back(BuildWeaponOrder{1});
        f.ticks(29);
        REQUIRE((u.weapons[0]->stockpileCount == 0));
        f.ticks(2);
        REQUIRE((u.weapons[0]->stockpileCount == 1));
        auto point = f.sim.getUnitState(b).position;
        UnitWeaponStateAttacking attack(b);
        attack.attackInfo = UnitWeaponStateAttacking::FireInfo{SimAngle(0), SimAngle(0), point, 0, 0, f.sim.gameTime};
        u.weapons[0]->state = attack;
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(u.weapons[0]->stockpileCount == 0);
        REQUIRE(u.weapons[0]->readyTime == f.sim.gameTime + GameTime(30));
    }
    TEST_CASE("turret spread uses the retail health term and divisor from twenty four kills", "[veterancy]")
    {
        REQUIRE((veteranSpread(1200, 1000, 1000, 0) == 1200));
        REQUIRE((veteranSpread(1200, 1000, 1000, 23) == 1200));
        REQUIRE((veteranSpread(1200, 1000, 1000, 24) == 600));
        REQUIRE((veteranSpread(1200, 1000, 1000, 36) == 400));
        REQUIRE((veteranSpread(0, 500, 1000, 0) == 1024));
        REQUIRE((veteranSpread(0, 500, 1000, 24) == 512));
        REQUIRE((veteranSpread(0, 1000, 1000, 25) == 0));
    }
    TEST_CASE("ballistic turret release consumes two bounded draws and changes authoritative velocity", "[veterancy]")
    {
        for (unsigned int k : {0u, 24u, 36u})
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 20);
            f.sim.getUnitState(a).kills = k;
            auto& w = f.sim.weaponDefinitions.at("GUN");
            w.turret = true;
            w.accuracy = 1200;
            w.physicsType = ProjectilePhysicsTypeBallistic{};
            w.randomDecay = std::nullopt;
            auto expected = f.sim.rng;
            auto bound = veteranSpread(1200, 1000, 1000, k);
            auto yaw = expected() % bound - bound / 2;
            auto pitch = expected() % bound - bound / 2;
            const auto& shooter = f.sim.getUnitState(a);
            const auto angles = computeLineOfSightHeadingAndPitch(shooter.rotation, shooter.position, f.sim.getUnitState(b).position);
            const auto expectedVelocity = toDirection(angles.first + shooter.rotation + SimAngle(yaw),
                                              -angles.second - SimAngle(pitch))
                * w.velocity;
            fire(f, a, b);
            REQUIRE((f.sim.rng == expected));
            REQUIRE((std::distance(f.sim.projectiles.begin(), f.sim.projectiles.end()) == 1));
            auto& p = f.sim.projectiles.begin()->second;
            REQUIRE((p.velocity.length() > 0_ss));
            REQUIRE((p.velocity.y != 0_ss));
            REQUIRE((p.velocity == expectedVelocity));
        }
    }
    TEST_CASE("non turret and zero spread weapons consume no accuracy draws", "[veterancy]")
    {
        for (bool turret : {false, true})
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 20);
            f.sim.getUnitState(a).kills = 100;
            auto& w = f.sim.weaponDefinitions.at("GUN");
            w.turret = turret;
            w.accuracy = turret ? 0 : 1200;
            w.randomDecay = std::nullopt;
            auto rng = f.sim.rng;
            fire(f, a, b);
            REQUIRE((f.sim.rng == rng));
        }
    }
    TEST_CASE("moving target lead begins strictly after five kills and cruise suppresses it", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("AIR", f.computer, 20);
        auto& u = f.sim.getUnitState(a);
        auto& t = f.sim.getUnitState(b);
        t.physics = UnitPhysicsInfoAir{AirMovementStateFlying{std::nullopt, false, SimVector(1_ss, 2_ss, 3_ss)}};
        auto& w = f.sim.weaponDefinitions.at("GUN");
        auto point = t.position;
        u.kills = 5;
        REQUIRE((veteranAimPoint(u, t, w, point) == point));
        u.kills = 6;
        auto time = (u.position - point).length() / w.velocity * SimScalar(52428.0f / 65536.0f);
        REQUIRE((veteranAimPoint(u, t, w, point) == point + SimVector(1_ss, 2_ss, 3_ss) * time));
        w.cruise = true;
        REQUIRE((veteranAimPoint(u, t, w, point) == point));
        w.cruise = false;
        w.velocity = 0_ss;
        REQUIRE((veteranAimPoint(u, t, w, point) == point));
    }
    TEST_CASE("veteran bomb releases retain inherited velocity and only adjust ordinary reload", "[veterancy][bomber]")
    {
        VeteranFixture f;
        auto a = f.spawn("AIR");
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto& u = f.sim.getUnitState(a);
        u.kills = 25;
        u.physics = UnitPhysicsInfoAir{AirMovementStateFlying{std::nullopt, false, SimVector(2_ss, 1_ss, 3_ss)}};
        auto& w = f.sim.weaponDefinitions.at("GUN");
        w.physicsType = ProjectilePhysicsTypeDropped{};
        w.turret = true;
        w.accuracy = 1000;
        w.randomDecay = std::nullopt;
        auto rng = f.sim.rng;
        fire(f, a, b);
        REQUIRE((f.sim.rng == rng));
        REQUIRE((f.sim.projectiles.begin()->second.velocity == SimVector(2_ss, 0_ss, 3_ss)));
        REQUIRE((u.weapons[0]->readyTime == GameTime(21)));
    }
    TEST_CASE("veterancy leaves maximum health and ground movement unchanged", "[veterancy]")
    {
        VeteranFixture a, b;
        auto x = a.spawn();
        auto y = b.spawn();
        b.sim.getUnitState(y).kills = 25;
        a.sim.getUnitState(x).addOrder(MoveOrder(SimVector(300_ss, 64_ss, 300_ss)));
        b.sim.getUnitState(y).addOrder(MoveOrder(SimVector(300_ss, 64_ss, 300_ss)));
        for (int i = 0; i < 80; ++i)
        {
            a.sim.tick();
            b.sim.tick();
            REQUIRE((a.sim.getUnitState(x).position == b.sim.getUnitState(y).position));
        }
        REQUIRE((b.sim.getUnitState(y).hitPoints == 1000));
        REQUIRE((b.sim.unitDefinitions.at("MOBILE").maxHitPoints == 1000));
    }
    TEST_CASE("ownership transfer clears experience without modifying its authored mission metadata", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        f.sim.getUnitState(a).kills = 25;
        f.sim.transferUnitOwnership(a, f.computer);
        REQUIRE((f.sim.getUnitState(a).kills == 0));
    }
    TEST_CASE("AI units acquire and consume the same veteran damage rules", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn("MOBILE", f.computer);
        auto b = f.spawn("MOBILE", f.human, 20);
        f.sim.getUnitState(a).kills = 4;
        f.sim.applyDamage(b, 10000, a);
        REQUIRE((f.sim.getUnitState(a).kills == 5));
        auto c = f.spawn("MOBILE", f.human, 25);
        f.sim.applyDamage(c, 100, a);
        REQUIRE((f.sim.getUnitState(c).hitPoints == 894));
    }
    TEST_CASE("veteran HUD labels use the retail singular plural and five kill presentation threshold", "[veterancy]")
    {
        REQUIRE((veteranLabel(0) == "0 kills"));
        REQUIRE((veteranLabel(1) == "1 kill"));
        REQUIRE((veteranLabel(4) == "4 kills"));
        REQUIRE((veteranLabel(5) == "5 kills - Veteran"));
    }
    TEST_CASE("unit kill words affect hashes independently of player totals", "[veterancy][save]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto hash = f.sim.computeHash();
        f.sim.getUnitState(a).kills = 1;
        REQUIRE((f.sim.computeHash() != hash));
        REQUIRE((f.sim.players[0].unitsKilled == 0));
    }
    TEST_CASE("veterancy saves preserve progression combat projectiles and EMP continuation", "[veterancy][save]")
    {
        for (unsigned int kills : {0u, 1u, 4u, 5u, 25u, 65535u})
        {
            VeteranFixture a;
            auto u = a.spawn();
            auto v = a.spawn("MOBILE", a.computer, 20);
            a.sim.getUnitState(u).kills = kills;
            fire(a, u, v);
            a.sim.applyDamage(u, 25, v, false, true);
            auto saved = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "VETERANCY", 0, 14));
            REQUIRE((saved.version == 14));
            VeteranFixture b;
            applySavedGame(b.sim, saved, nullptr);
            REQUIRE((b.sim.getUnitState(u).kills == kills));
            REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
            for (int tick = 0; tick < 100; ++tick)
            {
                a.sim.tick();
                b.sim.tick();
                REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
            }
            // Cross the promotion/wrap boundary after restoring, then save the
            // pending death and prove loading cannot credit it a second time.
            a.sim.applyDamage(v, 10000, u);
            b.sim.applyDamage(v, 10000, u);
            const auto credited = static_cast<uint16_t>(kills + 1);
            REQUIRE(a.sim.getUnitState(u).kills == credited);
            REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            auto afterDeath = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "AFTER_KILL", 0));
            VeteranFixture c;
            applySavedGame(c.sim, afterDeath, nullptr);
            REQUIRE(c.sim.getUnitState(v).isDead());
            c.sim.applyDamage(v, 10000, u);
            REQUIRE(c.sim.getUnitState(u).kills == credited);
            a.sim.tick();
            c.sim.tick();
            REQUIRE(a.sim.computeHash() == c.sim.computeHash());
        }
    }
    TEST_CASE("legacy versions initialize zero experience without fabricating past kills", "[veterancy][save]")
    {
        VeteranFixture a;
        auto u = a.spawn();
        a.sim.getUnitState(u).missionState = MissionUnitState{};
        a.sim.getUnitState(u).missionState->kills = 25;
        a.sim.players[0].unitsKilled = 123;
        for (unsigned int version = 1; version <= 13; ++version)
        {
            auto bytes = encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, std::max(version, 4u));
            if (version < 4)
            {
                uint32_t encodedVersion = 0;
                auto payload = decodeSaveFile(bytes, encodedVersion);
                // Genuine idle v1-v3 layouts: no meteor tail; v1 has no AI count.
                payload.resize(payload.size() - (version == 1 ? 34 : 30));
                bytes = encodeSaveFile(version, payload);
            }
            auto saved = decodeSavedGame(bytes);
            VeteranFixture b;
            applySavedGame(b.sim, saved, nullptr);
            REQUIRE((b.sim.getUnitState(u).kills == 0));
            REQUIRE((b.sim.getUnitState(u).hitPoints == 1000));
            REQUIRE((b.sim.getUnitState(u).missionState->kills == 25));
        }
        a.sim.getUnitState(u).kills = 1;
        REQUIRE_THROWS_AS(encodeSavedGame(a.sim, a.params(), nullptr, "LOSSY", 0, 13), SaveFormatException);
    }
    TEST_CASE("identical veteran combat commands replay with identical hashes", "[veterancy]")
    {
        VeteranFixture a, b;
        auto x = a.spawn();
        auto y = b.spawn();
        auto t = a.spawn("MOBILE", a.computer, 20);
        auto s = b.spawn("MOBILE", b.computer, 20);
        a.sim.getUnitState(x).kills = b.sim.getUnitState(y).kills = 25;
        a.sim.weaponDefinitions.at("GUN").turret = b.sim.weaponDefinitions.at("GUN").turret = true;
        a.sim.weaponDefinitions.at("GUN").accuracy = b.sim.weaponDefinitions.at("GUN").accuracy = 600;
        for (int i = 0; i < 120; ++i)
        {
            if (i % 30 == 0)
            {
                fire(a, x, t);
                fire(b, y, s);
            }
            a.sim.tick();
            b.sim.tick();
            REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
        }
    }
    TEST_CASE("different allied owners still earn retail kill credit", "[veterancy]")
    {
        VeteranFixture f;
        f.sim.players[0].allyGroup = 1;
        f.sim.players[1].allyGroup = 1;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        REQUIRE(f.sim.areAllied(f.human, f.computer));
        f.sim.applyDamage(b, 10000, a);
        REQUIRE(f.sim.getUnitState(a).kills == 1);
    }

    TEST_CASE("transport losses credit completed cargo once", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto carrier = f.spawn("MOBILE", f.computer, 20);
        auto passenger = f.spawn("MOBILE", f.computer, 24);
        f.sim.getUnitState(carrier).passengers.push_back(passenger);
        f.sim.getUnitState(passenger).carriedBy = carrier;
        f.sim.applyDamage(carrier, 10000, a);
        REQUIRE(f.sim.getUnitState(passenger).isDead());
        REQUIRE(f.sim.getUnitState(a).kills == 2);
        f.sim.killUnit(carrier);
        REQUIRE(f.sim.getUnitState(a).kills == 2);
    }

    TEST_CASE("removed shooter handles never credit a reused unit slot", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        f.sim.quietlyKillUnit(a);
        f.sim.deleteDeadUnits();
        auto replacement = f.spawn();
        f.sim.applyDamage(b, 10000, a);
        REQUIRE(f.sim.getUnitState(replacement).kills == 0);
    }

    TEST_CASE("kamikaze blasts are shooterless and never credit the suicidal unit", "[veterancy][kamikaze]")
    {
        VeteranFixture f;
        auto& def = f.sim.unitDefinitions.at("MOBILE");
        def.kamikaze = true;
        def.kamikazeDistance = 100_ss;
        def.selfDestructAs = "GUN";
        f.sim.weaponDefinitions.at("GUN").damage = {{"DEFAULT", 10000}};
        f.sim.weaponDefinitions.at("GUN").damageRadius = 100_ss;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 12);
        f.sim.getUnitState(a).kills = 25;
        f.sim.getUnitState(a).addOrder(AttackOrder(b));
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(f.sim.getUnitState(a).isDead());
        REQUIRE(f.sim.getUnitState(b).isDead());
        REQUIRE(f.sim.getUnitState(a).kills == 25);
    }

    TEST_CASE("environmental feature destruction does not award kills or scale its damage", "[veterancy][features]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        f.sim.getUnitState(a).kills = 25;
        FeatureDefinition def{};
        def.name = "ROCK";
        def.footprintX = def.footprintZ = 1;
        def.blocking = true;
        def.object = true;
        def.damage = 100;
        auto type = f.sim.featureDefinitions.insert(def);
        auto feature = f.sim.addFeature(type, 30, 30);
        REQUIRE(feature);
        auto p = f.sim.createProjectileFromWeapon(f.human, "GUN", f.sim.getFeature(*feature).position, SimVector(1_ss, 0_ss, 0_ss), 40_ss, std::nullopt);
        p.firedBy = a;
        f.sim.applyDamageInRadius(p.position, p.damageRadius, p);
        REQUIRE(f.sim.getFeature(*feature).damageAccumulated == 90);
        f.sim.applyDamageInRadius(p.position, p.damageRadius, p);
        REQUIRE_FALSE(f.sim.tryGetFeature(*feature));
        REQUIRE(f.sim.getUnitState(a).kills == 25);
    }

    TEST_CASE("live unit reclaim uses worker experience and credits enemy completion once", "[veterancy][reclaim]")
    {
        for (unsigned int kills : {0u, 5u, 25u, 30u})
        {
            VeteranFixture f;
            auto a = f.spawn();
            auto b = f.spawn("MOBILE", f.computer, 12);
            auto& worker = f.sim.getUnitState(a);
            worker.kills = kills;
            worker.inBuildStance = true;
            worker.addOrder(ReclaimOrder(b));
            UnitBehaviorService service(&f.sim);
            service.update(a); // deploy
            service.update(a); // native work pulse
            REQUIRE(f.sim.getUnitState(b).hitPoints == 1000 - 10 * ((kills + 5) / 5));
            for (unsigned int i = 0; i < 110; ++i)
            {
                service.update(a);
            }
            REQUIRE(f.sim.getUnitState(b).isDead());
            REQUIRE(worker.kills == kills + 1);
        }
    }

    TEST_CASE("capture snapshots the target veteran resistance and transfer resets kills", "[veterancy][capture]")
    {
        for (unsigned int kills : {0u, 5u, 25u, 30u})
        {
            VeteranFixture f;
            f.sim.unitDefinitions.at("MOBILE").canCapture = true;
            auto a = f.spawn();
            auto b = f.spawn("FACTORY", f.computer, 12);
            f.sim.getUnitState(b).kills = kills;
            f.sim.getUnitState(a).addOrder(CaptureOrder(b));
            UnitBehaviorService(&f.sim).update(a);
            const auto& order = std::get<CaptureOrder>(f.sim.getUnitState(a).orders.front());
            REQUIRE(order.workRequired == 300 * (kills / 5 + 10) / 10);
            f.sim.transferUnitOwnership(b, f.human);
            REQUIRE(f.sim.getUnitState(b).kills == 0);
        }
    }

    TEST_CASE("a promotion during one area sweep does not strengthen later recipients", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto c = f.spawn("MOBILE", f.computer, 23);
        f.sim.getUnitState(a).kills = 4;
        f.sim.getUnitState(b).hitPoints = 1;
        auto p = f.sim.createProjectileFromWeapon(f.human, "GUN", f.sim.getUnitState(b).position, SimVector(1_ss, 0_ss, 0_ss), 100_ss, b);
        p.firedBy = a;
        p.damageRadius = 100_ss;
        const auto distance = rweSqrt(f.sim.createBoundingBox(f.sim.getUnitState(c)).distanceSquared(p.position));
        const auto expected = simScalarToUInt(90_ss * (1_ss - distance / 100_ss));
        f.sim.applyDamageInRadius(p.position, p.damageRadius, p);
        REQUIRE(f.sim.getUnitState(a).kills == 5);
        REQUIRE(f.sim.getUnitState(c).hitPoints == 1000 - expected);
    }

    TEST_CASE("COB port 32 remains an unbound retail query even on veteran units", "[veterancy]")
    {
        VeteranFixture f;
        f.sim.unitScriptDefinitions.at("MOBILE") = CobScript{
            {static_cast<uint32_t>(OpCode::PUSH_CONSTANT), 32, static_cast<uint32_t>(OpCode::GET_VALUE), static_cast<uint32_t>(OpCode::POP_STATIC), 0, static_cast<uint32_t>(OpCode::PUSH_CONSTANT), 0, static_cast<uint32_t>(OpCode::RETURN)}, {"flare"}, {{"Probe", 0}}, 1};
        auto a = f.spawn();
        auto& unit = f.sim.getUnitState(a);
        unit.kills = 25;
        unit.cobEnvironment->_statics[0] = 99;
        REQUIRE(unit.cobEnvironment->createThread("Probe"));
        runUnitCobScripts(f.sim, a);
        REQUIRE(unit.cobEnvironment->_statics[0] == 0);
    }


    TEST_CASE("veteran lead preserves eligibility of an in range moving target", "[veterancy]")
    {
        VeteranFixture f;
        auto a = f.spawn();
        auto b = f.spawn("MOBILE", f.computer, 20);
        auto& shooter = f.sim.getUnitState(a);
        auto& target = f.sim.getUnitState(b);
        shooter.kills = 6;
        target.rotation = SimAngle(16384);
        std::get<UnitPhysicsInfoGround>(target.physics).currentSpeed = 10_ss;
        f.sim.weaponDefinitions.at("GUN").maxRange = 220_ss;
        shooter.weapons[0] = UnitWeapon{"GUN"};
        shooter.weapons[0]->state = UnitWeaponStateAttacking(b);
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(std::distance(f.sim.projectiles.begin(), f.sim.projectiles.end()) == 1);
        REQUIRE(f.sim.projectiles.begin()->second.targetPosition->x > target.position.x);
    }

    TEST_CASE("ordinary turret creators consume spread draws but re solve an unscattered trajectory", "[veterancy]")
    {
        VeteranFixture a, b;
        auto x = a.spawn();
        auto y = b.spawn();
        auto t = a.spawn("MOBILE", a.computer, 20);
        auto u = b.spawn("MOBILE", b.computer, 20);
        b.sim.getUnitState(y).kills = 24;
        for (auto* f : {&a, &b})
        {
            auto& weapon = f->sim.weaponDefinitions.at("GUN");
            weapon.turret = true;
            weapon.accuracy = 1200;
            weapon.randomDecay = std::nullopt;
        }
        auto rng = a.sim.rng;
        rng();
        rng();
        fire(a, x, t);
        fire(b, y, u);
        REQUIRE(a.sim.rng == rng);
        REQUIRE(b.sim.rng == rng);
        REQUIRE((a.sim.projectiles.begin()->second.velocity == b.sim.projectiles.begin()->second.velocity));
    }

    TEST_CASE("fixed cargo loss packets apply defence but not attacker veterancy", "[veterancy][transport]")
    {
        VeteranFixture f;
        f.sim.unitDefinitions.at("AIR").maxHitPoints = 25000;
        auto a = f.spawn();
        auto carrier = f.spawn("MOBILE", f.computer, 20);
        auto passenger = f.spawn("AIR", f.computer, 24);
        f.sim.getUnitState(a).kills = 25;
        f.sim.getUnitState(passenger).kills = 25;
        f.sim.getUnitState(carrier).passengers.push_back(passenger);
        f.sim.getUnitState(passenger).carriedBy = carrier;
        f.sim.applyDamage(carrier, 10000, a);
        REQUIRE(f.sim.getUnitState(passenger).hitPoints == 1000);
        REQUIRE(f.sim.getUnitState(passenger).isAlive());
        REQUIRE_FALSE(f.sim.getUnitState(passenger).carriedBy);
        REQUIRE(f.sim.getUnitState(a).kills == 26);
    }

}
