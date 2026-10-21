#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <rwe/io/tnt/TntArchive.h>
#include <rwe/util/SpanStream.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace rwe
{
    std::vector<char> makeTntWithMinimap(unsigned int mmWidth, unsigned int mmHeight, const std::vector<char>& pixels)
    {
        TntHeader header{};
        header.magicNumber = TntMagicNumber;
        header.minimapOffset = sizeof(TntHeader);

        TntMinimapHeader mmHeader{mmWidth, mmHeight};

        std::vector<char> bytes(sizeof(TntHeader));
        std::memcpy(bytes.data(), &header, sizeof(TntHeader));
        const char* mmBytes = reinterpret_cast<const char*>(&mmHeader);
        bytes.insert(bytes.end(), mmBytes, mmBytes + sizeof(TntMinimapHeader));
        bytes.insert(bytes.end(), pixels.begin(), pixels.end());
        return bytes;
    }

    TntMinimapInfo readMinimapInfo(const std::vector<char>& bytes)
    {
        SpanStream stream(bytes.data(), bytes.size());
        TntArchive tnt(&stream);
        return tnt.readMinimap();
    }

    TEST_CASE("readMinimap trims padded canvas down to authored content")
    {
        SECTION("bottom rows padded with 0x00")
        {
            // AC05 analogue: 8x5 authored image in an 8x8 canvas,
            // the pad run is palette index 0 instead of 0x64
            std::vector<char> pixels(8 * 8, 0x00);
            for (unsigned int y = 0; y < 5; ++y)
            {
                for (unsigned int x = 0; x < 8; ++x)
                {
                    pixels[y * 8 + x] = 0x41;
                }
            }
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 8);
            REQUIRE(mm.height == 5);
        }

        SECTION("right columns padded with 0x00")
        {
            // AC01 analogue: authored image occupies the left 5 of 8
            // columns across the full height
            std::vector<char> pixels(8 * 8, 0x00);
            for (unsigned int y = 0; y < 8; ++y)
            {
                for (unsigned int x = 0; x < 5; ++x)
                {
                    pixels[y * 8 + x] = 0x41;
                }
            }
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 5);
            REQUIRE(mm.height == 8);
        }

        SECTION("bottom rows padded with 0x64")
        {
            std::vector<char> pixels(8 * 8, 0x64);
            for (unsigned int y = 0; y < 5; ++y)
            {
                for (unsigned int x = 0; x < 8; ++x)
                {
                    pixels[y * 8 + x] = 0x41;
                }
            }
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 8);
            REQUIRE(mm.height == 5);
        }

        SECTION("corner padding on both axes")
        {
            std::vector<char> pixels(8 * 8, 0x00);
            for (unsigned int y = 0; y < 5; ++y)
            {
                for (unsigned int x = 0; x < 5; ++x)
                {
                    pixels[y * 8 + x] = 0x41;
                }
            }
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 5);
            REQUIRE(mm.height == 5);
        }

        SECTION("a line that starts with a pad byte but contains real pixels is kept")
        {
            // the bottom row's leading pixel is 0x00 but the row has
            // real content: it must not be trimmed
            std::vector<char> pixels(8 * 8, 0x41);
            pixels[7 * 8] = 0x00;
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 8);
            REQUIRE(mm.height == 8);
        }

        SECTION("fully populated canvas is left alone")
        {
            std::vector<char> pixels(8 * 8, 0x41);
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 8);
            REQUIRE(mm.height == 8);
        }

        SECTION("interior pad byte does not stop the content")
        {
            // a pad byte inside the image (e.g. a black water pixel)
            // must survive: only whole trailing pad lines are removed
            std::vector<char> pixels(8 * 8, 0x41);
            pixels[3 * 8 + 3] = 0x00;
            auto mm = readMinimapInfo(makeTntWithMinimap(8, 8, pixels));
            REQUIRE(mm.width == 8);
            REQUIRE(mm.height == 8);
            REQUIRE(mm.data[3 * 8 + 3] == 0x00);
        }
    }

    TEST_CASE("stock minimap canvas trims to authored content")
    {
        // set RWE_DATA_PATH to a TA data directory to enable this test
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }
        auto vfs = constructVfs(fs::path(dataPathEnv));

        auto readInfo = [&](const std::string& mapName) {
            auto bytes = vfs.readFile("maps/" + mapName + ".tnt");
            REQUIRE(bytes);
            SpanStream stream(bytes->data(), bytes->size());
            TntArchive tnt(&stream);
            return tnt.readMinimap();
        };

        SECTION("AC05's stored 252x256 canvas holds a 252x169 image")
        {
            auto mm = readInfo("AC05");
            REQUIRE(mm.width == 252);
            REQUIRE(mm.height == 169);
        }

        SECTION("AC01's stored 252x256 canvas holds a 207x256 image")
        {
            auto mm = readInfo("AC01");
            REQUIRE(mm.width == 207);
            REQUIRE(mm.height == 256);
        }

        SECTION("CC13 fills its whole stored canvas")
        {
            auto mm = readInfo("CC13");
            REQUIRE(mm.width == 252);
            REQUIRE(mm.height == 256);
        }
    }
}
