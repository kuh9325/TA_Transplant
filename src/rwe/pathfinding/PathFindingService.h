#pragma once

#include <cstdint>
#include <rwe/grid/DiscreteRect.h>
#include <rwe/grid/Point.h>
#include <rwe/pathfinding/AStarPathFinder.h>
#include <rwe/pathfinding/PathCost.h>
#include <rwe/pathfinding/UnitPath.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitId.h>

namespace rwe
{
    struct GameSimulation;

    class PathFindingService
    {
    public:
        /**
         * Measurement-only counters for the bench harness -- never
         * serialized or hashed; they record how much search work the
         * service actually performed.
         */
        struct Stats
        {
            std::uint64_t requestsProcessed{0};
            std::uint64_t totalExpansions{0};
            unsigned int maxExpansionsPerRequest{0};
            unsigned int maxQueueDepth{0};
        };

        AStarPathInfo<Point, PathCost> lastPathDebugInfo;
        Stats stats;

        void update(GameSimulation& simulation);

    private:
        UnitPath findPath(const GameSimulation& simulation, UnitId unitId, const SimVector& destination);
        UnitPath findPath(const GameSimulation& simulation, UnitId unitId, const DiscreteRect& destination);

        SimVector getWorldCenter(const GameSimulation& simulation, const DiscreteRect& discreteRect);
    };
}
