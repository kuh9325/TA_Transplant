#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace rwe
{
    /**
     * Per-mission completion state within one campaign run.
     * Mirrors the retail 25-entry status array at game ctx +0x391cf:
     * 'U' unplayed, 'W' won, 'L' lost (TotalA.exe 0x41da30 initialises
     * the array to 'U' on new-campaign Start; the mission-teardown stats
     * builder at 0x41dc20 stamps 'W'/'L' at ctx+0x391ab).
     */
    enum class CampaignMissionStatus : char
    {
        Unplayed = 'U',
        Won = 'W',
        Lost = 'L',
    };

    /**
     * Session-scoped campaign result state, shared across scenes.
     * Retail keeps this in the campaign frontend context; it is
     * in-memory only and is re-initialised each time a new campaign
     * run is started from the campaign screen (TotalA.exe 0x477bc8).
     * Persistence to disk belongs to Save/Load (C6) and is out of scope.
     *
     * Retail does not hard-lock missions: both the NEWGAME and ENDMSN
     * mission listboxes list every mission in the descriptor and Start
     * launches the selected entry (0x477c54, 0x41efba -> 0x435c00).
     * The status array is informational -- it drives the per-mission
     * W/L/U colour marks and the post-mission preselection.
     */
    class CampaignProgress
    {
    public:
        /** Statuses for one campaign run, indexed by [MISSIONn] ordinal. */
        struct Run
        {
            std::vector<CampaignMissionStatus> status;
        };

    private:
        std::unordered_map<std::string, Run> runs;

    public:
        /** Wipes the run back to all-unplayed (retail 0x41da30). */
        void resetCampaign(const std::string& campaignName, const std::string& side, unsigned int missionCount);

        /** Stamps 'W'/'L' at missionIndex (retail 0x41dc20: 'L'+0x0B=='W'). */
        void recordResult(
            const std::string& campaignName,
            const std::string& side,
            unsigned int missionIndex,
            bool won,
            unsigned int missionCount);

        CampaignMissionStatus missionStatus(const std::string& campaignName, const std::string& side, unsigned int missionIndex) const;

        /** The run's status array, or nullopt when the campaign was never started. */
        const Run* findRun(const std::string& campaignName, const std::string& side) const;

        /** Save/load: install a restored run's status array wholesale. */
        void restoreRun(
            const std::string& campaignName,
            const std::string& side,
            std::vector<CampaignMissionStatus>&& status);

    private:
        static std::string key(const std::string& campaignName, const std::string& side);
    };
}
