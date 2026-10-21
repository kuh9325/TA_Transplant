#pragma once

#include <rwe/sim/SimVector.h>

namespace rwe
{
    struct UnitPath
    {
        std::vector<SimVector> waypoints;

        /**
         * True when the path ends at the closest reachable point
         * rather than the requested goal: the requested destination
         * is unreachable for this unit's movement class. Retail's
         * move job completes when the mover's best-effort path is
         * exhausted, so a unit with a partial path is "done" once it
         * consumes the final waypoint.
         */
        bool isPartial{false};
    };
}
