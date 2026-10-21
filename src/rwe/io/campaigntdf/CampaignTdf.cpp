#include "CampaignTdf.h"

#include <algorithm>
#include <rwe/io/tdf/tdf.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    CampaignTdfParseException::CampaignTdfParseException(const std::string& arg) : runtime_error(arg)
    {
    }

    CampaignTdfParseException::CampaignTdfParseException(const char* string) : runtime_error(string)
    {
    }

    CampaignDescriptor parseCampaignDescriptor(const TdfBlock& tdf)
    {
        CampaignDescriptor d;

        auto header = tdf.findBlock("HEADER");
        if (!header)
        {
            throw CampaignTdfParseException("Missing HEADER block");
        }

        d.campaignSide = toUpper(header->get().expectString("campaignside"));

        unsigned int i = 0;
        while (auto block = tdf.findBlock("MISSION" + std::to_string(i)))
        {
            CampaignMissionEntry entry;
            entry.index = i;
            entry.missionFile = block->get().expectString("missionfile");
            entry.missionName = block->get().expectString("missionname");
            d.missions.push_back(std::move(entry));
            ++i;
        }

        if (d.missions.empty())
        {
            throw CampaignTdfParseException("Campaign descriptor has no missions");
        }

        return d;
    }

    std::vector<CampaignDescriptorFile> loadCampaignDescriptors(AbstractVirtualFileSystem& vfs, const std::string& campsPath)
    {
        std::vector<CampaignDescriptorFile> result;

        for (const auto& fileName : vfs.getFileNames(campsPath, ".tdf"))
        {
            // useonly unit whitelists live in a subdirectory, not here;
            // but guard anyway since getFileNames is not recursive
            auto bytes = vfs.readFile(campsPath + "/" + fileName);
            if (!bytes)
            {
                continue;
            }

            std::string str(bytes->begin(), bytes->end());

            try
            {
                auto descriptor = parseCampaignDescriptor(parseTdfFromString(str));

                CampaignDescriptorFile entry;
                entry.fileName = fileName;
                entry.displayName = fileName.substr(0, fileName.size() - 4);
                entry.descriptor = std::move(descriptor);
                result.push_back(std::move(entry));
            }
            catch (const std::exception&)
            {
                // not a campaign descriptor (or malformed); skip it
                continue;
            }
        }

        return result;
    }

    std::unordered_set<std::string> loadUseOnlyUnits(AbstractVirtualFileSystem& vfs, const std::string& useOnlyPath, const std::string& fileName)
    {
        std::unordered_set<std::string> units;

        if (fileName.empty())
        {
            return units;
        }

        auto bytes = vfs.readFile(useOnlyPath + "/" + fileName);
        if (!bytes)
        {
            return units;
        }

        std::string str(bytes->begin(), bytes->end());
        auto tdf = parseTdfFromString(str);

        // the whitelist is a flat set of [UNITNAME] blocks
        for (const auto& block : tdf.blocks)
        {
            units.insert(toUpper(block.first));
        }

        return units;
    }

    std::string missionFileToMapName(const std::string& missionFile)
    {
        auto name = missionFile;
        if (name.size() >= 4 && toUpper(name.substr(name.size() - 4)) == ".OTA")
        {
            name.resize(name.size() - 4);
        }

        return name;
    }

    std::string briefFileName(const std::string& brief)
    {
        auto name = brief;
        if (name.size() < 4 || toUpper(name.substr(name.size() - 4)) != ".TXT")
        {
            name += ".txt";
        }

        return name;
    }

    std::string sanitizeBriefText(const std::string& raw, bool keepEmphasis)
    {
        std::string out;
        out.reserve(raw.size());

        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            char c = raw[i];

            // normalize CRLF
            if (c == '\r')
            {
                continue;
            }

            // &x color codes: drop the & and the code char;
            // a bare & (line-end reset) is dropped on its own
            if (c == '&')
            {
                if (i + 1 < raw.size() && std::isalnum(static_cast<unsigned char>(raw[i + 1])))
                {
                    ++i;
                }
                continue;
            }

            // *emphasis* markers (drawn literally by the in-game
            // briefing renderer, stripped by the pre-mission screen)
            if (c == '*' && !keepEmphasis)
            {
                continue;
            }

            out.push_back(c);
        }

        return out;
    }
}
