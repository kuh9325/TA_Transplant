#include <catch2/catch_test_macros.hpp>
#include <rwe/proto/serialization.h>

namespace rwe
{
    namespace
    {
        PlayerCommand roundTrip(const PlayerCommand& command)
        {
            proto::PlayerCommand out;
            serializePlayerCommand(command, out);
            return deserializeCommand(out);
        }

        PlayerUnitCommand::IssueOrder getIssueOrder(const PlayerCommand& command)
        {
            return std::get<PlayerUnitCommand::IssueOrder>(std::get<PlayerUnitCommand>(command).command);
        }
    }

    TEST_CASE("unit order serialization")
    {
        SECTION("repair order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(RepairOrder(UnitId(7)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& repair = std::get<RepairOrder>(order.order);
            REQUIRE(repair.target == UnitId(7));
        }

        SECTION("queued repair order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(4),
                PlayerUnitCommand::IssueOrder(RepairOrder(UnitId(9)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& repair = std::get<RepairOrder>(order.order);
            REQUIRE(repair.target == UnitId(9));
        }

        SECTION("reclaim order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(ReclaimOrder(FeatureId(11)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& reclaim = std::get<ReclaimOrder>(order.order);
            REQUIRE(reclaim.target == ReclaimTarget(FeatureId(11)));
        }

        SECTION("queued reclaim order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(4),
                PlayerUnitCommand::IssueOrder(ReclaimOrder(FeatureId(258)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& reclaim = std::get<ReclaimOrder>(order.order);
            REQUIRE(reclaim.target == ReclaimTarget(FeatureId(258)));
        }

        SECTION("reclaim order with unit target round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(5),
                PlayerUnitCommand::IssueOrder(ReclaimOrder(UnitId(77)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& reclaim = std::get<ReclaimOrder>(order.order);
            REQUIRE(reclaim.target == ReclaimTarget(UnitId(77)));
        }

        SECTION("existing orders still round-trip")
        {
            {
                auto command = PlayerUnitCommand(UnitId(1),
                    PlayerUnitCommand::IssueOrder(GuardOrder(UnitId(5)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));
                const auto& order = getIssueOrder(roundTrip(command));
                REQUIRE(std::get<GuardOrder>(order.order).target == UnitId(5));
            }

            {
                auto command = PlayerUnitCommand(UnitId(1),
                    PlayerUnitCommand::IssueOrder(CompleteBuildOrder(UnitId(6)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));
                const auto& order = getIssueOrder(roundTrip(command));
                REQUIRE(std::get<CompleteBuildOrder>(order.order).target == UnitId(6));
            }

            {
                auto command = PlayerUnitCommand(UnitId(1),
                    PlayerUnitCommand::IssueOrder(MoveOrder(SimVector(1_ss, 2_ss, 3_ss)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));
                const auto& order = getIssueOrder(roundTrip(command));
                REQUIRE(static_cast<bool>(std::get<MoveOrder>(order.order).destination == SimVector(1_ss, 2_ss, 3_ss)));
            }

            {
                auto command = PlayerUnitCommand(UnitId(1),
                    PlayerUnitCommand::IssueOrder(AttackOrder(UnitId(8)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));
                const auto& order = getIssueOrder(roundTrip(command));
                REQUIRE(std::get<UnitId>(std::get<AttackOrder>(order.order).target) == UnitId(8));
            }

            {
                auto command = PlayerUnitCommand(UnitId(1),
                    PlayerUnitCommand::IssueOrder(BuildOrder("ARMFLASH", SimVector(4_ss, 5_ss, 6_ss)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));
                const auto& order = getIssueOrder(roundTrip(command));
                const auto& build = std::get<BuildOrder>(order.order);
                REQUIRE(build.unitType == "ARMFLASH");
                REQUIRE(static_cast<bool>(build.position == SimVector(4_ss, 5_ss, 6_ss)));
            }
        }

        SECTION("patrol order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(PatrolOrder({SimVector(7_ss, 8_ss, 9_ss)}), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& patrol = std::get<PatrolOrder>(order.order);
            REQUIRE(patrol.waypoints.size() == 1);
            REQUIRE(static_cast<bool>(patrol.waypoints[0] == SimVector(7_ss, 8_ss, 9_ss)));
        }

        SECTION("queued patrol order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(4),
                PlayerUnitCommand::IssueOrder(PatrolOrder({SimVector(1_ss, 2_ss, 3_ss)}), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& patrol = std::get<PatrolOrder>(order.order);
            REQUIRE(patrol.waypoints.size() == 1);
            REQUIRE(static_cast<bool>(patrol.waypoints[0] == SimVector(1_ss, 2_ss, 3_ss)));
        }

        SECTION("command fire order with unit target round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(CommandFireOrder(UnitId(12)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& commandFire = std::get<CommandFireOrder>(order.order);
            REQUIRE(commandFire.target == AttackTarget(UnitId(12)));
        }

        SECTION("command fire order with ground target round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(4),
                PlayerUnitCommand::IssueOrder(CommandFireOrder(SimVector(9_ss, 8_ss, 7_ss)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& commandFire = std::get<CommandFireOrder>(order.order);
            REQUIRE(commandFire.target == AttackTarget(SimVector(9_ss, 8_ss, 7_ss)));
        }

        SECTION("capture order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(CaptureOrder(UnitId(42)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& capture = std::get<CaptureOrder>(order.order);
            REQUIRE(capture.target == UnitId(42));
            // work state is execution-local, not wire state
            REQUIRE(capture.progress == 0);
            REQUIRE(!capture.workRequired);
            REQUIRE(capture.workTicks == 0);
        }

        SECTION("queued capture order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(3),
                PlayerUnitCommand::IssueOrder(CaptureOrder(UnitId(42)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& capture = std::get<CaptureOrder>(order.order);
            REQUIRE(capture.target == UnitId(42));
        }

        SECTION("load order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(7),
                PlayerUnitCommand::IssueOrder(LoadOrder(UnitId(55)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Immediate);
            const auto& load = std::get<LoadOrder>(order.order);
            REQUIRE(load.target == UnitId(55));
            // pickup state is execution-local, not wire state
            REQUIRE(load.attempts == 0);
            REQUIRE(load.slot == -1);
        }

        SECTION("queued load order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(7),
                PlayerUnitCommand::IssueOrder(LoadOrder(UnitId(55)), PlayerUnitCommand::IssueOrder::IssueKind::Queued));

            const auto& order = getIssueOrder(roundTrip(command));
            REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::IssueKind::Queued);
            const auto& load = std::get<LoadOrder>(order.order);
            REQUIRE(load.target == UnitId(55));
        }

        SECTION("unload order round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(7),
                PlayerUnitCommand::IssueOrder(UnloadOrder(SimVector(11_ss, 0_ss, 22_ss)), PlayerUnitCommand::IssueOrder::IssueKind::Immediate));

            const auto& order = getIssueOrder(roundTrip(command));
            const auto& unload = std::get<UnloadOrder>(order.order);
            REQUIRE(unload.destination.x == 11_ss);
            REQUIRE(unload.destination.y == 0_ss);
            REQUIRE(unload.destination.z == 22_ss);
        }

        SECTION("remove order command round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(6), PlayerUnitCommand::RemoveOrder{3});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            REQUIRE(unitCommand.unit == UnitId(6));
            const auto& removeOrder = std::get<PlayerUnitCommand::RemoveOrder>(unitCommand.command);
            REQUIRE(removeOrder.index == 3);
        }

        SECTION("cloak-on command round-trips")
        {
            // the retail CLOAK_ON order persists the unit's
            // cloak-requested flag; it is a state toggle, not an
            // order-queue entry
            auto command = PlayerUnitCommand(UnitId(6), PlayerUnitCommand::SetCloak{true});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            REQUIRE(unitCommand.unit == UnitId(6));
            const auto& setCloak = std::get<PlayerUnitCommand::SetCloak>(unitCommand.command);
            REQUIRE(setCloak.on);
        }

        SECTION("cloak-off command round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(9), PlayerUnitCommand::SetCloak{false});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            REQUIRE(unitCommand.unit == UnitId(9));
            const auto& setCloak = std::get<PlayerUnitCommand::SetCloak>(unitCommand.command);
            REQUIRE(!setCloak.on);
        }

        SECTION("toggle self-destruct command round-trips")
        {
            auto command = PlayerUnitCommand(UnitId(8), PlayerUnitCommand::ToggleSelfDestruct{});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            REQUIRE(unitCommand.unit == UnitId(8));
            REQUIRE(std::holds_alternative<PlayerUnitCommand::ToggleSelfDestruct>(unitCommand.command));
        }

        SECTION("modify weapon queue command round-trips")
        {
            // the commonattribs=8 stockpile button's click path: one
            // positive count per primary click
            auto command = PlayerUnitCommand(UnitId(9), PlayerUnitCommand::ModifyWeaponQueue{3});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            REQUIRE(unitCommand.unit == UnitId(9));
            const auto& modify = std::get<PlayerUnitCommand::ModifyWeaponQueue>(unitCommand.command);
            REQUIRE(modify.count == 3);
        }

        SECTION("modify weapon queue cancel round-trips")
        {
            // secondary clicks carry a negative count to dequeue
            auto command = PlayerUnitCommand(UnitId(9), PlayerUnitCommand::ModifyWeaponQueue{-2});

            auto result = roundTrip(command);
            const auto& unitCommand = std::get<PlayerUnitCommand>(result);
            const auto& modify = std::get<PlayerUnitCommand::ModifyWeaponQueue>(unitCommand.command);
            REQUIRE(modify.count == -2);
        }
    }
}
