#include <catch2/catch_test_macros.hpp>
#include <rwe/EndMissionScene_util.h>

namespace rwe
{
    TEST_CASE("debriefSelectedMissionIndex")
    {
        // retail ENDMSN preselection (TotalA.exe 0x41f2e6-0x41f311):
        // the selection advances past the finished mission on victory
        // and stays put on defeat.

        SECTION("victory preselects the next mission")
        {
            REQUIRE(debriefSelectedMissionIndex(0, true, 3) == 1);
            REQUIRE(debriefSelectedMissionIndex(1, true, 3) == 2);
        }

        SECTION("defeat keeps the current mission selected (retry)")
        {
            REQUIRE(debriefSelectedMissionIndex(0, false, 3) == 0);
            REQUIRE(debriefSelectedMissionIndex(2, false, 3) == 2);
        }

        SECTION("victory on the final mission clamps to the end")
        {
            REQUIRE(debriefSelectedMissionIndex(2, true, 3) == 2);
        }

        SECTION("empty campaign stays at zero")
        {
            REQUIRE(debriefSelectedMissionIndex(0, true, 0) == 0);
        }
    }

    TEST_CASE("endmsnHasContinuation")
    {
        // retail 0x41f115-0x41f1e4

        SECTION("defeat always offers continuation (retry)")
        {
            REQUIRE(endmsnHasContinuation(true, false, true));
            REQUIRE(endmsnHasContinuation(true, false, false));
        }

        SECTION("victory continues only when a next mission exists")
        {
            REQUIRE(endmsnHasContinuation(true, true, true));
            REQUIRE(!endmsnHasContinuation(true, true, false));
        }

        SECTION("no campaign context -> outcome0/MainMenu only")
        {
            REQUIRE(!endmsnHasContinuation(false, true, true));
            REQUIRE(!endmsnHasContinuation(false, false, true));
        }
    }

    TEST_CASE("debriefScore")
    {
        // retail score (TotalA.exe 0x41ddbe-0x41de12):
        // int(kills*killmul) + int(gameSeconds*timemul), clamped >= 0

        SECTION("stock AC01 multipliers: killmul 50, timemul 0")
        {
            REQUIRE(debriefScore(0, 0, 50, 0) == 0);
            REQUIRE(debriefScore(3, 600, 50, 0) == 150);
        }

        SECTION("time component")
        {
            REQUIRE(debriefScore(0, 60, 0, 10) == 600);
            REQUIRE(debriefScore(2, 60, 50, 10) == 700);
        }

        SECTION("negative totals clamp to zero")
        {
            REQUIRE(debriefScore(0, 0, -50, -10) == 0);
        }
    }

    TEST_CASE("debriefGameTimeText")
    {
        // retail "Game Time : %02d:%02d:%02d" (TotalA.exe 0x507794)
        REQUIRE(debriefGameTimeText(GameTime(0)) == "00:00:00");
        REQUIRE(debriefGameTimeText(GameTime(30)) == "00:00:01");
        REQUIRE(debriefGameTimeText(GameTime(30 * 61)) == "00:01:01");
        REQUIRE(debriefGameTimeText(GameTime(30 * 3661)) == "01:01:01");
    }
}
