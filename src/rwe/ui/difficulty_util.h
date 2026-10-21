#pragma once

#include <optional>
#include <string>

namespace rwe
{
    /**
     * Stage order matches the stock NEWGAME.GUI Difficulty gadget:
     * a staged button with stages=3 and text=Easy|Medium|Hard.
     * Each stage carries its own label and indicator sprite frame.
     */
    inline unsigned int campaignDifficultyStageIndex(const std::string& difficulty)
    {
        if (difficulty == "Easy")
        {
            return 0;
        }
        if (difficulty == "Medium")
        {
            return 1;
        }
        return 2;
    }

    inline const std::string& campaignDifficultyForStage(unsigned int stage)
    {
        static const std::string names[3]{"Easy", "Medium", "Hard"};
        return names[stage > 2 ? 2 : stage];
    }

    inline const std::string& nextCampaignDifficulty(const std::string& difficulty)
    {
        return campaignDifficultyForStage((campaignDifficultyStageIndex(difficulty) + 1) % 3);
    }
}
