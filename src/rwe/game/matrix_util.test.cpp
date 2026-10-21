#include <catch2/catch_test_macros.hpp>
#include <rwe/game/matrix_util.h>
#include <rwe/math/Vector3f.h>

namespace rwe
{
    namespace
    {
        // mirror of GameScene.cpp's camera math (computeView/computeProjection)
        Matrix4f testView(const Vector3f& cameraPosition)
        {
            auto translation = Matrix4f::translation(-cameraPosition);
            auto rotation = Matrix4f::rotationToAxes(Vector3f(1.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, -1.0f), Vector3f(0.0f, 1.0f, 0.0f));
            return rotation * translation;
        }

        Matrix4f testProjection(float width, float height)
        {
            float halfWidth = width / 2.0f;
            float halfHeight = height / 2.0f;
            auto cabinet = Matrix4f::cabinetProjection(0.0f, 0.5f);
            return Matrix4f::orthographicProjection(-halfWidth, halfWidth, -halfHeight, halfHeight, -1000.0f, 1000.0f) * cabinet;
        }

        ViewFrustum testFrustum(const Vector3f& cameraPosition, float w, float h)
        {
            return extractFrustum(testProjection(w, h) * testView(cameraPosition));
        }
    }

    TEST_CASE("extractFrustum: unit at camera center is inside")
    {
        auto f = testFrustum(Vector3f(1000.0f, 0.0f, -500.0f), 672.0f, 536.0f);
        REQUIRE(sphereInFrustum(f, Vector3f(1000.0f, 50.0f, -500.0f), 320.0f));
    }

    TEST_CASE("extractFrustum: unit inside left half of view")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // viewport half-extents are 336 x 268; x=-200 is well inside
        REQUIRE(sphereInFrustum(f, Vector3f(-200.0f, 0.0f, 0.0f), 10.0f));
    }

    TEST_CASE("extractFrustum: unit far right of view is culled")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // half-width 336; centre at +700 with radius 10 is fully outside
        REQUIRE(!sphereInFrustum(f, Vector3f(700.0f, 0.0f, 0.0f), 10.0f));
    }

    TEST_CASE("extractFrustum: unit far below view is culled")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // view z is negated (world +z maps to screen down) -- a unit at
        // z=+500 is 232 world units below the 268 half-height
        REQUIRE(!sphereInFrustum(f, Vector3f(0.0f, 0.0f, 500.0f), 10.0f));
        REQUIRE(!sphereInFrustum(f, Vector3f(0.0f, 0.0f, -500.0f), 10.0f));
    }

    TEST_CASE("extractFrustum: partially visible unit at the edge stays")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // centre 400 units off the right edge but radius 320 overlaps
        REQUIRE(sphereInFrustum(f, Vector3f(400.0f, 0.0f, 0.0f), 320.0f));
    }

    TEST_CASE("extractFrustum: unit beyond the depth slab is culled")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // near/far is +-1000 along world Y; a unit at y=3000 is out
        REQUIRE(!sphereInFrustum(f, Vector3f(0.0f, 3000.0f, 0.0f), 10.0f));
    }

    TEST_CASE("extractFrustum: high-altitude unit visible via cabinet shear")
    {
        auto f = testFrustum(Vector3f(0.0f, 0.0f, 0.0f), 672.0f, 536.0f);
        // cabinet projection shears screen position by world height;
        // a plane at y=300 stays well inside the +-1000 depth slab
        REQUIRE(sphereInFrustum(f, Vector3f(0.0f, 300.0f, 0.0f), 60.0f));
    }

    TEST_CASE("extractFrustum: camera panned far away culls the original area")
    {
        auto f = testFrustum(Vector3f(-2000.0f, 0.0f, -2000.0f), 672.0f, 536.0f);
        REQUIRE(!sphereInFrustum(f, Vector3f(0.0f, 0.0f, 0.0f), 320.0f));
    }
}
