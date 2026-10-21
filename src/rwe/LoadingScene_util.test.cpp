#include <catch2/catch_test_macros.hpp>

#include <variant>
#include <set>

#include <rwe/LoadingScene_util.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/util.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0);
        }

        GameSimulation makeSim(int minWindSpeed, int maxWindSpeed, int tidalStrength)
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, minWindSpeed, maxWindSpeed, tidalStrength);
        }

        UnitDefinition makeDefinition(const std::string& name)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 1000;
            def.buildTime = 100;
            return def;
        }

        OtaUnit makeOtaUnit(const std::string& name, unsigned int player, int x, int z)
        {
            OtaUnit u{};
            u.unitName = name;
            u.player = player;
            u.xPos = x;
            u.yPos = 0;
            u.zPos = z;
            u.healthPercentage = 100;
            u.angle = 0;
            return u;
        }
    }

    TEST_CASE("parseUnitDefinition cloak fields")
    {
        MovementClassDatabase movementClassDatabase;

        SECTION("the FBI cloak block propagates to the unit definition")
        {
            UnitFbi fbi;
            fbi.name = "ARMCOM";
            // footprintX/Z have no default initializers -- leaving
            // them unset feeds garbage to parseYardMap
            fbi.footprintX = 2;
            fbi.footprintZ = 2;
            fbi.cloakCost = 200.0f;
            fbi.cloakCostMoving = 1000.0f;
            fbi.minCloakDistance = 40;
            fbi.initCloaked = true;

            auto def = parseUnitDefinition(fbi, movementClassDatabase);

            REQUIRE(def.cloakCost == 200.0f);
            REQUIRE(def.cloakCostMoving == 1000.0f);
            REQUIRE(def.minCloakDistance == 40_ss);
            REQUIRE(def.initCloaked);
        }

        SECTION("a plain unit has no cloak capability")
        {
            // retail derives the cloak bit from CloakCost > 0
            // (unitdef +0x245 bit13) -- there is no dedicated key
            UnitFbi fbi;
            fbi.name = "ARMPW";
            fbi.footprintX = 2;
            fbi.footprintZ = 2;

            auto def = parseUnitDefinition(fbi, movementClassDatabase);

            REQUIRE(def.cloakCost == 0.0f);
            REQUIRE(!def.initCloaked);
        }
    }

    TEST_CASE("parseUnitDefinition sensor fields")
    {
        MovementClassDatabase movementClassDatabase;

        SECTION("the FBI sensor block propagates to the unit definition")
        {
            UnitFbi fbi;
            fbi.name = "ARMSJAM";
            fbi.footprintX = 2;
            fbi.footprintZ = 2;
            fbi.sightDistance = 300;
            fbi.radarDistance = 1440;
            fbi.sonarDistance = 1180;
            fbi.radarDistanceJam = 700;
            fbi.sonarDistanceJam = 650;
            fbi.stealth = true;

            auto def = parseUnitDefinition(fbi, movementClassDatabase);

            REQUIRE(def.sightDistance == 300_ss);
            REQUIRE(def.radarDistance == 1440_ss);
            REQUIRE(def.sonarDistance == 1180_ss);
            REQUIRE(def.radarDistanceJam == 700_ss);
            REQUIRE(def.sonarDistanceJam == 650_ss);
            REQUIRE(def.stealth);
        }

        SECTION("a plain unit has no sensor-jam or stealth properties")
        {
            UnitFbi fbi;
            fbi.name = "ARMPW";
            fbi.footprintX = 2;
            fbi.footprintZ = 2;

            auto def = parseUnitDefinition(fbi, movementClassDatabase);

            REQUIRE(def.radarDistanceJam == 0_ss);
            REQUIRE(def.sonarDistanceJam == 0_ss);
            REQUIRE(!def.stealth);
        }
    }

    TEST_CASE("findSchemaIndexByType")
    {
        OtaRecord ota{};
        OtaSchema easy{};
        easy.type = "Easy";
        OtaSchema medium{};
        medium.type = "Medium";
        ota.schemas = {easy, medium};

        SECTION("finds the matching schema")
        {
            REQUIRE(findSchemaIndexByType(ota, "Medium") == 1);
            REQUIRE(findSchemaIndexByType(ota, "Easy") == 0);
        }

        SECTION("matches case-insensitively")
        {
            REQUIRE(findSchemaIndexByType(ota, "medium") == 1);
        }

        SECTION("throws when no schema matches")
        {
            REQUIRE_THROWS(findSchemaIndexByType(ota, "Impossible"));
        }
    }

    TEST_CASE("findCampaignSchemaIndex")
    {
        OtaRecord ota{};
        OtaSchema easy{};
        easy.type = "Easy";
        OtaSchema medium{};
        medium.type = "Medium";
        OtaSchema hard{};
        hard.type = "Hard";

        SECTION("staged difficulty resolves directly")
        {
            ota.schemas = {easy, medium, hard};
            REQUIRE(findCampaignSchemaIndex(ota, "Easy") == 0);
            REQUIRE(findCampaignSchemaIndex(ota, "Medium") == 1);
            REQUIRE(findCampaignSchemaIndex(ota, "Hard") == 2);
        }

        SECTION("Krogoth Encounter: staged Easy/Medium resolve to the only Hard schema")
        {
            ota.schemas = {hard};
            REQUIRE(findCampaignSchemaIndex(ota, "Easy") == 0);
            REQUIRE(findCampaignSchemaIndex(ota, "Medium") == 0);
            REQUIRE(findCampaignSchemaIndex(ota, "Hard") == 0);
        }

        SECTION("staged Easy falls back Medium then Hard, not file order")
        {
            // retail 0x436860 mode 1: staged Easy -> Easy, Medium, Hard;
            // Hard precedes Medium in the file but Medium must win
            ota.schemas = {hard, medium};
            REQUIRE(findCampaignSchemaIndex(ota, "Easy") == 1);
        }

        SECTION("staged Medium falls back Easy then Hard, not file order")
        {
            // retail: staged Medium -> Medium, Easy, Hard;
            // Hard precedes Easy in the file but Easy must win
            ota.schemas = {hard, easy};
            REQUIRE(findCampaignSchemaIndex(ota, "Medium") == 1);
        }

        SECTION("staged Hard falls back Medium then Easy, not file order")
        {
            // retail: staged Hard -> Hard, Medium, Easy;
            // Easy precedes Medium in the file but Medium must win
            ota.schemas = {easy, medium};
            REQUIRE(findCampaignSchemaIndex(ota, "Hard") == 1);
        }

        SECTION("matches case-insensitively")
        {
            ota.schemas = {hard};
            REQUIRE(findCampaignSchemaIndex(ota, "hard") == 0);
        }

        SECTION("throws when no Easy/Medium/Hard schema exists")
        {
            OtaSchema network{};
            network.type = "Network 1";
            ota.schemas = {network};
            REQUIRE_THROWS(findCampaignSchemaIndex(ota, "Easy"));
        }
    }

    TEST_CASE("buildCampaignPlayers")
    {
        OtaSchema schema{};
        schema.humanMetal = 5000;
        schema.humanEnergy = 6000;
        schema.computerMetal = 7000;
        schema.computerEnergy = 8000;
        schema.units = {
            makeOtaUnit("PLAYERUNIT", 1, 100, 100),
            makeOtaUnit("ENEMYUNIT", 2, 200, 200),
            makeOtaUnit("ENEMYUNIT2", 2, 300, 300),
        };

        auto players = buildCampaignPlayers(schema, "ARM", "CORE");

        SECTION("creates one player per mission player number")
        {
            REQUIRE(players.size() == 2);
        }

        SECTION("mission player 1 is the human side")
        {
            REQUIRE(players[0].type == GamePlayerType::Human);
            REQUIRE(players[0].side == "ARM");
        }

        SECTION("other mission players are computers on the enemy side")
        {
            REQUIRE(players[1].type == GamePlayerType::Computer);
            REQUIRE(players[1].side == "CORE");
        }

        SECTION("applies mission economy, not skirmish defaults")
        {
            REQUIRE(players[0].metal == Metal(5000));
            REQUIRE(players[0].energy == Energy(6000));
            REQUIRE(players[0].startingMetal == Metal(5000));
            REQUIRE(players[1].metal == Metal(7000));
            REQUIRE(players[1].energy == Energy(8000));
        }
    }

    TEST_CASE("spawnMissionUnits")
    {
        auto sim = makeSim();
        sim.unitDefinitions.insert_or_assign("FRIEND", makeDefinition("FRIEND"));
        sim.unitDefinitions.insert_or_assign("FOE", makeDefinition("FOE"));
        sim.unitModelDefinitions.insert_or_assign("FRIEND", createUnitModelDefinition(0_ss, {}));
        sim.unitModelDefinitions.insert_or_assign("FOE", createUnitModelDefinition(0_ss, {}));
        sim.unitScriptDefinitions.insert_or_assign("FRIEND", CobScript{{}, {}, {}, 0});
        sim.unitScriptDefinitions.insert_or_assign("FOE", CobScript{{}, {}, {}, 0});

        auto makePlayerInfo = [](GamePlayerType type, unsigned int colorIndex) {
            return GamePlayerInfo{
                std::nullopt,
                type,
                PlayerColorIndex(colorIndex),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0)};
        };
        std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(GamePlayerType::Human, 0)), sim.addPlayer(makePlayerInfo(GamePlayerType::Computer, 1))};

        SECTION("spawns each unit for its owning mission player")
        {
            auto friend_ = makeOtaUnit("FRIEND", 1, 500, 600);
            auto foe = makeOtaUnit("FOE", 2, 700, 800);

            auto stats = spawnMissionUnits(sim, {friend_, foe}, missionPlayers);

            REQUIRE(stats.spawned == 2);
            REQUIRE(stats.failures.empty());

            unsigned int unitCount = 0;
            for (const auto& [id, unit] : sim.units)
            {
                (void)id;
                ++unitCount;
            }
            REQUIRE(unitCount == 2);

            auto expected = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(500), 0_ss, SimScalar(600)));
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.unitType == "FRIEND")
                {
                    REQUIRE(unit.owner == missionPlayers[0]);
                    REQUIRE(unit.position.x == expected.x);
                    REQUIRE(unit.position.z == expected.z);
                }
                else
                {
                    REQUIRE(unit.owner == missionPlayers[1]);
                }

                // spawned as a completed unit, not a nanoframe
                REQUIRE(!unit.isBeingBuilt(sim.unitDefinitions.at(unit.unitType)));
            }
        }

        SECTION("does not synthesize a commander")
        {
            auto stats = spawnMissionUnits(sim, {makeOtaUnit("FRIEND", 1, 500, 600)}, missionPlayers);

            REQUIRE(stats.spawned == 1);
            for (const auto& [id, unit] : sim.units)
            {
                REQUIRE(unit.unitType == "FRIEND");
            }
        }

        SECTION("applies health percentage")
        {
            auto damaged = makeOtaUnit("FOE", 2, 700, 800);
            damaged.healthPercentage = 50;

            auto stats = spawnMissionUnits(sim, {damaged}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            const auto& unit = sim.units.begin()->second;
            REQUIRE(unit.hitPoints == 500);
        }

        SECTION("retains InitialMission verbatim without executing it")
        {
            auto scripted = makeOtaUnit("FOE", 2, 700, 800);
            scripted.initialMission = "p 1750 1828";
            scripted.immunity = true;
            scripted.aiIgnore = true;

            auto stats = spawnMissionUnits(sim, {scripted}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            const auto& unit = sim.units.begin()->second;
            REQUIRE(unit.missionState);
            REQUIRE(unit.missionState->initialMission == "p 1750 1828");
            REQUIRE(unit.missionState->immunity);
            REQUIRE(unit.missionState->aiIgnore);

            // retained as data: no orders were queued
            REQUIRE(unit.orders.empty());
        }

        SECTION("missing unit definitions fail safely")
        {
            auto missing = makeOtaUnit("NOSUCHUNIT", 1, 500, 600);
            auto ok = makeOtaUnit("FRIEND", 1, 550, 600);

            auto stats = spawnMissionUnits(sim, {missing, ok}, missionPlayers);

            REQUIRE(stats.spawned == 1);
            REQUIRE(stats.failures.size() == 1);
            REQUIRE(stats.failures[0].find("NOSUCHUNIT") != std::string::npos);
        }

        SECTION("mission player numbers without a slot fail safely")
        {
            auto orphan = makeOtaUnit("FRIEND", 9, 500, 600);

            auto stats = spawnMissionUnits(sim, {orphan}, missionPlayers);

            REQUIRE(stats.spawned == 0);
            REQUIRE(stats.failures.size() == 1);
        }

        SECTION("activates units that activate when built")
        {
            auto& def = sim.unitDefinitions.at("FRIEND");
            def.activateWhenBuilt = true;

            auto stats = spawnMissionUnits(sim, {makeOtaUnit("FRIEND", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            REQUIRE(sim.units.begin()->second.activated);
        }

        SECTION("leaves units without ActivateWhenBuilt deactivated")
        {
            auto stats = spawnMissionUnits(sim, {makeOtaUnit("FRIEND", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            REQUIRE(!sim.units.begin()->second.activated);
        }
    }

    TEST_CASE("mission unit economy")
    {
        auto makePlayerInfo = [](unsigned int colorIndex) {
            return GamePlayerInfo{
                std::nullopt,
                GamePlayerType::Human,
                PlayerColorIndex(colorIndex),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0)};
        };

        auto makeStorageDefinition = [](const std::string& name) {
            auto def = makeDefinition(name);
            def.isMobile = false;
            def.yardMap = Grid<YardMapCell>(4, 4, YardMapCell{});
            def.energyStorage = Energy(100000);
            def.metalStorage = Metal(100000);
            return def;
        };

        SECTION("a pre-placed solar produces energy")
        {
            auto sim = makeSim();
            // solar model: production via negative energy use when activated
            auto def = makeStorageDefinition("SOLAR");
            def.activateWhenBuilt = true;
            def.energyUse = Energy(-20);
            sim.unitDefinitions.insert_or_assign("SOLAR", def);
            sim.unitModelDefinitions.insert_or_assign("SOLAR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SOLAR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            auto stats = spawnMissionUnits(sim, {makeOtaUnit("SOLAR", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            // production settles one round after it is buffered
            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(20));
        }

        SECTION("a pre-placed solar without activation produces nothing")
        {
            auto sim = makeSim();
            auto def = makeStorageDefinition("SOLAR");
            def.energyUse = Energy(-20);
            sim.unitDefinitions.insert_or_assign("SOLAR", def);
            sim.unitModelDefinitions.insert_or_assign("SOLAR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SOLAR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("SOLAR", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(0));
        }

        SECTION("a pre-placed tidal generator produces the map's tidal strength")
        {
            auto sim = makeSim(0, 0, 42);
            auto def = makeStorageDefinition("TIDE");
            def.activateWhenBuilt = true;
            def.tidalGenerator = true;
            sim.unitDefinitions.insert_or_assign("TIDE", def);
            sim.unitModelDefinitions.insert_or_assign("TIDE", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("TIDE", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("TIDE", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(42));
        }

        SECTION("a deactivated tidal generator produces nothing")
        {
            auto sim = makeSim(0, 0, 42);
            auto def = makeStorageDefinition("TIDE");
            def.tidalGenerator = true;
            sim.unitDefinitions.insert_or_assign("TIDE", def);
            sim.unitModelDefinitions.insert_or_assign("TIDE", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("TIDE", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("TIDE", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(0));
        }

        SECTION("a pre-placed wind generator produces from the map's wind speed")
        {
            auto sim = makeSim(MaxUtilizableWindSpeed, MaxUtilizableWindSpeed, 0);
            auto def = makeStorageDefinition("WIND");
            def.activateWhenBuilt = true;
            def.windGenerator = Energy(30);
            sim.unitDefinitions.insert_or_assign("WIND", def);
            sim.unitModelDefinitions.insert_or_assign("WIND", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("WIND", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("WIND", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(30));
        }

        SECTION("a pre-placed metal maker produces metal")
        {
            auto sim = makeSim();
            auto def = makeStorageDefinition("MAKR");
            def.activateWhenBuilt = true;
            def.makesMetal = Metal(1);
            sim.unitDefinitions.insert_or_assign("MAKR", def);
            sim.unitModelDefinitions.insert_or_assign("MAKR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("MAKR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("MAKR", 1, 500, 600)}, missionPlayers);

            // the first settle only establishes isSufficientlyPowered, and
            // buffered production lands a settle later
            for (int i = 0; i < 3 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].metal == Metal(1));
        }

        SECTION("commander passive production does not require activation")
        {
            auto sim = makeSim();
            auto def = makeStorageDefinition("COM");
            def.commander = true;
            def.energyMake = Energy(25);
            def.metalMake = Metal(1);
            sim.unitDefinitions.insert_or_assign("COM", def);
            sim.unitModelDefinitions.insert_or_assign("COM", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("COM", CobScript{{}, {}, {}, 0});
            auto player = makePlayerInfo(0);
            player.startingEnergy = Energy(1000);
            player.startingMetal = Metal(1000);
            std::vector<PlayerId> missionPlayers{sim.addPlayer(player)};

            spawnMissionUnits(sim, {makeOtaUnit("COM", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(25));
            REQUIRE(sim.players[0].metal == Metal(1));
        }

        SECTION("storage contributes to the player's maximum")
        {
            auto sim = makeSim();
            auto def = makeStorageDefinition("ESTOR");
            def.energyStorage = Energy(6000);
            def.metalStorage = Metal(3000);
            sim.unitDefinitions.insert_or_assign("ESTOR", def);
            sim.unitModelDefinitions.insert_or_assign("ESTOR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("ESTOR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0))};

            spawnMissionUnits(sim, {makeOtaUnit("ESTOR", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].maxEnergy == Energy(6000));
            REQUIRE(sim.players[0].maxMetal == Metal(3000));
        }

        SECTION("starting resources are not reapplied every tick")
        {
            auto sim = makeSim();
            auto def = makeStorageDefinition("ESTOR");
            def.energyStorage = Energy(10000);
            def.metalStorage = Metal(10000);
            sim.unitDefinitions.insert_or_assign("ESTOR", def);
            sim.unitModelDefinitions.insert_or_assign("ESTOR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("ESTOR", CobScript{{}, {}, {}, 0});
            auto player = makePlayerInfo(0);
            player.metal = Metal(500);
            player.energy = Energy(500);
            player.startingMetal = Metal(5000);
            player.startingEnergy = Energy(5000);
            std::vector<PlayerId> missionPlayers{sim.addPlayer(player)};

            spawnMissionUnits(sim, {makeOtaUnit("ESTOR", 1, 500, 600)}, missionPlayers);

            for (int i = 0; i < SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].metal == Metal(500));
            REQUIRE(sim.players[0].energy == Energy(500));
        }
    }

    TEST_CASE("campaign difficulty computer economy scaling")
    {
        auto makePlayerInfo = [](unsigned int colorIndex, GamePlayerType type) {
            return GamePlayerInfo{
                std::nullopt,
                type,
                PlayerColorIndex(colorIndex),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0)};
        };

        auto makeStorageDefinition = [](const std::string& name) {
            auto def = makeDefinition(name);
            def.isMobile = false;
            def.yardMap = Grid<YardMapCell>(4, 4, YardMapCell{});
            def.energyStorage = Energy(100000);
            def.metalStorage = Metal(100000);
            return def;
        };

        SECTION("staged difficulty maps to the retail coefficient")
        {
            // TotalA.exe ctx+0x37eee: 0->x0.5, 1->x0.7, other->x1.0
            REQUIRE(campaignDifficultyComputerResourceScale("Easy") == 0.5f);
            REQUIRE(campaignDifficultyComputerResourceScale("Medium") == 0.7f);
            REQUIRE(campaignDifficultyComputerResourceScale("Hard") == 1.0f);
            REQUIRE(campaignDifficultyComputerResourceScale("EASY") == 0.5f);
            REQUIRE(campaignDifficultyComputerResourceScale("unrecognized") == 1.0f);
        }

        SECTION("a computer-owned producer's income is scaled")
        {
            auto sim = makeSim();
            sim.computerResourceScale = 0.5f;
            auto def = makeStorageDefinition("SOLAR");
            def.activateWhenBuilt = true;
            def.energyUse = Energy(-20);
            sim.unitDefinitions.insert_or_assign("SOLAR", def);
            sim.unitModelDefinitions.insert_or_assign("SOLAR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SOLAR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0, GamePlayerType::Computer))};

            auto stats = spawnMissionUnits(sim, {makeOtaUnit("SOLAR", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            // x0.5 of the human-equivalent 20
            REQUIRE(sim.players[0].energy == Energy(10));
        }

        SECTION("a human-owned producer's income is unscaled")
        {
            auto sim = makeSim();
            sim.computerResourceScale = 0.5f;
            auto def = makeStorageDefinition("SOLAR");
            def.activateWhenBuilt = true;
            def.energyUse = Energy(-20);
            sim.unitDefinitions.insert_or_assign("SOLAR", def);
            sim.unitModelDefinitions.insert_or_assign("SOLAR", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SOLAR", CobScript{{}, {}, {}, 0});
            std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(0, GamePlayerType::Human))};

            auto stats = spawnMissionUnits(sim, {makeOtaUnit("SOLAR", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            REQUIRE(sim.players[0].energy == Energy(20));
        }

        SECTION("a computer owner's positive use cost is unscaled")
        {
            auto sim = makeSim();
            sim.computerResourceScale = 0.5f;
            auto def = makeStorageDefinition("DRAIN");
            def.activateWhenBuilt = true;
            def.energyUse = Energy(10);
            sim.unitDefinitions.insert_or_assign("DRAIN", def);
            sim.unitModelDefinitions.insert_or_assign("DRAIN", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("DRAIN", CobScript{{}, {}, {}, 0});
            auto player = makePlayerInfo(0, GamePlayerType::Computer);
            player.energy = Energy(1000);
            std::vector<PlayerId> missionPlayers{sim.addPlayer(player)};

            auto stats = spawnMissionUnits(sim, {makeOtaUnit("DRAIN", 1, 500, 600)}, missionPlayers);
            REQUIRE(stats.spawned == 1);

            for (int i = 0; i < 2 * SimTicksPerSecond; ++i)
            {
                sim.tick();
            }

            // positive use costs route through unscaled accumulators
            REQUIRE(sim.players[0].energy == Energy(990));
        }
    }

    TEST_CASE("campaign difficulty computer starting grant")
    {
        // real init path: OtaSchema -> buildCampaignPlayers ->
        // addCampaignPlayer. LoadingScene assigns
        // computerResourceScale before building players; retail
        // credits the computer starting grant through the
        // difficulty-scaled production accumulators (TotalA.exe
        // 0x465414/0x4654af) while the storage floor stays unscaled.
        OtaSchema schema{};
        schema.humanMetal = 1000;
        schema.humanEnergy = 1000;
        schema.computerMetal = 1000;
        schema.computerEnergy = 1000;
        schema.units = {
            makeOtaUnit("PLAYERUNIT", 1, 100, 100),
            makeOtaUnit("ENEMYUNIT", 2, 200, 200),
        };

        SECTION("the computer grant scales by difficulty; the human grant and storage floor do not")
        {
            for (const auto& [difficulty, expected] : std::vector<std::pair<std::string, unsigned int>>{
                     {"Easy", 500}, {"Medium", 700}, {"Hard", 1000}})
            {
                CAPTURE(difficulty);
                auto sim = makeSim();
                sim.computerResourceScale = campaignDifficultyComputerResourceScale(difficulty);
                auto players = buildCampaignPlayers(schema, "ARM", "CORE");
                auto humanId = addCampaignPlayer(sim, players[0]);
                auto computerId = addCampaignPlayer(sim, players[1]);

                REQUIRE(sim.getPlayer(computerId).metal == Metal(expected));
                REQUIRE(sim.getPlayer(computerId).energy == Energy(expected));
                REQUIRE(sim.getPlayer(humanId).metal == Metal(1000));
                REQUIRE(sim.getPlayer(humanId).energy == Energy(1000));

                // storage capacity and the starting-resource storage
                // floor stay unscaled at the authored grant
                REQUIRE(sim.getPlayer(computerId).maxMetal == Metal(1000));
                REQUIRE(sim.getPlayer(computerId).maxEnergy == Energy(1000));
                REQUIRE(sim.getPlayer(computerId).startingMetal == Metal(1000));
                REQUIRE(sim.getPlayer(computerId).startingEnergy == Energy(1000));
            }
        }
    }

    TEST_CASE("campaign mode suppresses skirmish victory")
    {
        auto sim = makeSim();

        GamePlayerInfo human{
            std::nullopt,
            GamePlayerType::Human,
            PlayerColorIndex(0),
            GamePlayerStatus::Alive,
            "ARM",
            std::nullopt,
            Metal(0),
            Energy(0),
            Metal(0),
            Energy(0),
            Metal(0),
            Energy(0)};
        sim.addPlayer(human);

        SECTION("single alive player is not a win in campaign mode")
        {
            sim.missionInfo = MissionInfo{};

            auto status = sim.computeWinStatus();
            REQUIRE(std::holds_alternative<WinStatusUndecided>(status));
        }

        SECTION("single alive player is a win in skirmish mode")
        {
            auto status = sim.computeWinStatus();
            REQUIRE(std::holds_alternative<WinStatusWon>(status));
        }
    }

    TEST_CASE("installInitialMissionOrders")
    {
        auto sim = makeSim();
        sim.unitDefinitions.insert_or_assign("FRIEND", makeDefinition("FRIEND"));
        sim.unitDefinitions.insert_or_assign("FOE", makeDefinition("FOE"));
        sim.unitModelDefinitions.insert_or_assign("FRIEND", createUnitModelDefinition(0_ss, {}));
        sim.unitModelDefinitions.insert_or_assign("FOE", createUnitModelDefinition(0_ss, {}));
        sim.unitScriptDefinitions.insert_or_assign("FRIEND", CobScript{{}, {}, {}, 0});
        sim.unitScriptDefinitions.insert_or_assign("FOE", CobScript{{}, {}, {}, 0});

        auto makePlayerInfo = [](GamePlayerType type, unsigned int colorIndex) {
            return GamePlayerInfo{
                std::nullopt,
                type,
                PlayerColorIndex(colorIndex),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0)};
        };
        std::vector<PlayerId> missionPlayers{sim.addPlayer(makePlayerInfo(GamePlayerType::Human, 0)), sim.addPlayer(makePlayerInfo(GamePlayerType::Computer, 1))};

        auto findUnitByType = [&](const std::string& type) -> UnitId {
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.unitType == type)
                {
                    return id;
                }
            }
            throw std::logic_error("unit not found: " + type);
        };

        SECTION("p installs a patrol anchored at the unit's position")
        {
            auto patroller = makeOtaUnit("FOE", 2, 700, 800);
            patroller.initialMission = "p 1750 1828";
            REQUIRE(spawnMissionUnits(sim, {patroller}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);

            REQUIRE(stats.scriptedUnits == 1);
            REQUIRE(stats.errors.empty());

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(unit.orders.size() == 1);
            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.waypoints.size() == 2);

            auto expectedWorld = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(1750), 0_ss, SimScalar(1828)));
            REQUIRE(patrol.waypoints[0].x == expectedWorld.x);
            REQUIRE(patrol.waypoints[0].z == expectedWorld.z);
            REQUIRE(patrol.waypoints[1].x == unit.position.x);
            REQUIRE(patrol.waypoints[1].z == unit.position.z);

            // retail's `p` handler suppresses the implicit trailing
            // make-selectable; the patroller never becomes selectable
            REQUIRE(!unit.missionState->selectable);
        }

        SECTION("o applies standing orders immediately, not via the queue")
        {
            auto gate = makeOtaUnit("FRIEND", 1, 500, 600);
            gate.initialMission = "o 0 0, w 10";
            REQUIRE(spawnMissionUnits(sim, {gate}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);

            REQUIRE(stats.stanceCommands == 1);
            const auto& unit = sim.getUnitState(findUnitByType("FRIEND"));
            REQUIRE(unit.moveOrders == UnitMoveOrders::HoldPosition);
            REQUIRE(unit.fireOrders == UnitFireOrders::HoldFire);
            // only the wait and the implicit make-selectable are queued
            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::get<WaitOrder>(unit.orders.front()).ticksRemaining == 300);
        }

        SECTION("wa resolves a named mission object after spawn")
        {
            auto tower = makeOtaUnit("FRIEND", 1, 500, 600);
            tower.ident = "TOWER1";
            auto watcher = makeOtaUnit("FOE", 2, 700, 800);
            watcher.initialMission = "wa TOWER1";
            // watcher declared first: resolution must not depend on [unitN] order
            REQUIRE(spawnMissionUnits(sim, {watcher, tower}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            auto towerId = findUnitByType("FRIEND");
            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& wait = std::get<WaitForAttackOrder>(unit.orders.front());
            REQUIRE(wait.target == towerId);
        }

        SECTION("bare wa watches the ordered unit itself")
        {
            auto watcher = makeOtaUnit("FOE", 2, 700, 800);
            watcher.initialMission = "wa";
            REQUIRE(spawnMissionUnits(sim, {watcher}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& wait = std::get<WaitForAttackOrder>(unit.orders.front());
            REQUIRE(!wait.target);
        }

        SECTION("wa with an unknown ident reports an error and skips the order")
        {
            auto watcher = makeOtaUnit("FOE", 2, 700, 800);
            watcher.initialMission = "wa NOSUCHOBJECT";
            REQUIRE(spawnMissionUnits(sim, {watcher}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(stats.errors[0].find("NOSUCHOBJECT") != std::string::npos);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            // no order was issued, so the implicit make-selectable
            // is not appended either (retail's edi gate)
            REQUIRE(unit.orders.empty());
        }

        SECTION("wa falls back to Unitname when no Ident matches")
        {
            // retail 0x487af0: per authored record, Ident is tested
            // first then Unitname -- a type reference resolves to the
            // first live unit of that type
            auto tower = makeOtaUnit("FRIEND", 1, 500, 600);
            auto watcher = makeOtaUnit("FOE", 2, 700, 800);
            watcher.initialMission = "wa friend";
            REQUIRE(spawnMissionUnits(sim, {watcher, tower}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            auto towerId = findUnitByType("FRIEND");
            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& wait = std::get<WaitForAttackOrder>(unit.orders.front());
            REQUIRE(wait.target == towerId);
        }

        SECTION("Unitname fallback follows authored order, Ident ties interleave per record")
        {
            sim.unitDefinitions.insert_or_assign("GUARD", makeDefinition("GUARD"));
            sim.unitModelDefinitions.insert_or_assign("GUARD", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("GUARD", CobScript{{}, {}, {}, 0});

            // the FIRST record's Unitname matches, the SECOND record's
            // Ident matches -- retail scans records in order and tests
            // Ident-then-Unitname per record, so the earlier type
            // match wins over the later ident
            auto first = makeOtaUnit("FOE", 2, 700, 800);
            auto second = makeOtaUnit("FRIEND", 2, 900, 1000);
            second.ident = "FOE";
            auto guard = makeOtaUnit("GUARD", 2, 500, 600);
            guard.initialMission = "wa FOE";
            REQUIRE(spawnMissionUnits(sim, {first, second, guard}, missionPlayers).spawned == 3);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            auto firstId = findUnitByType("FOE");
            const auto& unit = sim.getUnitState(findUnitByType("GUARD"));
            const auto& wait = std::get<WaitForAttackOrder>(unit.orders.front());
            REQUIRE(wait.target == firstId);
        }

        SECTION("g resolves through the Unitname fallback")
        {
            auto target = makeOtaUnit("FRIEND", 2, 700, 800);
            auto guard = makeOtaUnit("FOE", 2, 500, 600);
            guard.initialMission = "g FRIEND";
            REQUIRE(spawnMissionUnits(sim, {target, guard}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& order = std::get<GuardOrder>(unit.orders.front());
            REQUIRE(order.target == findUnitByType("FRIEND"));
        }

        SECTION("i boards a transport resolved by Unitname")
        {
            auto shipDef = makeDefinition("SHIP");
            shipDef.transportCapacity = 6;
            sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            sim.unitModelDefinitions.insert_or_assign("SHIP", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SHIP", CobScript{{}, {}, {}, 0});

            auto ship = makeOtaUnit("SHIP", 2, 700, 800);
            auto cargo = makeOtaUnit("FOE", 2, 500, 600);
            cargo.initialMission = "i ship";
            REQUIRE(spawnMissionUnits(sim, {ship, cargo}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            const auto& cargoUnit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(cargoUnit.carriedBy == std::optional<UnitId>(findUnitByType("SHIP")));
            REQUIRE(sim.getUnitState(findUnitByType("SHIP")).passengers.size() == 1);
        }

        SECTION("units without mission state are not resolver candidates")
        {
            // skirmish-spawned units carry no mission record; retail's
            // table only covers authored units, so type fallback must
            // not reach them
            auto authored = makeOtaUnit("FOE", 2, 500, 600);
            authored.initialMission = "wa FRIEND";
            REQUIRE(spawnMissionUnits(sim, {authored}, missionPlayers).spawned == 1);
            // a non-authored FRIEND spawned the skirmish way
            auto skirmish = sim.trySpawnUnit("FRIEND", missionPlayers[0], SimVector(100_ss, 0_ss, 100_ss), std::nullopt);
            REQUIRE(skirmish);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
        }

        SECTION("s installs an explicit make-selectable without a duplicate")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "s";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(unit.orders.size() == 1);
            REQUIRE(std::holds_alternative<MakeSelectableOrder>(unit.orders.front()));
        }

        SECTION("d installs a self-destruct order and suppresses the implicit make-selectable")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "w 5, d";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::holds_alternative<WaitOrder>(unit.orders.at(0)));
            const auto& destructOrder = std::get<SelfDestructOrder>(unit.orders.at(1));
            // FOE's definition leaves SelfDestructCountdown at the retail
            // default of 5 seconds
            REQUIRE(destructOrder.countdownTicks == 5 * SimTicksPerSecond);
            // issuing `d` clears the selectable flag and suppresses the
            // implicit make-selectable: the unit never becomes selectable
            REQUIRE(!unit.missionState->selectable);
        }

        SECTION("scripted units with orders wait for the make-selectable order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "w 5";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            // retail clears the selectable flag at parse end when the
            // script issued an order; the unit becomes selectable only
            // when the trailing make-selectable order executes
            REQUIRE(!unit.missionState->selectable);
            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::holds_alternative<WaitOrder>(unit.orders.at(0)));
            REQUIRE(std::holds_alternative<MakeSelectableOrder>(unit.orders.at(1)));
        }

        SECTION("scripts that issue no order stay selectable")
        {
            // `o` applies stance at parse time without issuing an
            // order (edi stays 0), so retail's epilogue never clears
            // the selectable bit
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "o 0 0";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(unit.missionState->selectable);
            REQUIRE(unit.orders.empty());
        }

        SECTION("units without InitialMission are untouched")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.scriptedUnits == 0);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(unit.orders.empty());
            REQUIRE(unit.missionState->selectable);
        }

        SECTION("skirmish units without mission state are untouched")
        {
            auto id = sim.trySpawnUnit("FOE", missionPlayers[1], sim.terrain.heightmapIndexToWorldCenter(10, 10), std::nullopt);
            REQUIRE(id);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.scriptedUnits == 0);
            REQUIRE(sim.getUnitState(*id).orders.empty());
        }

        SECTION("unsupported commands are reported, not silently dropped")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "p 100 200, z 9";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.unsupportedCommands.size() == 1);
            REQUIRE(stats.unsupportedCommands[0].find("z 9") != std::string::npos);
            // supported commands still install
            REQUIRE(std::holds_alternative<PatrolOrder>(sim.getUnitState(findUnitByType("FOE")).orders.front()));
        }

        SECTION("recognized-but-deferred commands count separately from unsupported")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "d, i BASE, u 100 200";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            // all stock command families execute; `i BASE` fails only
            // because no mission object named BASE exists
            REQUIRE(stats.deferredCommands == 0);
            REQUIRE(stats.unsupportedCommands.empty());
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(stats.errors[0].find("BASE") != std::string::npos);
        }

        SECTION("a <type> installs an attack-unit-type order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "a friend";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.installedOrders == 2);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& order = std::get<AttackUnitTypeOrder>(unit.orders.front());
            // unit type names resolve case-insensitively to the canonical key
            REQUIRE(order.unitType == "FRIEND");
        }

        SECTION("a <unknown> reports an error and installs no order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "a NOSUCHUNIT, w 5";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(std::holds_alternative<WaitOrder>(sim.getUnitState(findUnitByType("FOE")).orders.front()));
        }

        SECTION("b on a factory installs a factory build order with the requested count")
        {
            auto factoryDef = makeDefinition("FACTORY");
            factoryDef.isMobile = false;
            factoryDef.builder = true;
            factoryDef.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Passable);
            sim.unitDefinitions.insert_or_assign("FACTORY", factoryDef);
            sim.unitModelDefinitions.insert_or_assign("FACTORY", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("FACTORY", CobScript{{}, {}, {}, 0});

            auto unit_ = makeOtaUnit("FACTORY", 2, 700, 800);
            unit_.initialMission = "b FOE 3";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.installedOrders == 2);

            const auto& unit = sim.getUnitState(findUnitByType("FACTORY"));
            const auto& order = std::get<MissionFactoryBuildOrder>(unit.orders.front());
            REQUIRE(order.unitType == "FOE");
            REQUIRE(order.count == 3);
        }

        SECTION("b <type> <x> <z> on a mobile builder builds at the authored coordinates")
        {
            sim.unitDefinitions.at("FOE").builder = true;

            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "b FRIEND 1601 1883";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.installedOrders == 2);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            const auto& order = std::get<MissionBuildOrder>(unit.orders.front());
            REQUIRE(order.unitType == "FRIEND");
            auto expected = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(1601), 0_ss, SimScalar(1883)));
            REQUIRE(order.position.x == expected.x);
            REQUIRE(order.position.z == expected.z);
        }

        SECTION("bare b on a mobile builder inherits the previous coordinate command's position")
        {
            sim.unitDefinitions.at("FOE").builder = true;

            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "m 4773 4832, b FRIEND";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(std::holds_alternative<MoveOrder>(unit.orders.front()));
            const auto& order = std::get<MissionBuildOrder>(unit.orders.at(1));
            auto expected = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(4773), 0_ss, SimScalar(4832)));
            REQUIRE(order.position.x == expected.x);
            REQUIRE(order.position.z == expected.z);
        }

        SECTION("b on a non-builder reports an error and installs no order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "b FRIEND 1, w 5";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(std::holds_alternative<WaitOrder>(sim.getUnitState(findUnitByType("FOE")).orders.front()));
        }

        SECTION("malformed commands report errors without aborting the mission")
        {
            auto bad = makeOtaUnit("FOE", 2, 700, 800);
            bad.initialMission = "m, w 5";
            auto good = makeOtaUnit("FRIEND", 1, 500, 600);
            good.initialMission = "w 5";
            REQUIRE(spawnMissionUnits(sim, {bad, good}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);

            // the malformed segment did not stop the rest of the queue
            REQUIRE(std::holds_alternative<WaitOrder>(sim.getUnitState(findUnitByType("FOE")).orders.front()));
            REQUIRE(std::holds_alternative<WaitOrder>(sim.getUnitState(findUnitByType("FRIEND")).orders.front()));
        }

        SECTION("multiple scripted units initialize independently")
        {
            auto a = makeOtaUnit("FOE", 2, 700, 800);
            a.initialMission = "w 10";
            auto b = makeOtaUnit("FRIEND", 1, 500, 600);
            b.initialMission = "o 0 1";
            auto c = makeOtaUnit("FOE", 2, 900, 900);
            REQUIRE(spawnMissionUnits(sim, {a, b, c}, missionPlayers).spawned == 3);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.scriptedUnits == 2);

            int foes = 0;
            for (const auto& [id, unit] : sim.units)
            {
                (void)id;
                if (unit.unitType == "FOE")
                {
                    ++foes;
                }
            }
            REQUIRE(foes == 2);

            const auto& friendUnit = sim.getUnitState(findUnitByType("FRIEND"));
            // retail `o <fire> <move>`: o 0 1 = hold fire + maneuver
            REQUIRE(friendUnit.fireOrders == UnitFireOrders::HoldFire);
            REQUIRE(friendUnit.moveOrders == UnitMoveOrders::Maneuver);
        }

        SECTION("i boards the named transport immediately")
        {
            auto shipDef = makeDefinition("SHIP");
            shipDef.transportCapacity = 6;
            sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            sim.unitModelDefinitions.insert_or_assign("SHIP", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SHIP", CobScript{{}, {}, {}, 0});

            auto ship = makeOtaUnit("SHIP", 2, 700, 800);
            ship.ident = "FRED";
            auto cargo = makeOtaUnit("FOE", 2, 500, 600);
            cargo.initialMission = "i FRED";
            REQUIRE(spawnMissionUnits(sim, {ship, cargo}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.installedOrders == 1);
            REQUIRE(stats.deferredCommands == 0);

            const auto& cargoUnit = sim.getUnitState(findUnitByType("FOE"));
            const auto& shipUnit = sim.getUnitState(findUnitByType("SHIP"));
            REQUIRE(cargoUnit.carriedBy == std::optional<UnitId>(findUnitByType("SHIP")));
            REQUIRE(shipUnit.passengers.size() == 1);
            REQUIRE(shipUnit.passengers.front() == findUnitByType("FOE"));
            // `i` is not a queued order: no orders were installed and
            // the retail epilogue only clears selectable when an order
            // issued (edi at 0x487e5b), so the unit keeps its spawn
            // selectability -- it is merely unselectable while carried
            REQUIRE(cargoUnit.orders.empty());
            REQUIRE(cargoUnit.missionState->selectable);
        }

        SECTION("i-only cargo is selectable again after disembark (Mesa Run)")
        {
            auto shipDef = makeDefinition("SHIP");
            shipDef.transportCapacity = 6;
            shipDef.isMobile = true;
            sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            sim.unitModelDefinitions.insert_or_assign("SHIP", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SHIP", CobScript{{}, {}, {}, 0});

            auto ship = makeOtaUnit("SHIP", 1, 700, 800);
            ship.ident = "AIR7";
            auto cargo = makeOtaUnit("FOE", 1, 500, 600);
            cargo.initialMission = "i AIR7";
            REQUIRE(spawnMissionUnits(sim, {ship, cargo}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            const auto& foeDefRef = sim.unitDefinitions.at("FOE");
            auto shipId = findUnitByType("SHIP");
            auto cargoId = findUnitByType("FOE");
            const auto& cargoUnit = sim.getUnitState(cargoId);

            // retail: the `i`-only script issues no order, so the
            // selectable bit is never cleared; the unit is only
            // unselectable while physically aboard the transport
            REQUIRE(cargoUnit.missionState->selectable);
            REQUIRE(cargoUnit.carriedBy == std::optional<UnitId>(shipId));
            REQUIRE(!cargoUnit.isSelectableBy(foeDefRef, missionPlayers[0]));

            // disembark restores control: same-owner selection works
            // and the unit accepts a move order
            REQUIRE(sim.tryDisembarkUnit(cargoId, sim.getUnitState(shipId).position));
            REQUIRE(!sim.getUnitState(cargoId).carriedBy);
            REQUIRE(sim.getUnitState(cargoId).isSelectableBy(foeDefRef, missionPlayers[0]));

            sim.getUnitState(cargoId).addOrder(createMoveOrder(SimVector(100_ss, 0_ss, 100_ss)));
            REQUIRE(!sim.getUnitState(cargoId).orders.empty());
        }

        SECTION("a scripted order still suppresses selectability until it drains")
        {
            // control: `w` issues a real order, so retail's epilogue
            // clears selectable and the trailing implicit
            // make-selectable only restores it when the queue drains
            auto unit_ = makeOtaUnit("FOE", 2, 500, 600);
            unit_.initialMission = "w 600";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.empty());

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(!unit.missionState->selectable);
            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::get_if<WaitOrder>(&unit.orders.front()));
            REQUIRE(std::get_if<MakeSelectableOrder>(&unit.orders.back()));
        }

        SECTION("i on an unknown ident reports an error")
        {
            auto cargo = makeOtaUnit("FOE", 2, 500, 600);
            cargo.initialMission = "i NOSUCH";
            REQUIRE(spawnMissionUnits(sim, {cargo}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(stats.errors[0].find("NOSUCH") != std::string::npos);
            REQUIRE(!sim.getUnitState(findUnitByType("FOE")).carriedBy);
        }

        SECTION("i on a non-transport reports an error")
        {
            auto anchor = makeOtaUnit("FRIEND", 2, 700, 800);
            anchor.ident = "FRED";
            auto cargo = makeOtaUnit("FOE", 2, 500, 600);
            cargo.initialMission = "i FRED";
            REQUIRE(spawnMissionUnits(sim, {anchor, cargo}, missionPlayers).spawned == 2);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.errors.size() == 1);
            REQUIRE(!sim.getUnitState(findUnitByType("FOE")).carriedBy);
        }

        SECTION("u installs an unload order at the scripted destination")
        {
            auto shipDef = makeDefinition("SHIP");
            shipDef.transportCapacity = 6;
            sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            sim.unitModelDefinitions.insert_or_assign("SHIP", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SHIP", CobScript{{}, {}, {}, 0});

            auto ship = makeOtaUnit("SHIP", 2, 700, 800);
            ship.initialMission = "u 100 200";
            REQUIRE(spawnMissionUnits(sim, {ship}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.deferredCommands == 0);

            const auto& unit = sim.getUnitState(findUnitByType("SHIP"));
            const auto& order = std::get<UnloadOrder>(unit.orders.at(0));
            auto expected = sim.terrain.topLeftCoordinateToWorld(SimVector(100_ss, 0_ss, 200_ss));
            REQUIRE(order.destination.x == expected.x);
            REQUIRE(order.destination.z == expected.z);
        }

        SECTION("bare u inherits the shared script position")
        {
            auto shipDef = makeDefinition("SHIP");
            shipDef.transportCapacity = 6;
            sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            sim.unitModelDefinitions.insert_or_assign("SHIP", createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign("SHIP", CobScript{{}, {}, {}, 0});

            auto ship = makeOtaUnit("SHIP", 2, 700, 800);
            ship.initialMission = "u";
            REQUIRE(spawnMissionUnits(sim, {ship}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("SHIP"));
            const auto& order = std::get<UnloadOrder>(unit.orders.at(0));
            // no coordinate command ran, so the destination is the
            // transport's own spawn position
            REQUIRE(order.destination.x == unit.position.x);
            REQUIRE(order.destination.z == unit.position.z);
        }

        SECTION("d installs a self-destruct order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "d";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            installInitialMissionOrders(sim);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            REQUIRE(std::holds_alternative<SelfDestructOrder>(unit.orders.front()));
        }

        SECTION("bw installs a background build-weapon order")
        {
            auto unit_ = makeOtaUnit("FOE", 2, 700, 800);
            unit_.initialMission = "bw 4";
            REQUIRE(spawnMissionUnits(sim, {unit_}, missionPlayers).spawned == 1);

            auto stats = installInitialMissionOrders(sim);
            REQUIRE(stats.deferredCommands == 0);

            const auto& unit = sim.getUnitState(findUnitByType("FOE"));
            // background: not on the normal queue
            REQUIRE(unit.backgroundOrders.size() == 1);
            const auto& order = std::get<BuildWeaponOrder>(unit.backgroundOrders.front());
            REQUIRE(order.remaining == 4);
        }
    }

    TEST_CASE("parseYardMap")
    {
        SECTION("a single water cell floods the whole footprint")
        {
            // retail repeats the final yardmap cell when the string is
            // shorter than the footprint (e.g. ARMFMKR's YardMap=w)
            auto grid = parseYardMap(3, 3, "w");
            for (unsigned int y = 0; y < 3; ++y)
            {
                for (unsigned int x = 0; x < 3; ++x)
                {
                    REQUIRE(grid.get(x, y) == YardMapCell::Water);
                }
            }
        }

        SECTION("a short string repeats its last cell")
        {
            auto grid = parseYardMap(2, 2, "wo");
            REQUIRE(grid.get(0, 0) == YardMapCell::Water);
            REQUIRE(grid.get(1, 0) == YardMapCell::Ground);
            REQUIRE(grid.get(0, 1) == YardMapCell::Ground);
            REQUIRE(grid.get(1, 1) == YardMapCell::Ground);
        }

        SECTION("land and water letters keep their meaning")
        {
            auto grid = parseYardMap(4, 1, "owCY");
            REQUIRE(grid.get(0, 0) == YardMapCell::Ground);
            REQUIRE(grid.get(1, 0) == YardMapCell::Water);
            REQUIRE(grid.get(2, 0) == YardMapCell::WaterPassableWhenOpen);
            REQUIRE(grid.get(3, 0) == YardMapCell::WaterPassable);
        }

        SECTION("lowercase g is skipped like retail")
        {
            // retail's yardmap jump table has no 'g' arm, so it never
            // consumes a footprint cell
            auto grid = parseYardMap(2, 2, "go");
            REQUIRE(grid.get(0, 0) == YardMapCell::Ground);
            REQUIRE(grid.get(1, 0) == YardMapCell::Ground);
            REQUIRE(grid.get(0, 1) == YardMapCell::Ground);
            REQUIRE(grid.get(1, 1) == YardMapCell::Ground);
        }

        SECTION("uppercase G marks a geothermal cell")
        {
            auto grid = parseYardMap(1, 1, "G");
            REQUIRE(grid.get(0, 0) == YardMapCell::Geo);
        }
    }

    TEST_CASE("applyTransplantBalanceOverrides")
    {
        // the absent-key path logs a warning
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_transplant_override.log", false));

        std::unordered_map<std::string, UnitDefinition> defs;
        for (const auto& name : {"ARMATLAS", "CORVALK", "ARMTHOVR", "CORTHOVR", "ARMTSHIP", "CORTSHIP", "CORAK"})
        {
            defs.insert_or_assign(name, makeDefinition(name));
        }
        defs.at("ARMATLAS").transportCapacity = 5;
        defs.at("ARMATLAS").transportSize = 3;
        defs.at("ARMATLAS").canLoad = true;
        defs.at("ARMTSHIP").transportCapacity = 20;
        defs.at("CORTSHIP").transportCapacity = 5;

        applyTransplantBalanceOverrides(defs);

        SECTION("the six project capacities are applied")
        {
            REQUIRE(defs.at("ARMATLAS").transportCapacity == 1);
            REQUIRE(defs.at("CORVALK").transportCapacity == 1);
            REQUIRE(defs.at("ARMTHOVR").transportCapacity == 5);
            REQUIRE(defs.at("CORTHOVR").transportCapacity == 5);
            REQUIRE(defs.at("ARMTSHIP").transportCapacity == 24);
            REQUIRE(defs.at("CORTSHIP").transportCapacity == 24);
        }

        SECTION("only transportCapacity is touched")
        {
            applyTransplantBalanceOverrides(defs);
            REQUIRE(defs.at("ARMATLAS").transportSize == 3);
            REQUIRE(defs.at("ARMATLAS").canLoad);
            REQUIRE(defs.at("ARMATLAS").isMobile);
            REQUIRE(defs.at("CORAK").transportCapacity == 0);
        }

        SECTION("absent units are skipped without error")
        {
            defs.erase("ARMTSHIP");
            defs.erase("CORTSHIP");
            applyTransplantBalanceOverrides(defs);
            REQUIRE(defs.at("ARMATLAS").transportCapacity == 1);
        }
    }

    /**
     * Retail AI bootstrap slot loop (TotalA.exe 0x464950): the
     * resolved profile is applied once per computer player. Human
     * slots never receive a state, and a second pass over an
     * already-attached player is a no-op -- that is the save-restore
     * path where applySavedGame has already repopulated the table.
     * Shared by campaign and skirmish.
     */
    TEST_CASE("attachCampaignAiPlayers attaches one state per computer player")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto sim = makeSim();
        sim.unitDefinitions.insert_or_assign("ARMCOM", makeDefinition("ARMCOM"));
        sim.unitDefinitions.insert_or_assign("ARMFUS", makeDefinition("ARMFUS"));

        auto playerInfo = [](GamePlayerType type, unsigned int colorIndex) {
            return GamePlayerInfo{
                std::nullopt,
                type,
                PlayerColorIndex(colorIndex),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(1000),
                Energy(1000),
                Metal(1000),
                Energy(1000),
                Metal(1000),
                Energy(1000)};
        };

        auto human = sim.addPlayer(playerInfo(GamePlayerType::Human, 0));
        auto compA = sim.addPlayer(playerInfo(GamePlayerType::Computer, 1));
        auto compB = sim.addPlayer(playerInfo(GamePlayerType::Computer, 2));

        auto profile = parseCampaignAiProfile("plan any\nweight ARMCOM 1.0\n", CampaignAiDifficulty::Medium);
        attachCampaignAiPlayers(sim, profile);

        REQUIRE(sim.campaignAi.size() == 2);
        std::set<unsigned int> attached;
        for (const auto& st : sim.campaignAi)
        {
            attached.insert(st.player.value);
            REQUIRE(st.unitIndex.size() == sim.unitDefinitions.size());
        }
        REQUIRE(attached == std::set<unsigned int>{compA.value, compB.value});

        // save-restore path: states already present must not be
        // duplicated or re-initialized from the profile
        auto weights0 = sim.campaignAi.front().unitWeights;
        sim.campaignAi.front().nextAttackScan = GameTime(1234);
        attachCampaignAiPlayers(sim, profile);
        REQUIRE(sim.campaignAi.size() == 2);
        REQUIRE(sim.campaignAi.front().unitWeights == weights0);
        REQUIRE(sim.campaignAi.front().nextAttackScan == GameTime(1234));
    }
}
