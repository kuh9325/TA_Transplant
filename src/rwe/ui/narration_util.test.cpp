#include <catch2/catch_test_macros.hpp>

#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/narration_util.h>

namespace rwe
{
    TEST_CASE("campaign narration stage mapping")
    {
        REQUIRE(campaignNarrationStageIndex(true) == 1);
        REQUIRE(campaignNarrationStageIndex(false) == 0);
    }

    TEST_CASE("narration state drives the staged button state")
    {
        // mirrors the stock MSNBRIEF.GUI SHUTUP gadget:
        // a staged button with stages=2, text=No Narration|Narration
        std::vector<UiStagedButton::StageInfo> stages{
            {nullptr, "No Narration"},
            {nullptr, "Narration"}};
        UiStagedButton button(0, 0, 120, 20, stages, nullptr, nullptr);

        button.setStage(campaignNarrationStageIndex(true));
        REQUIRE(button.getStage() == 1);
        REQUIRE(button.getCurrentStageLabel() == "Narration");

        button.setStage(campaignNarrationStageIndex(false));
        REQUIRE(button.getStage() == 0);
        REQUIRE(button.getCurrentStageLabel() == "No Narration");
    }
}
