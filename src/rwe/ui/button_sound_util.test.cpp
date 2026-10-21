#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <rwe/ui/button_sound_util.h>

namespace rwe
{
    TEST_CASE("NEWGAME Difficulty maps to the SMLBUTTON sound key")
    {
        // The retail NEWGAME gadget handler plays "SmlButton"
        // ([SMLBUTTON] -> butmain4) when Difficulty is clicked.
        auto keys = deduceButtonSoundKeys("NEWGAME", "Difficulty", 108, 20);
        REQUIRE(keys == std::vector<std::string>{"Difficulty", "SMLBUTTON", "NEWGAME"});
    }

    TEST_CASE("NEWGAME ARM side gadgets map to SIDESELECT")
    {
        // The retail NEWGAME handler plays "SideSelect" for Side0/Arm.
        auto keys = deduceButtonSoundKeys("NEWGAME", "Side0", 132, 106);
        REQUIRE(keys == std::vector<std::string>{"Side0", "SIDESELECT", "NEWGAME"});

        keys = deduceButtonSoundKeys("NEWGAME", "Arm", 95, 20);
        REQUIRE(keys == std::vector<std::string>{"Arm", "SIDESELECT", "NEWGAME"});
    }

    TEST_CASE("NEWGAME CORE side gadgets map to SIDESELECT2")
    {
        // The retail NEWGAME handler plays "SideSelect2" for Side1/Core.
        auto keys = deduceButtonSoundKeys("NEWGAME", "Side1", 129, 104);
        REQUIRE(keys == std::vector<std::string>{"Side1", "SIDESELECT2", "NEWGAME"});

        keys = deduceButtonSoundKeys("NEWGAME", "Core", 95, 20);
        REQUIRE(keys == std::vector<std::string>{"Core", "SIDESELECT2", "NEWGAME"});
    }

    TEST_CASE("NEWGAME list boxes have no click sound")
    {
        // In retail, clicking a Campaign or Missions list box row is
        // silent: the gadgets get no selection callback that plays a
        // sound. The screen emits SmlButton itself when it applies a
        // campaign selection programmatically.
        auto key = screenGadgetSoundKey("NEWGAME", "Campaign");
        REQUIRE(!key);

        key = screenGadgetSoundKey("NEWGAME", "Missions");
        REQUIRE(!key);
    }

    TEST_CASE("NEWGAME programmatic campaign selection uses SMLBUTTON")
    {
        // Retail plays "smlbutton" ([SMLBUTTON] -> butmain4) from the
        // screen code that applies the campaign selection: init and
        // side changes.
        REQUIRE(std::string(newGameCampaignSelectSoundKey) == "SMLBUTTON");
    }

    TEST_CASE("Difficulty on other screens does not map to SMLBUTTON")
    {
        // Retail wires Difficulty per screen; SKIRMISH uses the
        // screen-level SKIRMISH key instead.
        auto keys = deduceButtonSoundKeys("SKIRMISH", "Difficulty", 108, 20);
        REQUIRE(keys == std::vector<std::string>{"Difficulty", "SKIRMISH"});
    }

    TEST_CASE("RESTART Difficulty maps to the OPTIONS sound key")
    {
        // The retail RESTART gadget handler plays "Options"
        // ([OPTIONS] -> butoptn) when Difficulty is clicked;
        // unlike SKIRMISH there is no screen-level RESTART key,
        // so without the explicit mapping the button is silent.
        auto key = screenGadgetSoundKey("RESTART", "Difficulty");
        REQUIRE(key);
        REQUIRE(*key == "OPTIONS");

        auto keys = deduceButtonSoundKeys("RESTART", "Difficulty", 108, 20);
        REQUIRE(keys == std::vector<std::string>{"Difficulty", "OPTIONS", "RESTART"});
    }

    TEST_CASE("RESTART keeps OPTIONS ahead of its screen-name fallback")
    {
        // precedence: gadget name, screen mapping, screen name
        auto keys = deduceButtonSoundKeys("RESTART", "Difficulty", 108, 20);
        auto options = std::find(keys.begin(), keys.end(), "OPTIONS");
        auto restart = std::find(keys.begin(), keys.end(), "RESTART");
        REQUIRE(options != keys.end());
        REQUIRE(options < restart);
    }

