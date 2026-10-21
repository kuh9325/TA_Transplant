#pragma once

#include <array>
#include <deque>
#include <optional>
#include <random>
#include <set>
#include <rwe/ColorPalette.h>
#include <rwe/GameParameters.h>
#include <rwe/SkirmishSettings.h>
#include <rwe/game/WeaponMediaInfo.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/fbi/UnitFbi.h>
#include <rwe/io/featuretdf/FeatureTdf.h>
#include <rwe/io/moveinfotdf/MovementClassTdf.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tnt/TntArchive.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/sim/CampaignAiProfile.h>
#include <rwe/sim/FeatureDefinitionId.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/MovementClassCollisionService.h>
#include <rwe/sim/MovementClassDatabase.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/WeaponDefinition.h>
#include <rwe/vfs/AbstractVirtualFileSystem.h>
#include <unordered_map>

namespace rwe
{
    MovementClassCollisionService createMovementClassCollisionService(const MapTerrain& terrain, const MovementClassDatabase& movementClassDatabase);

    std::unordered_map<std::string, CobScript> loadCobScripts(AbstractVirtualFileSystem& vfs);

    std::vector<std::string> getFeatureNames(TntArchive& tnt);

    Grid<std::size_t> getMapData(TntArchive& tnt);

    Grid<unsigned char> getHeightGrid(const Grid<TntTileAttributes>& attrs);

    Vector3f colorToVector(const Color& color);

    unsigned int colorDistance(const Color& a, const Color& b);

    Vector3f getLaserColorUtil(const std::vector<Color>& palette, const std::vector<Color>& guiPalette, unsigned int colorIndex);

    std::optional<std::string> getFxName(unsigned int code);

    MovementClassDefinition parseMovementClassDefinition(const MovementClassTdf& tdf);

    WeaponDefinition parseWeaponDefinition(const WeaponTdf& tdf);

    std::optional<YardMapCell> parseYardMapCell(char c);

    std::vector<YardMapCell> parseYardMapCells(const std::string& yardMap);

    Grid<YardMapCell> parseYardMap(unsigned int width, unsigned int height, const std::string& yardMap);

    UnitDefinition parseUnitDefinition(const UnitFbi& fbi, MovementClassDatabase& movementClassDatabase);

    WeaponMediaInfo parseWeaponMediaInfo(const std::vector<Color>& palette, const std::vector<Color>& guiPalette, const WeaponTdf& tdf);

    FeatureDefinitionId getFeatureId(FeatureDefinitionId& nextId, const std::unordered_map<std::string, FeatureDefinitionId>& featureNameIndex, std::deque<std::string>& openQueue, std::unordered_map<std::string, FeatureDefinitionId>& openSet, const std::string& featureName);

    /**
     * Registers the named feature definition plus its linked chain
     * (featuredead/featureburnt/featurereclamate) -- the pure-data
     * half of LoadingScene's feature loading. Definition `name`
     * fields keep authored case (the name used to reach the section);
     * featureNameIndex keys are always uppercased. Returns the TDFs
     * that were registered so the caller can load their media.
     */
    void loadFeatureAnimations(AbstractVirtualFileSystem& vfs, std::unordered_map<std::string, FeatureTdf>& tdfs);

    std::vector<const FeatureTdf*> loadFeatureDefinitions(
        const std::unordered_map<std::string, FeatureTdf>& tdfs,
        SimpleVectorMap<FeatureDefinition, FeatureDefinitionIdTag>& featureDefinitions,
        std::unordered_map<std::string, FeatureDefinitionId>& featureNameIndex,
        const std::string& initialFeatureName);

    /**
     * Returns the index of the OTA schema whose Type matches the given
     * name (case-insensitive), e.g. a campaign difficulty "Medium".
     * Throws OtaParseException when no such schema exists.
     */
    unsigned int findSchemaIndexByType(const OtaRecord& ota, const std::string& schemaType);

    /**
     * Campaign schema selection, matching retail TotalA.exe 0x436860
     * (mode 1): the staged difficulty is tried first, then the remaining
     * ranks in ascending order -- staged Easy yields Easy/Medium/Hard,
     * Medium yields Medium/Easy/Hard, Hard yields Hard/Medium/Easy.
     * Each candidate is matched against schema Types in file order and
     * the first match wins. Stock "Krogoth Encounter" ships a single
     * Type=Hard schema, so staging Easy or Medium resolves to it.
     * Throws OtaParseException when no Easy/Medium/Hard schema exists at
     * all; retail aborts the mission load with "No suitable schema type
     * in mission file!" in that case.
     */
    unsigned int findCampaignSchemaIndex(const OtaRecord& ota, const std::string& difficulty);

