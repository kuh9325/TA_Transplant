#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <rwe/CroppedViewport.h>
#include <rwe/Viewport.h>
#include <rwe/camera_util.h>
#include <rwe/math/Matrix4f.h>

namespace rwe
{
    TEST_CASE("ViewportService")
    {
        SECTION("toClipSpace")
        {
            SECTION("works in a simple case")
            {
                Viewport vp(0, 0, 8, 6);
                auto v = vp.toClipSpace(2, 2);
                REQUIRE(v.x == -0.5f);
                REQUIRE(v.y == Catch::Approx(0.33333333));
            }
        }

        SECTION("toViewportSpace")
        {
            SECTION("works in a simple case")
            {
                Viewport vp(0, 0, 8, 6);
                auto p = vp.toViewportSpace(-0.5f, 1.0f / 3.0f);
                REQUIRE(p.x == 2);
                REQUIRE(p.y == 2);
            }
        }

        SECTION("toOtherViewport")
        {
            SECTION("works in a simple case")
            {
                Viewport va(1, 2, 3, 4);
                Viewport vb(3, 4, 7, 9);

                auto p = va.toOtherViewport(vb, 2, 3);
                REQUIRE(p.x == 0);
                REQUIRE(p.y == 1);
            }

            SECTION("converts scene space to a cropped viewport with a non-zero origin")
            {
                // mirrors the GameScene world viewport: GUI chrome insets
                // the world viewport inside the window
                Viewport scene(0, 0, 1280, 800);
                CroppedViewport world(&scene, 128, 32, 0, 32);

                auto p = scene.toOtherViewport(world, 200, 100);
                REQUIRE(p.x == 72);
                REQUIRE(p.y == 68);
            }
        }

        SECTION("in-game menu coordinate mapping")
        {
            // mirrors GameScene::toInGameMenuCoordinates: the menu
            // renders through a fixed 640x480 orthographic space that
            // fills the window
            auto toMenuLogical = [](const Viewport& window, int x, int y) {
                auto inv = Matrix4f::inverseOrthographicProjection(0.0f, 640.0f, 480.0f, 0.0f, 100.0f, -100.0f);
                auto clip = screenToWorldRayUtil(inv, window.toClipSpace(x, y));
                return Point(static_cast<int>(clip.origin.x), static_cast<int>(clip.origin.y));
            };

            SECTION("640x480 window maps 1:1")
            {
                Viewport window(0, 0, 640, 480);
                // centre of the ARMOPT Resume gadget (16,148)-(136,168)
                auto p = toMenuLogical(window, 76, 158);
                REQUIRE(p.x == 76);
                REQUIRE(p.y == 158);
            }

            SECTION("1280x800 window maps into the same logical space")
            {
                Viewport window(0, 0, 1280, 800);
                // logical (76,158) renders at window (76*1280/640,
                // 158*800/480) = (152, 263); the inverse maps back
                // inside the Resume gadget rect (16,148)-(136,168)
                auto p = toMenuLogical(window, 152, 263);
                REQUIRE(p.x >= 16);
                REQUIRE(p.x <= 136);
                REQUIRE(p.y >= 148);
                REQUIRE(p.y <= 168);
            }

            SECTION("corners map proportionally, not letterboxed")
            {
                Viewport window(0, 0, 1280, 800);
                REQUIRE(toMenuLogical(window, 0, 0) == Point(0, 0));
                REQUIRE(toMenuLogical(window, 1279, 799) == Point(639, 479));
            }
        }
    }
}
