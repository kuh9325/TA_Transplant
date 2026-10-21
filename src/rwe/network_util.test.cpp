#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>
#include <rwe/network_util.h>
#include <limits>
#include <vector>

namespace rwe
{
    TEST_CASE("ema")
    {
        SECTION("combines value with average according to weight")
        {
            REQUIRE(ema(1.0f, 8.0f, 0.5f) == 4.5f);
            REQUIRE(ema(1.0f, 8.0f, 0.25f) == 6.25f);
        }
    }

    TEST_CASE("readInt")
    {
        rc::prop("readInt inverts writeInt", [](unsigned int i) {
            std::array<char, 4> arr;
            writeInt(arr.data(), i);
            auto result = readInt(arr.data());
            RC_ASSERT(result == i);
        });
    }

    TEST_CASE("computeCrc")
    {
        SECTION("empty input")
        {
            REQUIRE(computeCrc("", 0) == 0x00000000u);
        }

        SECTION("known CRC32 values")
        {
            // CRC32 of "123456789" is 0xCBF43926
            const char* input = "123456789";
            REQUIRE(computeCrc(input, 9) == 0xCBF43926u);
        }

        SECTION("single byte")
        {
            // CRC32 of "a" is 0xE8B7BE43
            REQUIRE(computeCrc("a", 1) == 0xE8B7BE43u);
        }
    }
    TEST_CASE("peer scene extrapolation follows the simulation tick period", "[followup][timing]")
    {
        const Timestamp now{};
        std::vector<std::pair<SceneTime, Timestamp>> peers{
            {SceneTime(100), now - std::chrono::milliseconds(1000)}};
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(130), peers, now) == 130);
        peers.front().second = now - std::chrono::milliseconds(33);
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(100), peers, now) == 100);
        peers.front().second = now - std::chrono::milliseconds(34);
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(101), peers, now) == 101);
    }

    TEST_CASE("peer scene averaging handles empty future and long-running samples", "[followup][timing]")
    {
        const Timestamp now{};
        std::vector<std::pair<SceneTime, Timestamp>> peers;
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(77), peers, now) == 77);
        peers.emplace_back(SceneTime(77), now + std::chrono::milliseconds(100));
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(77), peers, now) == 77);
        const auto late = std::numeric_limits<unsigned int>::max() - 100;
        peers = {{SceneTime(late), now}, {SceneTime(late), now}};
        REQUIRE(estimateAverageSceneTimeStatic(SceneTime(late), peers, now) == late);
    }

}
