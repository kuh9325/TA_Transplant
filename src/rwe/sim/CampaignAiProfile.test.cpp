#include <catch2/catch_test_macros.hpp>
#include <rwe/sim/CampaignAiProfile.h>

namespace rwe
{
    TEST_CASE("parseCampaignAiProfile plan gating")
    {
        const std::string text =
            "weight BEFORE 0.5\n"
            "plan easy\n"
            "weight EASYONLY 0.5\n"
            "limit EASYLIM 3\n"
            "plan medium\n"
            "weight MEDONLY 0.25\n"
            "plan hard\n"
            "weight HARDONLY 0.75\n"
            "plan any\n"
            "weight ALL 1.0\n";

        SECTION("easy sees easy + any sections only")
        {
            auto p = parseCampaignAiProfile(text, CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 2);
            REQUIRE(p.weightDirectives[0].selector == "EASYONLY");
            REQUIRE(p.weightDirectives[1].selector == "ALL");
            REQUIRE(p.limitDirectives.size() == 1);
            REQUIRE(p.limitDirectives[0].selector == "EASYLIM");
            REQUIRE(p.limitDirectives[0].value == 3);
        }

        SECTION("medium sees medium + any sections only")
        {
            auto p = parseCampaignAiProfile(text, CampaignAiDifficulty::Medium);
            REQUIRE(p.weightDirectives.size() == 2);
            REQUIRE(p.weightDirectives[0].selector == "MEDONLY");
            REQUIRE(p.weightDirectives[1].selector == "ALL");
            REQUIRE(p.limitDirectives.empty());
        }

        SECTION("hard sees hard + any sections only")
        {
            auto p = parseCampaignAiProfile(text, CampaignAiDifficulty::Hard);
            REQUIRE(p.weightDirectives.size() == 2);
            REQUIRE(p.weightDirectives[0].selector == "HARDONLY");
            REQUIRE(p.weightDirectives[1].selector == "ALL");
        }

        SECTION("plan with multiple tokens matches on any of them")
        {
            auto p = parseCampaignAiProfile(
                "plan easy hard\nweight X 0.5\n",
                CampaignAiDifficulty::Hard);
            REQUIRE(p.weightDirectives.size() == 1);
        }

        SECTION("bare plan deactivates the section")
        {
            auto p = parseCampaignAiProfile(
                "plan any\nweight A 0.5\nplan\nweight B 0.5\n",
                CampaignAiDifficulty::Medium);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].selector == "A");
        }
    }

    TEST_CASE("parseCampaignAiProfile directive syntax")
    {
        SECTION("keywords and names are case-insensitive")
        {
            auto p = parseCampaignAiProfile(
                "pLaN AnY\nWeIgHt armcom 0.5\nLiMiT CORTANK 7\n",
                CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].selector == "armcom");
            REQUIRE(p.limitDirectives.size() == 1);
            REQUIRE(p.limitDirectives[0].value == 7);
        }

        SECTION("comments and blank lines are skipped")
        {
            auto p = parseCampaignAiProfile(
                "// a comment\n"
                "\n"
                "plan any // trailing comment\n"
                "weight X 0.5 // half\n",
                CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 1);
        }

        SECTION("missing limit argument defaults to 0 (prohibited)")
        {
            auto p = parseCampaignAiProfile(
                "plan any\nlimit ARMTANK\n",
                CampaignAiDifficulty::Easy);
            REQUIRE(p.limitDirectives.size() == 1);
            REQUIRE(p.limitDirectives[0].value == 0);
        }

        SECTION("missing weight argument defaults to 0.0")
        {
            auto p = parseCampaignAiProfile(
                "plan any\nweight ARMTANK\n",
                CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].multiplier == 0.0f);
        }

        SECTION("malformed lines are tolerated")
        {
            auto p = parseCampaignAiProfile(
                "plan any\n"
                "weight\n"              // no name at all
                "limit\n"
                "bogus nonsense here\n"
                "weight X notafloat\n"  // atof fails -> 0.0
                "limit Y notanint\n",   // atoi fails -> 0
                CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].multiplier == 0.0f);
            REQUIRE(p.limitDirectives.size() == 1);
            REQUIRE(p.limitDirectives[0].value == 0);
        }

        SECTION("directives before any plan are dropped")
        {
            auto p = parseCampaignAiProfile(
                "weight X 0.5\nlimit Y 2\nplan any\nweight Z 0.5\n",
                CampaignAiDifficulty::Easy);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].selector == "Z");
            REQUIRE(p.limitDirectives.empty());
        }
    }

    TEST_CASE("parseCampaignAiSelfDirective")
    {
        SECTION("parses a limit directive with trailing semicolon")
        {
            // the CORDECOM.FBI ai_limit line verbatim
            CampaignAiProfile p;
            parseCampaignAiSelfDirective("limit CORDECOM 2;", p);
            REQUIRE(p.limitDirectives.size() == 1);
            REQUIRE(p.limitDirectives[0].selector == "CORDECOM");
            REQUIRE(p.limitDirectives[0].value == 2);
        }

        SECTION("parses a weight directive")
        {
            CampaignAiProfile p;
            parseCampaignAiSelfDirective("weight ARMTANK 0.5", p);
            REQUIRE(p.weightDirectives.size() == 1);
            REQUIRE(p.weightDirectives[0].multiplier == 0.5f);
        }

        SECTION("ignores non-directive text")
        {
            CampaignAiProfile p;
            parseCampaignAiSelfDirective("", p);
            parseCampaignAiSelfDirective("plan easy", p);
            parseCampaignAiSelfDirective("garbage", p);
            REQUIRE(p.weightDirectives.empty());
            REQUIRE(p.limitDirectives.empty());
        }
    }
}
