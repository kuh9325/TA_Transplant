#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/cob/CobOpCode.h>
#include <rwe/io/_3do/_3do.h>
#include <rwe/mesh_util.h>
#include <rwe/vertex_height.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/rwe_string.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/OpaqueId_io.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <cstdlib>
#include <filesystem>

namespace rwe
{
    namespace
    {
        struct BomberFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(257, 257, 64), 0_ss), 255, 0, 0};
            BomberFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_bomber_test.log", false));
                sim.lineOfSightEnabled = false;
                for (unsigned int i = 0; i < 2; ++i)
                {
                    sim.addPlayer(GamePlayerInfo{std::nullopt, i == 0 ? GamePlayerType::Human : GamePlayerType::Computer,
                        PlayerColorIndex(i), GamePlayerStatus::Alive, i == 0 ? "ARM" : "CORE", std::nullopt,
                        Metal(100000), Energy(100000), Metal(100000), Energy(100000), Metal(100000), Energy(100000)});
                }
                WeaponDefinition w{};
                w.physicsType = ProjectilePhysicsTypeDropped{};
                w.maxRange = 1280_ss; w.reloadTime = 0.18_ssf; w.burst = 1;
                w.damageRadius = 24_ss; w.damage = {{"DEFAULT", 158u}}; w.commandFire = true;
                sim.weaponDefinitions.emplace("BOMB", w);
                w.physicsType = ProjectilePhysicsTypeLineOfSight{}; w.velocity = 10_ss; w.commandFire = false;
                sim.weaponDefinitions.emplace("GUN", w);
                for (const auto* name : {"BOMBER", "TARGET", "FIGHTER"})
                {
                    UnitDefinition d{};
                    d.unitName = d.objectName = name;
                    d.movementCollisionInfo = UnitDefinition::AdHocMovementClass{1, 1, 255, 255, 0, 255};
                    d.maxHitPoints = 10000; d.buildTime = 100;
                    d.isMobile = d.canMove = d.canAttack = d.shootMe = true;
                    d.maxVelocity = 9_ss; d.acceleration = 0.08_ssf; d.brakeRate = 0.08_ssf; d.turnRate = 356_ss;
                    d.sightDistance = 350_ss; d.standingFireOrder = UnitFireOrders::HoldFire;
                    if (std::string(name) != "TARGET")
                    {
                        d.canFly = true; d.cruiseAltitude = 200_ss; d.attackRunLength = 120_ss;
                        d.weapon1 = std::string(name) == "BOMBER" ? "BOMB" : "GUN";
                    }
                    sim.unitDefinitions.emplace(name, d);
                    sim.unitModelDefinitions.emplace(name, createUnitModelDefinition(16_ss, {UnitPieceDefinition{"base", SimVector(0_ss, -4_ss, 0_ss)}}));
                    sim.unitScriptDefinitions.emplace(name, CobScript{{}, {"base"}, {}, 0});
                }
            }
            UnitId spawn(const std::string& type = "BOMBER", PlayerId owner = PlayerId(0), int x = 96, int z = 96)
            {
                auto position = sim.terrain.heightmapIndexToWorldCenter(x, z);
                position.y = sim.terrain.getHeightAt(position.x, position.z);
                auto id = sim.trySpawnUnit(type, owner, position, std::nullopt); REQUIRE((id));
                auto& u = sim.getUnitState(*id); u.finishBuilding(sim.unitDefinitions.at(type));
                return *id;
            }
            void fly(UnitId id, SimVector velocity = SimVector(0_ss, 0_ss, 9_ss))
            {
                auto& u = sim.getUnitState(id);
                u.position.y = 264_ss; u.previousPosition = u.position;
                // Match transitionFromGroundToAir's occupancy/index invariant.
                // Merely replacing physics leaves an impossible grounded mark,
                // which v14 correctly cannot reproduce for an airborne unit.
                auto rect = sim.computeFootprintRegion(u.position, sim.unitDefinitions.at(u.unitType).movementCollisionInfo);
                sim.occupiedGrid.forEach(sim.occupiedGrid.clipRegion(rect), [&](auto& cell) {
                    if (cell.mobileUnitId == id) { cell.mobileUnitId = std::nullopt; }
                });
                sim.flyingUnitsSet.insert(id);
                AirMovementStateFlying flying; flying.currentVelocity = velocity;
                UnitPhysicsInfoAir air; air.movementState = flying; u.physics = air;
            }
            void phase(UnitId id, const AttackTarget& target, BomberState::Phase p)
            {
                auto& u = sim.getUnitState(id);
                u.replaceOrders({std::visit([](const auto& t) { return UnitOrder(AttackOrder(t)); }, target)});
                u.bomberState = std::make_unique<BomberState>();
                u.bomberState->target = target; u.bomberState->phase = p;
                u.bomberState->releaseTarget = std::holds_alternative<UnitId>(target) ? sim.getUnitState(std::get<UnitId>(target)).position : std::get<SimVector>(target);
                u.bomberState->goal = u.position + SimVector(0_ss, 0_ss, 1080_ss);
            }
            void ticks(unsigned int n) { for (unsigned int i = 0; i < n; ++i) { sim.tick(); } }
            unsigned int releases() const
            {
                unsigned int n = 0;
                for (const auto& e : sim.events) { if (auto f = std::get_if<FireWeaponEvent>(&e); f && f->weaponType == "BOMB") { ++n; } }
                return n;
            }
            GameParameters params() const
            {
                GameParameters p("BOMBER_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(100000), Energy(100000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(100000), Energy(100000)};
                return p;
            }
            ProjectileId drop(SimVector position, SimVector velocity)
            {
                auto p = sim.createProjectileFromWeapon(PlayerId(0), "BOMB", position, SimVector(1_ss, 0_ss, 0_ss), 10_ss, std::nullopt);
                p.velocity = velocity;
                return sim.projectiles.emplace(std::move(p));
            }
        };
    }

    TEST_CASE("bomber definition keeps dropped physics separate and run length unsigned low word", "[bomber]")
    {
        MovementClassDatabase db;
        auto fbi = parseUnitFbi(parseTdfFromString("[UNITINFO]{UnitName=X;ObjectName=X;SoundCategory=X;CanFly=1;AttackRunLength=65656;}"));
        REQUIRE((parseUnitDefinition(fbi, db).attackRunLength == 120_ss));
        WeaponTdf t{}; t.dropped = true;
        REQUIRE((std::holds_alternative<ProjectilePhysicsTypeDropped>(parseWeaponDefinition(t).physicsType)));
        t.dropped = false; t.ballistic = true;
        REQUIRE((std::holds_alternative<ProjectilePhysicsTypeBallistic>(parseWeaponDefinition(t).physicsType)));
        t.ballistic = false; t.lineOfSight = true;
        REQUIRE((std::holds_alternative<ProjectilePhysicsTypeLineOfSight>(parseWeaponDefinition(t).physicsType)));
    }

    TEST_CASE("bomber standby acquires visible ground enemies for human and AI at a bounded cadence", "[bomber]")
    {
        for (unsigned int owner = 0; owner < 2; ++owner)
        {
            BomberFixture f; auto b = f.spawn("BOMBER", PlayerId(owner));
            auto t = f.spawn("TARGET", PlayerId(1-owner), 100);
            auto& u = f.sim.getUnitState(b); u.fireOrders = UnitFireOrders::FireAtWill;
            f.ticks(2); REQUIRE_FALSE(u.orders.empty()); REQUIRE((u.bomberState->target == AttackTarget(t)));
        }
        BomberFixture f; auto b = f.spawn(); f.spawn("FIGHTER", PlayerId(1), 100);
        auto& u = f.sim.getUnitState(b); u.fireOrders = UnitFireOrders::FireAtWill;
        f.ticks(2); REQUIRE((u.orders.empty())); REQUIRE((u.bomberState->nextScan > f.sim.gameTime));
        const auto wake = u.bomberState->nextScan; f.ticks(10); REQUIRE((u.bomberState->nextScan == wake));
    }

    TEST_CASE("hold fire bombers still obey explicit player and AI attack orders", "[bomber]")
    {
        for (unsigned int owner = 0; owner < 2; ++owner)
        {
            BomberFixture f; auto b = f.spawn("BOMBER", PlayerId(owner)); auto t = f.spawn("TARGET", PlayerId(1-owner), 130);
            auto& u = f.sim.getUnitState(b); f.ticks(2); REQUIRE((u.orders.empty()));
            u.addOrder(AttackOrder(t)); f.ticks(1); REQUIRE((u.bomberState)); REQUIRE((u.navigationState.desiredDestination));
        }
    }

    TEST_CASE("bomber close approach uses setup legs while distant approach swings before release", "[bomber]")
    {
        for (bool close : {false, true})
        {
            BomberFixture f; auto b = f.spawn(); f.fly(b); auto t = f.spawn("TARGET", PlayerId(1), close ? 100 : 150);
            auto& u = f.sim.getUnitState(b); u.addOrder(AttackOrder(t)); UnitBehaviorService(&f.sim).update(b);
            REQUIRE((u.bomberState->phase == (close ? BomberState::Phase::Reposition : BomberState::Phase::Swing)));
            REQUIRE((f.releases() == 0));
        }
    }

    TEST_CASE("bomber release lead derives from cruise altitude raw gravity and integer live speed", "[bomber]")
    {
        for (int gravity : {112, 224})
        {
            for (int altitude : {100, 200})
            {
                for (int outside : {0, 1})
                {
                    BomberFixture f; f.sim.mapGravity = gravity; f.sim.unitDefinitions.at("BOMBER").cruiseAltitude = SimScalar(altitude);
                    auto b = f.spawn(); f.fly(b, SimVector(0_ss, 3_ss, 9.9_ssf)); auto& u = f.sim.getUnitState(b);
                    const int lead = static_cast<int>(std::sqrt(2.0 * altitude / gravity) * 30.0 * 9);
                    const auto target = u.position + SimVector(0_ss, -200_ss, SimScalar(lead + 120 + outside));
                    f.phase(b, target, BomberState::Phase::Approach); UnitBehaviorService(&f.sim).update(b);
                    REQUIRE((f.releases() == (outside ? 0 : 1)));
                    REQUIRE((u.bomberState->phase == (outside ? BomberState::Phase::Approach : BomberState::Phase::Overfly)));
                }
            }
        }
    }

    TEST_CASE("dropped emitter and horizontal velocity come from the aircraft at release", "[bomber][projectile]")
    {
        BomberFixture f; auto b = f.spawn(); f.fly(b, SimVector(3_ss, 7_ss, 4_ss)); auto& u = f.sim.getUnitState(b);
        auto before = u.position;
        f.phase(b, before + SimVector(0_ss, -200_ss, 50_ss), BomberState::Phase::Overfly);
        UnitBehaviorService(&f.sim).update(b); REQUIRE((f.releases() == 1)); REQUIRE((std::distance(f.sim.projectiles.begin(), f.sim.projectiles.end()) == 1));
        const auto& p = f.sim.projectiles.begin()->second;
        REQUIRE((p.position == before + SimVector(0_ss, -4_ss, 0_ss)));
        REQUIRE((p.velocity == SimVector(3_ss, 0_ss, 4_ss))); REQUIRE((p.firedBy == b));
        REQUIRE_FALSE(p.targetUnit); REQUIRE_FALSE(p.targetPosition); REQUIRE_FALSE(p.dieOnFrame);
    }

    TEST_CASE("dropped roots ignore aim velocity timers acceleration and guidance", "[bomber][projectile]")
    {
        BomberFixture f; auto& w = f.sim.weaponDefinitions.at("BOMB");
        w.velocity = 100_ss; w.weaponTimer = GameTime(1); w.randomDecay = GameTime(0);
        w.acceleration = 5_ss; w.guidance = true; w.vLaunch = true; w.flightTime = GameTime(1); w.cruise = true;
        auto id = f.drop(SimVector(0_ss, 200_ss, 0_ss), SimVector(3_ss, 0_ss, 4_ss));
        auto& p = f.sim.projectiles.find(id)->second; REQUIRE_FALSE(p.dieOnFrame);
        f.sim.updateProjectiles(); REQUIRE((p.position == SimVector(3_ss, 200_ss, 4_ss)));
        REQUIRE((p.velocity.x == 3_ss)); REQUIRE((p.velocity.z == 4_ss));
        REQUIRE((p.velocity.y == -SimScalar(8155.0f / 65536.0f)));
        f.sim.updateProjectiles(); REQUIRE((p.position.y == 200_ss - SimScalar(8155.0f / 65536.0f)));
    }

    TEST_CASE("dropped discrete gravity and wind displacement produce the authoritative trajectory", "[bomber][projectile]")
    {
        BomberFixture f; f.sim.projectileWind = SimVector(0.125_ssf, 0_ss, -0.0625_ssf);
        auto id = f.drop(SimVector(0_ss, 200_ss, 0_ss), SimVector(3_ss, 0_ss, 4_ss));
        for (int n = 1; n <= 30; ++n)
        {
            f.sim.updateProjectiles(); const auto& p = f.sim.projectiles.find(id)->second;
            REQUIRE((p.position.x.value == Catch::Approx(n * 3.125f)));
            REQUIRE((p.position.z.value == Catch::Approx(n * 3.9375f)));
            REQUIRE((p.position.y.value == Catch::Approx(200.0f - (8155.0f / 65536.0f) * n * (n - 1) / 2)));
        }
        f.sim.mapGravity = 0; REQUIRE((f.sim.droppedProjectileGravity() == 0_ss));
        f.sim.mapGravity = -1; REQUIRE((f.sim.droppedProjectileGravity() == SimScalar(8155.0f / 65536.0f)));
        f.sim.mapGravity = 224; REQUIRE((f.sim.droppedProjectileGravity() == SimScalar(16311.0f / 65536.0f)));
    }

    TEST_CASE("bombs hit terrain and sea through the existing impact path exactly once", "[bomber][projectile]")
    {
        for (bool water : {false, true})
        {
            BomberFixture f;
            if (water) { f.sim.terrain = MapTerrain(Grid<unsigned char>(257, 257, 0), 100_ss); }
            auto id = f.drop(SimVector(0_ss, water ? 101_ss : 65_ss, 0_ss), SimVector(0_ss, -2_ss, 0_ss));
            f.sim.updateProjectiles(); REQUIRE((f.sim.projectiles.find(id)->second.isDead));
            REQUIRE((f.sim.events.size() == 1));
            const auto& e = std::get<ProjectileDiedEvent>(f.sim.events.front());
            REQUIRE((e.deathType == (water ? ProjectileDiedEvent::DeathType::WaterImpact : ProjectileDiedEvent::DeathType::NormalImpact)));
            f.sim.updateProjectiles(); REQUIRE((f.sim.events.size() == 1));
        }
    }

    TEST_CASE("bomb damage uses existing area damage friendly fire and attribution", "[bomber][projectile]")
    {
        BomberFixture f; auto b = f.spawn(); auto t = f.spawn("TARGET", PlayerId(1), 100); auto ally = f.spawn("TARGET", PlayerId(0), 101);
        auto pos = f.sim.getUnitState(t).position;
        f.sim.weaponDefinitions.at("BOMB").damageRadius = 80_ss;
        f.sim.getUnitState(t).hitPoints = 1;
        auto id = f.drop(pos + SimVector(0_ss, 1_ss, 0_ss), SimVector(0_ss, -2_ss, 0_ss));
        f.sim.projectiles.find(id)->second.firedBy = b;
        f.sim.updateProjectiles(); REQUIRE((f.sim.getUnitState(t).isDead()));
        REQUIRE((f.sim.players[0].unitsKilled == 1)); REQUIRE((f.sim.getUnitState(ally).hitPoints < 10000));
        auto hp = f.sim.getUnitState(ally).hitPoints; f.sim.updateProjectiles();
        REQUIRE((f.sim.getUnitState(ally).hitPoints == hp)); REQUIRE((f.sim.players[0].unitsKilled == 1));
    }

    TEST_CASE("moving target is followed during approach and frozen during a bombing pass", "[bomber]")
    {
        BomberFixture f; auto b = f.spawn(); f.fly(b); auto t = f.spawn("TARGET", PlayerId(1), 140);
        f.phase(b, t, BomberState::Phase::Approach); auto& u = f.sim.getUnitState(b);
        UnitBehaviorService(&f.sim).update(b); REQUIRE((u.bomberState->phase == BomberState::Phase::Approach));
        f.sim.getUnitState(t).position = u.position + SimVector(0_ss, -200_ss, 400_ss);
        UnitBehaviorService(&f.sim).update(b); REQUIRE((u.bomberState->phase == BomberState::Phase::Overfly));
        auto snapshot = u.bomberState->releaseTarget;
        f.sim.getUnitState(t).position.x += 100_ss;
        UnitBehaviorService(&f.sim).update(b); REQUIRE((u.bomberState->releaseTarget == snapshot));
        REQUIRE_FALSE(f.sim.projectiles.begin()->second.targetUnit);
    }

    TEST_CASE("bomb reload produces multiple roots in a pass without burst duplication", "[bomber]")
    {
        BomberFixture f; auto b = f.spawn(); f.fly(b); auto& u = f.sim.getUnitState(b);
        f.phase(b, u.position + SimVector(0_ss, -200_ss, 300_ss), BomberState::Phase::Overfly);
        UnitBehaviorService service(&f.sim);
        for (unsigned int tick = 0; tick < 12; ++tick)
        {
            f.sim.gameTime = GameTime(tick); service.update(b);
            REQUIRE((f.releases() == 1 + tick / deltaSecondsToTicks(0.18_ssf).value));
        }
        REQUIRE((f.releases() > 1));
    }

    TEST_CASE("lost target cancels future releases while already dropped bombs continue", "[bomber]")
    {
        for (bool released : {false, true})
        {
            BomberFixture f; auto b = f.spawn(); f.fly(b); auto t = f.spawn("TARGET", PlayerId(1), 100);
            f.phase(b, t, released ? BomberState::Phase::Overfly : BomberState::Phase::Approach);
            if (released) { UnitBehaviorService(&f.sim).update(b); REQUIRE((f.releases() == 1)); }
            f.sim.getUnitState(t).markAsDead(); UnitBehaviorService(&f.sim).update(b);
            REQUIRE((f.sim.getUnitState(b).orders.empty())); REQUIRE_FALSE(f.sim.getUnitState(b).bomberState);
            if (released)
            {
                auto old = f.sim.projectiles.begin()->second.position; f.sim.updateProjectiles();
                REQUIRE((f.sim.projectiles.begin()->second.position != old));
            }
            REQUIRE((f.releases() == (released ? 1 : 0)));
        }
    }

    TEST_CASE("bomber EMP freezes releases and phase but preserves existing aircraft momentum", "[bomber][emp]")
    {
        BomberFixture f; auto b = f.spawn(); f.fly(b); auto& u = f.sim.getUnitState(b);
        f.phase(b, u.position + SimVector(0_ss, -200_ss, 300_ss), BomberState::Phase::Overfly);
        const auto interruptedTarget = u.bomberState->target;
        u.backgroundOrders.emplace_back(AttackOrder(SimVector(0_ss, 64_ss, 0_ss)));
        f.sim.applyDamage(b, 20000, std::nullopt, false, true);
        f.ticks(5); REQUIRE((u.isParalyzed())); REQUIRE((f.releases() == 0));
        REQUIRE((u.bomberState->phase == BomberState::Phase::Overfly));
        REQUIRE((u.bomberState->target == interruptedTarget));
        u.clearOrders(); REQUIRE((u.isParalyzed())); REQUIRE_FALSE(u.bomberState);
    }

    TEST_CASE("zero raw map gravity cancels bombing orders without corrupting the queue", "[bomber]")
    {
        BomberFixture f; auto b = f.spawn(); f.sim.mapGravity = 0;
        f.phase(b, SimVector(0_ss, 0_ss, 0_ss), BomberState::Phase::Approach);
        f.ticks(1); REQUIRE((f.sim.getUnitState(b).orders.empty())); REQUIRE((f.releases() == 0));
    }

    TEST_CASE("bomber repeated attack passes continue through the existing flight mover", "[bomber]")
    {
        BomberFixture f; auto b = f.spawn(); f.fly(b); auto& u = f.sim.getUnitState(b);
        auto target = u.position + SimVector(0_ss, -200_ss, 700_ss);
        u.addOrder(AttackOrder(target));
        unsigned int recoveryEntries = 0; auto previous = BomberState::Phase::Takeoff;
        for (int tick = 0; tick < 2400; ++tick)
        {
            f.ticks(1); REQUIRE((u.bomberState));
            auto phase = u.bomberState->phase;
            if (phase == BomberState::Phase::BreakAway && previous != phase) { ++recoveryEntries; }
            previous = phase;
        }
        REQUIRE((recoveryEntries >= 2)); REQUIRE((f.releases() >= 4)); REQUIRE_FALSE(u.orders.empty());
    }

    TEST_CASE("bomber save continuation covers alignment approach release flight and recovery", "[bomber][save][determinism]")
    {
        const auto saveVersion = GENERATE(14u, 15u, SaveGameFormatVersion);
        for (auto phase : {BomberState::Phase::Takeoff, BomberState::Phase::Reposition, BomberState::Phase::Swing,
            BomberState::Phase::Approach, BomberState::Phase::Overfly, BomberState::Phase::BreakAway})
        {
            BomberFixture a; auto id = a.spawn(); a.fly(id); auto& u = a.sim.getUnitState(id);
            a.phase(id, u.position + SimVector(0_ss, -200_ss, 500_ss), phase);
            a.sim.projectileWind = SimVector(0.125_ssf, 0_ss, 0_ss); a.sim.nextWindSpeedChange = GameTime(9999);
            a.drop(u.position, SimVector(3_ss, 0_ss, 4_ss));
            auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "BOMBER", 0, saveVersion)); REQUIRE((save.version == saveVersion));
            BomberFixture b; applySavedGame(b.sim, save, nullptr);
            CAPTURE(saveVersion, static_cast<unsigned int>(phase), a.sim.occupiedGrid.stateHash(), b.sim.occupiedGrid.stateHash());
            CHECK(computeHashOf(a.sim.units) == computeHashOf(b.sim.units));
            CHECK(computeHashOf(a.sim.projectiles) == computeHashOf(b.sim.projectiles));
            REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
            a.sim.events.clear(); b.sim.events.clear();
            for (int tick = 0; tick < 240; ++tick)
            {
                a.ticks(1); b.ticks(1); REQUIRE((a.sim.computeHash() == b.sim.computeHash())); REQUIRE((a.releases() == b.releases()));
            }
        }
    }

    TEST_CASE("bomber state and projectile inputs change the authoritative hash", "[bomber][save]")
    {
        BomberFixture f; auto b = f.spawn(); auto& u = f.sim.getUnitState(b);
        auto h = f.sim.computeHash(); u.bomberState = std::make_unique<BomberState>(); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.bomberState->goal.x = 1_ss; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.bomberState->releaseTarget.z = 1_ss; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.bomberState->phase = BomberState::Phase::Overfly; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.bomberState->target = UnitId(0); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.bomberState->nextScan = GameTime(0); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); f.sim.mapGravity = 224; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); f.sim.projectileWind.x = 0.125_ssf; REQUIRE((f.sim.computeHash() != h));
        REQUIRE_THROWS_AS(encodeSavedGame(f.sim, f.params(), nullptr, "OLD", 0, 10), SaveFormatException);
    }

    TEST_CASE("version ten saves restore bomber orders as a fresh run using configured map gravity", "[bomber][save]")
    {
        BomberFixture a; auto id = a.spawn(); a.sim.getUnitState(id).addOrder(AttackOrder(SimVector(0_ss, 64_ss, 0_ss)));
        auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, 10));
        BomberFixture b; b.sim.mapGravity = 224; applySavedGame(b.sim, save, nullptr);
        REQUIRE_FALSE(b.sim.getUnitState(id).bomberState); REQUIRE((b.sim.mapGravity == 224));
        REQUIRE((b.sim.projectileWind == SimVector(0_ss, 0_ss, 0_ss)));
        b.ticks(1); REQUIRE((b.sim.getUnitState(id).bomberState));
    }

    TEST_CASE("identical bomber player and AI command sequences replay deterministically", "[bomber][determinism]")
    {
        BomberFixture a; BomberFixture b;
        for (auto* f : {&a, &b})
        {
            auto human = f->spawn(); auto ai = f->spawn("BOMBER", PlayerId(1), 100, 140);
            f->fly(human); f->fly(ai);
            f->sim.getUnitState(human).addOrder(AttackOrder(SimVector(0_ss, 64_ss, 0_ss)));
            f->sim.getUnitState(ai).addOrder(AttackOrder(SimVector(0_ss, 64_ss, 0_ss)));
        }
        for (int tick = 0; tick < 900; ++tick)
        {
            a.ticks(1); b.ticks(1); REQUIRE((a.sim.computeHash() == b.sim.computeHash())); REQUIRE((a.releases() == b.releases()));
        }
        REQUIRE((a.releases() > 0));
    }

    TEST_CASE("fighter and surface projectile physics retain their existing trajectory order", "[bomber][projectile]")
    {
        BomberFixture f; auto fighter = f.spawn("FIGHTER"); f.fly(fighter);
        f.sim.getUnitState(fighter).addOrder(AttackOrder(SimVector(0_ss, 64_ss, 0_ss))); f.ticks(1);
        REQUIRE_FALSE(f.sim.getUnitState(fighter).bomberState);
        for (bool ballistic : {false, true})
        {
            f.sim.weaponDefinitions.at("GUN").physicsType = ballistic ? ProjectilePhysicsType(ProjectilePhysicsTypeBallistic{}) : ProjectilePhysicsType(ProjectilePhysicsTypeLineOfSight{});
            auto p = f.sim.createProjectileFromWeapon(PlayerId(0), "GUN", SimVector(0_ss, 200_ss, 0_ss), SimVector(1_ss, 0_ss, 0_ss), 1000_ss, std::nullopt);
            auto id = f.sim.projectiles.emplace(std::move(p)); f.sim.projectileWind = SimVector(1_ss, 0_ss, 1_ss);
            f.sim.updateProjectiles(); const auto& shot = f.sim.projectiles.find(id)->second;
            REQUIRE((shot.position.x == 10_ss)); REQUIRE((shot.position.z == 0_ss));
            REQUIRE((shot.position.y == (ballistic ? 200_ss - 112_ss / 900_ss : 200_ss)));
        }
    }
    TEST_CASE("dropped firing queries the live emitter without aim or fire callbacks", "[bomber][cob]")
    {
        BomberFixture f;
        using Op = OpCode;
        f.sim.unitModelDefinitions.at("BOMBER") = createUnitModelDefinition(16_ss,
            {UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)}, UnitPieceDefinition{"drop", SimVector(0_ss, -12_ss, 0_ss)}});
        f.sim.unitScriptDefinitions.at("BOMBER") = CobScript{
            {static_cast<unsigned int>(Op::PUSH_CONSTANT), 1, static_cast<unsigned int>(Op::POP_LOCAL_VAR), 0,
                static_cast<unsigned int>(Op::PUSH_CONSTANT), 0, static_cast<unsigned int>(Op::RETURN),
                static_cast<unsigned int>(Op::PUSH_CONSTANT), 42, static_cast<unsigned int>(Op::POP_STATIC), 0,
                static_cast<unsigned int>(Op::PUSH_CONSTANT), 0, static_cast<unsigned int>(Op::RETURN)},
            {"base", "drop"}, {{"QueryPrimary", 0}, {"AimPrimary", 7}, {"FirePrimary", 7}}, 1};
        auto id = f.spawn(); f.fly(id); auto& u = f.sim.getUnitState(id);
        f.sim.moveObjectNow(id, "drop", SimAxis::X, 8_ss);
        auto origin = u.getTransform() * SimVector(8_ss, -12_ss, 0_ss);
        f.phase(id, u.position + SimVector(0_ss, -200_ss, 300_ss), BomberState::Phase::Overfly);
        f.ticks(1); REQUIRE((f.releases() == 1));
        REQUIRE((u.cobEnvironment->_statics[0] == 0));
        REQUIRE((f.sim.projectiles.begin()->second.origin == origin));
    }

    TEST_CASE("retail four bomber FBIs weapons models and COBs execute actual attack passes", "[bomber][retail]")
    {
        auto path = std::getenv("RWE_DATA_PATH");
        if (!path || !std::filesystem::exists(path)) { SKIP("RWE_DATA_PATH required for retail bomber integration"); }
        auto vfs = constructVfs(path); MovementClassDatabase db;
        auto scripts = loadCobScripts(vfs);
        for (const auto* name : {"ARMTHUND", "CORSHAD", "ARMPNIX", "CORHURC"})
        {
            INFO(name);
            BomberFixture f;
            for (const auto& file : vfs.getFileNames("weapons", ".tdf"))
            {
                auto data = vfs.readFile("weapons/" + file); REQUIRE(data);
                for (const auto& [key, tdf] : parseWeaponTdf(parseTdfFromBytes(*data)))
                {
                    f.sim.weaponDefinitions.insert_or_assign(toUpper(key), parseWeaponDefinition(tdf));
                }
            }
            auto data = vfs.readFile(std::string("units/") + name + ".fbi"); REQUIRE(data);
            auto fbi = parseUnitFbi(parseTdfFromBytes(*data));
            auto def = parseUnitDefinition(fbi, db);
            REQUIRE(def.canFly); REQUIRE((def.attackRunLength > 0_ss));
            auto model = vfs.readFile("objects3d/" + fbi.objectName + ".3do"); REQUIRE(model);
            SpanStream stream(model->data(), model->size()); auto objects = parse3doObjects(stream, stream.tellg());
            f.sim.unitModelDefinitions.insert_or_assign(toUpper(fbi.objectName), createUnitModelDefinition(simScalarFromFixed(findHighestVertex(objects.front()).y), unitMeshFrom3do(objects.front())));
            f.sim.unitScriptDefinitions = scripts;
            f.sim.unitScriptDefinitions.emplace("TARGET", CobScript{{}, {"base"}, {}, 0});
            f.sim.unitDefinitions.emplace(name, def);
            const auto& script = scripts.at(name);
            REQUIRE((std::any_of(script.functions.begin(), script.functions.end(), [](const auto& fn) { return fn.name == "QueryPrimary"; })));
            REQUIRE((std::none_of(script.functions.begin(), script.functions.end(), [](const auto& fn) { return fn.name == "AimPrimary" || fn.name == "FirePrimary"; })));
            const auto& weapon = f.sim.weaponDefinitions.at(toUpper(def.weapon1));
            REQUIRE(std::holds_alternative<ProjectilePhysicsTypeDropped>(weapon.physicsType));
            REQUIRE(weapon.commandFire);
            // Stock TDFs omit burst; the existing parser represents omission as zero.
            REQUIRE((weapon.burst == 0));
            auto id = f.spawn(name); f.fly(id, SimVector(0_ss, 0_ss, def.maxVelocity));
            auto& u = f.sim.getUnitState(id); u.position.y = 64_ss + def.cruiseAltitude;
            auto target = f.spawn("TARGET", PlayerId(1), 96, 144);
            u.addOrder(AttackOrder(target));
            f.ticks(1800);
            unsigned int releases = 0, impacts = 0;
            for (const auto& event : f.sim.events)
            {
                if (auto fire = std::get_if<FireWeaponEvent>(&event); fire && fire->weaponType == toUpper(def.weapon1)) { ++releases; }
                if (auto impact = std::get_if<ProjectileDiedEvent>(&event); impact && impact->weaponType == toUpper(def.weapon1)) { ++impacts; }
            }
            INFO("releases=" << releases << " impacts=" << impacts);
            REQUIRE((releases > 1)); REQUIRE((impacts > 0)); REQUIRE((impacts <= releases));
            auto victim = f.sim.tryGetUnitState(target);
            REQUIRE((!victim || victim->get().hitPoints < 10000));
        }
    }


}
