#pragma once

#include <rwe/sim/GameTime.h>
#include <string>

namespace rwe
{
    /**
     * Retail ENDMSN mission-list preselection (TotalA.exe 0x41f2e6-
     * 0x41f311): the listbox selection is set to the current mission
     * index, then advanced by one when the mission was won. When the
     * won mission was the campaign's last, the selection clamps.
     */
    unsigned int debriefSelectedMissionIndex(unsigned int missionIndex, bool won, unsigned int missionCount);

    /**
     * Retail ENDMSN outcome mode (TotalA.exe 0x41f115-0x41f1e4):
     * the screen offers continuation (outcome1 background + enabled
     * Start + populated mission list) only inside a campaign context,
     * and on victory only when a further MISSIONn exists in the
     * descriptor. Defeat always continues -- Start replays the
     * selection, which defaults to the mission just lost.
     * A campaign-final victory (or a non-campaign result) drops to the
     * outcome0/MainMenu-only presentation.
     */
    bool endmsnHasContinuation(bool hasCampaignContext, bool won, bool nextMissionExists);

    /**
     * Retail debrief score (TotalA.exe 0x41ddbe-0x41de12):
     *   score = int(kills * killmul) + int(gameSeconds * timemul)
     * clamped to >= 0. killmul/timemul are the OTA header keys
     * (default 50 / 0).
     */
    int debriefScore(int kills, unsigned int gameSeconds, double killMul, double timeMul);

    /**
     * Retail debrief "Game Time : %02d:%02d:%02d" (TotalA.exe 0x507794).
     * GameTime is in sim ticks (30/sec).
     */
    std::string debriefGameTimeText(GameTime gameTime);

    /**
     * One row of the ENDMSN stats table drawn over the outcome PCX's
     * baked column headers: Name | Kills | Losses | Energy Produced |
     * Metal Produced | Excess Energy | Excess Metal | Score.
     * Produced/excess fields are the integer-truncated totals retail
     * stores as doubles (TotalA.exe 0x41dd86-0x41ddbb).
     */
    struct DebriefPlayerRow
    {
        std::string name;
        int kills;
        int losses;
        int energyProduced;
        int metalProduced;
        int energyExcess;
        int metalExcess;
        int score;
    };
}
