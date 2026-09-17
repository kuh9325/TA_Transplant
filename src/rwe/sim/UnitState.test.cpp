#include <catch2/catch_test_macros.hpp>
#include <rwe/io/cob/Cob.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitState.h>

namespace rwe
{
    namespace
    {
        UnitState makeUnit()
        {
            static CobScript emptyScript{{}, {}, {}, 0};
            return UnitState(std::vector<UnitMesh>(), std::make_unique<CobEnvironment>(&emptyScript));
        }

        UnitDefinition makeTargetDefinition()
        {
            UnitDefinition def{};
            def.buildTime = 100;
            def.maxHitPoints = 500;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }
    }

    TEST_CASE("UnitState::getRepairProgressForHitPoints")
    {
        auto def = makeTargetDefinition();
        auto unit = makeUnit();

        SECTION("maps hit points onto the build-time axis")
        {
            unit.hitPoints = 250;
            REQUIRE(unit.getRepairProgressForHitPoints(def) == 50);
        }

        SECTION("full hit points map to the full build time")
        {
            unit.hitPoints = 500;
            REQUIRE(unit.getRepairProgressForHitPoints(def) == 100);
        }

        SECTION("zero hit points map to zero")
        {
            unit.hitPoints = 0;
            REQUIRE(unit.getRepairProgressForHitPoints(def) == 0);
        }

        SECTION("zero max hit points is safe")
        {
            def.maxHitPoints = 0;
            unit.hitPoints = 0;
            REQUIRE(unit.getRepairProgressForHitPoints(def) == 0);
        }
    }

    TEST_CASE("UnitState::getRepairCostInfo")
    {
        auto def = makeTargetDefinition();

        SECTION("cost is proportional to the repaired fraction")
        {
            auto unit = makeUnit();
            auto costs = unit.getRepairCostInfo(def, 50, 10);
            REQUIRE(costs.workerTime == 10);
            REQUIRE(costs.energyCost == Energy(100));
            REQUIRE(costs.metalCost == Metal(50));
        }

        SECTION("contribution clamps at the end of the build-time axis")
        {
            auto unit = makeUnit();
            auto costs = unit.getRepairCostInfo(def, 95, 10);
            REQUIRE(costs.workerTime == 5);
            REQUIRE(costs.energyCost == Energy(50));
            REQUIRE(costs.metalCost == Metal(25));
        }

        SECTION("zero build time produces no cost")
        {
            def.buildTime = 0;
            auto unit = makeUnit();
            auto costs = unit.getRepairCostInfo(def, 0, 10);
            REQUIRE(costs.workerTime == 0);
            REQUIRE(costs.energyCost == Energy(0));
            REQUIRE(costs.metalCost == Metal(0));
        }
    }

    TEST_CASE("UnitState::addRepairProgress")
    {
        auto def = makeTargetDefinition();

        SECTION("restores hit points along the progress axis")
        {
            auto unit = makeUnit();
            unit.hitPoints = 250;
            unsigned int progress = unit.getRepairProgressForHitPoints(def);
            REQUIRE_FALSE(unit.addRepairProgress(def, progress, 10));
            REQUIRE(unit.hitPoints == 300);
            REQUIRE(progress == 60);
        }

        SECTION("caps hit points at the maximum and reports done")
        {
            auto unit = makeUnit();
            unit.hitPoints = 480;
            unsigned int progress = unit.getRepairProgressForHitPoints(def);
            REQUIRE(unit.addRepairProgress(def, progress, 10));
            REQUIRE(unit.hitPoints == 500);
        }

        SECTION("reports done when already at full health")
        {
            auto unit = makeUnit();
            unit.hitPoints = 500;
            unsigned int progress = unit.getRepairProgressForHitPoints(def);
            REQUIRE(unit.addRepairProgress(def, progress, 10));
            REQUIRE(unit.hitPoints == 500);
        }

        SECTION("re-anchors progress when the unit took damage mid-repair")
        {
            auto unit = makeUnit();
            unit.hitPoints = 250;
            unsigned int progress = unit.getRepairProgressForHitPoints(def);
            REQUIRE_FALSE(unit.addRepairProgress(def, progress, 10));
            REQUIRE(unit.hitPoints == 300);
            REQUIRE(progress == 60);

            // the unit loses 200 hit points mid-repair
            unit.hitPoints = 100;

            // progress re-anchors to 100hp -> 20 build ticks,
            // then advances 10 -> implied hp 150
            REQUIRE_FALSE(unit.addRepairProgress(def, progress, 10));
            REQUIRE(unit.hitPoints == 150);
            REQUIRE(progress == 30);
        }

        SECTION("repairing to full health costs the damaged fraction of the build cost")
        {
            auto unit = makeUnit();
            unit.hitPoints = 250;
            unsigned int progress = unit.getRepairProgressForHitPoints(def);

            Energy totalEnergy(0);
            Metal totalMetal(0);
            bool done = false;
            while (!done)
            {
                auto costs = unit.getRepairCostInfo(def, progress, 10);
                totalEnergy += costs.energyCost;
                totalMetal += costs.metalCost;
                done = unit.addRepairProgress(def, progress, 10);
            }

            // 250 of 500 hit points repaired => half the build cost
            REQUIRE(totalEnergy == Energy(500));
            REQUIRE(totalMetal == Metal(250));
            REQUIRE(unit.hitPoints == 500);
        }
    }
}
