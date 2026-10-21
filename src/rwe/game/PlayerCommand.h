#pragma once

#include <rwe/game/SceneTime.h>
#include <rwe/sim/UnitFireOrders.h>
#include <rwe/sim/UnitId.h>
#include <rwe/sim/UnitOrder.h>
#include <variant>

namespace rwe
{
    struct PlayerUnitCommand
    {
        struct IssueOrder
        {
            enum IssueKind
            {
                Immediate,
                Queued
            };
            UnitOrder order;
            IssueKind issueKind;

            IssueOrder(const UnitOrder& order, IssueKind issueKind) : order(order), issueKind(issueKind)
            {
            }
        };

        struct ModifyBuildQueue
        {
            int count;
            std::string unitType;
        };

        struct Stop
        {
        };

        struct SetFireOrders
        {
            UnitFireOrders orders;
        };

        struct SetOnOff
        {
            bool on;
        };

        /**
         * The retail CLOAK_ON / CLOAK_OFF persistent orders
         * (TotalA.exe order-table entries handled at 0x403070 /
         * 0x4030a0): sets or clears the unit's cloak-requested flag
         * (unit+0x110 bit11), gated on the unitdef cloak-capability
         * bit (+0x245 bit13 = CloakCost > 0). Not a foreground order —
         * the toggle persists alongside the order queue.
         */
        struct SetCloak
        {
            bool on;
        };

        /**
         * Removes the order at `index` in the unit's order queue
         * (retail shift+click on a queued order's map marker).
         * Out-of-range indices are ignored.
         */
        struct RemoveOrder
        {
            int index;
        };

        /**
         * The retail SELFDESTRUCT unit command (TotalA.exe Ctrl/Alt+D
         * dispatch at 0x49641d). Toggles the unit's interactive
         * self-destruct state: arms the countdown if idle, cancels it
         * ("Self destruct terminated") if armed. A unit state, not a
         * queue order.
         */
        struct ToggleSelfDestruct
        {
        };

        /**
         * Queues or cancels rounds of the unit's stockpile weapon: the
         * GUI `commonattribs=8` weapon build button (ARMSILO's
         * ARMMAKENUKE, ARMAMD's ARMMAKEANTI, ...) is the only
         * player-facing stockpile control. Positive counts append
         * BuildWeaponOrder rounds to backgroundOrders; negative counts
         * cancel pending rounds. Stockpile production runs in the
         * background, in parallel with the unit's order queue.
         */
        struct ModifyWeaponQueue
        {
            int count;
        };

        using Command = std::variant<IssueOrder, ModifyBuildQueue, Stop, SetFireOrders, SetOnOff, SetCloak, RemoveOrder, ToggleSelfDestruct, ModifyWeaponQueue>;

        UnitId unit;
        Command command;

        PlayerUnitCommand(const UnitId& unit, const Command& command) : unit(unit), command(command)
        {
        }
    };

    struct PlayerPauseGameCommand
    {
    };

    struct PlayerUnpauseGameCommand
    {
    };

    using PlayerCommand = std::variant<PlayerUnitCommand, PlayerPauseGameCommand, PlayerUnpauseGameCommand>;
}
