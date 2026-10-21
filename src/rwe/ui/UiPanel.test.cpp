#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <rwe/io/gui/gui.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiPanel.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/UiSurface.h>
#include <rwe/ui/UiTextBox.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace fs = std::filesystem;

namespace rwe
{
    namespace
    {
        /**
         * A gadget that runs a callback on keyDown, letting a test
         * mutate the owning panel (append/remove children) from
         * inside the broadcast loop, as real activations do.
         */
        class CallbackGadget : public UiComponent
        {
        public:
            std::function<void(KeyEvent)> onKey;

            CallbackGadget(int x, int y, unsigned int w, unsigned int h)
                : UiComponent(x, y, w, h)
            {
            }

            bool keyDown(KeyEvent event) override
            {
                if (onKey)
                {
                    onKey(event);
                }
                return true;
            }
        };
    }

    TEST_CASE("UiPanel overlapping list boxes")
    {
        auto font = std::make_shared<SpriteSeries>();
        UiPanel panel(0, 0, 640, 480);

        // mirrors the stock NEWGAME.GUI layout, where the
        // declared Campaign rect overlaps the Missions rect
        panel.appendChild(std::make_unique<UiListBox>(34, 314, 318, 142, font));
        panel.appendChild(std::make_unique<UiListBox>(34, 386, 318, 78, font));

        auto& campaign = dynamic_cast<UiListBox&>(*panel.getChildren()[0]);
        auto& missions = dynamic_cast<UiListBox&>(*panel.getChildren()[1]);

        SECTION("the earlier list box yields the shared band to the later one")
        {
            REQUIRE(campaign.getClipRect() == Rectangle2f::fromTLBR(314.0f, 34.0f, 386.0f, 352.0f));
            REQUIRE(missions.getClipRect() == Rectangle2f::fromTopLeft(34.0f, 386.0f, 318.0f, 78.0f));
        }

        SECTION("clicks in the shared band reach the later list box")
        {
            missions.appendItem("m1");
            missions.appendItem("m2");

            std::optional<unsigned int> selected;
            auto sub = missions.selectedIndex().subscribe([&selected](const auto& v) {
                selected = v;
            });

            panel.mouseDown(MouseButtonEvent(40, 390, MouseButtonEvent::MouseButton::Left));

            REQUIRE(selected == std::optional<unsigned int>(0));
        }
    }

    TEST_CASE("UiPanel escdefault/crdefault activation")
    {
        UiPanel panel(0, 0, 640, 480);
        panel.setName("TESTPANEL");

        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "OK");
        auto button = std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
        button->setName("OK");
        panel.appendChild(std::move(button));

        panel.setEscDefault("OK");
        panel.setCrDefault("OK");

        std::optional<GroupMessage> received;
        auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

        SECTION("activating the escdefault control emits its group message")
        {
            auto found = panel.find<UiStagedButton>(panel.getEscDefault());
            REQUIRE(found);
            // retail dispatch focuses the default control, then SPACE
            // activates the focused control via the panel broadcast
            panel.setFocusByName(panel.getEscDefault());
            panel.keyDown(KeyEvent(SDLK_SPACE));

            REQUIRE(received);
            REQUIRE(received->controlName == "OK");
            REQUIRE(std::holds_alternative<ActivateMessage>(received->message));
        }

        SECTION("a disabled escdefault control does not activate")
        {
            auto found = panel.find<UiStagedButton>(panel.getEscDefault());
            REQUIRE(found);
            found->get().setEnabled(false);
            panel.setFocusByName(panel.getEscDefault());
            panel.keyDown(KeyEvent(SDLK_SPACE));

            REQUIRE(!received);
        }

