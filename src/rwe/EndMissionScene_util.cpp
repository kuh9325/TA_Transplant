#include "EndMissionScene_util.h"

#include <algorithm>
#include <cstdio>

namespace rwe
{
    unsigned int debriefSelectedMissionIndex(unsigned int missionIndex, bool won, unsigned int missionCount)
    {
        if (missionCount == 0)
        {
            return 0;
        }
        unsigned int next = won && missionIndex + 1 < missionCount ? missionIndex + 1 : missionIndex;
        return std::min(next, missionCount - 1);
    }

    bool endmsnHasContinuation(bool hasCampaignContext, bool won, bool nextMissionExists)
    {
        if (!hasCampaignContext)
        {
            return false;
        }
        // 0x41f124-0x41f15f: won -> continuation iff MISSION(cur+1)
        // exists; lost -> always continuation
        return won ? nextMissionExists : true;
    }

    int debriefScore(int kills, unsigned int gameSeconds, double killMul, double timeMul)
    {
        int score = static_cast<int>(kills * killMul) + static_cast<int>(gameSeconds * timeMul);
        return std::max(score, 0);
    }

    std::string debriefGameTimeText(GameTime gameTime)
    {
        unsigned int seconds = gameTime.value / 30u;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", seconds / 3600u, (seconds / 60u) % 60u, seconds % 60u);
        return buffer;
    }
}
