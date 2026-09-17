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
    }
}
