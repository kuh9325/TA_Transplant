#include <catch2/catch_test_macros.hpp>

#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/difficulty_util.h>

namespace rwe
{
    TEST_CASE("campaign difficulty stage mapping")
    {
        REQUIRE(campaignDifficultyStageIndex("Easy") == 0);
        REQUIRE(campaignDifficultyStageIndex("Medium") == 1);
        REQUIRE(campaignDifficultyStageIndex("Hard") == 2);

        REQUIRE(nextCampaignDifficulty("Easy") == "Medium");
        REQUIRE(nextCampaignDifficulty("Medium") == "Hard");
        REQUIRE(nextCampaignDifficulty("Hard") == "Easy");
    }

    TEST_CASE("difficulty stage drives the staged button state")
    {
        // mirrors the stock NEWGAME.GUI gadget:
        // a staged button with stages=3, text=Easy|Medium|Hard
        std::vector<UiStagedButton::StageInfo> stages{
            {nullptr, "Easy"},
            {nullptr, "Medium"},
            {nullptr, "Hard"}};
        UiStagedButton button(0, 0, 108, 20, stages, nullptr, nullptr);

        auto difficulty = nextCampaignDifficulty("Medium");
        button.setStage(campaignDifficultyStageIndex(difficulty));

        REQUIRE(difficulty == "Hard");
        REQUIRE(button.getStage() == 2);
        REQUIRE(button.getCurrentStageLabel() == "Hard");
    }
}