        SECTION("an empty default finds no control")
        {
            UiPanel bare(0, 0, 640, 480);
            REQUIRE(bare.getEscDefault().empty());
            REQUIRE(bare.getCrDefault().empty());
            REQUIRE(!bare.find<UiStagedButton>(bare.getEscDefault()));
        }
    }

    TEST_CASE("UiPanel clicks pass through display-only gadgets")
    {
        // mirrors the stock ARMOPT.GUI layout: the first gadget is a
        // full-panel background PictureBox, and a text label overlaps
        // the button row; neither may swallow clicks
        UiPanel panel(0, 128, 128, 352);
        panel.setName("ARMOPT");

        panel.appendChild(std::make_unique<UiSurface>(0, 0, 128, 362));
        panel.appendChild(std::make_unique<UiLabel>(0, 10, 128, 40, "GAME OPTIONS", nullptr));

        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "Resume");
        auto button = std::make_unique<UiStagedButton>(16, 20, 120, 20, std::move(stages), nullptr, nullptr);
        button->setName("OK");
        panel.appendChild(std::move(button));

        std::optional<GroupMessage> received;
        auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

        SECTION("a click under a covering surface and label reaches the button")
        {
            // logical point inside both the label and the button,
            // in panel coordinates (panel origin 0,128)
            panel.mouseDown(MouseButtonEvent(40, 128 + 30, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(40, 128 + 30, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "OK");
            REQUIRE(std::holds_alternative<ActivateMessage>(received->message));
        }

        SECTION("a click on the surface but outside the button emits nothing")
        {
            panel.mouseDown(MouseButtonEvent(40, 128 + 300, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(40, 128 + 300, MouseButtonEvent::MouseButton::Left));

            REQUIRE(!received);
        }
    }

    TEST_CASE("UiPanel skips hidden gadgets")
    {
        // retail gadget active byte (+0x29): inactive gadgets render
        // nothing and receive no mouse, key or message dispatch
        UiPanel panel(0, 0, 640, 480);
        panel.setName("TESTPANEL");

        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "OK");
        auto button = std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
        button->setName("OK");
        button->setQuickKey(SDLK_O);
        auto* buttonPtr = button.get();
        panel.appendChild(std::move(button));

        std::optional<GroupMessage> received;
        auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

        SECTION("a hidden gadget ignores clicks")
        {
            buttonPtr->setHidden(true);
            panel.mouseDown(MouseButtonEvent(10, 10, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(10, 10, MouseButtonEvent::MouseButton::Left));
            REQUIRE(!received);
        }

        SECTION("a hidden gadget ignores its quickkey")
        {
            buttonPtr->setHidden(true);
            panel.keyDown(KeyEvent(SDLK_O));
            REQUIRE(!received);
        }

        SECTION("a re-shown gadget accepts clicks again")
        {
            buttonPtr->setHidden(true);
            buttonPtr->setHidden(false);
            panel.mouseDown(MouseButtonEvent(10, 10, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(10, 10, MouseButtonEvent::MouseButton::Left));
            REQUIRE(received);
            REQUIRE(received->controlName == "OK");
        }
    }

    TEST_CASE("UiPanel keyDown survives children mutation mid-broadcast")
    {
        // Human-QA regression: a child's activation handler can remove
        // or replace the panel's children while keyDown is still
        // broadcasting (dialog paging, message-driven rebuilds, and
        // owners retiring the dialog's children list)
        UiPanel panel(0, 0, 640, 480);
        panel.setName("TESTPANEL");

        std::vector<std::string> visited;

        auto mutator = std::make_unique<CallbackGadget>(0, 0, 10, 10);
        mutator->setName("MUTATE");
        panel.appendChild(std::move(mutator));

        auto tail = std::make_unique<CallbackGadget>(0, 0, 10, 10);
        tail->setName("TAIL");
        tail->onKey = [&visited](KeyEvent) { visited.push_back("TAIL"); };
        panel.appendChild(std::move(tail));

        auto& mutateRef = panel.find<CallbackGadget>("MUTATE")->get();

        SECTION("a child that removes siblings does not corrupt the loop")
        {
            mutateRef.onKey = [&panel](KeyEvent) { panel.removeChildrenWithPrefix("MUTATE"); };
            panel.keyDown(KeyEvent(SDLK_X));
            // the mutator destroyed itself; TAIL must still be visited
            REQUIRE(visited == std::vector<std::string>{"TAIL"});
        }

        SECTION("a child that appends a sibling does not corrupt the loop")
        {
            mutateRef.onKey = [&panel](KeyEvent) {
                auto extra = std::make_unique<CallbackGadget>(0, 0, 10, 10);
                extra->setName("EXTRA");
                panel.appendChild(std::move(extra));
            };
            panel.keyDown(KeyEvent(SDLK_X));
            REQUIRE(visited == std::vector<std::string>{"TAIL"});
            REQUIRE(panel.find<CallbackGadget>("EXTRA"));
        }
    }

    TEST_CASE("UiPanel broadcast completes when the owner pops the panel mid-dispatch")
    {
        // reproduces the save-dialog crash shape at panel level: the
        // Enter/quickkey activation pops the dialog off its owner
        // stack while UiPanel::keyDown is still iterating it; the
        // owner defers destruction until the broadcast returns
        std::vector<std::unique_ptr<UiPanel>> stack;
        std::vector<std::unique_ptr<UiPanel>> retired;

        stack.push_back(std::make_unique<UiPanel>(0, 0, 640, 480));
        stack.back()->setName("SAVELIST");

        unsigned int activations = 0;
        stack.back()->groupMessages().subscribe([&](const GroupMessage&) {
            // the owner pops the activated dialog; the panel survives
            // in retired until keyDown unwinds
            ++activations;
            retired.push_back(std::move(stack.back()));
            stack.pop_back();
        });

        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "Load");
        auto button = std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
        button->setName("LOAD");
        button->setQuickKey(SDLK_O);
        stack.back()->appendChild(std::move(button));

        stack.back()->keyDown(KeyEvent(SDLK_O));

        REQUIRE(activations == 1);
        REQUIRE(stack.empty());
        REQUIRE(retired.size() == 1);
        // safe to destroy once the outermost dispatch has returned
        retired.clear();
        SUCCEED();
    }

    TEST_CASE("UiPanel clears focus when the focused child is removed")
    {
        // the save dialog's list refresh and skirmish roster rebuilds
        // remove children; a stale focusedChild would dangle on the
        // next mouseUp/mouseWheel
        UiPanel panel(0, 0, 640, 480);
        panel.setName("TESTPANEL");

        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "OK");
        auto button = std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
        button->setName("OK");
        panel.appendChild(std::move(button));

        panel.setFocusByName("OK");
        panel.removeChildrenWithPrefix("OK");

        // must not dereference the removed child
        panel.mouseUp(MouseButtonEvent(10, 10, MouseButtonEvent::MouseButton::Left));
        panel.mouseWheel(MouseWheelEvent(0, 1));
        panel.focus();
        panel.unfocus();

        SUCCEED();
    }

    TEST_CASE("UiPanel focus tolerates no focused child")
    {
        UiPanel panel(0, 0, 640, 480);
        // previously these dereferenced an empty optional
        panel.focus();
        panel.unfocus();
        SUCCEED();
    }

    TEST_CASE("UiPanel suppresses sibling quickkeys while a TextBox owns focus")
    {
        // Human-QA regression: typing 'c' into the save dialog's
        // GAMENAME fired CANCEL's quickkey and closed the dialog.
        // Retail suppresses button quickkeys while a type-3 TextBox
        // holds keyboard focus (TotalA.exe 0x4a70a7), so 'c' is text.
        UiPanel panel(0, 0, 640, 480);
        panel.setName("TESTPANEL");

        auto font = std::make_shared<SpriteSeries>();
        auto box = std::make_unique<UiTextBox>(0, 0, 100, 20, "", font, 20);
        box->setName("GAMENAME");
        panel.appendChild(std::move(box));

        unsigned int cancels = 0;
        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "Cancel");
        auto cancel = std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
        cancel->setName("CANCEL");
        cancel->setQuickKey(SDLK_C);
        cancel->onClick().subscribe([&](const ButtonClickEvent&) { ++cancels; });
        panel.appendChild(std::move(cancel));

        auto& boxRef = panel.find<UiTextBox>("GAMENAME")->get();

        SECTION("focused TextBox consumes a quickkey letter as text")
        {
            panel.setFocusByName("GAMENAME");
            REQUIRE(panel.keyDown(KeyEvent(SDLK_C)));
            REQUIRE(boxRef.getText() == "c");
            REQUIRE(cancels == 0);
        }

        SECTION("Enter on a focused TextBox only drops focus")
        {
            panel.setFocusByName("GAMENAME");
            // retail: Enter is consumed by the TextBox to defocus --
            // it does not reach crdefault or quickkey handling
            REQUIRE(panel.keyDown(KeyEvent(SDLK_RETURN)));
            REQUIRE_FALSE(boxRef.isFocused());
            // focus released: the next 'c' hits CANCEL's quickkey
            REQUIRE(panel.keyDown(KeyEvent(SDLK_C)));
            REQUIRE(cancels == 1);
        }

        SECTION("quickkey still fires when the TextBox is not focused")
        {
            REQUIRE(panel.keyDown(KeyEvent(SDLK_C)));
            REQUIRE(boxRef.getText().empty());
            REQUIRE(cancels == 1);
        }
    }

    TEST_CASE("clickable labels emit ActivateMessage")
    {
        // the in-game briefing's TextRegion/MOREBAR are labels that act
        // as pager controls; an ordinary label stays display-only
        UiPanel panel(0, 0, 640, 480);
        panel.setName("BRIEFING");

        auto label = std::make_unique<UiLabel>(24, 17, 360, 148, "body", nullptr);
        label->setName("TextRegion");
        auto* labelPtr = label.get();
        panel.appendChild(std::move(label));

        std::optional<GroupMessage> received;
        auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

        SECTION("a plain label ignores clicks")
        {
            REQUIRE(!labelPtr->acceptsMouseInput());
            panel.mouseDown(MouseButtonEvent(30, 20, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(30, 20, MouseButtonEvent::MouseButton::Left));
            REQUIRE(!received);
        }

        SECTION("a clickable label activates on release")
        {
            labelPtr->setClickable(true);
            REQUIRE(labelPtr->acceptsMouseInput());
            panel.mouseDown(MouseButtonEvent(30, 20, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(30, 20, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->topic == "BRIEFING");
            REQUIRE(received->controlName == "TextRegion");
            REQUIRE(std::holds_alternative<ActivateMessage>(received->message));
        }
    }

    TEST_CASE("UiPanel real ARMOPT.GUI click routing")
    {
        // end-to-end probe against authentic TA data: parses the real
        // ARMOPT.GUI gadget table, builds a panel with the same child
        // types/rects/order UiFactory produces, and clicks the Resume
        // gadget. Skips silently when stock data is not mounted;
        // set RWE_DATA_PATH to a TA data directory to enable it.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto guiBytes = vfs.readFile("guis/ARMOPT.GUI");
        REQUIRE(guiBytes);
        auto entries = parseGuiFromBytes(*guiBytes);
        REQUIRE(entries);
        REQUIRE(entries->size() > 1);

        const auto& panelEntry = (*entries)[0];
        UiPanel panel(
            panelEntry.common.xpos,
            panelEntry.common.ypos,
            panelEntry.common.width,
            panelEntry.common.height);
        panel.setName("ARMOPT");

        // mirror componentFromGuiEntry's type mapping; sprites/fonts
        // are unused by the input paths under test
        for (std::size_t i = 1; i < entries->size(); ++i)
        {
            const auto& e = (*entries)[i];
            std::unique_ptr<UiComponent> c;
            switch (e.common.id)
            {
                case GuiElementType::Button:
                {
                    std::vector<UiStagedButton::StageInfo> stages;
                    stages.emplace_back(nullptr, e.text.value_or(""));
                    auto b = std::make_unique<UiStagedButton>(e.common.xpos, e.common.ypos, e.common.width, e.common.height, std::move(stages), nullptr, nullptr);
                    c = std::move(b);
                    break;
                }
                case GuiElementType::Label:
                    c = std::make_unique<UiLabel>(e.common.xpos, e.common.ypos, e.common.width, e.common.height, e.text.value_or(""), nullptr);
                    break;
                case GuiElementType::Surface:
                case GuiElementType::PictureBox:
                    c = std::make_unique<UiSurface>(e.common.xpos, e.common.ypos, e.common.width, e.common.height);
                    break;
                default:
                    c = std::make_unique<UiComponent>(0, 0, 1, 1);
                    break;
            }
            c->setName(e.common.name);
            panel.appendChild(std::move(c));
        }

        std::optional<GroupMessage> received;
        auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

        SECTION("clicking the Resume gadget emits OK")
        {
            const auto& ok = *std::find_if(entries->begin(), entries->end(), [](const auto& e) { return e.common.name == "OK"; });
            auto cx = ok.common.xpos + (ok.common.width / 2);
            auto cy = panelEntry.common.ypos + ok.common.ypos + (ok.common.height / 2);

            panel.mouseDown(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "OK");
            REQUIRE(std::holds_alternative<ActivateMessage>(received->message));
        }

        SECTION("clicking the Options gadget emits PREFS")
        {
            const auto& prefs = *std::find_if(entries->begin(), entries->end(), [](const auto& e) { return e.common.name == "PREFS"; });
            auto cx = prefs.common.xpos + (prefs.common.width / 2);
            auto cy = panelEntry.common.ypos + prefs.common.ypos + (prefs.common.height / 2);

            panel.mouseDown(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "PREFS");
        }
    }

    TEST_CASE("real SPEEDSRT.GUI gadget table")
    {
        // verifies the stock INTERFACE options screen exposes the
        // gadgets the in-game menu wires: LEFTCLICK staged button
        // (Button Interface), RESTORE/UNDO transaction buttons, and
        // horizontal scrollbar sliders marked by attribs bit 1
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto guiBytes = vfs.readFile("guis/SPEEDSRT.GUI");
        REQUIRE(guiBytes);
        auto entries = parseGuiFromBytes(*guiBytes);
        REQUIRE(entries);

        auto find = [&](const std::string& name) {
            return std::find_if(entries->begin(), entries->end(), [&](const auto& e) { return e.common.name == name; });
        };

        auto leftClick = find("LEFTCLICK");
        REQUIRE(leftClick != entries->end());
        REQUIRE(leftClick->common.id == GuiElementType::Button);
        REQUIRE(leftClick->stages.value_or(0) == 2);
        REQUIRE(leftClick->text.value_or("") == "Left Click|Right Click");

        REQUIRE(find("RESTORE") != entries->end());
        REQUIRE(find("UNDO") != entries->end());

        // every scrollbar gadget on this screen is horizontal
        for (const auto& e : *entries)
        {
            if (e.common.id == GuiElementType::ScrollBar)
            {
                REQUIRE((e.common.attribs & GuiScrollHorizontalAttrib) != 0);
            }
        }
        REQUIRE(std::any_of(entries->begin(), entries->end(), [](const auto& e) { return e.common.id == GuiElementType::ScrollBar; }));
    }
}
