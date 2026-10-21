#include "CampaignProgress.h"

namespace rwe
{
    std::string CampaignProgress::key(const std::string& campaignName, const std::string& side)
    {
        // '\x1f' cannot appear in a campaign file name, so the pair
        // remains unambiguous; ARM and CORE runs stay independent.
        return campaignName + '\x1f' + side;
    }

    void CampaignProgress::resetCampaign(const std::string& campaignName, const std::string& side, unsigned int missionCount)
    {
        Run run;
        run.status.assign(missionCount, CampaignMissionStatus::Unplayed);
        runs[key(campaignName, side)] = std::move(run);
    }

    void CampaignProgress::recordResult(
        const std::string& campaignName,
        const std::string& side,
        unsigned int missionIndex,
        bool won,
        unsigned int missionCount)
    {
        auto& run = runs[key(campaignName, side)];
        if (run.status.size() < missionCount)
        {
            run.status.resize(missionCount, CampaignMissionStatus::Unplayed);
        }
        if (missionIndex < run.status.size())
        {
            run.status[missionIndex] = won ? CampaignMissionStatus::Won : CampaignMissionStatus::Lost;
        }
    }

    CampaignMissionStatus CampaignProgress::missionStatus(const std::string& campaignName, const std::string& side, unsigned int missionIndex) const
    {
        auto it = runs.find(key(campaignName, side));
        if (it == runs.end() || missionIndex >= it->second.status.size())
        {
            return CampaignMissionStatus::Unplayed;
        }
        return it->second.status[missionIndex];
    }

    const CampaignProgress::Run* CampaignProgress::findRun(const std::string& campaignName, const std::string& side) const
    {
        auto it = runs.find(key(campaignName, side));
        return it == runs.end() ? nullptr : &it->second;
    }

    void CampaignProgress::restoreRun(
        const std::string& campaignName,
        const std::string& side,
        std::vector<CampaignMissionStatus>&& status)
    {
        Run run;
        run.status = std::move(status);
        runs[key(campaignName, side)] = std::move(run);
    }
}
