#include <catch2/catch_test_macros.hpp>

#include <rwe/sim/MissionInitialCommand.h>
#include <rwe/util/match.h>

namespace rwe
{
    namespace
    {
        const MissionInitialCommand& onlyCommand(const ParsedInitialMission& parsed)
        {
            REQUIRE(parsed.commands.size() == 1);
            return parsed.commands.front();
        }
    }

    TEST_CASE("parseInitialMission")
    {
        SECTION("empty input produces no commands")
        {
            auto parsed = parseInitialMission("");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.empty());
            REQUIRE(parsed.unsupportedCommands.empty());
            REQUIRE(!parsed.suppressImplicitSelectable);
            REQUIRE(!parsed.hasExplicitSelectable);
        }

        SECTION("p x z parses as a patrol destination")
        {
            auto parsed = parseInitialMission("p 1750 1828");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(cmd.destination);
            REQUIRE(cmd.destination->x == SimScalar(1750));
            REQUIRE(cmd.destination->z == SimScalar(1828));
            REQUIRE(parsed.errors.empty());
        }

        SECTION("uppercase P parses as patrol")
        {
            auto parsed = parseInitialMission("P 100 200");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(cmd.destination);
            REQUIRE(cmd.destination->x == SimScalar(100));
        }

        SECTION("bare p patrols in place")
        {
            auto parsed = parseInitialMission("p");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(!cmd.destination);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("p with unparseable coordinates patrols in place")
        {
            auto parsed = parseInitialMission("p nowhere fast");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(!cmd.destination);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("p x z dwell parses the optional dwell in seconds")
        {
            auto parsed = parseInitialMission("p 1750 1828 90");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(cmd.destination);
            REQUIRE(cmd.dwellTicks);
            REQUIRE(*cmd.dwellTicks == 2700);
        }

        SECTION("p swallows arguments past the third like retail's sscanf")
        {
            auto parsed = parseInitialMission("p 100 200 30 400 500");
            const auto& cmd = std::get<MissionCommandPatrol>(onlyCommand(parsed));
            REQUIRE(cmd.destination);
            REQUIRE(cmd.dwellTicks);
            REQUIRE(*cmd.dwellTicks == 900);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("m x z parses as a move destination")
        {
            auto parsed = parseInitialMission("m 300 400");
            const auto& cmd = std::get<MissionCommandMove>(onlyCommand(parsed));
            REQUIRE(cmd.destination.x == SimScalar(300));
            REQUIRE(cmd.destination.z == SimScalar(400));
        }

        SECTION("m with missing coordinate is an error")
        {
            auto parsed = parseInitialMission("m 300");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.size() == 1);
        }

        SECTION("a x z parses as an attack position")
        {
            auto parsed = parseInitialMission("a 500 600");
            const auto& cmd = std::get<MissionCommandAttackPosition>(onlyCommand(parsed));
            REQUIRE(cmd.target.x == SimScalar(500));
            REQUIRE(cmd.target.z == SimScalar(600));
        }

        SECTION("a <unit type> parses as ATTACKUTYPE")
        {
            auto parsed = parseInitialMission("a SOMETYPE");
            const auto& cmd = std::get<MissionCommandAttackType>(onlyCommand(parsed));
            REQUIRE(cmd.unitType == "SOMETYPE");
            REQUIRE(parsed.unsupportedCommands.empty());
        }

        SECTION("g <ident> parses as a guard command")
        {
            auto parsed = parseInitialMission("g TOWER1");
            const auto& cmd = std::get<MissionCommandGuard>(onlyCommand(parsed));
            REQUIRE(cmd.targetIdent == "TOWER1");
        }

        SECTION("g without a name is an error")
        {
            auto parsed = parseInitialMission("g");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.size() == 1);
        }

        SECTION("o fire move sets standing orders")
        {
            auto parsed = parseInitialMission("o 0 0");
            const auto& cmd = std::get<MissionCommandSetStance>(onlyCommand(parsed));
            REQUIRE(cmd.fireOrder == UnitFireOrders::HoldFire);
            REQUIRE(cmd.moveOrder == UnitMoveOrders::HoldPosition);
        }

        SECTION("o 1 2 maps to return-fire and roam")
        {
            // retail `o <fire> <move>`
            auto parsed = parseInitialMission("o 1 2");
            const auto& cmd = std::get<MissionCommandSetStance>(onlyCommand(parsed));
            REQUIRE(cmd.fireOrder == UnitFireOrders::ReturnFire);
            REQUIRE(cmd.moveOrder == UnitMoveOrders::Roam);
        }

        SECTION("o with one argument sets fire order and retains move")
        {
            auto parsed = parseInitialMission("o 1");
            const auto& cmd = std::get<MissionCommandSetStance>(onlyCommand(parsed));
            REQUIRE(cmd.fireOrder == UnitFireOrders::ReturnFire);
            REQUIRE(!cmd.moveOrder);
        }

        SECTION("o with no arguments is an error")
        {
            auto parsed = parseInitialMission("o");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.size() == 1);
        }

