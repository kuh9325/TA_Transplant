#pragma once

#include <deque>
#include <string>
#include <unordered_map>

#include <rwe/sim/UnitOrder.h>

namespace rwe
{
    class UnitState;

    void removeFromBuildQueue(std::deque<std::pair<std::string, int>>& buildQueue, const std::string& buildUnitType, int count);

    std::unordered_map<std::string, int> getBuildQueueTotalsStatic(const std::deque<std::pair<std::string, int>>& buildQueue);

    /**
     * A player-issued patrol command carries only the clicked
     * destination(s); the unit's position at issue time becomes the
     * final waypoint so the route loops back through it.
     * Non-patrol orders pass through unchanged.
     */
    UnitOrder expandPatrolOrder(const UnitState& unit, const UnitOrder& order);

    /**
     * Enqueues an order on the unit. A queued patrol extends the route
     * of a patrol already at the back of the queue rather than stacking
     * a second patrol order.
     */
    void enqueueUnitOrder(UnitState& unit, const UnitOrder& order);
}
