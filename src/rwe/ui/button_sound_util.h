#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace rwe
{
    /**
     * Per-screen gadget sound keys used by the retail frontend.
     * Each screen's gadget handler plays a fixed ALLSOUND.TDF key
     * for a given gadget, independent of the gadget's own name.
     */
    struct ScreenGadgetSound
    {
        const char* guiName;
        const char* gadgetName;
        const char* soundKey;
    };

    /**
     * The retail NEWGAME handler plays the SmlButton sound when
     * Difficulty is clicked, SideSelect when the ARM side is chosen,
     * and SideSelect2 when the CORE side is chosen. The Campaign and
     * Missions list boxes have no click sound in retail; the screen
     * plays SmlButton itself when it applies a campaign selection
     * programmatically (screen init and side changes).
     */
    inline const ScreenGadgetSound screenGadgetSounds[]{
        {"NEWGAME", "Difficulty", "SMLBUTTON"},
        {"NEWGAME", "Side0", "SIDESELECT"},
        {"NEWGAME", "Arm", "SIDESELECT"},
        {"NEWGAME", "Side1", "SIDESELECT2"},
        {"NEWGAME", "Core", "SIDESELECT2"},
        {"RESTART", "Difficulty", "OPTIONS"},
        // the PREFS cancel gadget plays Previous ([PREVIOUS]=button1);
        // every other PREFS gadget plays the screen's Options key
        {"PREFS", "CANCEL", "PREVIOUS"},
    };

    /**
     * Per-screen sound keys used by the retail in-game (F2) menu
     * screens. Each screen's gadget dispatcher plays one fixed
     * ALLSOUND.TDF key for every active control: TotalA.exe plays
     * "Options" at dispatch entry (EXITMENU 0x460800, RESTART 0x4607f0)
     * or inside each matched gadget branch (ARMOPT, PREFS, SPEEDSRT
     * 0x45ead0, HELP, BRIEFING 0x45f770, GAMEOPTIONS); the YESORNO
     * dispatcher plays "Exit" (0x4605c0). [OPTIONS]=butoptn,
     * [EXIT]=button2 in stock ALLSOUND.TDF.
     */
    inline const ScreenGadgetSound screenSounds[]{
        {"ARMOPT", "", "OPTIONS"},
        {"PREFS", "", "OPTIONS"},
        {"SPEEDSRT", "", "OPTIONS"},
        {"HELP", "", "OPTIONS"},
        {"BRIEFING", "", "OPTIONS"},
        {"EXITMENU", "", "OPTIONS"},
        {"RESTART", "", "OPTIONS"},
        {"GAMEOPTIONS", "", "OPTIONS"},
        {"YESORNO", "", "EXIT"},
    };

    /**
     * Returns the ALLSOUND.TDF key the retail frontend plays for any
     * active control on the given screen, or nullopt when the screen
     * has no fixed key.
     */
    inline std::optional<std::string> screenSoundKey(const std::string& guiName)
    {
        for (const auto& m : screenSounds)
        {
            if (guiName == m.guiName)
            {
                return std::string(m.soundKey);
            }
        }
        return std::nullopt;
    }

    /**
     * Returns the ALLSOUND.TDF key the retail frontend wires to a
     * (screen, gadget) pair, or nullopt when there is no mapping.
     */
    inline std::optional<std::string> screenGadgetSoundKey(const std::string& guiName, const std::string& gadgetName)
    {
        for (const auto& m : screenGadgetSounds)
        {
            if (guiName == m.guiName && gadgetName == m.gadgetName)
            {
                return std::string(m.soundKey);
            }
        }
        return std::nullopt;
    }

    /**
     * Returns the ALLSOUND.TDF block names to try, in order, for a
     * frontend button. Mirrors how the retail frontend wires sounds
     * to gadgets: the gadget's own name first, then any fixed mapping
     * for that (screen, gadget) pair, then the screen name, then
     * generic fallbacks keyed on screen and button size.
     */
    inline std::vector<std::string> deduceButtonSoundKeys(const std::string& guiName, const std::string& name, int width, int height)
    {
        std::vector<std::string> keys{name};

        if (name == "PrevMenu" || name == "PREVMENU")
        {
            keys.emplace_back("PREVIOUS");
        }
        if (name == "Start")
        {
            keys.emplace_back("BIGBUTTON");
        }
        if (auto key = screenGadgetSoundKey(guiName, name))
        {
            keys.push_back(*key);
        }
        if (auto key = screenSoundKey(guiName); key && std::find(keys.begin(), keys.end(), *key) == keys.end())
        {
            keys.push_back(*key);
        }

        keys.push_back(guiName);

        if (guiName == "SELMAP")
        {
            keys.emplace_back("SMALLBUTTON");
        }
        if (width == 96 && height == 20)
        {
            keys.emplace_back("BIGBUTTON");
        }

        return keys;
    }

    /**
     * The ALLSOUND.TDF key the retail NEWGAME screen plays when it
     * applies a campaign selection programmatically (screen init and
     * side changes). List box clicks themselves are silent in retail;
     * this sound is emitted by the screen's selection code, not by
     * the list box gadget.
     */
    inline constexpr const char* newGameCampaignSelectSoundKey = "SMLBUTTON";
}
