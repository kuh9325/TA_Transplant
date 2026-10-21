#include <catch2/catch_test_macros.hpp>
#include <rwe/io/gui/gui.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <rwe/optional_io.h>

namespace fs = std::filesystem;

namespace rwe
{
    void requireCommonsEqual(const GuiCommon& a, const GuiCommon& b)
    {
        REQUIRE(a.id == b.id);
        REQUIRE(a.assoc == b.assoc);
        REQUIRE(a.name == b.name);
        REQUIRE(a.xpos == b.xpos);
        REQUIRE(a.ypos == b.ypos);
        REQUIRE(a.width == b.width);
        REQUIRE(a.height == b.height);
        REQUIRE(a.attribs == b.attribs);
        REQUIRE(a.colorf == b.colorf);
        REQUIRE(a.colorb == b.colorb);
        REQUIRE(a.textureNumber == b.textureNumber);
        REQUIRE(a.fontNumber == b.fontNumber);
        REQUIRE(a.active == b.active);
        REQUIRE(a.commonAttribs == b.commonAttribs);
        REQUIRE(a.help == b.help);
    }

    void requireVersionsEqual(const GuiVersion& a, const GuiVersion& b)
    {
        REQUIRE(a.majorVersion == b.majorVersion);
        REQUIRE(a.minorVersion == b.minorVersion);
        REQUIRE(a.revision == b.revision);
    }

    void requireEntriesEqual(const GuiEntry& a, const GuiEntry& b)
    {
        requireCommonsEqual(a.common, b.common);
        REQUIRE(a.panel == b.panel);
        REQUIRE(a.crDefault == b.crDefault);
        REQUIRE(a.escdefault == b.escdefault);
        REQUIRE(a.defaultFocus == b.defaultFocus);
        REQUIRE(a.totalGadgets == b.totalGadgets);

        REQUIRE(!!a.version == !!b.version);
        if (a.version && b.version)
        {
            requireVersionsEqual(*(a.version), *(b.version));
        }

        REQUIRE(a.status == b.status);
        REQUIRE(a.text == b.text);
        REQUIRE(a.quickKey == b.quickKey);
        REQUIRE(a.grayedOut == b.grayedOut);
        REQUIRE(a.stages == b.stages);
    }

