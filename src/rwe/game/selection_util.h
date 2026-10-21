#pragma once

#include <unordered_set>

#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/UnitId.h>

namespace rwe
{
    /**
     * Removes selection ids that no longer resolve to a unit in the
     * simulation. Sim events are processed after the sim has already
     * removed dead units, so the selection set can transiently hold
     * stale ids — e.g. when two units die in the same tick, the first
     * death's notification still sees the second unit's stale id.
     */
    inline void purgeStaleSelectedUnits(std::unordered_set<UnitId>& selectedUnits, const GameSimulation& sim)
    {
        for (auto it = selectedUnits.begin(); it != selectedUnits.end();)
        {
            if (!sim.tryGetUnitState(*it))
            {
                it = selectedUnits.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