        SECTION("w seconds converts to ticks at 30 per second")
        {
            auto parsed = parseInitialMission("w 30");
            const auto& cmd = std::get<MissionCommandWait>(onlyCommand(parsed));
            REQUIRE(cmd.ticks == 900);
        }

        SECTION("w 3600 produces a two-hour wait")
        {
            auto parsed = parseInitialMission("w 3600");
            const auto& cmd = std::get<MissionCommandWait>(onlyCommand(parsed));
            REQUIRE(cmd.ticks == 108000);
        }

        SECTION("w consumes trailing tokens within its segment")
        {
            auto parsed = parseInitialMission("w 300 b CORFAV");
            const auto& cmd = std::get<MissionCommandWait>(onlyCommand(parsed));
            REQUIRE(cmd.ticks == 9000);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("w tolerates the retail second integer argument")
        {
            auto parsed = parseInitialMission("w 3600 0");
            const auto& cmd = std::get<MissionCommandWait>(onlyCommand(parsed));
            REQUIRE(cmd.ticks == 108000);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("w accepts fractional seconds like retail's %f")
        {
            auto parsed = parseInitialMission("w 1.5");
            const auto& cmd = std::get<MissionCommandWait>(onlyCommand(parsed));
            REQUIRE(cmd.ticks == 45);
        }

        SECTION("w with a negative duration is an error")
        {
            auto parsed = parseInitialMission("w -5");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.size() == 1);
        }

        SECTION("w with no duration is an error")
        {
            auto parsed = parseInitialMission("w");
            REQUIRE(parsed.commands.empty());
            REQUIRE(parsed.errors.size() == 1);
        }

        SECTION("wa with a name waits on that mission object")
        {
            auto parsed = parseInitialMission("wa TOWER1");
            const auto& cmd = std::get<MissionCommandWaitForAttack>(onlyCommand(parsed));
            REQUIRE(cmd.targetIdent);
            REQUIRE(*cmd.targetIdent == "TOWER1");
        }

        SECTION("bare wa waits on the ordered unit")
        {
            auto parsed = parseInitialMission("wa");
            const auto& cmd = std::get<MissionCommandWaitForAttack>(onlyCommand(parsed));
            REQUIRE(!cmd.targetIdent);
        }

        SECTION("s parses as make-selectable")
        {
            auto parsed = parseInitialMission("s");
            REQUIRE(std::holds_alternative<MissionCommandMakeSelectable>(onlyCommand(parsed)));
            REQUIRE(parsed.hasExplicitSelectable);
            REQUIRE(!parsed.suppressImplicitSelectable);
        }

        SECTION("d parses as self-destruct and suppresses the implicit make-selectable")
        {
            auto parsed = parseInitialMission("d");
            REQUIRE(std::holds_alternative<MissionCommandSelfDestruct>(onlyCommand(parsed)));
            REQUIRE(parsed.suppressImplicitSelectable);
            REQUIRE(!parsed.hasExplicitSelectable);
        }

        SECTION("multiple commands parse in order")
        {
            auto parsed = parseInitialMission("o 0 0, w 10, p 100 200");
            REQUIRE(parsed.commands.size() == 3);
            REQUIRE(std::holds_alternative<MissionCommandSetStance>(parsed.commands[0]));
            REQUIRE(std::holds_alternative<MissionCommandWait>(parsed.commands[1]));
            REQUIRE(std::holds_alternative<MissionCommandPatrol>(parsed.commands[2]));
        }

        SECTION("a trailing comma is permitted")
        {
            auto parsed = parseInitialMission("p 100 200,");
            REQUIRE(parsed.commands.size() == 1);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("b <type> <count> parses as a build command")
        {
            auto parsed = parseInitialMission("b CORAK 5");
            const auto& cmd = std::get<MissionCommandBuild>(onlyCommand(parsed));
            REQUIRE(cmd.unitType == "CORAK");
            REQUIRE(cmd.count == 5);
            REQUIRE(!cmd.coordX);
            REQUIRE(!cmd.coordZ);
        }

        SECTION("b <type> defaults the count to 1")
        {
            auto parsed = parseInitialMission("b CORCRUS");
            const auto& cmd = std::get<MissionCommandBuild>(onlyCommand(parsed));
            REQUIRE(cmd.unitType == "CORCRUS");
            REQUIRE(cmd.count == 1);
            REQUIRE(!cmd.coordX);
        }

        SECTION("b <type> <count> <x> <z> parses a build position")
        {
            auto parsed = parseInitialMission("b corint 1 1601 1883");
            const auto& cmd = std::get<MissionCommandBuild>(onlyCommand(parsed));
            REQUIRE(cmd.unitType == "corint");
            REQUIRE(cmd.count == 1);
            REQUIRE(cmd.coordX);
            REQUIRE(*cmd.coordX == 1601.0f);
            REQUIRE(cmd.coordZ);
            REQUIRE(*cmd.coordZ == 1883.0f);
        }

        SECTION("b <type> <x> <z> stores x in the count slot and z in the first coord slot like retail")
        {
            // retail's sscanf consumes the second argument as %d and
            // the third as the first %f; stock mobile-build scripts
            // use this shape for build coordinates
            auto parsed = parseInitialMission("b CORRL 1986 804");
            const auto& cmd = std::get<MissionCommandBuild>(onlyCommand(parsed));
            REQUIRE(cmd.unitType == "CORRL");
            REQUIRE(cmd.count == 1986);
            REQUIRE(cmd.coordX);
            REQUIRE(*cmd.coordX == 804.0f);
            REQUIRE(!cmd.coordZ);
        }

        SECTION("bw parses as a build-weapon directive")
        {
            auto parsed = parseInitialMission("bw 2");
            const auto& cmd = std::get<MissionCommandBuildWeapon>(onlyCommand(parsed));
            REQUIRE(cmd.count == 2);
        }

        SECTION("i <ident> parses as a join-group command")
        {
            auto parsed = parseInitialMission("i JENN");
            const auto& cmd = std::get<MissionCommandJoinGroup>(onlyCommand(parsed));
            REQUIRE(cmd.targetIdent == "JENN");
        }

        SECTION("u x z parses as an unload command")
        {
            auto parsed = parseInitialMission("u 4682 2662");
            const auto& cmd = std::get<MissionCommandUnload>(onlyCommand(parsed));
            REQUIRE(cmd.destination);
            REQUIRE(cmd.destination->x == SimScalar(4682));
        }

        SECTION("bare u unloads at the current position")
        {
            auto parsed = parseInitialMission("u");
            const auto& cmd = std::get<MissionCommandUnload>(onlyCommand(parsed));
            REQUIRE(!cmd.destination);
            REQUIRE(parsed.errors.empty());
        }

        SECTION("unknown verbs are retained as unsupported")
        {
            auto parsed = parseInitialMission("z FOO");
            const auto& cmd = std::get<MissionCommandUnsupported>(onlyCommand(parsed));
            REQUIRE(cmd.raw == "z FOO");
            REQUIRE(parsed.unsupportedCommands.size() == 1);
        }

        SECTION("negative coordinates parse")
        {
            auto parsed = parseInitialMission("m -10 -20");
            const auto& cmd = std::get<MissionCommandMove>(onlyCommand(parsed));
            REQUIRE(cmd.destination.x == SimScalar(-10));
            REQUIRE(cmd.destination.z == SimScalar(-20));
        }

        SECTION("parsing is deterministic")
        {
            auto a = parseInitialMission("o 1 2, w 5, p 10 20, wa BASE, s");
            auto b = parseInitialMission("o 1 2, w 5, p 10 20, wa BASE, s");
            REQUIRE(a.commands.size() == b.commands.size());
            REQUIRE(a.errors == b.errors);
            REQUIRE(a.unsupportedCommands == b.unsupportedCommands);
        }
    }
}