    /**
     * Skirmish (multiplayer) schema selection, matching retail
     * TotalA.exe 0x436860 mode 3. Candidates are the schemas whose
     * Type is "Network 1".."Network 4", scanned in that order; within
     * a type the physical "Schema i" blocks are scanned in file order.
     * A candidate whose declared StartPos count equals `playerCount`
     * wins (the last exact match in scan order, per retail's
     * overwrite-on-accept); otherwise the candidate with the largest
     * StartPos count wins, first maximum in scan order. Schemas with
     * zero StartPos are never selected. Throws OtaParseException when
     * no Network schema declares any StartPos, mirroring retail's
     * "No suitable schema type in mission file!" abort.
     */
    unsigned int resolveSkirmishSchemaIndex(const OtaRecord& ota, unsigned int playerCount);

    /** Number of declared StartPos* specials in a schema. */
    unsigned int countStartPositions(const OtaSchema& schema);

    /**
     * The schema's declared StartPos specials ordered by their
     * StartPosN number (StartPos1 first). Sparse numbers are kept
     * sparse -- the returned vector is indexed by position in the
     * sorted list, NOT by N; callers needing StartPosN specifically
     * should match on specialWhat.
     */
    std::vector<const OtaSpecial*> collectStartPositions(const OtaSchema& schema);

    /**
     * Parses the OTA GlobalHeader `numplayers` legal player-count
     * list ("2, 3, 4"). Unparseable/absent input yields an empty set;
     * callers treat an empty set as "no declared restriction".
     */
    std::set<unsigned int> parseNumPlayers(const std::string& numPlayers);

    /**
     * Retail start-position lookup (TotalA.exe 0x437320 scanning the
     * ident table built at 0x437123-0x437160): each special named
     * StartPos<digits> carries ident = suffix - 1, so slot posNum
     * resolves to the special named "StartPos{posNum+1}". Specials
     * are scanned in file order; the first ident match wins.
     * Returns nullptr when no StartPos has that ident -- retail then
     * reports "Could not find start position number N on the map!"
     * and the player enters the game with no commander.
     */
    const OtaSpecial* findStartPosSpecial(const OtaSchema& schema, unsigned int posNum);

    /**
     * Retail skirmish start assignment (TotalA.exe 0x497908-0x497ad8).
     * Returns, per physical lobby slot, the StartPos ident the slot
     * attempts to occupy; empty slots hold nullopt.
     *
     * Fixed: posNum equals the physical slot index -- slot i occupies
     * StartPos{i+1}, and open slots never shift later assignments.
     * Random: the pool of OCCUPIED slot indices is shuffled once
     * (retail's swap-with-strictly-earlier loop, 0x497a10-0x497a71)
     * and dealt to occupied slots in physical slot order, so the
     * assignment is a permutation of the players' own fixed
     * positions (identical to drawing from all declared positions
     * when the occupied slots are dense and the schema seats exactly
     * that many players). Deterministic for a given rng seed; exact
     * retail PRNG-stream parity is not claimed.
     */
    std::array<std::optional<unsigned int>, 10> assignSkirmishStartPosNums(
        const std::array<std::optional<PlayerInfo>, 10>& players,
        SkirmishStartLocation location,
        std::minstd_rand& rng);

    /**
     * Retail skirmish Start-button validation (TotalA.exe Start
     * handler 0x47ae60-0x47b0f7). Checks, in retail order:
     *
     *   1. at least one human and at least one computer player --
     *      "There must be at least one player and one computer
     *      opponent"
     *   2. occupied player count <= the StartPos count of the schema
     *      resolved for that count -- "There are too many players
     *      enabled for this map". A map with no resolvable Network
     *      schema contributes zero positions, so this rejects it.
     *   3. all occupied slots may not share one explicit ally group
     *      (0x479760) -- "All players may not be in the same allied
     *      group." An absent allyGroup (retail sentinel 5) is an
     *      independent alliance per player and never counts as
     *      "the same" group.
     *
     * The OTA GlobalHeader `numplayers` field is NOT a legality gate:
     * retail never reads it in the Start path (it informs schema
     * selection only). Side and color are not validated at Start.
     *
     * Returns the retail rejection message, or nullopt when the
     * configuration may start. The caller additionally handles the
     * terrain check ("The terrain for the selected map does not
     * exist.") and the no-map case (a silent return in retail).
     */
    std::optional<std::string> checkSkirmishStart(
        const std::array<std::optional<PlayerInfo>, 10>& players,
        const OtaRecord& ota);

