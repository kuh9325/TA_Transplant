#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/io/_3do/_3do.h>
#include <rwe/mesh_util.h>
#include <rwe/vertex_height.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/rwe_string.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <cstdlib>
#include <filesystem>
#include <rwe/cob/CobOpCode.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/proto/serialization.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/OpaqueId_io.h>

namespace rwe
{
    namespace
    {
        using ServicePhase = AircraftServiceState::Phase;
        struct ServiceFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(257, 257, 64), 0_ss), 255, 0, 0};
            ServiceFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_service_test.log", false));
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
                for (const auto* name : {"AIR", "BOMBER", "PAD", "GROUND"})
                {
                    UnitDefinition d{};
                    d.unitName = d.objectName = name;
                    d.movementCollisionInfo = UnitDefinition::AdHocMovementClass{1, 1, 255, 255, 0, 255};
                    d.maxHitPoints = 100; d.buildTime = 1000; d.buildCostEnergy = Energy(2000); d.buildCostMetal = Metal(200);
                    d.isMobile = d.canMove = true; d.turnRate = 2048_ss;
                    d.canFly = std::string(name) == "AIR" || std::string(name) == "BOMBER";
                    d.maxVelocity = 9_ss; d.acceleration = 0.5_ssf; d.brakeRate = 0.5_ssf; d.cruiseAltitude = 80_ss;
                    d.isAirBase = d.builder = std::string(name) == "PAD";
                    d.workerTimePerTick = 6; d.energyStorage = Energy(100000); d.metalStorage = Metal(100000);
                    if (std::string(name) == "BOMBER") { d.weapon1 = "BOMB"; d.canAttack = true; d.attackRunLength = 120_ss; }
                    sim.unitDefinitions.emplace(name, d);
                    sim.unitModelDefinitions.emplace(name, createUnitModelDefinition(16_ss, {
                        UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)},
                        UnitPieceDefinition{"pad", SimVector(24_ss, 16_ss, 0_ss)}}));
                    sim.unitScriptDefinitions.emplace(name, CobScript{{}, {"base", "pad"}, {}, 0});
                }
                pads({1, 1, -1, -1}); // stock repair-pad COB returns the same piece twice
            }
            void pads(const std::vector<int>& pieces)
            {
                std::vector<uint32_t> code;
                for (unsigned int i = 0; i < 4; ++i) { code.push_back(static_cast<uint32_t>(OpCode::CREATE_LOCAL_VAR)); }
                for (unsigned int i = 0; i < pieces.size(); ++i)
                {
                    code.insert(code.end(), {static_cast<uint32_t>(OpCode::PUSH_CONSTANT), static_cast<uint32_t>(pieces[i]),
                        static_cast<uint32_t>(OpCode::POP_LOCAL_VAR), i});
                }
                code.insert(code.end(), {static_cast<uint32_t>(OpCode::PUSH_CONSTANT), 0, static_cast<uint32_t>(OpCode::RETURN)});
                sim.unitScriptDefinitions.at("PAD") = CobScript{code, {"base", "pad"}, {{"QueryLandingPad", 0}}, 0};
            }
            UnitId spawn(const std::string& type, int x = 96, PlayerId owner = PlayerId(0))
            {
                auto pos = sim.terrain.heightmapIndexToWorldCenter(x, 96); pos.y = 64_ss;
                auto id = sim.trySpawnUnit(type, owner, pos, std::nullopt); REQUIRE(id);
                auto& u = sim.getUnitState(*id); u.finishBuilding(sim.unitDefinitions.at(type)); u.activate();
                if (sim.unitDefinitions.at(type).canFly)
                {
                    // Use the real transition so occupied and flying indexes agree.
                    u.replaceOrders({MoveOrder(pos + SimVector(0_ss, 0_ss, 100_ss))});
                    for (int i = 0; i < 82; ++i) { sim.tick(); }
                    u.clearOrders();
                    u.physics = UnitPhysicsInfoAir{AirMovementStateFlying{}};
                    u.position.y = 144_ss;
                }
                return *id;
            }
            void ticks(unsigned int n) { for (unsigned int i = 0; i < n; ++i) { sim.tick(); } }
            bool until(UnitId id, ServicePhase phase, unsigned int max = 3000)
            {
                for (unsigned int i = 0; i < max; ++i)
                {
                    auto& u = sim.getUnitState(id);
                    if (u.aircraftService && u.aircraftService->phase == phase) { return true; }
                    sim.tick();
                }
                return false;
            }
            void land(UnitId aircraft, UnitId pad) { sim.getUnitState(aircraft).replaceOrders({LandOrder(pad)}); }
            void dock(UnitId aircraft, UnitId pad)
            {
                land(aircraft, pad); REQUIRE(until(aircraft, ServicePhase::Repair));
            }
            GameParameters params() const
            {
                GameParameters p("SERVICE_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(100000), Energy(100000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(100000), Energy(100000)};
                return p;
            }
        };
    }

    TEST_CASE("air service FBI flag and landing command round trip", "[air-service]")
    {
        auto fbi = parseUnitFbi(parseTdfFromString("[UNITINFO]{UnitName=X;ObjectName=X;SoundCategory=X;IsAirBase=1;Builder=1;}"));
        MovementClassDatabase db;
        REQUIRE(fbi.isAirBase); REQUIRE(parseUnitDefinition(fbi, db).isAirBase);
        for (auto kind : {PlayerUnitCommand::IssueOrder::IssueKind::Immediate, PlayerUnitCommand::IssueOrder::IssueKind::Queued})
        {
            PlayerCommand c = PlayerUnitCommand(UnitId(3), PlayerUnitCommand::IssueOrder(LandOrder(UnitId(7)), kind));
            proto::PlayerCommand wire; serializePlayerCommand(c, wire);
            auto decoded = deserializeCommand(wire);
            const auto& issued = std::get<PlayerUnitCommand::IssueOrder>(std::get<PlayerUnitCommand>(decoded).command);
            REQUIRE(issued.issueKind == kind); REQUIRE(std::get<LandOrder>(issued.order).target == UnitId(7));
        }
    }

    TEST_CASE("air service rejects ground enemy incomplete and carried facilities", "[air-service]")
    {
        for (int scenario = 0; scenario < 5; ++scenario)
        {
            ServiceFixture f;
            auto pad = f.spawn(scenario == 4 ? "GROUND" : "PAD", 120, PlayerId(scenario == 1 ? 1 : 0));
            auto a = f.spawn(scenario == 0 ? "GROUND" : "AIR");
            if (scenario == 2) { f.sim.getUnitState(pad).buildTimeCompleted = 0; }
            if (scenario == 3) { f.sim.getUnitState(pad).carriedBy = UnitId(123); }
            f.land(a, pad); f.ticks(1); REQUIRE_FALSE(f.sim.getUnitState(a).aircraftService);
            REQUIRE(f.sim.airServiceOccupants.empty());
        }
    }

    TEST_CASE("air service approaches and docks at oriented elevated model piece", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        auto& base = f.sim.getUnitState(pad); base.rotation = SimAngle(16384); base.position.y = 110_ss;
        auto& u = f.sim.getUnitState(a); u.hitPoints = 80;
        f.land(a, pad); f.ticks(1); REQUIRE(u.aircraftService); REQUIRE(f.sim.airServiceOccupants.empty());
        REQUIRE(f.until(a, ServicePhase::Repair));
        REQUIRE((u.position == f.sim.computeAttachPosition(base, 1)));
        REQUIRE(u.rotation == base.rotation); REQUIRE(f.sim.airServiceOccupants.size() == 1);
        REQUIRE_FALSE(u.carriedBy); // pad guests remain visible, damageable simulation units
    }

    TEST_CASE("air service stock repair quantum bills energy only once per tick", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        auto& u = f.sim.getUnitState(a); u.hitPoints = 90; f.dock(a, pad);
        const auto hp = u.hitPoints;
        auto& player = f.sim.players[0];
        player.metalStalled = true;
        auto energy = player.actualEnergyConsumptionBuffer.value; auto metal = player.actualMetalConsumptionBuffer.value;
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(u.hitPoints == hp + 1);
        REQUIRE(player.actualEnergyConsumptionBuffer.value == energy + 1);
        REQUIRE(player.actualMetalConsumptionBuffer.value == metal);
        player.energyStalled = true;
        UnitBehaviorService(&f.sim).update(a); REQUIRE(u.hitPoints == hp + 1);
        player.energyStalled = false;
        for (unsigned int i = hp + 1; i < 100; ++i) { UnitBehaviorService(&f.sim).update(a); }
        REQUIRE(u.hitPoints == 100);
        UnitBehaviorService(&f.sim).update(a); REQUIRE(u.aircraftService->phase == ServicePhase::Parked);
        energy = player.actualEnergyConsumptionBuffer.value;
        UnitBehaviorService(&f.sim).update(a); REQUIRE(player.actualEnergyConsumptionBuffer.value == energy);
    }

    TEST_CASE("air service touchdown uses retail horizontal altitude and heading gates", "[air-service]")
    {
        struct Example { SimVector offset; bool aligned; bool arrived; };
        for (const auto& example : {
            Example{SimVector(0.5_ssf, 0_ss, 0_ss), true, true},
            Example{SimVector(0.501_ssf, 0_ss, 0_ss), true, false},
            Example{SimVector(0.4_ssf, 0_ss, 0.4_ssf), true, false},
            Example{SimVector(0_ss, 1_ss, 0_ss), true, true},
            Example{SimVector(0_ss, simScalarFromFixed(0x10001), 0_ss), true, false},
            Example{SimVector(0_ss, 0_ss, 0_ss), false, false}})
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
            f.land(a, pad); f.ticks(1);
            auto& u = f.sim.getUnitState(a); auto& base = f.sim.getUnitState(pad);
            REQUIRE(u.aircraftService);
            u.aircraftService->phase = ServicePhase::Descend; u.aircraftService->piece = 1;
            u.position = f.sim.computeAttachPosition(base, 1) + example.offset;
            u.rotation = base.rotation + SimAngle(example.aligned ? 0 : 1);
            UnitBehaviorService(&f.sim).update(a);
            REQUIRE(u.aircraftService->docked() == example.arrived);
            REQUIRE(f.sim.airServiceOccupants.size() == (example.arrived ? 1 : 0));
        }
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        f.land(a, pad); f.ticks(1);
        auto& u = f.sim.getUnitState(a); auto& base = f.sim.getUnitState(pad);
        u.aircraftService->phase = ServicePhase::Approach;
        u.position = base.position + SimVector(160_ss, 80_ss, 0_ss);
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(u.aircraftService->phase == ServicePhase::Approach);
        u.position = base.position + SimVector(159_ss, 80_ss, 0_ss);
        UnitBehaviorService(&f.sim).update(a);
        REQUIRE(u.aircraftService->phase == ServicePhase::Align);
    }

    TEST_CASE("air service revalidates transported occupants during alignment and descent", "[air-service]")
    {
        for (auto phase : {ServicePhase::Align, ServicePhase::Descend})
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
            auto cargo = f.spawn("GROUND", 130);
            f.land(a, pad); REQUIRE(f.until(a, phase));
            auto& base = f.sim.getUnitState(pad); auto& u = f.sim.getUnitState(a);
            const auto piece = u.aircraftService->piece;
            base.passengers.push_back(cargo);
            f.sim.getUnitState(cargo).carriedBy = pad;
            f.sim.getUnitState(cargo).attachSlot = piece;
            u.position = f.sim.computeAttachPosition(base, piece); u.rotation = base.rotation;
            UnitBehaviorService(&f.sim).update(a);
            REQUIRE(u.aircraftService->phase == ServicePhase::Waiting);
            REQUIRE(f.sim.airServiceOccupants.empty());
            base.passengers.clear(); f.sim.getUnitState(cargo).carriedBy.reset();
            REQUIRE(f.until(a, ServicePhase::Parked));
        }
    }

    TEST_CASE("air service manual full aircraft parks until a new order then departs", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR"); f.dock(a, pad); f.ticks(2);
        auto& u = f.sim.getUnitState(a); REQUIRE(u.aircraftService->phase == ServicePhase::Parked);
        const auto dock = u.position; f.ticks(15); REQUIRE((u.position == dock));
        u.replaceOrders({MoveOrder(dock + SimVector(600_ss, 0_ss, 0_ss))});
        f.ticks(1); REQUIRE(u.aircraftService->phase == ServicePhase::Depart); REQUIRE(f.sim.airServiceOccupants.empty());
        f.ticks(200); REQUIRE_FALSE(u.aircraftService); REQUIRE(u.position.x > dock.x);
    }

    TEST_CASE("air service approach cancellation releases no phantom reservation", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 150); auto a = f.spawn("AIR");
        f.land(a, pad); f.ticks(3); REQUIRE(f.sim.getUnitState(a).aircraftService);
        f.sim.getUnitState(a).replaceOrders({MoveOrder(SimVector(0_ss, 0_ss, 0_ss))});
        REQUIRE_FALSE(f.sim.getUnitState(a).aircraftService); REQUIRE(f.sim.airServiceOccupants.empty());
    }

    TEST_CASE("air service repair survives replacement orders and takes additional landing damage", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        auto& u = f.sim.getUnitState(a); u.hitPoints = 90; f.dock(a, pad);
        u.replaceOrders({MoveOrder(u.position + SimVector(500_ss, 0_ss, 0_ss))});
        f.sim.applyDamage(a, 30); REQUIRE(u.hitPoints == 60);
        f.ticks(10); REQUIRE(u.hitPoints == 70); REQUIRE(u.aircraftService->phase == ServicePhase::Repair);
        f.ticks(200); REQUIRE(u.hitPoints == 100); REQUIRE_FALSE(u.aircraftService);
    }

    TEST_CASE("air service one authored pad allows only one occupant and waiters retry", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR", 96); auto b = f.spawn("AIR", 98);
        f.land(a, pad); f.land(b, pad);
        for (int i = 0; i < 1500; ++i) { f.sim.tick(); REQUIRE(f.sim.airServiceOccupants.size() <= 1); }
        REQUIRE(f.sim.airServiceOccupants.size() == 1);
        auto occupied = f.sim.airServiceOccupants.begin()->second; auto waiting = occupied == a ? b : a;
        REQUIRE_FALSE(f.sim.getUnitState(waiting).aircraftService->docked());
        f.sim.killUnit(occupied); REQUIRE(f.sim.airServiceOccupants.empty());
        REQUIRE(f.until(waiting, ServicePhase::Parked)); REQUIRE(f.sim.airServiceOccupants.size() == 1);
    }

    TEST_CASE("air service query skips invalid cells and supports multiple distinct pads", "[air-service]")
    {
        ServiceFixture f; f.pads({-1, 1, 999, 0});
        auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR", 96); auto b = f.spawn("AIR", 98);
        f.land(a, pad); f.land(b, pad); f.ticks(1800);
        REQUIRE(f.sim.airServiceOccupants.size() == 2);
        REQUIRE(f.sim.getUnitState(a).aircraftService->piece != f.sim.getUnitState(b).aircraftService->piece);
    }

    TEST_CASE("air service no-query and full facilities keep bounded aircraft-local waiting state", "[air-service]")
    {
        ServiceFixture f; f.pads({-1, -1, -1, -1}); auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        f.land(a, pad); f.ticks(1000);
        REQUIRE(f.sim.getUnitState(a).aircraftService->phase == ServicePhase::Waiting);
        REQUIRE(f.sim.airServiceOccupants.empty()); REQUIRE(f.sim.getUnitState(a).orders.empty());
    }

    TEST_CASE("air service automatic threshold discovery applies to human and computer aircraft", "[air-service]")
    {
        for (unsigned int owner = 0; owner < 2; ++owner)
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120, PlayerId(owner)); auto a = f.spawn("AIR", 96, PlayerId(owner));
            auto& u = f.sim.getUnitState(a); u.hitPoints = 75;
            f.ticks(1); REQUIRE_FALSE(u.aircraftService);
            u.hitPoints = 74; f.ticks(1); REQUIRE(u.aircraftService); REQUIRE(u.aircraftService->facility == pad);
            REQUIRE(u.aircraftService->automatic); REQUIRE(f.until(a, ServicePhase::Depart));
            f.ticks(200); REQUIRE_FALSE(u.aircraftService); REQUIRE(u.hitPoints == 100);
        }
    }

    TEST_CASE("air service no available facility does not force landing or consume resources", "[air-service]")
    {
        ServiceFixture f; auto a = f.spawn("AIR"); auto& u = f.sim.getUnitState(a); u.hitPoints = 20;
        auto goal = u.position + SimVector(1000_ss, 0_ss, 0_ss); u.replaceOrders({PatrolOrder({goal, u.position})});
        f.ticks(100); REQUIRE_FALSE(u.aircraftService); REQUIRE(u.hitPoints == 20); REQUIRE(u.orders.size() == 1);
        auto pad = f.spawn("PAD", 120, PlayerId(1)); f.ticks(60); REQUIRE_FALSE(u.aircraftService);
        f.sim.getUnitState(pad).owner = PlayerId(0); f.sim.getUnitState(pad).deactivate(); f.ticks(60); REQUIRE_FALSE(u.aircraftService);
    }

    TEST_CASE("air service loss of facility aborts approach and kills attached guests once", "[air-service]")
    {
        for (bool docked : {false, true})
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
            if (docked) { f.dock(a, pad); } else { f.land(a, pad); f.ticks(2); }
            f.sim.killUnit(pad); f.sim.killUnit(pad);
            if (docked) { REQUIRE(f.sim.getUnitState(a).isDead()); REQUIRE(f.sim.players[0].unitsLost == 2); }
            else { UnitBehaviorService(&f.sim).update(a); REQUIRE_FALSE(f.sim.getUnitState(a).aircraftService); REQUIRE(f.sim.getUnitState(a).isAlive()); }
            REQUIRE(f.sim.airServiceOccupants.empty()); REQUIRE(f.sim.airServiceFacilities.empty());
        }
    }

    TEST_CASE("air service captured facility releases its incompatible guest", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR"); f.dock(a, pad);
        REQUIRE(f.sim.transferUnitOwnership(pad, PlayerId(1)));
        f.ticks(1); REQUIRE_FALSE(f.sim.getUnitState(a).aircraftService); REQUIRE(f.sim.airServiceOccupants.empty());
    }

    TEST_CASE("air service moving carrier binding follows live piece transforms", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR"); f.dock(a, pad);
        auto& base = f.sim.getUnitState(pad); base.position += SimVector(12_ss, 7_ss, 9_ss); base.rotation = SimAngle(16384);
        f.ticks(1); REQUIRE((f.sim.getUnitState(a).position == f.sim.computeAttachPosition(base, 1)));
    }

    TEST_CASE("air service EMP pauses approach phases and docked work without losing binding", "[air-service]")
    {
        for (bool docked : {false, true})
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR"); auto& u = f.sim.getUnitState(a); u.hitPoints = 90;
            if (docked) { f.dock(a, pad); } else { f.land(a, pad); f.ticks(2); }
            const auto phase = u.aircraftService->phase; const auto hp = u.hitPoints;
            f.sim.applyDamage(a, 90, std::nullopt, false, true); f.ticks(1);
            for (int i = 0; i < 20; ++i) { f.sim.tick(); REQUIRE(u.aircraftService->phase == phase); REQUIRE(u.hitPoints == hp); }
            f.ticks(100); REQUIRE_FALSE(u.isParalyzed());
            if (docked) { REQUIRE(u.hitPoints == 100); }
        }
    }
    TEST_CASE("air service save continuation preserves request approach docking repair and departure", "[air-service][save][determinism]")
    {
        const auto saveVersion = GENERATE(14u, 15u, SaveGameFormatVersion);
        for (auto phase : {ServicePhase::Takeoff, ServicePhase::Waiting, ServicePhase::Approach,
            ServicePhase::Align, ServicePhase::Descend, ServicePhase::Repair, ServicePhase::Parked, ServicePhase::Depart})
        {
            INFO(static_cast<unsigned int>(phase));
            ServiceFixture a; auto pad = a.spawn("PAD", 120); auto id = a.spawn("AIR");
            auto& u = a.sim.getUnitState(id); u.hitPoints = 90;
            a.land(id, pad);
            if (phase == ServicePhase::Depart)
            {
                REQUIRE(a.until(id, ServicePhase::Parked)); u.addOrder(MoveOrder(u.position + SimVector(500_ss, 0_ss, 0_ss)));
            }
            REQUIRE(a.until(id, phase));
            auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "SERVICE", 0, saveVersion));
            REQUIRE(save.version == saveVersion);
            ServiceFixture b; applySavedGame(b.sim, save, nullptr);
            REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
            REQUIRE(a.sim.airServiceOccupants == b.sim.airServiceOccupants);
            for (int tick = 0; tick < 400; ++tick)
            {
                a.ticks(1); b.ticks(1);
                REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
                REQUIRE(a.sim.airServiceOccupants == b.sim.airServiceOccupants);
            }
        }
    }

    TEST_CASE("air service save after facility destruction has no repeated death or occupancy", "[air-service][save]")
    {
        ServiceFixture a; auto pad = a.spawn("PAD", 120); auto id = a.spawn("AIR"); a.dock(id, pad);
        a.sim.killUnit(pad);
        auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "DEAD", 0));
        ServiceFixture b; applySavedGame(b.sim, save, nullptr);
        REQUIRE(b.sim.airServiceOccupants.empty()); REQUIRE(b.sim.airServiceFacilities.empty());
        a.ticks(5); b.ticks(5); REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
        REQUIRE(b.sim.players[0].unitsLost == 2);
    }

    TEST_CASE("air service version eleven and earlier writers reject new authoritative state", "[air-service][save]")
    {
        ServiceFixture a; auto id = a.spawn("AIR");
        // v4-v11 use genuine older writer layouts; v1-v3 fixtures remain in SavedGame tests.
        for (unsigned int version = 4; version <= 11; ++version)
        {
            a.sim.projectileWind = SimVector(0_ss, 0_ss, 0_ss);
            auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, version));
            ServiceFixture b; applySavedGame(b.sim, save, nullptr);
            REQUIRE_FALSE(b.sim.getUnitState(id).aircraftService); REQUIRE_FALSE(b.sim.getUnitState(id).nextServiceScan);
        }
        a.sim.getUnitState(id).nextServiceScan = GameTime(30);
        REQUIRE_THROWS_AS(encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, 11), SaveFormatException);
        a.sim.getUnitState(id).nextServiceScan.reset(); a.sim.getUnitState(id).addOrder(LandOrder(UnitId(99)));
        REQUIRE_THROWS_AS(encodeSavedGame(a.sim, a.params(), nullptr, "OLD", 0, 11), SaveFormatException);
    }

    TEST_CASE("air service all authoritative state inputs affect multiplayer hashing", "[air-service][determinism]")
    {
        ServiceFixture f; auto id = f.spawn("AIR"); auto& u = f.sim.getUnitState(id);
        auto h = f.sim.computeHash(); u.nextServiceScan = GameTime(0); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService = std::make_unique<AircraftServiceState>(); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->facility = UnitId(7); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->piece = 1; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->phase = ServicePhase::Repair; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->loiterBearing = SimAngle(100); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->goal.x = 1_ss; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); u.aircraftService->automatic = true; REQUIRE((f.sim.computeHash() != h));
        auto base = f.spawn("PAD", 120); auto& facility = f.sim.getUnitState(base);
        h = f.sim.computeHash(); facility.cobEnvironment->_statics.push_back(0); REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); facility.cobEnvironment->_statics[0] = 1; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); facility.pieces[1].offset.x += 1_ss; REQUIRE((f.sim.computeHash() != h));
        h = f.sim.computeHash(); facility.pieces[1].xMoveOperation = UnitMesh::MoveOperation(30_ss, 1_ss); REQUIRE((f.sim.computeHash() != h));
    }

    TEST_CASE("air service repeated deterministic contention replay", "[air-service][determinism]")
    {
        ServiceFixture a; ServiceFixture b;
        for (auto* f : {&a, &b})
        {
            auto pad = f->spawn("PAD", 120);
            for (int i = 0; i < 3; ++i) { auto id = f->spawn("AIR", 96 + i * 2); f->sim.getUnitState(id).hitPoints = 70; f->land(id, pad); }
        }
        for (int tick = 0; tick < 1600; ++tick)
        {
            if (tick == 900 && !a.sim.airServiceOccupants.empty())
            {
                a.sim.killUnit(a.sim.airServiceOccupants.begin()->second);
                b.sim.killUnit(b.sim.airServiceOccupants.begin()->second);
            }
            a.ticks(1); b.ticks(1); REQUIRE((a.sim.computeHash() == b.sim.computeHash()));
        }
    }

    TEST_CASE("air service bomber diverts after overflight and resumes attack without refilling weapon state", "[air-service][bomber]")
    {
        for (unsigned int owner = 0; owner < 2; ++owner)
        {
            ServiceFixture f; auto pad = f.spawn("PAD", 120, PlayerId(owner)); auto id = f.spawn("BOMBER", 96, PlayerId(owner));
            auto& u = f.sim.getUnitState(id); u.hitPoints = 70;
            auto target = u.position + SimVector(0_ss, -80_ss, 700_ss);
            u.replaceOrders({AttackOrder(target)}); u.bomberState = std::make_unique<BomberState>();
            u.bomberState->target = target; u.bomberState->phase = BomberState::Phase::Overfly;
            u.bomberState->goal = u.position; u.bomberState->releaseTarget = target;
            u.weapons[0]->readyTime = GameTime(5000); u.weapons[0]->stockpileCount = 3;
            f.ticks(1); REQUIRE(u.aircraftService); REQUIRE(u.aircraftService->facility == pad);
            REQUIRE(u.bomberState->phase == BomberState::Phase::Takeoff);
            REQUIRE(f.until(id, ServicePhase::Depart)); f.ticks(180);
            REQUIRE_FALSE(u.aircraftService); REQUIRE(u.hitPoints == 100);
            REQUIRE(u.bomberState); REQUIRE(u.bomberState->target == AttackTarget(target));
            REQUIRE(u.weapons[0]->stockpileCount == 3); REQUIRE(u.weapons[0]->readyTime == GameTime(5000));
        }
    }
    TEST_CASE("retail aircraft service facilities expose actual COB pads and repair aircraft", "[air-service][retail]")
    {
        const char* path = std::getenv("RWE_DATA_PATH");
        if (!path || !std::filesystem::exists(path)) { SKIP("RWE_DATA_PATH required for retail service integration"); }
        auto vfs = constructVfs(path); MovementClassDatabase db; auto scripts = loadCobScripts(vfs);
        for (const auto* facility : {"ARMASP", "CORASP", "ARMCARRY", "CORCARRY"})
        {
            for (const auto* aircraft : {"ARMFIG", "ARMBRAWL", "ARMTHUND", "ARMCA", "ARMATLAS"})
            {
                INFO(std::string(facility) + "/" + aircraft);
                ServiceFixture f;
                for (const auto& file : vfs.getFileNames("weapons", ".tdf"))
                {
                    auto bytes = vfs.readFile("weapons/" + file); REQUIRE(bytes);
                    for (const auto& [key, tdf] : parseWeaponTdf(parseTdfFromBytes(*bytes)))
                    { f.sim.weaponDefinitions.insert_or_assign(toUpper(key), parseWeaponDefinition(tdf)); }
                }
                for (auto name : {facility, aircraft})
                {
                    auto bytes = vfs.readFile(std::string("units/") + name + ".fbi"); REQUIRE(bytes);
                    auto fbi = parseUnitFbi(parseTdfFromBytes(*bytes));
                    auto def = parseUnitDefinition(fbi, db);
                    auto objectBytes = vfs.readFile("objects3d/" + def.objectName + ".3do"); REQUIRE(objectBytes);
                    SpanStream stream(objectBytes->data(), objectBytes->size()); auto objects = parse3doObjects(stream, stream.tellg());
                    auto model = createUnitModelDefinition(simScalarFromFixed(findHighestVertex(objects.front()).y), unitMeshFrom3do(objects.front()));
                    f.sim.unitDefinitions.insert_or_assign(name, def);
                    f.sim.unitModelDefinitions.insert_or_assign(def.objectName, model);
                    f.sim.unitScriptDefinitions.insert_or_assign(def.objectName, scripts.at(toUpper(def.objectName)));
                }
                REQUIRE(f.sim.unitDefinitions.at(facility).isAirBase);
                REQUIRE(f.sim.unitDefinitions.at(facility).workerTimePerTick == 6);
                auto pad = f.spawn(facility, 120); auto id = f.spawn(aircraft, 96);
                auto& u = f.sim.getUnitState(id); const auto maxHp = f.sim.unitDefinitions.at(aircraft).maxHitPoints;
                u.hitPoints = maxHp - 10;
                f.land(id, pad); REQUIRE(f.until(id, ServicePhase::Repair, 6000));
                auto& base = f.sim.getUnitState(pad);
                const auto piece = u.aircraftService->piece;
                REQUIRE((u.position == f.sim.computeAttachPosition(base, piece)));
                REQUIRE(piece >= 1); REQUIRE(piece <= (std::string(facility).find("CARRY") != std::string::npos ? 2 : 1));
                f.ticks(12); REQUIRE(u.hitPoints == maxHp); REQUIRE(u.aircraftService->phase == ServicePhase::Parked);
                u.addOrder(MoveOrder(u.position + SimVector(500_ss, 0_ss, 0_ss)));
                f.ticks(500); REQUIRE_FALSE(u.aircraftService); REQUIRE(f.sim.airServiceOccupants.empty());
            }
        }
    }

    TEST_CASE("air service queued request save and malformed service state validation", "[air-service][save]")
    {
        ServiceFixture a; auto pad = a.spawn("PAD", 120); auto id = a.spawn("AIR"); a.land(id, pad);
        auto saved = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "REQUEST", 0));
        ServiceFixture b; applySavedGame(b.sim, saved, nullptr);
        REQUIRE(std::get<LandOrder>(b.sim.getUnitState(id).orders.front()).target == pad);
        for (int i = 0; i < 600; ++i) { a.ticks(1); b.ticks(1); REQUIRE((a.sim.computeHash() == b.sim.computeHash())); }
        auto& u = a.sim.getUnitState(id); REQUIRE(u.aircraftService);
        u.aircraftService->phase = static_cast<ServicePhase>(99);
        saved = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "INVALID", 0));
        REQUIRE_THROWS_AS(applySavedGame(b.sim, saved, nullptr), SaveFormatException);
    }

    TEST_CASE("air service carrier movement remains bound regardless of unit allocation order", "[air-service]")
    {
        ServiceFixture f; auto a = f.spawn("AIR", 96); auto pad = f.spawn("PAD", 120); f.dock(a, pad);
        auto& base = f.sim.getUnitState(pad); base.addOrder(MoveOrder(base.position + SimVector(300_ss, 0_ss, 0_ss)));
        auto start = base.position;
        for (int i = 0; i < 100; ++i)
        {
            f.ticks(1); REQUIRE((f.sim.getUnitState(a).position == f.sim.computeAttachPosition(base, 1)));
        }
        REQUIRE((base.position != start));
    }

    TEST_CASE("air service aircraft continues repair while facility primary orders are paralyzed", "[air-service][emp]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        f.sim.getUnitState(a).hitPoints = 90; f.dock(a, pad);
        f.sim.applyDamage(pad, 90, std::nullopt, false, true);
        f.ticks(10); REQUIRE(f.sim.getUnitState(pad).isParalyzed()); REQUIRE(f.sim.getUnitState(a).hitPoints == 100);
    }

    TEST_CASE("air service damage during descent is repaired after docking at a moving carrier", "[air-service]")
    {
        ServiceFixture f; auto pad = f.spawn("PAD", 120); auto a = f.spawn("AIR");
        auto& u = f.sim.getUnitState(a); u.hitPoints = 90; f.land(a, pad);
        REQUIRE(f.until(a, ServicePhase::Descend));
        f.sim.applyDamage(a, 20); REQUIRE(u.hitPoints == 70);
        auto& base = f.sim.getUnitState(pad);
        // Carrier movement uses its existing navigation, bounded below aircraft speed.
        f.sim.unitDefinitions.at("PAD").maxVelocity = 2_ss;
        base.addOrder(MoveOrder(base.position + SimVector(160_ss, 0_ss, 0_ss)));
        REQUIRE(f.until(a, ServicePhase::Repair)); REQUIRE(u.hitPoints == 70);
        f.ticks(30); REQUIRE(u.hitPoints == 100);
    }
}
