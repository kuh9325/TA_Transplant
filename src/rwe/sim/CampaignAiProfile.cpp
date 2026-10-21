#include "CampaignAiProfile.h"
#include <cstdlib>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    namespace
    {
        const char* difficultyName(CampaignAiDifficulty difficulty)
        {
            switch (difficulty)
            {
                case CampaignAiDifficulty::Easy:
                    return "EASY";
                case CampaignAiDifficulty::Medium:
                    return "MEDIUM";
                case CampaignAiDifficulty::Hard:
                    return "HARD";
            }
            return "";
        }

        bool planMatches(const std::vector<std::string>& args, CampaignAiDifficulty difficulty)
        {
            if (args.empty())
            {
                return false;
            }

            const auto* name = difficultyName(difficulty);
            for (const auto& arg : args)
            {
                auto upper = toUpper(arg);
                if (upper == "ANY" || upper == name)
                {
                    return true;
                }
            }
            return false;
        }

        void parseDirectiveLine(const std::vector<std::string>& tokens, bool planActive, CampaignAiProfile& profile)
        {
            if (tokens.empty())
            {
                return;
            }

            const auto keyword = toUpper(tokens.front());
            if (!planActive)
            {
                return;
            }

            if (keyword == "WEIGHT")
            {
                if (tokens.size() < 2)
                {
                    return;
                }
                // retail reads a missing/garbage float as 0.0
                float multiplier = tokens.size() >= 3 ? std::atof(tokens[2].c_str()) : 0.0f;
                profile.weightDirectives.push_back({tokens[1], multiplier});
            }
            else if (keyword == "LIMIT")
            {
                if (tokens.size() < 2)
                {
                    return;
                }
                // retail's missing-argument default is 0 (prohibited)
                int value = tokens.size() >= 3 ? std::atoi(tokens[2].c_str()) : 0;
                profile.limitDirectives.push_back({tokens[1], value});
            }
        }
    }

    CampaignAiProfile parseCampaignAiProfile(const std::string& text, CampaignAiDifficulty difficulty)
    {
        CampaignAiProfile profile;
        bool planActive = false;

        std::size_t pos = 0;
        while (pos <= text.size())
        {
            auto end = text.find('\n', pos);
            auto line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() + 1 : end + 1;

            auto commentStart = line.find("//");
            if (commentStart != std::string::npos)
            {
                line.erase(commentStart);
            }

            auto tokens = split(line, {' ', '\t', '\r', '\v', '\f'});
            tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const auto& t) { return t.empty(); }), tokens.end());
            if (tokens.empty())
            {
                continue;
            }

            if (toUpper(tokens.front()) == "PLAN")
            {
                planActive = planMatches(std::vector<std::string>(tokens.begin() + 1, tokens.end()), difficulty);
                continue;
            }

            parseDirectiveLine(tokens, planActive, profile);
        }

        return profile;
    }

    void parseCampaignAiSelfDirective(const std::string& line, CampaignAiProfile& profile)
    {
        auto commentStart = line.find("//");
        auto trimmed = commentStart == std::string::npos ? line : line.substr(0, commentStart);
        auto tokens = split(trimmed, {' ', '\t', '\r', '\v', '\f', ';'});
        tokens.erase(std::remove_if(tokens.begin(), tokens.end(), [](const auto& t) { return t.empty(); }), tokens.end());
        parseDirectiveLine(tokens, true, profile);
    }
}