    /**
     * Retail computer-player resource-flow multiplier for the staged
     * skirmish Difficulty (TotalA.exe ctx+0x37eee -- the same global
     * drives AI plan selection and the economy coefficient):
     * Easy x0.5, Medium x0.7, Hard x1.0.
     */
    float skirmishDifficultyComputerResourceScale(SkirmishDifficulty difficulty);

    /**
     * Adds a skirmish player. Computer players' starting metal/energy
     * pools are multiplied by simulation.computerResourceScale, the
     * same difficulty-scaled grant retail applies (see
     * addCampaignPlayer). Humans always receive their configured grant.
     */
    PlayerId addSkirmishPlayer(GameSimulation& simulation, const GamePlayerInfo& info);

    /**
     * Returns the opposing faction for the given side name.
     * TA has exactly two sides; anything that is not ARM is CORE.
     */
    std::string oppositeSide(const std::string& side);

    /**
     * Builds the simulation player roster for a campaign mission.
     * Entry i corresponds to mission player number i+1.
     * Mission player 1 is the human side per stock campaign data;
     * all other mission players are computers on the opposing side.
     * Resources come from the schema's Human and Computer fields.
     */
    std::vector<GamePlayerInfo> buildCampaignPlayers(const OtaSchema& schema, const std::string& humanSide, const std::string& enemySide);

    /** Converts an OTA unit Angle field (whole degrees) to a SimAngle. */
    SimAngle otaAngleToSimAngle(unsigned int degrees);

    struct MissionSpawnStats
    {
        unsigned int spawned{0};

        /** Human-readable reasons for each unit that failed to spawn. */
        std::vector<std::string> failures;
    };

    /**
     * Instantiates a campaign schema's pre-placed [unitN] units.
     * units[i].player (1-based) selects missionPlayers[player-1] as owner.
     * Units spawn as completed structures/units with the OTA's
     * position, facing and health, and retain their mission data
     * (InitialMission etc.) on UnitState::missionState.
     * Failures are collected in the result rather than thrown,
     * so one bad unit does not abort the mission load.
     */
    MissionSpawnStats spawnMissionUnits(GameSimulation& sim, const std::vector<OtaUnit>& units, const std::vector<PlayerId>& missionPlayers);

    struct MissionBootstrapStats
    {
        /** Mission units that carried a non-empty InitialMission string. */
        unsigned int scriptedUnits{0};

        /** Individual parsed commands encountered (including unsupported). */
        unsigned int parsedCommands{0};

        /** UnitOrders actually installed onto units. */
        unsigned int installedOrders{0};

        /** Standing-order (`o`) commands applied immediately. */
        unsigned int stanceCommands{0};

        /**
         * Commands the parser recognized (typed) but this build does
         * not yet execute — ATTACKUTYPE, build/train, BuildWeapon,
         * join-group, unload, self-destruct.
         */
        unsigned int deferredCommands{0};

        /**
         * Raw command segments this build does not execute.
         * Includes the owning unit type for diagnostics.
         */
        std::vector<std::string> unsupportedCommands;

        /** Human-readable parse/resolution failures. */
        std::vector<std::string> errors;
    };

    /**
     * Executes retained InitialMission strings on all mission units.
     * Must run after spawnMissionUnits so that named mission-object
     * references (Ident) resolve regardless of [unitN] order.
     * Supported commands become real simulation orders on the unit's
     * order queue; `o` applies standing orders immediately; unknown
     * or unsupported commands are reported, never silently dropped.
     */
    MissionBootstrapStats installInitialMissionOrders(GameSimulation& sim);

    /**
     * Reads the stock `ai/<name>` profile text through the VFS,
     * trying the name verbatim, then with a `.TXT` extension, then
     * `DEFAULT`/`DEFAULT.TXT` (retail's fallback path, ReloadAIProfiles
     * at TotalA.exe 0x4648e0). Returns std::nullopt when nothing
     * resolves.
     */
    std::optional<std::string> loadCampaignAiProfileText(
        AbstractVirtualFileSystem& vfs,
        const std::string& aiDirectory,
        const std::string& profileName);

