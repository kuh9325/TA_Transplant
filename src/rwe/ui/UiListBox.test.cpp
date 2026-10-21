#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rwe/ui/UiListBox.h>

namespace rwe
{
    TEST_CASE("UiListBox clip region")
    {
        auto font = std::make_shared<SpriteSeries>();
        UiListBox listBox(34, 314, 318, 142, font);

        SECTION("defaults to the component rect")
        {
            REQUIRE(listBox.contains(34, 314));
            REQUIRE(listBox.contains(351, 455));
            REQUIRE(!listBox.contains(34, 460));
            REQUIRE(!listBox.contains(360, 400));
        }

        SECTION("drives visible rows, scroll range and hit region")
        {
            // a later-declared list box claims the band below y=386
            listBox.setClipRect(Rectangle2f::fromTopLeft(34.0f, 314.0f, 318.0f, 72.0f));

            for (unsigned int i = 0; i < 7; ++i)
            {
                listBox.appendItem("item " + std::to_string(i));
            }

            // 72px at 12px pitch gives 6 lines, and the
            // TA-rect fudge factor leaves 5 visible rows
            REQUIRE(listBox.getViewportPercent() == Catch::Approx(5.0f / 7.0f));

            // the clipped band is not part of the hit region
            REQUIRE(listBox.contains(34, 385));
            REQUIRE(!listBox.contains(34, 400));

            // overflow rows are still reachable by scrolling
            listBox.mouseWheel(MouseWheelEvent(0, -1));
            REQUIRE(listBox.getScrollPercent() > 0.0f);
        }
    }
}