    TEST_CASE("parseGui")
    {
        SECTION("understands a simple GUI file")
        {

            std::string input = R"TDF(
[GADGET0]
    {
    [COMMON]
        {
        id=0;
        assoc=205;
        name=Mainmenu.GUI;
        xpos=0;
        ypos=0;
        width=640;
        height=480;
        attribs=52685;
        colorf=52685;
        colorb=52685;
        texturenumber=0;
        fontnumber=-51;
        active=1;
        commonattribs=-51;
        help=;
        }
    totalgadgets=6;
    [VERSION]
        {
        major=-51;
        minor=-51;
        revision=-51;
        }
    panel=;
    crdefault=;
    escdefault=;
    defaultfocus=SINGLE;
    }
[GADGET1]
    {
    [COMMON]
        {
        id=1;
        assoc=126;
        name=SINGLE;
        xpos=139;
        ypos=393;
        width=96;
        height=20;
        attribs=2;
        colorf=0;
        colorb=0;
        texturenumber=0;
        fontnumber=0;
        active=1;
        commonattribs=54;
        help=;
        }
    status=0;
    text=SINGLE;
    quickkey=83;
    grayedout=0;
    stages=0;
    }
)TDF";

            GuiEntry gadget0;
            gadget0.common.id = GuiElementType::Panel;
            gadget0.common.assoc = 205;
            gadget0.common.name = "Mainmenu.GUI";
            gadget0.common.xpos = 0;
            gadget0.common.ypos = 0;
            gadget0.common.width = 640;
            gadget0.common.height = 480;
            gadget0.common.attribs = 52685;
            gadget0.common.colorf = 52685;
            gadget0.common.colorb = 52685;
            gadget0.common.textureNumber = 0;
            gadget0.common.fontNumber = -51;
            gadget0.common.active = true;
            gadget0.common.commonAttribs = -51;
            gadget0.common.help = "";

            gadget0.totalGadgets = 6;

            gadget0.version = GuiVersion(-51, -51, -51);

            gadget0.panel = "";
            gadget0.crDefault = "";
            gadget0.escdefault = "";
            gadget0.defaultFocus = "SINGLE";


            GuiEntry gadget1;
            gadget1.common.id = GuiElementType::Button;
            gadget1.common.assoc = 126;
            gadget1.common.name = "SINGLE";
            gadget1.common.xpos = 139;
            gadget1.common.ypos = 393;
            gadget1.common.width = 96;
            gadget1.common.height = 20;
            gadget1.common.attribs = 2;
            gadget1.common.colorf = 0;
            gadget1.common.colorb = 0;
            gadget1.common.textureNumber = 0;
            gadget1.common.fontNumber = 0;
            gadget1.common.active = true;
            gadget1.common.commonAttribs = 54;
            gadget1.common.help = "";

            gadget1.status = 0;
            gadget1.text = "SINGLE";
            gadget1.quickKey = 83;
            gadget1.grayedOut = false;
            gadget1.stages = 0;

            std::vector<GuiEntry> expected{gadget0, gadget1};

            auto gui = parseGui(parseListTdfFromString(input));
            if (!gui)
            {
                FAIL();
            }

            REQUIRE(gui->size() == expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                requireEntriesEqual((*gui)[i], expected[i]);
            }
        }
    }

    TEST_CASE("the real GAMEOPTIONS.GUI is a bare GameSettings shell")
    {
        // the stock skirmish "GameSettings" dialog (F2 -> Mission on a
        // non-campaign game) ships only in post-3.1 archives and
        // contains a transparent header plus a single OK gadget; all
        // settings text is exe-drawn. Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto bytes = vfs.readFile("guis/GAMEOPTIONS.GUI");
        REQUIRE(bytes.has_value());

        auto gui = parseGuiFromBytes(*bytes);
        REQUIRE(gui.has_value());
        REQUIRE(gui->size() == 2);

        const auto& header = (*gui)[0];
        REQUIRE(header.common.id == GuiElementType::Panel);
        REQUIRE(header.common.name == "HEADER");
        REQUIRE(header.common.xpos == 0);
        REQUIRE(header.common.ypos == 0);
        REQUIRE(header.common.width == 269);
        REQUIRE(header.common.height == 351);
        REQUIRE(header.totalGadgets == 1);
        REQUIRE(header.panel.has_value());
        REQUIRE(header.panel->empty());
        REQUIRE(header.crDefault == "OK");
        REQUIRE(header.escdefault == "OK");

        const auto& ok = (*gui)[1];
        REQUIRE(ok.common.id == GuiElementType::Button);
        REQUIRE(ok.common.name == "OK");
        REQUIRE(ok.common.xpos == 85);
        REQUIRE(ok.common.ypos == 311);
        REQUIRE(ok.common.width == 95);
        REQUIRE(ok.common.height == 20);
        REQUIRE(ok.common.active);
        REQUIRE(ok.text == "OK");

        // no scrollbar/slider gadgets exist; anything resembling the
        // thin fragments seen in earlier broken builds must come from
        // elsewhere
        for (const auto& entry : *gui)
        {
            REQUIRE(entry.common.id != GuiElementType::ScrollBar);
        }
    }

    TEST_CASE("the real ENDMSN.GUI debrief layout parses")
    {
        // the campaign result screen driven by the endgame state
        // machine (TotalA.exe 0x41f0a0): a 640x480 panel with the
        // Missions listbox, KNOB scrollbar, Start/LoadGame/SaveGame/
        // MainMenu buttons, and a staged Difficulty control.
        // Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto bytes = vfs.readFile("guis/ENDMSN.GUI");
        REQUIRE(bytes.has_value());

        auto gui = parseGuiFromBytes(*bytes);
        REQUIRE(gui.has_value());
        REQUIRE(gui->size() == 9);

        const auto& header = (*gui)[0];
        REQUIRE(header.common.id == GuiElementType::Panel);
        REQUIRE(header.common.xpos == 0);
        REQUIRE(header.common.ypos == 0);
        REQUIRE(header.common.width == 640);
        REQUIRE(header.common.height == 480);
        REQUIRE(header.totalGadgets == 8);
        REQUIRE(header.crDefault == "Start");

        std::unordered_map<std::string, const GuiEntry*> byName;
        for (const auto& entry : *gui)
        {
            byName[entry.common.name] = &entry;
        }

        REQUIRE(byName.count("Missions") == 1);
        REQUIRE(byName.at("Missions")->common.id == GuiElementType::ListBox);
        REQUIRE(byName.at("Missions")->common.xpos == 16);
        REQUIRE(byName.at("Missions")->common.ypos == 385);

        REQUIRE(byName.count("KNOB") == 1);
        REQUIRE(byName.at("KNOB")->common.id == GuiElementType::ScrollBar);

        REQUIRE(byName.count("Start") == 1);
        REQUIRE(byName.at("Start")->common.id == GuiElementType::Button);
        REQUIRE(byName.at("Start")->common.xpos == 460);
        REQUIRE(byName.at("Start")->common.ypos == 315);

        REQUIRE(byName.count("LoadGame") == 1);
        REQUIRE(byName.count("SaveGame") == 1);
        REQUIRE(byName.count("MainMenu") == 1);

        REQUIRE(byName.count("Difficulty") == 1);
        REQUIRE(byName.at("Difficulty")->common.id == GuiElementType::Button);
        REQUIRE(byName.at("Difficulty")->common.ypos == 442);
    }
}