    /**
     * Maps the staged campaign difficulty name to the retail
     * difficulty index (ctx+0x37eee: 0 easy, 1 medium, 2 hard) used by
     * `plan` gating. Unrecognized names resolve to Medium, matching
     * the difficulty selector's default stage.
     */
    CampaignAiDifficulty campaignAiDifficultyIndex(const std::string& difficulty);

    /**
     * Maps the staged skirmish difficulty toggle to the same retail
     * difficulty index (ctx+0x37eee: 0 easy, 1 medium, 2 hard) the
     * campaign path uses for `plan` gating. Retail writes the same
     * global from the skirmish difficulty selector, so the skirmish
     * enum order corresponds 1:1.
     */
    CampaignAiDifficulty skirmishAiDifficultyIndex(SkirmishDifficulty difficulty);

    /**
     * Retail AI bootstrap slot loop (TotalA.exe 0x464950): walks the
     * player table and applies the resolved profile to every computer
     * player that does not already carry a restored AI state. Human
     * and network players never receive one. Shared by campaign and
     * skirmish -- the profile itself is the only per-game difference.
     */
    void attachCampaignAiPlayers(GameSimulation& simulation, const CampaignAiProfile& profile);

    /**
     * Retail computer-player resource-flow multiplier for the staged
     * campaign difficulty (TotalA.exe ctx+0x37eee, applied to
     * owner type+0x73==2 units in the economy update): Easy x0.5,
     * Medium x0.7, Hard and unrecognized names x1.0.
     */
    float campaignDifficultyComputerResourceScale(const std::string& difficulty);

    /**
     * Adds a campaign mission player. For computer players the
     * starting metal/energy pools are multiplied by
     * simulation.computerResourceScale: retail credits the OTA
     * starting grant through the difficulty-scaled production
     * accumulators (TotalA.exe 0x465414/0x4654af). The storage fields
     * (maxMetal/maxEnergy/startingMetal/startingEnergy) stay
     * unscaled, matching retail's separate max(200,start) floor.
     * Human players always receive the authored grant. The caller
     * must assign simulation.computerResourceScale from the campaign
     * difficulty before calling (LoadingScene does so before building
     * players).
     */
    PlayerId addCampaignPlayer(GameSimulation& simulation, const GamePlayerInfo& info);

    /**
     * Reads gamedata/meteor.tdf [Default] -- the fallback hazard
     * configuration retail loads whenever an OTA schema's Meteor*
     * record is incomplete (TotalA.exe 0x419450). Returns std::nullopt
     * when the file or block is absent; the shower then resolves to a
     * zeroed (inert) configuration.
     */
    std::optional<MeteorShowerParams> loadMeteorShowerDefaults(AbstractVirtualFileSystem& vfs);

    /**
     * Applies the meteor-shower fields of the selected OTA schema to
     * the simulation (retail schema read 0x4219d1..0x421a8e + apply
     * 0x418ed0 + init 0x418e50): empty weapon name or any zero numeric
     * field discards the whole record in favour of `defaults`.
     */
    void configureMeteorShower(GameSimulation& simulation, const OtaSchema& schema, const std::optional<MeteorShowerParams>& defaults);

    /**
     * TA-Transplant balance override (owner-approved divergence from
     * retail 3.1, documented in docs/macos27/DECISIONS.md).
     * Patches the interactive transportCapacity of the six stock
     * transports after the effective retail FBI definitions have been
     * parsed:
     *
     *   ARMATLAS 1 (retail 5), CORVALK 1 (1), ARMTHOVR 5 (5),
     *   CORTHOVR 5 (5), ARMTSHIP 24 (20), CORTSHIP 24 (5).
     *
     * Only transportCapacity is touched -- CanLoad, TransportSize,
     * CantBeTransported, COB pickup/attach and unload behaviour are
     * unchanged, and scripted InitialMission `i` boarding never
     * consulted the count, so it still bypasses the limit. Because the
     * override mutates definition data before any simulation consumes
     * it, the values are identical in campaign, Battle Tactics and
     * skirmish. Retail FBI/archive bytes are never modified; a missing
     * unit name logs a warning.
     */
    void applyTransplantBalanceOverrides(std::unordered_map<std::string, UnitDefinition>& unitDefinitions);

}
