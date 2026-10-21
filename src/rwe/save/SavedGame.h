#pragma once

#include <rwe/CampaignProgress.h>
#include <rwe/GameParameters.h>
#include <rwe/save/SaveCodec.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/PlayerId.h>
#include <string>
#include <vector>

namespace rwe
{
    /**
     * Native RWE saved-game payload.
     *
     * Retail contract (TotalA.exe): saves live at `savegame\<name>.sav`,
     * are written from the F2 options screen (OPTION.GUI SAVEGAME) and the
     * ENDMSN result screen, are listed via the stock LOADGAME/SAVEGAME GUI
     * files, and carry a tagged key-value metadata block whose keys include
     * Gametype/Players/Side/Map/Mission/CommanderDeath/Thumbs/BUILD TIME.
     * The campaign mission W/L/U array (25 bytes, 'U'/'W'/'L') is stored
     * inside the save under `Thumbs` and is the ONLY persistence of campaign
     * progression retail performs -- there is no separate profile file
     * (game-state array at ctx+0x391cf; init 0x41da30 fills 'U' on new
     * campaign, teardown 0x491b8b -> 0x41dc20 stamps 'W'/'L', save writer
     * 0x432863, load path 0x492802 validates length==25).
     *
     * RWE keeps the retail directory/name contract but writes its own
     * versioned binary payload (RWESAVE magic); retail .sav binaries are
     * NOT readable -- their internal tagged serializer is undocumented.
     */
    constexpr std::uint32_t SaveGameFormatVersion = 16;

    /**
     * Version 16 preserves AI recovery watchdog distances/deadlines, cooldowns,
     * builder exclusions and eviction counts. v1-v15 retain deterministic empty
     * history defaults, but cannot reproduce recovery history never recorded.
     *
     * Version 15 persists the mobile occupancy cells. Authored overlaps and
     * subsequent movement leave history that positions alone cannot reconstruct.
     * Versions 1-14 retain their historical position-derived occupancy fallback.
     * Version 14 persists the live unit kill word. Versions 1-13 initialize it
     * to zero without reconstructing history from mission/player metadata.
     * Version 13 persists feature damage, finite event cursors, spark countdowns
     * and the active feature order. Versions 1-12 load intact, inactive features.
     *
     * Oldest payload version this build can still read. Version 1
     * predates campaign AI state; those saves restore with empty
     * `GameSimulation::campaignAi` (LoadingScene re-initializes fresh
     * AI tables for computer players, documented as a mission-start
     * AI reset). Version 2 predates the navigation failed-path retry
     * timestamp; those saves restore with `pathExhaustedAt` cleared,
     * which delays nothing on load. Version 3 predates meteor-shower
     * hazard state; those saves keep the freshly-configured scheduler
     * (the shower starts as if the mission had just begun). Version 4
     * predates skirmish settings: those saves restore with default
     * skirmish settings (Fixed starts, Game ends, no ally groups,
     * unit-scale 1.0) matching the state v4 could express. Version 5
     * predates the interactive self-destruct unit state; those saves
     * restore with no unit armed (the v5 format could not express the
     * mid-countdown state). Version 7 predates projectile
     * targetPosition; those saves restore position-targeted shots
     * (command-fired missiles) as straight-flying ones. Version 8
     * predates internal ParalyzeOrder (tag 21); older saves have no
     * paralysis state. Version 9 predates the kamikaze standby scan deadline;
     * older saves start a fresh standby wait. Version 10 predates bomber legs and
     * projectile wind: older saves resume attack orders with a fresh approach
     * and zero projectile wind until the next existing wind update. Map gravity
     * comes from the loaded OTA. Version 11 predates aircraft servicing; older saves
     * restore without a service state or scan deadline. LandOrder appends tag 22.
     * Existing order tags are unchanged.
     */
    constexpr std::uint32_t OldestSupportedSaveGameFormatVersion = 1;

    /** Per-player row carried in the metadata block: enough to rebuild
        GameParameters::PlayerInfo on load and to label the save. */
    struct SavedPlayerMeta
    {
        std::optional<std::string> name;
        bool isHuman{false};
        std::string side;
        PlayerColorIndex color{0};
        Metal metal{0};
        Energy energy{0};

