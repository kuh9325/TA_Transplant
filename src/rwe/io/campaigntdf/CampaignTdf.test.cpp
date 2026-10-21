#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    TEST_CASE("parseCampaignDescriptor")
    {
        std::string input = R"TDF(
[HEADER]
    {
    campaignside=ARM;
    }
[MISSION0]
    {
    missionfile=EX01.ota;
    missionname=1: First Mission;
    Germanmissionname=1: Erste Mission;
    Frenchmissionname=1: Premiere Mission;
    }
[MISSION1]
    {
    missionfile=EX02.ota;
    missionname=2: Second Mission;
    }
[MISSION2]
    {
    missionfile=EX03.ota;
    missionname=3: Third Mission;
    }
)TDF";

        auto descriptor = parseCampaignDescriptor(parseTdfFromString(input));

        SECTION("parses the campaign side")
        {
            REQUIRE(descriptor.campaignSide == "ARM");
        }

        SECTION("parses mission ordering and identifiers")
        {
            REQUIRE(descriptor.missions.size() == 3);

            REQUIRE(descriptor.missions[0].index == 0);
            REQUIRE(descriptor.missions[0].missionFile == "EX01.ota");
            REQUIRE(descriptor.missions[0].missionName == "1: First Mission");

            REQUIRE(descriptor.missions[1].index == 1);
            REQUIRE(descriptor.missions[1].missionFile == "EX02.ota");
            REQUIRE(descriptor.missions[1].missionName == "2: Second Mission");

            REQUIRE(descriptor.missions[2].index == 2);
            REQUIRE(descriptor.missions[2].missionFile == "EX03.ota");
        }
    }

    TEST_CASE("parseCampaignDescriptor fails without missions")
    {
        std::string input = R"TDF(
[HEADER]
    {
    campaignside=CORE;
    }
)TDF";

        REQUIRE_THROWS(parseCampaignDescriptor(parseTdfFromString(input)));
    }

    TEST_CASE("parseCampaignDescriptor fails without a header")
    {
        std::string input = R"TDF(
[MISSION0]
    {
    missionfile=EX01.ota;
    missionname=1: First Mission;
    }
)TDF";

        REQUIRE_THROWS(parseCampaignDescriptor(parseTdfFromString(input)));
    }

    TEST_CASE("missionFileToMapName")
    {
        REQUIRE(missionFileToMapName("EX01.ota") == "EX01");
        REQUIRE(missionFileToMapName("EX01.OTA") == "EX01");
        REQUIRE(missionFileToMapName("EX01") == "EX01");
    }

    TEST_CASE("briefFileName")
    {
        // bare stems gain the extension
        REQUIRE(briefFileName("ArmCampaign1") == "ArmCampaign1.txt");
        // stock values that already carry .txt are left alone
        // (CC07/CC08/CC09/CC11/CC18, AC18, tactics T09 etc.)
        REQUIRE(briefFileName("CoreCampaign7.txt") == "CoreCampaign7.txt");
        REQUIRE(briefFileName("T09Brief.txt") == "T09Brief.txt");
        REQUIRE(briefFileName("CoreCampaign7.TXT") == "CoreCampaign7.TXT");
    }

    TEST_CASE("sanitizeBriefText")
    {
        SECTION("normalizes line endings")
        {
            REQUIRE(sanitizeBriefText("a\r\nb\r\nc") == "a\nb\nc");
        }

        SECTION("strips color and emphasis markup")
        {
            REQUIRE(sanitizeBriefText("&Y*PRIORITY*& normal") == "PRIORITY normal");
        }

        SECTION("leaves plain text unchanged")
        {
            REQUIRE(sanitizeBriefText("plain text") == "plain text");
        }

        SECTION("keepEmphasis preserves asterisks but still strips colors")
        {
            // the in-game briefing renderer draws *emphasis* markers
            // literally while still interpreting &x color codes
            REQUIRE(sanitizeBriefText("&Y*PRIORITY*& normal", true) == "*PRIORITY* normal");
        }
    }

    TEST_CASE("sanitizeBriefText against the real AC01 brief")
    {
        // AC01.ota declares brief=ArmCampaign1; retail resolves it to
        // camps/briefs/ArmCampaign1.txt. Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto rawBytes = vfs.readFile("camps/briefs/ArmCampaign1.txt");
        REQUIRE(rawBytes);
        std::string raw(rawBytes->begin(), rawBytes->end());

        auto text = sanitizeBriefText(raw, true);

        SECTION("keeps the authored mission title and emphasis markers")
        {
            REQUIRE(text.find("MISSION 1") != std::string::npos);
            REQUIRE(text.find('*') != std::string::npos);
        }

        SECTION("carries no color codes or carriage returns")
        {
            REQUIRE(text.find('&') == std::string::npos);
            REQUIRE(text.find('\r') == std::string::npos);
        }
    }

    TEST_CASE("every stock mission brief resolves to a real file")
    {
        // Guards the camps/briefs lookup: several stock OTAs carry the
        // .txt extension inside `brief` (CC07/08/09/11/18, AC18 and the
        // tactics missions like T09Brief.txt), so the path must be built
        // through briefFileName rather than a blind +".txt".
        // Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        REQUIRE(!campaigns.empty());

        unsigned int checked = 0;
        for (const auto& c : campaigns)
        {
            for (const auto& m : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(m.missionFile);
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                if (!otaRaw)
                {
                    continue;
                }
                std::string otaStr(otaRaw->begin(), otaRaw->end());
                auto ota = parseOta(parseTdfFromString(otaStr));
                if (ota.brief.empty())
                {
                    continue;
                }
                auto path = "camps/briefs/" + briefFileName(ota.brief);
                INFO("mission " + m.missionFile + " brief " + ota.brief + " -> " + path);
                REQUIRE(vfs.readFile(path));
                ++checked;
            }
        }
        REQUIRE(checked > 0);
    }
}
