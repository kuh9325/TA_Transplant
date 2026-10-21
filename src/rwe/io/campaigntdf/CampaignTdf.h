#pragma once

#include <optional>
#include <rwe/io/tdf/TdfBlock.h>
#include <rwe/vfs/AbstractVirtualFileSystem.h>
#include <string>
#include <unordered_set>
#include <vector>

namespace rwe
{
    class CampaignTdfParseException : public std::runtime_error
    {
    public:
        explicit CampaignTdfParseException(const std::string& arg);
        explicit CampaignTdfParseException(const char* string);
    };

    struct CampaignMissionEntry
    {
        /** 0-based position in the campaign, from the [MISSIONn] ordinal. */
        unsigned int index;

        /** The mission's OTA file, e.g. "AC01.ota". */
        std::string missionFile;

        /** Display title, e.g. "1: A Hero Returns". */
        std::string missionName;
    };

    /**
     * A stock campaign descriptor (camps/<name>.tdf).
     * Descriptors are the authoritative source of campaign side,
     * mission ordering and mission identifiers.
     */
    struct CampaignDescriptor
    {
        /** The faction this campaign is played as, e.g. "ARM". */
        std::string campaignSide;

        std::vector<CampaignMissionEntry> missions;
    };

    /**
     * A campaign descriptor paired with the file it was loaded from.
     * The file name (minus extension) is the campaign's display name,
     * e.g. "Arm Campaign".
     */
    struct CampaignDescriptorFile
    {
        std::string fileName;
        std::string displayName;
        CampaignDescriptor descriptor;
    };

    /**
     * Parses a single campaign descriptor TDF.
     * Requires [HEADER]{campaignside=...} and sequential
     * [MISSION0..n]{missionfile, missionname} blocks.
     */
    CampaignDescriptor parseCampaignDescriptor(const TdfBlock& tdf);

    /**
     * Loads every campaign descriptor under the camps directory.
     * Skips files that fail to parse so one malformed descriptor
     * cannot break campaign selection.
     */
    std::vector<CampaignDescriptorFile> loadCampaignDescriptors(AbstractVirtualFileSystem& vfs, const std::string& campsPath);

    /**
     * Loads a useonly unit whitelist (camps/useonly/<file>).
     * Returns the set of uppercase unit names the mission allows,
     * or an empty set when the file is absent/empty (unrestricted).
     */
    std::unordered_set<std::string> loadUseOnlyUnits(AbstractVirtualFileSystem& vfs, const std::string& useOnlyPath, const std::string& fileName);

    /** "AC01.ota" -> "AC01". Extension is stripped case-insensitively. */
    std::string missionFileToMapName(const std::string& missionFile);

    /**
     * Resolves an OTA `brief` key to its `camps/briefs` file name.
     * Stock data is inconsistent: most missions give a bare stem
     * ("ArmCampaign10") while a handful already carry the ".txt"
     * extension ("CoreCampaign7.txt", "T09Brief.txt"); appending
     * unconditionally would miss those files entirely.
     */
    std::string briefFileName(const std::string& brief);

    /**
     * Strips briefing-file markup: CRLF is normalized to LF and the
     * retail `&x` inline color codes are removed. When `keepEmphasis`
     * is set, `*emphasis*` asterisks are preserved -- the in-game
     * briefing renderer draws them literally (MSNBRIEF strips them).
     */
    std::string sanitizeBriefText(const std::string& raw, bool keepEmphasis = false);
}
