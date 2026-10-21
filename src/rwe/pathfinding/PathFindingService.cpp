#include "PathFindingService.h"
#include <algorithm>
#include <rwe/pathfinding/UnitPathFinder.h>
#include <rwe/pathfinding/UnitPerimeterPathFinder.h>
#include <rwe/pathfinding/pathfinding_utils.h>
#include <rwe/util/Index.h>

namespace rwe
{
    static const unsigned int MaxTasksPerTick = 10;

    void PathFindingService::update(GameSimulation& simulation)
    {
        int remainingBudget = 4000;

        auto& requests = simulation.pathRequests;
        stats.maxQueueDepth = std::max<unsigned int>(stats.maxQueueDepth, requests.size());
        while (!requests.empty() && remainingBudget > 0)
        {
            auto& request = requests.front();

            auto unit = simulation.tryGetUnitState(request.unitId);
            if (!unit)
            {
                // Unit that made the request no longer exists.
                // Possibly the unit died. Just skip it.
                requests.pop_front();
                continue;
            }

            if (auto movingState = std::get_if<NavigationStateMoving>(&unit->get().navigationState.state); movingState != nullptr)
            {
                auto path = match(
                    movingState->pathDestination,
                    [&](const SimVector& pos) {
                        return findPath(simulation, request.unitId, pos);
                    },
                    [&](const DiscreteRect& pos) {
                        return findPath(simulation, request.unitId, pos);
                    });

                const bool isPartial = path.isPartial;

                movingState->path = PathFollowingInfo(std::move(path), simulation.gameTime);
                movingState->pathRequested = false;
                movingState->pathExhausted = false;
                // A partial path means the search hit the vertex cap --
                // a failed request. Retail sleeps the move job ~30
                // ticks on a failed path request (TotalA.exe 0x439e80);
                // delay the order-visible exhaustion result to match,
                // so a request that fails instantly cannot re-issue a
                // full search every few ticks.
                movingState->pathExhaustedAt = isPartial ? simulation.gameTime + GameTime(30) : GameTime(0);

                // HACK: we know that lastPathDebugInfo is set by the call to findPath,
                // so we'll exploit it here to deduct from out budget.
                const auto expansions = getSize(lastPathDebugInfo.closedVertices);
                remainingBudget -= expansions;
                ++stats.requestsProcessed;
                stats.totalExpansions += expansions;
                stats.maxExpansionsPerRequest = std::max<unsigned int>(stats.maxExpansionsPerRequest, expansions);
            }

            requests.pop_front();
        }
    }

    UnitPath PathFindingService::findPath(const GameSimulation& simulation, UnitId unitId, const DiscreteRect& destination)
    {
        const auto& unit = simulation.getUnitState(unitId);
        const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);

        auto start = simulation.computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        // expand the goal rect to take into account our own collision rect
        auto goal = destination.expandTopLeft(start.width, start.height);

        auto movementClassId = match(
            unitDefinition.movementCollisionInfo, [&](const UnitDefinition::NamedMovementClass& mc) { return std::make_optional(mc.movementClassId); }, [&](const auto&) { return std::optional<MovementClassId>(); });

        UnitPerimeterPathFinder pathFinder(&simulation, &simulation.movementClassCollisionService, unitId, movementClassId, start.width, start.height, goal);

        auto path = pathFinder.findPath(Point(start.x, start.y));
        lastPathDebugInfo = AStarPathInfo<Point, PathCost>{path.type, path.path, std::move(path.closedVertices)};

        assert(path.path.size() >= 1);

        bool isPartial = path.type == AStarPathType::Partial;

        if (path.path.size() == 1)
        {
            // The path is trivial, we are already at the goal
            // (or at the closest reachable point, for a partial path).
            return UnitPath{std::vector<SimVector>{unit.position}, isPartial};
        }

        auto simplifiedPath = runSimplifyPath(path.path);

        std::vector<SimVector> waypoints;
        for (auto it = ++simplifiedPath.cbegin(); it != simplifiedPath.cend(); ++it)
        {
            waypoints.push_back(getWorldCenter(simulation, DiscreteRect(it->x, it->y, start.width, start.height)));
        }

        return UnitPath{std::move(waypoints), isPartial};
    }

    UnitPath PathFindingService::findPath(const GameSimulation& simulation, UnitId unitId, const SimVector& destination)
    {
        const auto& unit = simulation.getUnitState(unitId);
        const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);

        auto start = simulation.computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        auto goal = simulation.computeFootprintRegion(destination, unitDefinition.movementCollisionInfo);

        auto movementClassId = match(
            unitDefinition.movementCollisionInfo, [&](const UnitDefinition::NamedMovementClass& mc) { return std::make_optional(mc.movementClassId); }, [&](const auto&) { return std::optional<MovementClassId>(); });

        UnitPathFinder pathFinder(&simulation, &simulation.movementClassCollisionService, unitId, movementClassId, start.width, start.height, Point(goal.x, goal.y));

        auto path = pathFinder.findPath(Point(start.x, start.y));
        lastPathDebugInfo = AStarPathInfo<Point, PathCost>{path.type, path.path, std::move(path.closedVertices)};

        // For a partial path the waypoints end at the closest
        // reachable vertex; the requested goal is deliberately not
        // appended so the mover stops where retail's move job would
        // finish rather than grinding on an unreachable waypoint.
        bool isPartial = path.type == AStarPathType::Partial;

        assert(path.path.size() >= 1);

        if (path.path.size() == 1)
        {
            // The path is trivial, we are already at the goal
            // (or at the closest reachable point, for a partial path).
            return UnitPath{std::vector<SimVector>{isPartial ? unit.position : destination}, isPartial};
        }

        auto simplifiedPath = runSimplifyPath(path.path);

        std::vector<SimVector> waypoints;
        for (auto it = ++simplifiedPath.cbegin(); it != simplifiedPath.cend(); ++it)
        {
            waypoints.push_back(getWorldCenter(simulation, DiscreteRect(it->x, it->y, start.width, start.height)));
        }
        if (!isPartial)
        {
            waypoints.back() = destination;
        }

        return UnitPath{std::move(waypoints), isPartial};
    }

    SimVector PathFindingService::getWorldCenter(const GameSimulation& simulation, const DiscreteRect& rect)
    {
        auto corner = simulation.terrain.heightmapIndexToWorldCorner(rect.x, rect.y);

        auto halfWorldWidth = (SimScalar(rect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss;
        auto halfWorldHeight = (SimScalar(rect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss;

        auto center = corner + SimVector(halfWorldWidth, 0_ss, halfWorldHeight);
        center.y = simulation.terrain.getHeightAt(center.x, center.z);
        return center;
    }
}
