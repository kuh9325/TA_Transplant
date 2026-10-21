#pragma once

namespace rwe
{
    /**
     * Stage order matches the stock MSNBRIEF.GUI SHUTUP gadget:
     * a staged button with stages=2 and text=No Narration|Narration.
     * Stage 1 ("Narration", lit indicator) is shown while narration
     * plays; stage 0 ("No Narration", dark) when nothing is playing.
     * Retail initializes the gadget to stage 1 on briefing entry,
     * resets it to 0 once no voice is active, and only ever stops
     * playback — SHUTUP never restarts narration.
     */
    inline unsigned int campaignNarrationStageIndex(bool narrationPlaying)
    {
        return narrationPlaying ? 1 : 0;
    }
}
