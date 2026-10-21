#include <catch2/catch_test_macros.hpp>
#include <rwe/io/cob/Cob.h>
#include <rwe/sim/UnitState.h>
#include <rwe/sim/UnitState_util.h>

namespace rwe
{
    namespace
    {
        UnitState makeUnit()
        {
            static CobScript emptyScript{{}, {}, {}, 0};
            return UnitState(std::vector<UnitMesh>(), std::make_unique<CobEnvironment>(&emptyScript));
        }
    }

    TEST_CASE("removeFromBuildQueue")
    {
        SECTION("Removes nothing from the empty list")
        {
            std::deque<std::pair<std::string, int>> buildQueue;

            removeFromBuildQueue(buildQueue, "ARMPW", 1);

            std::deque<std::pair<std::string, int>> expected;
            REQUIRE(buildQueue == expected);
        }

        SECTION("Removes the only element in a trivial case")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1}};

            removeFromBuildQueue(buildQueue, "ARMPW", 1);

            std::deque<std::pair<std::string, int>> expected;
            REQUIRE(buildQueue == expected);
        }

        SECTION("Removes nothing when count is 0")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1}};

            removeFromBuildQueue(buildQueue, "ARMPW", 0);

            std::deque<std::pair<std::string, int>> expected{
                std::pair{"ARMPW", 1}};
            REQUIRE(buildQueue == expected);
        }

        SECTION("Doesn't break when removing more than there are in the list")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1}};

            removeFromBuildQueue(buildQueue, "ARMPW", 2);

            std::deque<std::pair<std::string, int>> expected;
            REQUIRE(buildQueue == expected);
        }

        SECTION("Leaves excess elements there")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 8}};

            removeFromBuildQueue(buildQueue, "ARMPW", 3);

            std::deque<std::pair<std::string, int>> expected{
                std::pair{"ARMPW", 5}};
            REQUIRE(buildQueue == expected);
        }

        SECTION("Removes consecutive elements from the back")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1},
                std::pair{"ARMPW", 5},
                std::pair{"ARMPW", 3}};

            removeFromBuildQueue(buildQueue, "ARMPW", 4);

            std::deque<std::pair<std::string, int>> expected{
                std::pair{"ARMPW", 1},
                std::pair{"ARMPW", 4}};
            REQUIRE(buildQueue == expected);
        }

        SECTION("Ignores non-matching elements")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1},
                std::pair{"ARMPW", 5},
                std::pair{"ARMCK", 7},
                std::pair{"ARMPW", 3}};

            removeFromBuildQueue(buildQueue, "ARMPW", 4);

            std::deque<std::pair<std::string, int>> expected{
                std::pair{"ARMPW", 1},
                std::pair{"ARMPW", 4},
                std::pair{"ARMCK", 7}};
            REQUIRE(buildQueue == expected);
        }
    }

    TEST_CASE("getBuildQueueTotalsStatic")
    {
        SECTION("gets totals from build queue")
        {
            std::deque<std::pair<std::string, int>> buildQueue{
                std::pair{"ARMPW", 1},
                std::pair{"ARMPW", 5},
                std::pair{"ARMCK", 7},
                std::pair{"ARMPW", 3}};

            std::unordered_map<std::string, int> expected{
                {"ARMPW", 9},
                {"ARMCK", 7}};

            REQUIRE(getBuildQueueTotalsStatic(buildQueue) == expected);
        }
    }

    TEST_CASE("expandPatrolOrder")
    {
        SECTION("appends the unit position as the route anchor")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            auto expanded = expandPatrolOrder(unit, PatrolOrder({SimVector(100_ss, 0_ss, 200_ss)}));

            const auto& patrol = std::get<PatrolOrder>(expanded);
            REQUIRE(patrol.waypoints.size() == 2);
            REQUIRE(static_cast<bool>(patrol.waypoints[0] == SimVector(100_ss, 0_ss, 200_ss)));
            REQUIRE(static_cast<bool>(patrol.waypoints[1] == SimVector(10_ss, 0_ss, 20_ss)));
        }

        SECTION("passes non-patrol orders through unchanged")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            auto expanded = expandPatrolOrder(unit, MoveOrder(SimVector(5_ss, 0_ss, 5_ss)));

            const auto& move = std::get<MoveOrder>(expanded);
            REQUIRE(static_cast<bool>(move.destination == SimVector(5_ss, 0_ss, 5_ss)));
        }
    }

    TEST_CASE("enqueueUnitOrder")
    {
        SECTION("a patrol on an empty queue installs an anchored route")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            enqueueUnitOrder(unit, PatrolOrder({SimVector(100_ss, 0_ss, 200_ss)}));

            REQUIRE(unit.orders.size() == 1);
            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.waypoints.size() == 2);
            REQUIRE(static_cast<bool>(patrol.waypoints[1] == SimVector(10_ss, 0_ss, 20_ss)));
        }

        SECTION("a queued patrol merges into the patrol at the back of the queue")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            enqueueUnitOrder(unit, PatrolOrder({SimVector(100_ss, 0_ss, 200_ss)}));
            enqueueUnitOrder(unit, PatrolOrder({SimVector(300_ss, 0_ss, 400_ss)}));

            REQUIRE(unit.orders.size() == 1);
            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.waypoints.size() == 3);
            REQUIRE(static_cast<bool>(patrol.waypoints[0] == SimVector(100_ss, 0_ss, 200_ss)));
            REQUIRE(static_cast<bool>(patrol.waypoints[1] == SimVector(300_ss, 0_ss, 400_ss)));
            // the anchor stays last
            REQUIRE(static_cast<bool>(patrol.waypoints[2] == SimVector(10_ss, 0_ss, 20_ss)));
        }

        SECTION("a patrol queued behind a non-patrol order starts a new patrol")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            enqueueUnitOrder(unit, MoveOrder(SimVector(1_ss, 0_ss, 1_ss)));
            enqueueUnitOrder(unit, PatrolOrder({SimVector(100_ss, 0_ss, 200_ss)}));

            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::holds_alternative<MoveOrder>(unit.orders.front()));
            const auto& patrol = std::get<PatrolOrder>(unit.orders.back());
            REQUIRE(patrol.waypoints.size() == 2);
        }

        SECTION("a non-patrol queued behind a patrol does not merge")
        {
            auto unit = makeUnit();
            unit.position = SimVector(10_ss, 0_ss, 20_ss);

            enqueueUnitOrder(unit, PatrolOrder({SimVector(100_ss, 0_ss, 200_ss)}));
            enqueueUnitOrder(unit, MoveOrder(SimVector(5_ss, 0_ss, 5_ss)));

            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::holds_alternative<PatrolOrder>(unit.orders.front()));
            REQUIRE(std::holds_alternative<MoveOrder>(unit.orders.back()));
        }
    }
}
