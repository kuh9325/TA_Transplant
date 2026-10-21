#pragma once

namespace rwe
{
    /**
     * The unit's standing move order: how far it may leave its position
     * to engage targets it acquired itself. Mirrors the values stored
     * in FBI StandingMoveOrder and the retail InitialMission `o` command.
     * Currently retained state only; no chase behavior consumes it yet.
     */
    enum class UnitMoveOrders
    {
        HoldPosition,
        Maneuver,
        Roam,
    };
}
