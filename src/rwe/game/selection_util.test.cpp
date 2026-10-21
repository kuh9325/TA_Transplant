#include <catch2/catch_test_macros.hpp>

#include <rwe/game/selection_util.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/util.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0);
        }

        GamePlayerInfo makePlayer()
        {
            return GamePlayerInfo{
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
        }
    }

    TEST_CASE("purgeStaleSelectedUnits")
    {
        auto sim = makeSim();
        auto player = sim.addPlayer(makePlayer());

        UnitDefinition def{};
        def.unitName = "TESTUNIT";
        def.objectName = "TESTUNIT";
        def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
        def.turnRate = SimScalar(1);
        def.maxVelocity = SimScalar(10);
        def.acceleration = SimScalar(1);
        def.brakeRate = SimScalar(1);
        def.canMove = true;
        def.isMobile = true;
        def.maxHitPoints = 1000;
        def.buildTime = 100;
        sim.unitDefinitions.insert_or_assign("TESTUNIT", def);
        sim.unitModelDefinitions.insert_or_assign("TESTUNIT", createUnitModelDefinition(0_ss, {}));
        sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", CobScript{{}, {}, {}, 0});

        SECTION("keeps ids that resolve to live units")
        {
            auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            std::unordered_set<UnitId> selected{id};

            purgeStaleSelectedUnits(selected, sim);

            REQUIRE(selected.size() == 1);
            REQUIRE(selected.count(id) == 1);
        }

        SECTION("removes ids for units already deleted by the sim")
        {
            auto idA = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            auto idB = sim.trySpawnUnit("TESTUNIT", player, SimVector(200_ss, 0_ss, 200_ss), SimAngle(0)).value();
            sim.units.erase(sim.units.find(idB));

            // simulates the death-event window: the first dead unit's
            // deselect notification runs while the second dead unit's
            // id is still in the selection set
            std::unordered_set<UnitId> selected{idA, idB};

            purgeStaleSelectedUnits(selected, sim);

            REQUIRE(selected.size() == 1);
            REQUIRE(selected.count(idA) == 1);
        }
    }
}
