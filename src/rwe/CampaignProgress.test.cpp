#include <catch2/catch_test_macros.hpp>
#include <rwe/CampaignProgress.h>

namespace rwe
{
    TEST_CASE("CampaignProgress")
    {
        CampaignProgress progress;

        SECTION("missions are unplayed before the campaign is started")
        {
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Unplayed);
            REQUIRE(progress.findRun("Arm Campaign", "ARM") == nullptr);
        }

        SECTION("resetCampaign initialises all missions to unplayed")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 3);
            for (unsigned int i = 0; i < 3; ++i)
            {
                REQUIRE(progress.missionStatus("Arm Campaign", "ARM", i) == CampaignMissionStatus::Unplayed);
            }
        }

        SECTION("recordResult stamps W and L")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 3);
            progress.recordResult("Arm Campaign", "ARM", 0, true, 3);
            progress.recordResult("Arm Campaign", "ARM", 1, false, 3);

            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Won);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 1) == CampaignMissionStatus::Lost);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 2) == CampaignMissionStatus::Unplayed);
        }

        SECTION("a defeat does not unlock anything; statuses stay per mission")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 3);
            progress.recordResult("Arm Campaign", "ARM", 0, false, 3);

            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Lost);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 1) == CampaignMissionStatus::Unplayed);
        }

        SECTION("replay overwrites the prior result")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 2);
            progress.recordResult("Arm Campaign", "ARM", 0, false, 2);
            progress.recordResult("Arm Campaign", "ARM", 0, true, 2);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Won);
        }

        SECTION("ARM and CORE campaigns are independent")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 2);
            progress.resetCampaign("Core Campaign", "CORE", 2);
            progress.recordResult("Arm Campaign", "ARM", 0, true, 2);

            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Won);
            REQUIRE(progress.missionStatus("Core Campaign", "CORE", 0) == CampaignMissionStatus::Unplayed);
        }

        SECTION("same display name on different sides stays separate")
        {
            progress.resetCampaign("Campaign", "ARM", 2);
            progress.resetCampaign("Campaign", "CORE", 2);
            progress.recordResult("Campaign", "CORE", 0, true, 2);

            REQUIRE(progress.missionStatus("Campaign", "ARM", 0) == CampaignMissionStatus::Unplayed);
            REQUIRE(progress.missionStatus("Campaign", "CORE", 0) == CampaignMissionStatus::Won);
        }

        SECTION("resetCampaign wipes a prior run")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 2);
            progress.recordResult("Arm Campaign", "ARM", 0, true, 2);
            progress.resetCampaign("Arm Campaign", "ARM", 2);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Unplayed);
        }

        SECTION("recordResult grows an existing run to the descriptor size")
        {
            // mirrors a --campaign direct launch stamping a mission the
            // in-session run had not yet sized for
            progress.recordResult("Arm Campaign", "ARM", 4, true, 6);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 4) == CampaignMissionStatus::Won);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 3) == CampaignMissionStatus::Unplayed);
        }

        SECTION("out-of-range indexes are ignored")
        {
            progress.resetCampaign("Arm Campaign", "ARM", 2);
            progress.recordResult("Arm Campaign", "ARM", 5, true, 2);
            REQUIRE(progress.missionStatus("Arm Campaign", "ARM", 0) == CampaignMissionStatus::Unplayed);
        }
    }
}