        /** Retail skirmish ally group 0-4 (format v5); absent = none. */
        std::optional<unsigned int> allyGroup;
    };

    /** Save-list metadata, stored first in the payload so it can be read cheaply. */
    struct SavedGameMetadata
    {
        std::string saveName;
        std::uint64_t unixTimeSeconds{0};
        std::string mapName;
        unsigned int schemaIndex{0};
        bool isCampaign{false};

        /** Campaign fields; meaningful only when isCampaign. */
        std::string campaignName;
        std::string campaignSide;
        unsigned int missionIndex{0};
        std::string missionFile;
        std::string missionName;
        std::string difficulty;

        /** Local (human) player's id within the saved game. */
        PlayerId localPlayerId{0};
        std::string localSide;
        unsigned int playerCount{0};

        /** Game clock at save time, in simulation ticks (30/sec). */
        GameTime gameTime{0};

        /** Skirmish CommanderDeath=Continues (format v5); always false before v5. */
        bool commanderDeath{false};

        /**
         * Skirmish frontend stages (format v5), as the enum ordinals
         * of SkirmishStartLocation/Mapping/LineOfSight/Difficulty.
         * Absent in v1-v4 payloads; gameParametersFromSave falls back
         * to the retail defaults then.
         */
        std::optional<unsigned int> skirmishStartLocation;
        std::optional<unsigned int> skirmishMapping;
        std::optional<unsigned int> skirmishLineOfSight;
        std::optional<unsigned int> skirmishDifficulty;

        /** Saved player table used to rebuild GameParameters on load. */
        std::vector<SavedPlayerMeta> players;
    };

    /** A decoded save file: parsed metadata plus the still-encoded simulation body. */
    struct DecodedSaveGame
    {
        SavedGameMetadata meta;
        std::vector<char> simulationBytes;

        /** Payload format version (RWESAVE header), <= SaveGameFormatVersion. */
        std::uint32_t version{0};
    };

    /**
     * Encode a complete save file (RWESAVE container + payload) capturing
     * `sim` plus the campaign-progression thumbs for the active campaign.
     * `progress` may be null for skirmish saves; when set and the game is a
     * campaign mission the active run's W/L/U state is embedded (retail
     * `Thumbs`).
     */
    /**
     * `formatVersion` selects which payload fields are emitted; the
     * writer always emits the layout the matching reader expects.
     * Production saves always pass the latest version -- older
     * versions exist so the backward-compat tests can emit genuine
     * old-format images instead of truncating new ones.
     */
    std::vector<char> encodeSavedGame(
        const GameSimulation& sim,
        const GameParameters& params,
        const CampaignProgress* progress,
        const std::string& saveName,
        std::uint64_t unixTimeSeconds,
        std::uint32_t formatVersion = SaveGameFormatVersion);

    /**
     * Decode a save file: validates the container, parses metadata, and
     * returns the raw simulation body for `applySavedGame`.
     * Throws SaveFormatException on any structural problem.
     */
    DecodedSaveGame decodeSavedGame(const std::vector<char>& fileBytes);

    /** Read just the metadata block (save-list UI). Throws SaveFormatException. */
    SavedGameMetadata readSaveMetadata(const std::vector<char>& fileBytes);

    /**
     * Reconstruct the GameParameters a loaded game needs (map, schema,
     * players rebuilt as local human + computers, campaign block).
     * Reads the player table from the save body.
     */
    GameParameters gameParametersFromSave(const DecodedSaveGame& save);

    /**
     * Restore a fully-populated simulation from the save body.
     * `sim` must already contain the map terrain and all static databases
     * (unit/weapon/feature/model/script definitions) loaded for
     * `meta.mapName`; this function replaces all dynamic state.
     *
     * `progress` (non-null) has the saved campaign thumbs applied into it,
     * matching retail's load path (0x41da30 reset -> apply `Thumbs`).
     *
     * Throws SaveFormatException on corrupt data or missing definitions.
     */
    void applySavedGame(GameSimulation& sim, const DecodedSaveGame& save, CampaignProgress* progress);
}