    TEST_CASE("PrevMenu falls back to PREVIOUS")
    {
        auto keys = deduceButtonSoundKeys("SINGLE", "PrevMenu", 119, 19);
        REQUIRE(keys == std::vector<std::string>{"PrevMenu", "PREVIOUS", "SINGLE"});
    }

    TEST_CASE("Start falls back to BIGBUTTON")
    {
        auto keys = deduceButtonSoundKeys("NEWGAME", "Start", 95, 29);
        REQUIRE(keys == std::vector<std::string>{"Start", "BIGBUTTON", "NEWGAME"});
    }

    TEST_CASE("SELMAP buttons fall back to SMALLBUTTON")
    {
        auto keys = deduceButtonSoundKeys("SELMAP", "PrevMenu", 96, 20);
        REQUIRE(keys == std::vector<std::string>{"PrevMenu", "PREVIOUS", "SELMAP", "SMALLBUTTON", "BIGBUTTON"});
    }

    TEST_CASE("96x20 buttons fall back to BIGBUTTON")
    {
        auto keys = deduceButtonSoundKeys("FOO", "Bar", 96, 20);
        REQUIRE(keys == std::vector<std::string>{"Bar", "FOO", "BIGBUTTON"});
    }

    TEST_CASE("ordinary buttons only try the gadget and screen names")
    {
        auto keys = deduceButtonSoundKeys("FOO", "Bar", 40, 20);
        REQUIRE(keys == std::vector<std::string>{"Bar", "FOO"});
    }

    TEST_CASE("in-game menu screens fall back to their retail sound key")
    {
        // TotalA.exe plays "Options" for every active control on the
        // F2 menu screens and "Exit" on YESORNO; the screen-level key
        // covers gadgets without a more specific mapping
        for (const auto* screen : {"ARMOPT", "PREFS", "SPEEDSRT", "HELP", "BRIEFING", "EXITMENU", "RESTART", "GAMEOPTIONS"})
        {
            auto keys = deduceButtonSoundKeys(screen, "SomeGadget", 95, 20);
            auto options = std::find(keys.begin(), keys.end(), "OPTIONS");
            REQUIRE(options != keys.end());
            INFO(screen);
        }

        auto yesNoKeys = deduceButtonSoundKeys("YESORNO", "CHOICE1", 96, 20);
        REQUIRE(std::find(yesNoKeys.begin(), yesNoKeys.end(), "EXIT") != yesNoKeys.end());
        REQUIRE(std::find(yesNoKeys.begin(), yesNoKeys.end(), "OPTIONS") == yesNoKeys.end());
    }

    TEST_CASE("screen sound key yields to more specific mappings")
    {
        // PREFS CANCEL plays Previous in retail, ahead of the
        // screen-level Options key
        auto keys = deduceButtonSoundKeys("PREFS", "CANCEL", 95, 20);
        auto previous = std::find(keys.begin(), keys.end(), "PREVIOUS");
        auto options = std::find(keys.begin(), keys.end(), "OPTIONS");
        REQUIRE(previous != keys.end());
        REQUIRE(options != keys.end());
        REQUIRE(previous < options);
    }

    TEST_CASE("screen sound key does not duplicate a gadget mapping")
    {
        // RESTART Difficulty's per-gadget OPTIONS and the RESTART
        // screen key coincide; the key list must not retry it twice
        auto keys = deduceButtonSoundKeys("RESTART", "Difficulty", 108, 20);
        REQUIRE(keys == std::vector<std::string>{"Difficulty", "OPTIONS", "RESTART"});
    }

    TEST_CASE("non-menu screens have no screen-level sound key")
    {
        auto keys = deduceButtonSoundKeys("MAINMENU", "Exit", 96, 20);
        REQUIRE(std::find(keys.begin(), keys.end(), "OPTIONS") == keys.end());
    }
}
