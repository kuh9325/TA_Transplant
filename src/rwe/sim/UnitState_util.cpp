#include "UnitState_util.h"

#include <rwe/sim/UnitState.h>

namespace rwe
{
    void removeFromBuildQueue(std::deque<std::pair<std::string, int>>& buildQueue, const std::string& buildUnitType, int count)
    {
        auto it = buildQueue.end();
        while (count > 0 && it != buildQueue.begin())
        {
            --it;

            auto& elem = *it;
            if (elem.first != buildUnitType)
            {
                continue;
            }

            if (count < elem.second)
            {
                elem.second -= count;
                break;
            }

            count -= elem.second;
            it = buildQueue.erase(it);
        }
    }

    std::unordered_map<std::string, int> getBuildQueueTotalsStatic(const std::deque<std::pair<std::string, int>>& buildQueue)
    {
        std::unordered_map<std::string, int> map;
        for (const auto& e : buildQueue)
        {
            auto it = map.find(e.first);
            if (it == map.end())
            {
                map.insert(e);
                continue;
            }

            it->second += e.second;
        }

        return map;
    }

    UnitOrder expandPatrolOrder(const UnitState& unit, const UnitOrder& order)
    {
        auto patrol = std::get_if<PatrolOrder>(&order);
        if (!patrol)
        {
            return order;
        }

        auto expanded = *patrol;
        expanded.waypoints.push_back(unit.position);
        return expanded;
    }

    void enqueueUnitOrder(UnitState& unit, const UnitOrder& order)
    {
        // a queued patrol extends the route of a patrol already at
        // the back of the queue rather than stacking a second order
        if (auto newPatrol = std::get_if<PatrolOrder>(&order))
        {
            if (!unit.orders.empty())
            {
                if (auto existingPatrol = std::get_if<PatrolOrder>(&unit.orders.back()))
                {
                    // insert before the anchor (the final waypoint is
                    // always the unit's position at issue time)
                    auto insertAt = existingPatrol->waypoints.empty()
                        ? existingPatrol->waypoints.begin()
                        : existingPatrol->waypoints.begin() + (existingPatrol->waypoints.size() - 1);
                    existingPatrol->waypoints.insert(insertAt, newPatrol->waypoints.begin(), newPatrol->waypoints.end());
                    return;
                }
            }
        }

        unit.addOrder(expandPatrolOrder(unit, order));
    }
}
