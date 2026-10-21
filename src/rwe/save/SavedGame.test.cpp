#include <catch2/catch_test_macros.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/game/GameScene_util.h>
#include <rwe/io/_3do/_3do.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/moveinfotdf/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/mesh_util.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vertex_height.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);
        }

        GamePlayerInfo makePlayer(PlayerColorIndex color, const std::string& side, GamePlayerType type = GamePlayerType::Human)
        {
            return GamePlayerInfo{
                std::nullopt,
                type,
                color,
                GamePlayerStatus::Alive,
                side,
                std::nullopt,
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000)};
        }

        UnitDefinition makeMobileDef(const std::string& name)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1024);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 500;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }

        UnitDefinition makeBuildingDef(const std::string& name)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.isMobile = false;
            def.canMove = false;
            def.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Ground);
            def.maxHitPoints = 500;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }

        /**
         * Build a sim with the definition tables populated -- the
         * equivalent of what LoadingScene produces from the map+schema,
         * which a save-load reuses rather than re-serializing.
         */
        GameSimulation makeWorld(PlayerId& playerA, PlayerId& playerB)
        {
            auto sim = makeSim();
            for (const auto& name : {"ARMCOM", "CORCOM", "ARMTANK", "CORTANK"})
            {
                sim.unitDefinitions.insert_or_assign(name, makeMobileDef(name));
            }
            sim.unitDefinitions.insert_or_assign("ARMFUS", makeBuildingDef("ARMFUS"));
            for (const auto& name : {"ARMCOM", "CORCOM", "ARMTANK", "CORTANK", "ARMFUS"})
            {
                sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }
            playerA = sim.addPlayer(makePlayer(PlayerColorIndex(0), "ARM"));
            playerB = sim.addPlayer(makePlayer(PlayerColorIndex(1), "CORE", GamePlayerType::Computer));
            return sim;
        }

        GameParameters makeParams()
        {
            GameParameters params("TESTMAP", 0);
            params.players[0] = PlayerInfo{
                std::nullopt,
                PlayerControllerType(PlayerControllerTypeHuman()),
                "ARM",
                PlayerColorIndex(0),
                Metal(10000),
                Energy(10000)};
            params.players[1] = PlayerInfo{
                std::nullopt,
                PlayerControllerType(PlayerControllerTypeComputer()),
                "CORE",
                PlayerColorIndex(1),
                Metal(10000),
                Energy(10000)};
            return params;
        }
    }

    TEST_CASE("SavedGame v15 retains history-dependent mobile occupancy", "[mission-regression][save]")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));
        PlayerId a{0}, b{0};
        auto sim = makeWorld(a, b);
        const auto position = sim.terrain.heightmapIndexToWorldCenter(8, 8);
        const auto first = sim.trySpawnUnit("ARMTANK", a, position, std::nullopt, true);
        const auto departing = sim.trySpawnUnit("ARMTANK", a, position, std::nullopt, true);
        REQUIRE(first); REQUIRE(departing);
        auto& moving = sim.getUnitState(*departing);
        const auto oldRect = sim.computeFootprintRegion(position, sim.unitDefinitions.at("ARMTANK").movementCollisionInfo);
        moving.position = sim.terrain.heightmapIndexToWorldCenter(15, 15);
        const auto newRect = sim.computeFootprintRegion(moving.position, sim.unitDefinitions.at("ARMTANK").movementCollisionInfo);
        sim.moveUnitOccupiedArea(oldRect, newRect, *departing);
        REQUIRE_FALSE(sim.occupiedGrid.get(oldRect.x, oldRect.y).mobileUnitId);
        auto mobileCells = [](const GameSimulation& world) {
            std::vector<std::optional<UnitId>> cells;
            for (const auto& cell : world.occupiedGrid.getVector()) { cells.push_back(cell.mobileUnitId); }
            return cells;
        };
        const auto saved = decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "OVERLAP", 0, 15));
        REQUIRE(saved.version == 15);
        auto copy = makeWorld(a, b);
        applySavedGame(copy, saved, nullptr);
        REQUIRE((mobileCells(copy) == mobileCells(sim)));
        REQUIRE(copy.computeHash() == sim.computeHash());
        for (unsigned int n = 0; n < 600; ++n)
        {
            sim.tick(); copy.tick();
            REQUIRE(copy.computeHash() == sim.computeHash());
            REQUIRE((mobileCells(copy) == mobileCells(sim)));
            sim.events.clear(); copy.events.clear();
        }
        // Explicitly retain the v14 reader, including its documented fallback.
        auto legacy = makeWorld(a, b);
        applySavedGame(legacy, decodeSavedGame(encodeSavedGame(copy, makeParams(), nullptr, "LEGACY", 0, 14)), nullptr);
        REQUIRE((mobileCells(legacy) != mobileCells(copy)));
        REQUIRE(legacy.computeHash() != copy.computeHash()); // the occupancy difference is now hashed
    }

    TEST_CASE("SavedGame restores clipped authored building collision at map edges", "[save][phase8]")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));
        for (const auto edge : {Point(-1, 8), Point(64, 8), Point(8, -1), Point(8, 64)})
        {
            PlayerId a{0}, b{0};
            auto sim = makeWorld(a, b);
            auto position = sim.terrain.heightmapIndexToWorldCorner(edge.x, edge.y);
            auto id = sim.trySpawnUnit("ARMFUS", a, position, std::nullopt, true);
            REQUIRE(id);
            const auto rect = sim.computeFootprintRegion(position, sim.unitDefinitions.at("ARMFUS").movementCollisionInfo);
            REQUIRE_FALSE(sim.occupiedGrid.tryToRegion(rect));
            auto clipped = sim.occupiedGrid.clipRegion(rect);
            for (unsigned int version : {14u, 15u, SaveGameFormatVersion})
            {
                CAPTURE(edge.x, edge.y, version);
                auto copy = makeWorld(a, b);
                applySavedGame(copy, decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "EDGE", 0, version)), nullptr);
                clipped.forEach([&](const auto& cell) {
                    const auto& expected = sim.occupiedGrid.get(cell).buildingInfo;
                    const auto& actual = copy.occupiedGrid.get(cell).buildingInfo;
                    REQUIRE(expected);
                    REQUIRE(actual);
                    REQUIRE(actual->unit == expected->unit);
                    REQUIRE(actual->passable == expected->passable);
                    REQUIRE(copy.isCollisionAt(DiscreteRect(cell.x, cell.y, 1, 1)));
                });
            }
        }
    }

    TEST_CASE("SavedGame v15 repeated transport and movement cycles retain occupancy", "[save][phase8]")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));
        PlayerId a{0}, b{0};
        auto sim = makeWorld(a, b);
        sim.unitDefinitions.at("ARMCOM").transportCapacity = 1;
        const auto carrier = sim.trySpawnUnit("ARMCOM", a, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        const auto cargo = sim.trySpawnUnit("ARMTANK", a, sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
        REQUIRE(carrier); REQUIRE(cargo);
        for (const auto id : {*carrier, *cargo})
        { sim.getUnitState(id).finishBuilding(sim.unitDefinitions.at(sim.getUnitState(id).unitType)); }
        auto copy = makeWorld(a, b);
        copy.unitDefinitions.at("ARMCOM").transportCapacity = 1;
        auto equalCells = [&]() {
            const auto& x = sim.occupiedGrid.getVector();
            const auto& y = copy.occupiedGrid.getVector();
            bool equal = x.size() == y.size();
            for (std::size_t i = 0; equal && i < x.size(); ++i) { equal = x[i].mobileUnitId == y[i].mobileUnitId; }
            REQUIRE(equal);
        };
        auto restore = [&]() {
            applySavedGame(copy, decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "TRANSPORT", 0)), nullptr);
            REQUIRE(copy.computeHash() == sim.computeHash());
            equalCells();
        };
        restore();
        for (unsigned int cycle = 0; cycle < 3; ++cycle)
        {
            REQUIRE(sim.tryBoardTransport(*cargo, *carrier));
            restore();
            REQUIRE(copy.getUnitState(*cargo).carriedBy == carrier);
            for (const auto& cell : copy.occupiedGrid.getVector()) { REQUIRE(cell.mobileUnitId != cargo); }
            const auto destination = sim.terrain.heightmapIndexToWorldCenter(12 + cycle * 4, 12);
            REQUIRE(sim.tryDisembarkUnit(*cargo, destination));
            REQUIRE(copy.tryDisembarkUnit(*cargo, destination));
            const auto moveTo = sim.terrain.heightmapIndexToWorldCenter(30, 30);
            sim.getUnitState(*cargo).replaceOrders({MoveOrder(moveTo)});
            copy.getUnitState(*cargo).replaceOrders({MoveOrder(moveTo)});
            for (unsigned int tick = 0; tick < 60; ++tick)
            {
                sim.tick(); copy.tick();
                REQUIRE(sim.computeHash() == copy.computeHash());
                equalCells();
                sim.events.clear(); copy.events.clear();
                if (tick == 15) { restore(); }
            }
        }
    }

    TEST_CASE("SavedGame writes and reads each v7 through v14 payload", "[save]")
    {
        PlayerId a{0}, b{0};
        auto sim = makeWorld(a, b);
        for (unsigned int version = 7; version <= 14; ++version)
        {
            CAPTURE(version);
            auto decoded = decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "LEGACY", 0, version));
            REQUIRE(decoded.version == version);
            auto loaded = makeWorld(a, b);
            REQUIRE_NOTHROW(applySavedGame(loaded, decoded, nullptr));
            REQUIRE(loaded.players.size() == sim.players.size());
            REQUIRE(loaded.gameTime == sim.gameTime);
        }
    }

    TEST_CASE("SavedGame v15 rejects malformed mobile occupancy records", "[save]")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));
        PlayerId a{0}, b{0};
        auto sim = makeWorld(a, b);
        REQUIRE(sim.trySpawnUnit("ARMTANK", a, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt));
        auto saved = decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "GRID", 0));
        // A 2x2 tank has four occupied cells; v15's final block is
        // width,height,count followed by sorted (cell,owner) pairs.
        const auto start = saved.simulationBytes.size() - 12 - 4 * 8;
        auto put32 = [&](std::size_t at, std::uint32_t value) {
            for (unsigned int i = 0; i < 4; ++i) { saved.simulationBytes[at + i] = static_cast<char>(value >> (i * 8)); }
        };
        SECTION("wrong map dimensions") { put32(start, 1); }
        SECTION("unbounded element count") { put32(start + 8, 0xffffffffu); }
        SECTION("out of range cell") { put32(start + 12, 0xffffffffu); }
        SECTION("missing owner") { put32(start + 16, 0xffffffffu); }
        SECTION("duplicate cell")
        { std::copy_n(saved.simulationBytes.begin() + start + 12, 4, saved.simulationBytes.begin() + start + 20); }
        auto target = makeWorld(a, b);
        REQUIRE_THROWS_AS(applySavedGame(target, saved, nullptr), SaveFormatException);
    }

    TEST_CASE("SavedGame encode/decode round-trip preserves hash-covered state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        // make the state interesting: one moving unit, one damaged unit,
        // a building, modified resources, some elapsed time
        auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(tank);
        auto enemyTank = simA.trySpawnUnit("CORTANK", playerB, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
        REQUIRE(enemyTank);
        auto fus = simA.trySpawnUnit("ARMFUS", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 20), std::nullopt);
        REQUIRE(fus);

        simA.getUnitState(*tank).orders.push_back(UnitOrder(MoveOrder(SimVector(100_ss, 0_ss, 300_ss))));
        simA.getUnitState(*enemyTank).hitPoints = 123;
        // interactive self-destruct armed mid-flight: the countdown
        // expires during post-load continuation, so the fuse RNG draw
        // ordering is hash-verified on both sides
        simA.getUnitState(*fus).finishBuilding(simA.unitDefinitions.at("ARMFUS"));
        simA.getUnitState(*fus).selfDestruct = SelfDestructOrder{60};
        simA.getPlayer(playerA).metal = Metal(4242);
        simA.getPlayer(playerA).unitsKilled = 7;

        for (int i = 0; i < 45; ++i)
        {
            simA.tick();
        }

        const auto hashBefore = simA.computeHash();

        const auto bytes = encodeSavedGame(simA, params, nullptr, "TESTSAVE", 1700000000);

        // metadata must be readable standalone
        const auto meta = readSaveMetadata(bytes);
        REQUIRE(meta.saveName == "TESTSAVE");
        REQUIRE(meta.mapName == "TESTMAP");
        REQUIRE(meta.playerCount == 2);
        REQUIRE(meta.players.size() == 2);
        REQUIRE(meta.players[0].side == "ARM");
        REQUIRE(meta.players[0].isHuman);
        REQUIRE(!meta.players[1].isHuman);
        REQUIRE(!meta.isCampaign);
        REQUIRE(meta.gameTime.value == 45);
        REQUIRE(meta.unixTimeSeconds == 1700000000);

        auto decoded = decodeSavedGame(bytes);
        REQUIRE(decoded.meta.saveName == "TESTSAVE");

        auto paramsRoundTrip = gameParametersFromSave(decoded);
        REQUIRE(paramsRoundTrip.mapName == "TESTMAP");
        REQUIRE(paramsRoundTrip.players[0].has_value());
        REQUIRE(paramsRoundTrip.players[0]->side == "ARM");
        REQUIRE(paramsRoundTrip.players[1].has_value());
        REQUIRE(std::holds_alternative<PlayerControllerTypeComputer>(paramsRoundTrip.players[1]->controller));

        // build the fresh sim the way LoadingScene would (same map+defs),
        // then apply
        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);

        REQUIRE(simB.computeHash() == hashBefore);

        // deterministic continuation: same inputs -> identical hashes
        for (int i = 0; i < 60; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simB.computeHash() == simA.computeHash());
        }
    }

    TEST_CASE("SavedGame v8 round-trips F2 sim state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        // a patrolling unit mid-diversion carries the new order state;
        // armored/busy/buggerOff flags and the unit cap live on the
        // unit and the sim respectively
        auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(tank);
        auto wreck = simA.trySpawnUnit("CORTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
        REQUIRE(wreck);

        PatrolOrder patrol({SimVector(50_ss, 0_ss, 50_ss), SimVector(60_ss, 0_ss, 60_ss)});
        patrol.nextWaypointIndex = 1;
        patrol.scanTicks = 37;
        patrol.divertTarget = PatrolRepairTarget{*wreck};
        simA.getUnitState(*tank).orders.push_back(UnitOrder(std::move(patrol)));

        auto& wreckState = simA.getUnitState(*wreck);
        wreckState.armored = true;
        wreckState.busy = true;
        wreckState.buggerOff = true;

        simA.maxUnitsPerPlayer = 321;

        // an in-flight interceptor claim rides the projectile
        WeaponDefinition amd{};
        amd.velocity = SimScalar(10);
        amd.damage["DEFAULT"] = 5;
        amd.damageRadius = SimScalar(4);
        amd.physicsType = ProjectilePhysicsTypeBallistic{};
        simA.weaponDefinitions.insert_or_assign("AMD", amd);
        auto missile = simA.createProjectileFromWeapon(playerB, "AMD", simA.terrain.heightmapIndexToWorldCenter(20, 8), SimVector(0_ss, 0_ss, -1_ss), 0_ss, std::nullopt);
        auto missileId = simA.projectiles.emplace(std::move(missile));
        auto claim = simA.createProjectileFromWeapon(playerA, "AMD", simA.terrain.heightmapIndexToWorldCenter(8, 8), SimVector(0_ss, 1_ss, 0_ss), 0_ss, std::nullopt);
        claim.targetProjectile = missileId;
        // v8 state: a guided shot steering onto a clicked ground point
        claim.targetPosition = SimVector(33_ss, 4_ss, 55_ss);
        auto claimId = simA.projectiles.emplace(std::move(claim));

        const auto hashBefore = simA.computeHash();
        const auto bytes = encodeSavedGame(simA, params, nullptr, "F2SAVE", 1700000000);

        auto decoded = decodeSavedGame(bytes);
        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.weaponDefinitions.insert_or_assign("AMD", amd);
        applySavedGame(simB, decoded, nullptr);

        REQUIRE(simB.computeHash() == hashBefore);

        const auto& restoredPatrol = std::get<PatrolOrder>(simB.getUnitState(*tank).orders.front());
        REQUIRE(restoredPatrol.nextWaypointIndex == 1);
        REQUIRE(restoredPatrol.scanTicks == 37);
        REQUIRE(restoredPatrol.divertTarget);
        REQUIRE(std::holds_alternative<PatrolRepairTarget>(*restoredPatrol.divertTarget));
        REQUIRE(std::get<PatrolRepairTarget>(*restoredPatrol.divertTarget).target == *wreck);

        REQUIRE(simB.getUnitState(*wreck).armored);
        REQUIRE(simB.getUnitState(*wreck).busy);
        REQUIRE(simB.getUnitState(*wreck).buggerOff);
        REQUIRE(simB.maxUnitsPerPlayer == 321);

        auto restoredClaim = simB.projectiles.tryGet(claimId);
        REQUIRE(restoredClaim);
        REQUIRE(restoredClaim->get().targetProjectile);
        REQUIRE(*restoredClaim->get().targetProjectile == missileId);
        REQUIRE(restoredClaim->get().targetPosition);
        REQUIRE(static_cast<bool>(*restoredClaim->get().targetPosition == SimVector(33_ss, 4_ss, 55_ss)));

        // deterministic continuation
        for (int i = 0; i < 30; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simB.computeHash() == simA.computeHash());
        }
    }

    TEST_CASE("SavedGame round-trip preserves meteor-shower scheduler state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        WeaponDefinition meteor{};
        meteor.velocity = SimScalar(10);
        meteor.isMeteor = true;
        meteor.damage.insert_or_assign("DEFAULT", 100);
        meteor.damageRadius = SimScalar(20);

        MeteorShowerParams mp;
        mp.weaponName = "Meteor";
        mp.radius = 300;
        mp.density = 2.0f;
        mp.duration = 5.0f;
        mp.interval = 60.0f;
        simA.weaponDefinitions.insert_or_assign("METEOR", meteor);
        simA.configureMeteorShower(mp, true);

        // first strike runs t15..t165; park mid-strike so the save
        // carries live scheduler state and in-flight meteors
        for (int i = 0; i < 60; ++i)
        {
            simA.tick();
        }
        REQUIRE(simA.meteorShower.active);
        const auto hashBefore = simA.computeHash();

        const auto bytes = encodeSavedGame(simA, params, nullptr, "METEOR", 0);
        auto decoded = decodeSavedGame(bytes);

        // configure first (as LoadingScene does), then let the save
        // restore the mutable scheduler state over it
        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.weaponDefinitions.insert_or_assign("METEOR", meteor);
        simB.configureMeteorShower(mp, true);
        applySavedGame(simB, decoded, nullptr);

        const auto& a = simA.meteorShower;
        const auto& b = simB.meteorShower;
        REQUIRE(b.enabled);
        REQUIRE(b.active);
        REQUIRE(b.nextStrikeTime == a.nextStrikeTime);
        REQUIRE(b.timeStrikeEnds == a.timeStrikeEnds);
        REQUIRE(b.nextHitTime == a.nextHitTime);
        REQUIRE(b.originX == a.originX);
        REQUIRE(b.originZ == a.originZ);
        REQUIRE(b.targetX == a.targetX);
        REQUIRE(b.targetZ == a.targetZ);
        REQUIRE(simB.computeHash() == hashBefore);

        // the strike resumes from the saved cadence identically --
        // no restarted duration, no skipped or duplicated hits
        for (int i = 0; i < 200; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simB.computeHash() == simA.computeHash());
        }
    }

    TEST_CASE("SavedGame round-trip preserves explored fog-of-war history")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        simA.unitDefinitions.at("ARMTANK").sightDistance = SimScalar(100);
        simA.lineOfSightEnabled = true;
        simA.mappingEnabled = true;

        auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(10, 10), std::nullopt);
        REQUIRE(tank);
        simA.getUnitState(*tank).buildTimeCompleted = simA.unitDefinitions.at("ARMTANK").buildTime;

        // first visibility pass: (10,10) visible and explored
        simA.updateVisibility();
        REQUIRE(simA.playerVisibility[playerA.value].losGrid.get(10, 10) == 1);
        REQUIRE(simA.playerVisibility[playerA.value].exploredGrid.get(10, 10) == 1);

        // move away: (10,10) becomes explored-but-not-visible,
        // (50,50) becomes visible, (30,30) stays unexplored
        simA.getUnitState(*tank).position = simA.terrain.heightmapIndexToWorldCenter(50, 50);
        simA.updateVisibility();

        const auto& pvA = simA.playerVisibility[playerA.value];
        REQUIRE(pvA.losGrid.get(10, 10) == 0);
        REQUIRE(pvA.exploredGrid.get(10, 10) == 1);
        REQUIRE(pvA.losGrid.get(50, 50) == 1);
        REQUIRE(pvA.exploredGrid.get(50, 50) == 1);
        REQUIRE(pvA.losGrid.get(30, 30) == 0);
        REQUIRE(pvA.exploredGrid.get(30, 30) == 0);

        const auto bytes = encodeSavedGame(simA, params, nullptr, "FOG", 0);
        auto decoded = decodeSavedGame(bytes);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.unitDefinitions.at("ARMTANK").sightDistance = SimScalar(100);
        applySavedGame(simB, decoded, nullptr);
        REQUIRE(playerA == dummyA);

        // decode must restore the accumulated history even though
        // losGrid is intentionally absent from the save
        const auto& pvB = simB.playerVisibility[playerA.value];
        REQUIRE(pvB.exploredGrid.get(10, 10) == 1);
        REQUIRE(pvB.exploredGrid.get(30, 30) == 0);

        // the first post-load visibility pass must preserve that
        // history while recomputing current LOS from unit positions
        simB.updateVisibility();
        const auto& pvB2 = simB.playerVisibility[playerA.value];
        REQUIRE(pvB2.exploredGrid.get(10, 10) == 1);
        REQUIRE(pvB2.losGrid.get(50, 50) == 1);
        REQUIRE(pvB2.losGrid.get(10, 10) == 0);
        REQUIRE(pvB2.exploredGrid.get(30, 30) == 0);
        REQUIRE(pvB2.losGrid.get(30, 30) == 0);
    }

    TEST_CASE("SavedGame round-trip restores slot generations")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        // create and destroy a unit so a free slot with bumped
        // generation exists in the middle of the table
        auto doomed = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(doomed);
        auto keeper = simA.trySpawnUnit("CORTANK", playerB, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
        REQUIRE(keeper);
        simA.quietlyKillUnit(*doomed);
        simA.deleteDeadUnits();

        const auto bytes = encodeSavedGame(simA, params, nullptr, "GEN", 0);
        auto decoded = decodeSavedGame(bytes);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);

        // the surviving unit must keep its id, and the next emplaced
        // unit must reuse the freed slot with the bumped generation
        REQUIRE(simB.unitExists(*keeper));
        auto spawnedA = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(30, 8), std::nullopt);
        auto spawnedB = simB.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(30, 8), std::nullopt);
        REQUIRE(spawnedA);
        REQUIRE(spawnedB);
        REQUIRE(*spawnedA == *spawnedB);
    }

    TEST_CASE("SavedGame round-trip preserves campaign thumbs")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        params.campaign = CampaignMissionParameters{
            "Arm Campaign",
            "ARM",
            0,
            "AC01.ota",
            "1: A Hero Returns",
            "Medium"};

        MissionInfo info;
        info.campaignName = "Arm Campaign";
        info.campaignSide = "ARM";
        info.missionName = "1: A Hero Returns";
        info.missionIndex = 0;
        info.missionFile = "AC01.ota";
        info.schemaType = "Medium";
        info.localPlayerId = playerA;
        info.objectives = {MissionObjective{"DestroyAllUnits", {}}};
        simA.missionInfo = info;
        simA.initMissionObjectives();

        CampaignProgress progressA;
        progressA.resetCampaign("Arm Campaign", "ARM", 25);
        progressA.recordResult("Arm Campaign", "ARM", 0, true, 25);

        const auto bytes = encodeSavedGame(simA, params, &progressA, "AC01", 42);
        auto decoded = decodeSavedGame(bytes);
        REQUIRE(decoded.meta.isCampaign);
        REQUIRE(decoded.meta.campaignName == "Arm Campaign");
        REQUIRE(decoded.meta.campaignSide == "ARM");
        REQUIRE(decoded.meta.missionFile == "AC01.ota");
        REQUIRE(decoded.meta.difficulty == "Medium");

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        CampaignProgress progressB;
        applySavedGame(simB, decoded, &progressB);

        // mission state survives
        REQUIRE(simB.missionInfo.has_value());
        REQUIRE(simB.missionInfo->missionFile == "AC01.ota");
        REQUIRE(simB.missionObjectives.has_value());
        REQUIRE(simB.missionObjectives->getVictoryObjectives().size() == 1);

        // campaign thumbs land in the receiving CampaignProgress
        const auto* run = progressB.findRun("Arm Campaign", "ARM");
        REQUIRE(run != nullptr);
        REQUIRE(run->status.size() == 25);
        REQUIRE(run->status[0] == CampaignMissionStatus::Won);
        REQUIRE(run->status[1] == CampaignMissionStatus::Unplayed);

        auto paramsRoundTrip = gameParametersFromSave(decoded);
        REQUIRE(paramsRoundTrip.campaign.has_value());
        REQUIRE(paramsRoundTrip.campaign->missionIndex == 0);
        REQUIRE(paramsRoundTrip.campaign->difficulty == "Medium");
    }

    /**
     * The computer economy coefficient is not serialized: it is
     * re-derived from the saved campaign difficulty when the scene
     * is built. LoadingScene assigns computerResourceScale before
     * building players, then applySavedGame restores sim state on
     * top. This exercises the same chain a real campaign save load
     * runs: meta.difficulty -> gameParametersFromSave ->
     * campaignDifficultyComputerResourceScale -> sim state.
     */
    TEST_CASE("SavedGame campaign difficulty re-derives the computer resource scale")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        params.campaign = CampaignMissionParameters{
            "Arm Campaign",
            "ARM",
            0,
            "AC01.ota",
            "1: A Hero Returns",
            "Medium"};

        // the live sim carries the difficulty-derived coefficient at
        // save time (LoadingScene sets it before play); v5 persists it
        simA.computerResourceScale = campaignDifficultyComputerResourceScale(params.campaign->difficulty);
        REQUIRE(simA.computerResourceScale == 0.7f);

        const auto bytes = encodeSavedGame(simA, params, nullptr, "AC01", 42);
        auto decoded = decodeSavedGame(bytes);

        auto paramsRoundTrip = gameParametersFromSave(decoded);
        REQUIRE(paramsRoundTrip.campaign.has_value());
        REQUIRE(paramsRoundTrip.campaign->difficulty == "Medium");

        // rebuild the sim the way LoadingScene does for a campaign
        // load: coefficient re-derived from the restored difficulty,
        // then the saved state applied over it
        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.computerResourceScale = campaignDifficultyComputerResourceScale(paramsRoundTrip.campaign->difficulty);
        applySavedGame(simB, decoded, nullptr);
        REQUIRE(simB.computerResourceScale == 0.7f);

        // a computer-owned unit's contribution now lands at 0.7x
        auto tank = simB.trySpawnUnit("CORTANK", playerB, simB.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(tank);
        simB.addScaledResourceDelta(*tank, Energy(100), Metal(0));
        REQUIRE(simB.getPlayer(playerB).energyProductionBuffer == Energy(70));

        // a human-owned unit in the same sim stays unscaled
        auto tankA = simB.trySpawnUnit("ARMTANK", playerA, simB.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
        REQUIRE(tankA);
        simB.addScaledResourceDelta(*tankA, Energy(100), Metal(0));
        REQUIRE(simB.getPlayer(playerA).energyProductionBuffer == Energy(100));
    }

    /**
     * Format v2 appends the resolved campaign AI tables (weights,
     * limits, task timers, group membership, squad target) after the
     * campaign thumbs. The deterministic unit index is not stored;
     * restore rebuilds it from the loaded definitions. A v1 file has
     * no such section and restores with an empty campaignAi, which the
     * loader then re-initializes from the mission's aiprofile.
     */
    TEST_CASE("SavedGame round-trip preserves campaign AI state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        params.campaign = CampaignMissionParameters{
            "Arm Campaign",
            "ARM",
            0,
            "AC01.ota",
            "1: A Hero Returns",
            "Medium"};

        auto profile = parseCampaignAiProfile(
            "plan any\nweight CORTANK 0.5\nlimit ARMTANK 2\n",
            CampaignAiDifficulty::Medium);
        initCampaignAiPlayer(simA, playerB, profile);

        auto enemyTank = simA.trySpawnUnit("CORTANK", playerB, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
        REQUIRE(enemyTank);

        for (int i = 0; i < 45; ++i)
        {
            simA.tick();
        }
        REQUIRE(!simA.campaignAi.empty());

        const auto hashBefore = simA.computeHash();
        const auto bytes = encodeSavedGame(simA, params, nullptr, "AI", 0);
        auto decoded = decodeSavedGame(bytes);
        REQUIRE(decoded.version == SaveGameFormatVersion);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);

        REQUIRE(simB.campaignAi.size() == 1);
        const auto& st = simB.campaignAi.front();
        REQUIRE(st.player == playerB);
        // index rebuilt deterministically from the loaded definitions
        REQUIRE(std::is_sorted(st.unitIndex.begin(), st.unitIndex.end()));
        auto idx = std::find(st.unitIndex.begin(), st.unitIndex.end(), "CORTANK");
        REQUIRE(idx != st.unitIndex.end());
        auto at = static_cast<std::size_t>(idx - st.unitIndex.begin());
        REQUIRE(st.unitWeights[at] == 50);
        auto idxT = std::find(st.unitIndex.begin(), st.unitIndex.end(), "ARMTANK");
        REQUIRE(st.unitLimits[static_cast<std::size_t>(idxT - st.unitIndex.begin())] == 2);

        REQUIRE(simB.computeHash() == hashBefore);

        // deterministic continuation: the AI keeps issuing the same
        // orders after restore because timers/groups/rng survived
        for (int i = 0; i < 60; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simB.computeHash() == simA.computeHash());
        }
    }

    /**
     * C13B: the campaignAi section is mode-agnostic -- a skirmish
     * save (no campaign params) carries each computer player's AI
     * state, restores it without re-initialization, and continues
     * deterministically. The skirmish frontend toggles ride the v5
     * metadata so the re-attach can re-derive the plan gate.
     */
    TEST_CASE("SavedGame round-trip preserves skirmish AI state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        // a second computer player exercises multi-AI round-trip
        auto playerC = simA.addPlayer(makePlayer(PlayerColorIndex(2), "CORE", GamePlayerType::Computer));
        auto params = makeParams(); // no campaign -> skirmish params
        params.players[2] = PlayerInfo{
            std::nullopt,
            PlayerControllerType(PlayerControllerTypeComputer()),
            "CORE",
            PlayerColorIndex(2),
            Metal(10000),
            Energy(10000)};
        params.skirmish.difficulty = SkirmishDifficulty::Hard;
        simA.computerResourceScale = skirmishDifficultyComputerResourceScale(params.skirmish.difficulty);

        auto profile = parseCampaignAiProfile(
            "plan hard\nweight CORTANK 0.5\nlimit ARMTANK 2\n",
            skirmishAiDifficultyIndex(params.skirmish.difficulty));
        attachCampaignAiPlayers(simA, profile);
        REQUIRE(simA.campaignAi.size() == 2);

        simA.trySpawnUnit("CORTANK", playerB, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
        simA.trySpawnUnit("ARMTANK", playerC, simA.terrain.heightmapIndexToWorldCenter(24, 8), std::nullopt);

        for (int i = 0; i < 45; ++i)
        {
            simA.tick();
        }

        const auto hashBefore = simA.computeHash();
        const auto bytes = encodeSavedGame(simA, params, nullptr, "SKIRM", 0);
        auto decoded = decodeSavedGame(bytes);
        REQUIRE(decoded.version == SaveGameFormatVersion);

        // the restored params carry the skirmish difficulty so the
        // LoadingScene re-attach resolves the same plan gate
        auto paramsRoundTrip = gameParametersFromSave(decoded);
        REQUIRE(!paramsRoundTrip.campaign);
        REQUIRE(paramsRoundTrip.skirmish.difficulty == SkirmishDifficulty::Hard);
        REQUIRE(paramsRoundTrip.players[1]->controller.index() == params.players[1]->controller.index());

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.addPlayer(makePlayer(PlayerColorIndex(2), "CORE", GamePlayerType::Computer));
        applySavedGame(simB, decoded, nullptr);

        REQUIRE(simB.campaignAi.size() == 2);
        for (const auto& st : simB.campaignAi)
        {
            REQUIRE(std::is_sorted(st.unitIndex.begin(), st.unitIndex.end()));
            auto idx = std::find(st.unitIndex.begin(), st.unitIndex.end(), "ARMTANK");
            REQUIRE(idx != st.unitIndex.end());
            REQUIRE(st.unitLimits[static_cast<std::size_t>(idx - st.unitIndex.begin())] == 2);
        }

        REQUIRE(simB.computeHash() == hashBefore);

        // re-attach must skip restored states -- no duplicates
        attachCampaignAiPlayers(simB, profile);
        REQUIRE(simB.campaignAi.size() == 2);

        // deterministic continuation across both AI controllers
        for (int i = 0; i < 60; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simB.computeHash() == simA.computeHash());
        }
    }

    TEST_CASE("SavedGame version 1 restores without campaign AI state")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);

        // a v1 file is a v4 payload minus the trailing campaign-Ai
        // u32 count (v2+, empty here) and the 30-byte meteor-shower
        // block (v4+: 2 bools + 3 GameTimes + 4 i32s), re-wrapped
        // with version 1. Format v5 inserted its additions inside the
        // metadata block and after the meteor block, so the fixture is
        // generated by encoding a genuine v4 image rather than
        // truncating a v5 one.
        std::uint32_t v4version = 0;
        const auto v4Payload = decodeSaveFile(encodeSavedGame(simA, params, nullptr, "V1", 0, 4), v4version);
        REQUIRE(v4version == 4);
        std::vector<char> v1Payload(v4Payload.begin(), v4Payload.end() - 4 - 30);
        const auto v1Bytes = encodeSaveFile(1, v1Payload);

        auto decoded = decodeSavedGame(v1Bytes);
        REQUIRE(decoded.version == 1);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);
        REQUIRE(simB.campaignAi.empty());
    }

    /**
     * Every supported save version must decode through its own reader
     * path. The versioned writer emits a faithful image for v4+; v1-v3
     * fixtures are synthesized by stripping the later tail blocks from
     * a v4 image (v1 lacks the campaign-AI count and the 30-byte meteor
     * block, v2/v3 lack only the meteor block). All units stay idle so
     * no NavigationStateMoving pathExhaustedAt (v3+) field exists and
     * the stripped payload is byte-valid for v2 as well.
     */
    TEST_CASE("SavedGame format versions 1-6 decode through their respective readers")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(tank);
        auto fus = simA.trySpawnUnit("ARMFUS", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 20), std::nullopt);
        REQUIRE(fus);
        simA.getUnitState(*fus).finishBuilding(simA.unitDefinitions.at("ARMFUS"));
        simA.getUnitState(*tank).hitPoints = 321;
        // interactive self-destruct armed mid-countdown with a live
        // fuse (v6 only -- older readers must leave it absent)
        simA.getUnitState(*fus).selfDestruct = SelfDestructOrder(60);
        simA.getUnitState(*fus).selfDestruct->fuseTicks = 7;
        // v4 meteor-shower state
        simA.meteorShower.enabled = true;
        simA.meteorShower.active = true;
        simA.meteorShower.nextStrikeTime = GameTime(1234);
        simA.meteorShower.originX = 11;
        simA.meteorShower.originZ = 22;
        simA.meteorShower.targetX = 33;
        simA.meteorShower.targetZ = 44;
        // v5 skirmish fields
        simA.commanderDeathContinues = true;
        simA.computerResourceScale = 2.5f;
        simA.getPlayer(playerA).allyGroup = 1;
        // campaignAi stays empty so the v4 tail is exactly [u32 0][30B meteor]

        // v4 image: v2 campaign-AI + v4 meteor blocks written
        // unconditionally, v5/v6 additions gated off
        std::uint32_t v4version = 0;
        const auto v4Payload = decodeSaveFile(encodeSavedGame(simA, params, nullptr, "VMATRIX", 0, 4), v4version);
        REQUIRE(v4version == 4);
        const std::vector<char> noMeteorPayload(v4Payload.begin(), v4Payload.end() - 30);
        const std::vector<char> v1Payload(v4Payload.begin(), v4Payload.end() - 34);

        // decode a file image and apply it to a freshly built world;
        // returns the decoded version and the restored simulation
        auto decodeAndApply = [&](const std::vector<char>& bytes) {
            auto decoded = decodeSavedGame(bytes);
            const auto version = decoded.version;
            PlayerId dummyA{0}, dummyB{0};
            auto simOut = makeWorld(dummyA, dummyB);
            applySavedGame(simOut, decoded, nullptr);
            return std::pair{version, std::move(simOut)};
        };

        SECTION("version 1")
        {
            const auto bytes = encodeSaveFile(1, v1Payload);
            auto [version, simOut] = decodeAndApply(bytes);
            REQUIRE(version == 1);
            REQUIRE(simOut.campaignAi.empty());
            REQUIRE(!simOut.meteorShower.active);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 2")
        {
            const auto bytes = encodeSaveFile(2, noMeteorPayload);
            auto [version, simOut] = decodeAndApply(bytes);
            REQUIRE(version == 2);
            REQUIRE(simOut.campaignAi.empty());
            REQUIRE(!simOut.meteorShower.active);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 3")
        {
            const auto bytes = encodeSaveFile(3, noMeteorPayload);
            auto [version, simOut] = decodeAndApply(bytes);
            REQUIRE(version == 3);
            REQUIRE(!simOut.meteorShower.active);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 4")
        {
            auto [version, simOut] = decodeAndApply(encodeSavedGame(simA, params, nullptr, "VMATRIX", 0, 4));
            REQUIRE(version == 4);
            REQUIRE(simOut.meteorShower.active);
            REQUIRE(simOut.meteorShower.nextStrikeTime == GameTime(1234));
            REQUIRE(simOut.meteorShower.originX == 11);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            // v5 fields absent: reader keeps the fresh-sim values
            REQUIRE(!simOut.commanderDeathContinues);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 5")
        {
            auto [version, simOut] = decodeAndApply(encodeSavedGame(simA, params, nullptr, "VMATRIX", 0, 5));
            REQUIRE(version == 5);
            REQUIRE(simOut.commanderDeathContinues);
            REQUIRE(simOut.computerResourceScale == 2.5f);
            REQUIRE(simOut.getPlayer(playerA).allyGroup == 1);
            // the writer gates selfDestruct at v6: absent at v5
            REQUIRE(!simOut.getUnitState(*fus).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 6")
        {
            auto [version, simOut] = decodeAndApply(encodeSavedGame(simA, params, nullptr, "VMATRIX", 0, 6));
            REQUIRE(version == 6);
            // armed unit survives with the exact remaining countdown
            // and fuse; the unarmed unit stays unarmed
            const auto& sd = simOut.getUnitState(*fus).selfDestruct;
            REQUIRE(sd);
            REQUIRE(sd->countdownTicks == 60);
            REQUIRE(sd->fuseTicks == 7);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
        }

        SECTION("version 7")
        {
            const auto v7Bytes = encodeSavedGame(simA, params, nullptr, "VMATRIX", 0, 7);
            auto [version, simOut] = decodeAndApply(v7Bytes);
            REQUIRE(version == 7);
            const auto& sd = simOut.getUnitState(*fus).selfDestruct;
            REQUIRE(sd);
            REQUIRE(sd->countdownTicks == 60);
            REQUIRE(sd->fuseTicks == 7);
            REQUIRE(!simOut.getUnitState(*tank).selfDestruct);
            REQUIRE(simOut.getUnitState(*tank).hitPoints == 321);
            REQUIRE(simOut.campaignAi.empty());
            // v8 added projectile targetPosition + deadlock state;
            // the v7 image carries neither, so the restored world is
            // state-identical to the saved one and must continue
            // deterministically -- rng state is serialized, so equal
            // hashes at t0 imply equal hashes every later tick
            REQUIRE(computeHashOf(simOut) == computeHashOf(simA));
            for (int i = 0; i < 300; ++i)
            {
                simA.tick();
                simOut.tick();
            }
            REQUIRE(computeHashOf(simOut) == computeHashOf(simA));

            // when RWE_WRITE_V7_SAVE is set, also emit the image to
            // disk with a real retail map name so the
            // RWE_TEST_SAVE_FILE harness can apply it against freshly
            // loaded retail definitions -- same writer path a v7
            // build produced, with every `version >= 8` field absent
            if (const char* outPath = std::getenv("RWE_WRITE_V7_SAVE"))
            {
                auto fileParams = params;
                fileParams.mapName = "Coast To Coast";
                auto fileBytes = encodeSavedGame(simA, fileParams, nullptr, "v7qa", 0, 7);
                std::ofstream out(outPath, std::ios::binary);
                out.write(fileBytes.data(), fileBytes.size());
            }
        }
    }

    /**
     * ENDMSN SaveGame encodes the completed mission simulation
     * after the debrief's recordResult call has stamped the W/L
     * thumb (retail order: teardown stamps +0x391cf at 0x41dc97,
     * then the save writer serializes it at 0x432863). Loading the
     * save restores the terminal mission state; the objective
     * evaluator's latched result then re-triggers the campaign
     * result transition exactly once in the new GameScene lifecycle
     * (missionEndAction gates repeats). Whether retail presents the
     * same flow after loading an already-terminal save is not
     * decoded -- the assertions here are RWE-observed behavior.
     */
    TEST_CASE("SavedGame ENDMSN save of a completed mission round-trips terminal state and thumbs")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        params.campaign = CampaignMissionParameters{
            "Arm Campaign",
            "ARM",
            0,
            "AC01.ota",
            "1: A Hero Returns",
            "Medium"};

        MissionInfo info;
        info.campaignName = "Arm Campaign";
        info.campaignSide = "ARM";
        info.missionName = "1: A Hero Returns";
        info.missionIndex = 0;
        info.missionFile = "AC01.ota";
        info.schemaType = "Medium";
        info.localPlayerId = playerA;
        simA.missionInfo = info;

        SECTION("victory")
        {
            // local unit present, no enemy units: DestroyAllUnits is
            // vacuously satisfied and latches Victory after the
            // evaluator's debounce countdown
            info.objectives = {MissionObjective{"DestroyAllUnits", {}}};
            simA.initMissionObjectives();
            auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
            REQUIRE(tank);
            // the evaluator runs once per 30 ticks and its debounce
            // counts evaluations, so the latch needs ~6 evals
            for (int i = 0; i < 240 && simA.getMissionResult() == MissionResult::InProgress; ++i)
            {
                simA.tick();
            }
            REQUIRE(simA.getMissionResult() == MissionResult::Victory);

            CampaignProgress progressA;
            progressA.resetCampaign("Arm Campaign", "ARM", 25);
            progressA.recordResult("Arm Campaign", "ARM", 0, true, 25);

            const auto gameTime = simA.gameTime;
            const auto bytes = encodeSavedGame(simA, params, &progressA, "ENDMSN-WIN", 7);
            auto decoded = decodeSavedGame(bytes);
            REQUIRE(decoded.meta.isCampaign);

            PlayerId dummyA{0}, dummyB{0};
            auto simB = makeWorld(dummyA, dummyB);
            CampaignProgress progressB;
            applySavedGame(simB, decoded, &progressB);

            // the just-stamped W landed in the save
            REQUIRE(progressB.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Won);

            // terminal state + identity survive the round trip
            REQUIRE(simB.getMissionResult() == MissionResult::Victory);
            REQUIRE(simB.gameTime == gameTime);
            REQUIRE(simB.unitExists(*tank));
            REQUIRE(simB.computeHash() == simA.computeHash());

            // restoring an already-terminal sim deterministically
            // re-declares the result at most once per GameScene
            REQUIRE(missionEndAction(simB.getMissionResult(), false) == MissionEndAction::DeclareVictory);
            REQUIRE(missionEndAction(simB.getMissionResult(), true) == MissionEndAction::None);
        }

        SECTION("defeat")
        {
            // enemy unit present, no local units: AllUnitsKilled
            // latches Defeat
            info.objectives = {MissionObjective{"AllUnitsKilled", {}}};
            simA.initMissionObjectives();
            auto enemyTank = simA.trySpawnUnit("CORTANK", playerB, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
            REQUIRE(enemyTank);
            for (int i = 0; i < 240 && simA.getMissionResult() == MissionResult::InProgress; ++i)
            {
                simA.tick();
            }
            REQUIRE(simA.getMissionResult() == MissionResult::Defeat);

            CampaignProgress progressA;
            progressA.resetCampaign("Arm Campaign", "ARM", 25);
            progressA.recordResult("Arm Campaign", "ARM", 0, false, 25);

            const auto bytes = encodeSavedGame(simA, params, &progressA, "ENDMSN-LOSS", 7);
            auto decoded = decodeSavedGame(bytes);

            PlayerId dummyA{0}, dummyB{0};
            auto simB = makeWorld(dummyA, dummyB);
            CampaignProgress progressB;
            applySavedGame(simB, decoded, &progressB);

            // the just-stamped L landed in the save
            REQUIRE(progressB.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Lost);
            REQUIRE(simB.getMissionResult() == MissionResult::Defeat);
            REQUIRE(missionEndAction(simB.getMissionResult(), false) == MissionEndAction::DeclareDefeat);
        }
    }

    TEST_CASE("SavedGame rejects malformed input")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();
        simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        const auto good = encodeSavedGame(simA, params, nullptr, "TEST", 0);

        SECTION("empty file")
        {
            REQUIRE_THROWS_AS(decodeSavedGame({}), SaveFormatException);
            REQUIRE_THROWS_AS(readSaveMetadata({}), SaveFormatException);
        }
        SECTION("truncated header")
        {
            std::vector<char> bad(good.begin(), good.begin() + 5);
            REQUIRE_THROWS_AS(decodeSavedGame(bad), SaveFormatException);
        }
        SECTION("truncated payload")
        {
            std::vector<char> bad(good.begin(), good.end() - 20);
            REQUIRE_THROWS_AS(decodeSavedGame(bad), SaveFormatException);
        }
        SECTION("bad magic")
        {
            auto bad = good;
            bad[0] = 'X';
            REQUIRE_THROWS_AS(decodeSavedGame(bad), SaveFormatException);
        }
        SECTION("corrupt payload (checksum)")
        {
            auto bad = good;
            bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] + 1);
            REQUIRE_THROWS_AS(decodeSavedGame(bad), SaveFormatException);
        }
        SECTION("unsupported future version")
        {
            // bytes 8..11 carry the version after the 8-byte magic
            auto bad = good;
            bad[8] = static_cast<char>(99);
            bad[9] = bad[10] = bad[11] = 0;
            // rewrite the checksum so only the version gate trips
            // (can't recompute here -- decode checks version first only
            // after checksum; instead verify the file IS rejected)
            REQUIRE_THROWS_AS(decodeSavedGame(bad), SaveFormatException);
        }
        SECTION("apply rejects trailing garbage")
        {
            PlayerId dummyA{0}, dummyB{0};
            auto decoded = decodeSavedGame(good);
            decoded.simulationBytes.push_back(0xAB);
            auto simB = makeWorld(dummyA, dummyB);
            REQUIRE_THROWS_AS(applySavedGame(simB, decoded, nullptr), SaveFormatException);
        }
        SECTION("unknown unit type rejected on apply")
        {
            auto decoded = decodeSavedGame(good);
            // find "ARMTANK" in the sim body and corrupt a byte of the name
            const std::string needle = "ARMTANK";
            for (std::size_t i = 0; i + needle.size() <= decoded.simulationBytes.size(); ++i)
            {
                if (std::equal(needle.begin(), needle.end(), decoded.simulationBytes.begin() + i))
                {
                    decoded.simulationBytes[i] = 'Z';
                    break;
                }
            }
            PlayerId dummyA{0}, dummyB{0};
            auto simB = makeWorld(dummyA, dummyB);
            REQUIRE_THROWS_AS(applySavedGame(simB, decoded, nullptr), SaveFormatException);
        }
    }

    /**
     * Stock features/green/TREES.TDF authors [Tree2Dead] and reaches it
     * through [Tree2] featureburnt=Tree2Dead. The loader records
     * FeatureDefinition::name in authored case while featureNameIndex
     * keys are uppercased, so a save that serializes the recorded name
     * verbatim (like the AC01 QA save carrying 'Tree2Dead') must have
     * it canonicalized on read; new saves write the canonical name.
     */
    TEST_CASE("SavedGame round-trips feature defs recorded in authored case")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        FeatureDefinition deadTree;
        deadTree.name = "Tree2Dead";
        deadTree.footprintX = 1;
        deadTree.footprintZ = 1;
        auto deadTreeId = simA.featureDefinitions.insert(deadTree);
        simA.featureNameIndex.insert({"TREE2DEAD", deadTreeId});

        const auto pos = simA.terrain.heightmapIndexToWorldCenter(10, 10);
        simA.trySpawnFeature("Tree2Dead", pos, SimAngle(0));

        FeatureId featureId{0};
        unsigned int featureCount = 0;
        for (const auto& [id, f] : simA.features)
        {
            featureId = id;
            ++featureCount;
        }
        REQUIRE(featureCount == 1);

        const auto bytes = encodeSavedGame(simA, params, nullptr, "TREES", 0);
        auto decoded = decodeSavedGame(bytes);

        // the wire name is canonical (uppercase) in new saves
        const std::string upperName = "TREE2DEAD";
        REQUIRE(std::search(decoded.simulationBytes.begin(), decoded.simulationBytes.end(), upperName.begin(), upperName.end()) != decoded.simulationBytes.end());

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        simB.featureDefinitions.insert(deadTree);
        simB.featureNameIndex.insert({"TREE2DEAD", deadTreeId});
        applySavedGame(simB, decoded, nullptr);

        unsigned int restoredCount = 0;
        for (const auto& [id, f] : simB.features)
        {
            ++restoredCount;
            REQUIRE(id == featureId);
            REQUIRE(f.featureName == deadTreeId);
            REQUIRE((f.position == pos));
            REQUIRE((f.rotation == SimAngle(0)));
        }
        REQUIRE(restoredCount == 1);
        REQUIRE(simB.computeHash() == simA.computeHash());
    }

    TEST_CASE("SavedGame rejects a feature reference with no matching definition")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        FeatureDefinition deadTree;
        deadTree.name = "Tree2Dead";
        deadTree.footprintX = 1;
        deadTree.footprintZ = 1;
        auto deadTreeId = simA.featureDefinitions.insert(deadTree);
        simA.featureNameIndex.insert({"TREE2DEAD", deadTreeId});
        simA.trySpawnFeature("Tree2Dead", simA.terrain.heightmapIndexToWorldCenter(10, 10), SimAngle(0));

        const auto bytes = encodeSavedGame(simA, params, nullptr, "TREES", 0);

        SECTION("definition absent in the restore sim")
        {
            auto decoded = decodeSavedGame(bytes);
            PlayerId dummyA{0}, dummyB{0};
            auto simB = makeWorld(dummyA, dummyB);
            REQUIRE_THROWS_AS(applySavedGame(simB, decoded, nullptr), SaveFormatException);
        }

        SECTION("corrupted feature name in the save body")
        {
            auto decoded = decodeSavedGame(bytes);
            const std::string needle = "TREE2DEAD";
            auto it = std::search(decoded.simulationBytes.begin(), decoded.simulationBytes.end(), needle.begin(), needle.end());
            REQUIRE(it != decoded.simulationBytes.end());
            *it = 'X';
            PlayerId dummyA{0}, dummyB{0};
            auto simB = makeWorld(dummyA, dummyB);
            simB.featureDefinitions.insert(deadTree);
            simB.featureNameIndex.insert({"TREE2DEAD", deadTreeId});
            REQUIRE_THROWS_AS(applySavedGame(simB, decoded, nullptr), SaveFormatException);
        }
    }

    TEST_CASE("SavedGame rejects a weapon reference with no matching definition")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);
        auto params = makeParams();

        // armed unit: def weapon1 must resolve to a weaponDefinitions
        // entry for the slot to materialize
        auto armedTank = makeMobileDef("ARMGUNTANK");
        armedTank.weapon1 = "TESTGUN";
        simA.unitDefinitions.insert_or_assign("ARMGUNTANK", armedTank);
        simA.unitModelDefinitions.insert_or_assign("ARMGUNTANK", createUnitModelDefinition(0_ss, {}));
        simA.unitScriptDefinitions.insert_or_assign("ARMGUNTANK", CobScript{{}, {}, {}, 0});
        simA.weaponDefinitions.insert({"TESTGUN", WeaponDefinition{}});

        auto tank = simA.trySpawnUnit("ARMGUNTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(tank);
        REQUIRE(simA.getUnitState(*tank).weapons[0].has_value());

        const auto bytes = encodeSavedGame(simA, params, nullptr, "WEAP", 0);
        auto decoded = decodeSavedGame(bytes);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        auto armedTankB = makeMobileDef("ARMGUNTANK");
        armedTankB.weapon1 = "TESTGUN";
        simB.unitDefinitions.insert_or_assign("ARMGUNTANK", armedTankB);
        simB.unitModelDefinitions.insert_or_assign("ARMGUNTANK", createUnitModelDefinition(0_ss, {}));
        simB.unitScriptDefinitions.insert_or_assign("ARMGUNTANK", CobScript{{}, {}, {}, 0});
        // deliberately no weaponDefinitions entry: the save's weapon
        // reference is unresolvable and must fail cleanly
        REQUIRE_THROWS_AS(applySavedGame(simB, decoded, nullptr), SaveFormatException);
    }

    /**
     * Proves the real stock definition resolves: parses every feature
     * TDF in the retail data (no bytes are copied into the
     * repo), registers the [Tree2] chain through the same
     * loadFeatureDefinitions path the loader uses, and round-trips a
     * [Tree2Dead] feature instance. Skips without RWE_DATA_PATH.
     */
    TEST_CASE("retail Tree2Dead definition resolves through save restore")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));

        std::unordered_map<std::string, FeatureTdf> featureTdfs;
        for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
        {
            auto bytes = vfs.readFile("features/" + name);
            REQUIRE(bytes);
            auto tdfRoot = parseTdfFromString(std::string(bytes->data(), bytes->size()));
            for (const auto& e : tdfRoot.blocks)
            {
                featureTdfs.insert({toUpper(e.first), parseFeatureTdf(*e.second)});
            }
        }

        // the stock definition exists and is authored in mixed case
        REQUIRE(featureTdfs.count("TREE2DEAD") == 1);
        bool authoredMixedCase = false;
        for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
        {
            auto bytes = vfs.readFile("features/" + name);
            auto tdfRoot = parseTdfFromString(std::string(bytes->data(), bytes->size()));
            for (const auto& e : tdfRoot.blocks)
            {
                if (e.first == "Tree2Dead")
                {
                    authoredMixedCase = true;
                }
            }
        }
        REQUIRE(authoredMixedCase);

        auto buildWorld = [&](PlayerId& a, PlayerId& b) {
            auto sim = makeSim();
            // the same seed set the real loader computes for a map
            // that places Tree2: TNT features + FBI corpses, closed
            // under featuredead/featureburnt/featurereclamate
            loadFeatureDefinitions(featureTdfs, sim.featureDefinitions, sim.featureNameIndex, "Tree2");
            a = sim.addPlayer(makePlayer(PlayerColorIndex(0), "ARM"));
            b = sim.addPlayer(makePlayer(PlayerColorIndex(1), "CORE", GamePlayerType::Computer));
            return sim;
        };

        PlayerId playerA{0}, playerB{0};
        auto simA = buildWorld(playerA, playerB);
        auto params = makeParams();

        // the def was reached via the authored-case featureburnt value,
        // so its recorded name keeps that case
        auto deadId = simA.featureNameIndex.at("TREE2DEAD");
        REQUIRE(simA.featureDefinitions.get(deadId).name == "Tree2Dead");

        simA.trySpawnFeature("Tree2Dead", simA.terrain.heightmapIndexToWorldCenter(10, 10), SimAngle(0));

        const auto bytes = encodeSavedGame(simA, params, nullptr, "TREES", 0);
        auto decoded = decodeSavedGame(bytes);

        PlayerId dummyA{0}, dummyB{0};
        auto simB = buildWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);

        unsigned int restoredCount = 0;
        for (const auto& [id, f] : simB.features)
        {
            ++restoredCount;
            REQUIRE(f.featureName == simB.featureNameIndex.at("TREE2DEAD"));
        }
        REQUIRE(restoredCount == 1);
        REQUIRE(simB.computeHash() == simA.computeHash());
    }

    /**
     * Applies a real save file against definitions rebuilt from retail
     * data, mirroring what LoadingScene's restore mode produces without
     * the GL/audio stages. Set RWE_DATA_PATH to a TA data directory and
     * RWE_TEST_SAVE_FILE to a save (e.g. ~/.rwe/savegame/TA.sav) to run.
     */
    TEST_CASE("an existing save file applies against freshly loaded definitions")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        const char* savePathEnv = std::getenv("RWE_TEST_SAVE_FILE");
        if (dataPathEnv == nullptr || savePathEnv == nullptr
            || !std::filesystem::exists(std::filesystem::path(dataPathEnv))
            || !std::filesystem::exists(std::filesystem::path(savePathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        std::ifstream saveFile(savePathEnv, std::ios::binary);
        std::vector<char> fileBytes((std::istreambuf_iterator<char>(saveFile)), std::istreambuf_iterator<char>());
        auto decoded = decodeSavedGame(fileBytes);

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));

        // terrain + feature seed names from the save's map
        auto tntBytes = vfs.readFile("maps/" + decoded.meta.mapName + ".tnt");
        REQUIRE(tntBytes);
        SpanStream tntStream(tntBytes->data(), tntBytes->size());
        TntArchive tnt(&tntStream);
        Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
        tnt.readMapAttributes(mapAttributes.getData());
        auto heightGrid = getHeightGrid(mapAttributes);
        auto featureNames = getFeatureNames(tnt);

        auto otaBytes = vfs.readFile("maps/" + decoded.meta.mapName + ".ota");
        REQUIRE(otaBytes);
        auto ota = parseOta(parseTdfFromString(std::string(otaBytes->data(), otaBytes->size())));

        auto surfaceMetal = ota.schemas.empty() ? 0 : ota.schemas.front().surfaceMetal;
        if (decoded.meta.schemaIndex < ota.schemas.size())
        {
            surfaceMetal = ota.schemas[decoded.meta.schemaIndex].surfaceMetal;
        }
        GameSimulation sim(
            MapTerrain(std::move(heightGrid), SimScalar(tnt.getHeader().seaLevel)),
            static_cast<unsigned char>(surfaceMetal),
            ota.minWindSpeed,
            ota.maxWindSpeed,
            ota.tidalStrength);

        // movement classes + unit definitions (models: geometry only,
        // no GL upload)
        {
            auto moveBytes = vfs.readFile("gamedata/MOVEINFO.TDF");
            REQUIRE(moveBytes);
            for (const auto& c : parseMoveInfoTdf(parseTdfFromString(std::string(moveBytes->data(), moveBytes->size()))))
            {
                sim.movementClassDatabase.registerMovementClass(parseMovementClassDefinition(c.second));
            }
        }
        std::unordered_set<std::string> requiredFeatures;
        for (const auto& fbiName : vfs.getFileNames("units", ".fbi"))
        {
            auto bytes = vfs.readFile("units/" + fbiName);
            REQUIRE(bytes);
            auto fbi = parseUnitFbi(parseTdfFromString(std::string(bytes->data(), bytes->size())));
            auto unitDefinition = parseUnitDefinition(fbi, sim.movementClassDatabase);
            sim.unitDefinitions.insert({toUpper(fbi.unitName), std::move(unitDefinition)});

            auto objBytes = vfs.readFile("objects3d/" + fbi.objectName + ".3do");
            REQUIRE(objBytes);
            SpanStream objStream(objBytes->data(), objBytes->size());
            auto objects = parse3doObjects(objStream, objStream.tellg());
            REQUIRE(objects.size() == 1);
            sim.unitModelDefinitions.insert({toUpper(fbi.objectName),
                createUnitModelDefinition(simScalarFromFixed(findHighestVertex(objects.front()).y), unitMeshFrom3do(objects.front()))});

            if (!fbi.corpse.empty())
            {
                requiredFeatures.insert(toUpper(fbi.corpse));
            }
        }
        sim.unitScriptDefinitions = loadCobScripts(vfs);

        // weapon definitions (names only matter to restore)
        for (const auto& fileName : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + fileName);
            REQUIRE(bytes);
            for (const auto& pair : parseWeaponTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
            {
                sim.weaponDefinitions.insert({toUpper(pair.first), parseWeaponDefinition(pair.second)});
            }
        }

        // feature definitions: the same seed set production computes
        // (TNT + schema features + unit corpses) closed under the
        // featuredead/featureburnt/featurereclamate chains
        mapAttributes.forEachIndexed([&](auto, const auto& e) {
            if (e.feature != TntTileAttributes::FeatureNone
                && e.feature != TntTileAttributes::FeatureUnknown
                && e.feature != TntTileAttributes::FeatureVoid)
            {
                requiredFeatures.insert(toUpper(featureNames.at(e.feature)));
            }
        });
        for (const auto& schema : ota.schemas)
        {
            for (const auto& f : schema.features)
            {
                requiredFeatures.insert(toUpper(f.featureName));
            }
        }

        std::unordered_map<std::string, FeatureTdf> featureTdfs;
        for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
        {
            auto bytes = vfs.readFile("features/" + name);
            REQUIRE(bytes);
            auto tdfRoot = parseTdfFromString(std::string(bytes->data(), bytes->size()));
            for (const auto& e : tdfRoot.blocks)
            {
                featureTdfs.insert({toUpper(e.first), parseFeatureTdf(*e.second)});
            }
        }
        for (const auto& name : requiredFeatures)
        {
            if (featureTdfs.count(name) != 0)
            {
                loadFeatureDefinitions(featureTdfs, sim.featureDefinitions, sim.featureNameIndex, name);
            }
        }

        CampaignProgress progress;
        REQUIRE_NOTHROW(applySavedGame(sim, decoded, &progress));

        unsigned int unitCount = 0;
        for (const auto& [id, u] : sim.units)
        {
            (void)u;
            ++unitCount;
        }
        REQUIRE(unitCount > 0);

        // campaign saves carry a thumbs block; a save taken before the
        // first result stamp legitimately holds an empty status array
        if (decoded.meta.isCampaign)
        {
            auto run = progress.findRun(decoded.meta.campaignName, decoded.meta.campaignSide);
            REQUIRE(run != nullptr);
            if (decoded.meta.missionIndex < run->status.size())
            {
                REQUIRE((run->status[decoded.meta.missionIndex] == CampaignMissionStatus::Won
                    || run->status[decoded.meta.missionIndex] == CampaignMissionStatus::Lost
                    || run->status[decoded.meta.missionIndex] == CampaignMissionStatus::Unplayed));
            }
        }
    }

    TEST_CASE("SavedGame round-trip preserves skirmish settings and teams")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto simA = makeWorld(playerA, playerB);

        // skirmish frontend state: Random starts, Continues mode,
        // mapped LOS, Hard difficulty -- all must survive the load
        auto params = makeParams();
        params.skirmish.startLocation = SkirmishStartLocation::Random;
        params.skirmish.commanderDeath = SkirmishCommanderDeath::Continues;
        params.skirmish.mapping = SkirmishMapping::Mapped;
        params.skirmish.lineOfSight = SkirmishLineOfSight::Circular;
        params.skirmish.difficulty = SkirmishDifficulty::Hard;
        params.players[1]->allyGroup = 2;

        simA.commanderDeathContinues = true;
        // a non-default difficulty economy coefficient -- the v5 sim
        // body is the source of truth for it, and GameHash must see
        // the same value after the round-trip
        simA.computerResourceScale = 0.5f;
        simA.lineOfSightEnabled = true;
        simA.lineOfSightMode = static_cast<int>(SkirmishLineOfSight::Circular);
        simA.mappingEnabled = true;
        simA.getPlayer(playerB).allyGroup = 2;

        const auto hashBefore = simA.computeHash();

        const auto bytes = encodeSavedGame(simA, params, nullptr, "SKIRMISH", 0);
        const auto meta = readSaveMetadata(bytes);
        REQUIRE(meta.commanderDeath);
        REQUIRE(meta.skirmishStartLocation == std::optional<unsigned int>(static_cast<unsigned int>(SkirmishStartLocation::Random)));
        REQUIRE(meta.skirmishMapping == std::optional<unsigned int>(static_cast<unsigned int>(SkirmishMapping::Mapped)));
        REQUIRE(meta.skirmishLineOfSight == std::optional<unsigned int>(static_cast<unsigned int>(SkirmishLineOfSight::Circular)));
        REQUIRE(meta.skirmishDifficulty == std::optional<unsigned int>(static_cast<unsigned int>(SkirmishDifficulty::Hard)));
        REQUIRE(meta.players[1].allyGroup == std::optional<unsigned int>(2));

        auto decoded = decodeSavedGame(bytes);
        auto paramsRoundTrip = gameParametersFromSave(decoded);
        REQUIRE(paramsRoundTrip.skirmish.startLocation == SkirmishStartLocation::Random);
        REQUIRE(paramsRoundTrip.skirmish.commanderDeath == SkirmishCommanderDeath::Continues);
        REQUIRE(paramsRoundTrip.skirmish.mapping == SkirmishMapping::Mapped);
        REQUIRE(paramsRoundTrip.skirmish.lineOfSight == SkirmishLineOfSight::Circular);
        REQUIRE(paramsRoundTrip.skirmish.difficulty == SkirmishDifficulty::Hard);
        REQUIRE(paramsRoundTrip.players[1]->allyGroup == std::optional<unsigned int>(2));

        PlayerId dummyA{0}, dummyB{0};
        auto simB = makeWorld(dummyA, dummyB);
        applySavedGame(simB, decoded, nullptr);
        REQUIRE(simB.commanderDeathContinues);
        REQUIRE(simB.computerResourceScale == 0.5f);
        REQUIRE(simB.getPlayer(playerB).allyGroup == std::optional<unsigned int>(2));

        // save -> load restores the full hash-covered state,
        // computerResourceScale included
        REQUIRE(simB.computeHash() == hashBefore);
    }
#include "Phase8bSaveHash.inc"
#include "Phase8cHashRegression.inc"

}
