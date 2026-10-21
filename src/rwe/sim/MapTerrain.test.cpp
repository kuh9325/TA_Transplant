#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <rwe/geometry/Line3x.h>
#include <rwe/sim/MapTerrain.h>

namespace rwe
{
    TEST_CASE("MapTerrain::intersectLine")
    {
        MapTerrain terrain(Grid<unsigned char>(65, 65, 64), 0_ss);

        SECTION("terminates for a line with NaN coordinates")
        {
            // a degenerate line (e.g. from a NaN projectile position)
            // must not send the cell walk marching forever
            auto nan = SimScalar(std::numeric_limits<float>::quiet_NaN());
            Line3x<SimScalar> line(
                SimVector(nan, MapTerrain::MaxHeight, nan),
                SimVector(nan, MapTerrain::MinHeight, nan));

            REQUIRE(!terrain.intersectLine(line));
        }

        SECTION("terminates for tryGetHeightAt with NaN coordinates")
        {
            auto nan = SimScalar(std::numeric_limits<float>::quiet_NaN());
            REQUIRE(!terrain.tryGetHeightAt(nan, nan));
        }
    }
}
