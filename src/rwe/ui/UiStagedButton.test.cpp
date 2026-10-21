#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_keycode.h>
#include <rwe/ui/UiStagedButton.h>

namespace rwe
{
    // Sprite handles are unused by the input paths under test; the
    // button is never rendered in these tests.
    std::unique_ptr<UiStagedButton> makeButton()
    {
        std::vector<UiStagedButton::StageInfo> stages;
        stages.emplace_back(nullptr, "OK");
        return std::make_unique<UiStagedButton>(0, 0, 100, 20, std::move(stages), nullptr, nullptr);
    }

    TEST_CASE("UiStagedButton enabled/disabled input")
    {
        auto button = makeButton();

        int clicks = 0;
        auto sub = button->onClick().subscribe([&clicks](const auto&) { ++clicks; });

        SECTION("an enabled button emits a click on mouse up")
        {
            button->mouseDown(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            button->mouseUp(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            REQUIRE(clicks == 1);
        }

        SECTION("a disabled button emits no click")
        {
            button->setEnabled(false);
            button->mouseDown(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            button->mouseUp(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            REQUIRE(clicks == 0);
        }

        SECTION("a disabled button ignores its quick key")
        {
            button->setQuickKey(SDLK_Y);
            button->setEnabled(false);
            button->keyDown(KeyEvent(SDLK_Y));
            REQUIRE(clicks == 0);
        }

        SECTION("a disabled button ignores space bar activation")
        {
            button->setEnabled(false);
            button->keyDown(KeyEvent(SDLK_SPACE));
            REQUIRE(clicks == 0);
        }

        SECTION("re-enabling restores clicks")
        {
            button->setEnabled(false);
            button->setEnabled(true);
            button->mouseDown(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            button->mouseUp(MouseButtonEvent(5, 5, MouseButtonEvent::MouseButton::Left));
            REQUIRE(clicks == 1);
        }
    }
}
