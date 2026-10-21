#pragma once

#include <optional>
#include <rwe/io/tdf/TdfBlock.h>
#include <string>
#include <vector>

namespace rwe
{
    class OtaParseException : public std::runtime_error
    {
    public:
        explicit OtaParseException(const std::string& __arg);
        explicit OtaParseException(const char* string);
    };

    struct OtaSpecial
    {
        std::string specialWhat;
        int xPos;
        int zPos;
    };

    struct OtaFeature
    {
        std::string featureName;
        int xPos;
        int zPos;
    };

    /**
     * A pre-placed unit from a campaign schema's [units]/[unitN] block.
     * These only appear in campaign mission OTAs, not multiplayer maps.
     * Coordinates are in the same top-left-origin pixel space as specials
     * and features (convert via MapTerrain::topLeftCoordinateToWorld).
     */
    struct OtaUnit
    {
        /** The [unitN] ordinal, determines spawn order. */
        unsigned int index;

        std::string unitName;

        /** 1-based mission player number. Player 1 is the human side. */
        unsigned int player;

        int xPos;
        int yPos;
        int zPos;

        std::optional<unsigned int> healthPercentage;

        /** Facing in whole degrees. */
        std::optional<unsigned int> angle;

        std::optional<unsigned int> kills;

        std::optional<unsigned int> creationCountdown;

        /**
         * The verbatim TA InitialMission command string.
         * Retained for later interpretation; never executed by the parser.
         */
        std::string initialMission;

        /**
         * The unit's mission-object identifier, e.g. TOWER1.
         * InitialMission commands that reference a named mission object
         * (wa <name>, g <name>, i <name>) resolve against this field.
         */
        std::optional<std::string> ident;

        std::optional<unsigned int> initialGroup;

        bool immunity{false};

        bool aiIgnore{false};
    };

    /**
     * A mission victory/defeat condition key from the OTA GlobalHeader,
     * e.g. MoveUnitToRadius=ANYTYPE, 992, 656, 64.
     * Params are retained verbatim (split on ','); evaluation is not implemented.
     */
    struct OtaMissionObjective
    {
        std::string type;
        std::vector<std::string> params;
    };

    struct OtaSchema
    {
        std::string type;
        std::string aiProfile;
        int surfaceMetal;
        int mohoMetal;
        int humanMetal;
        int computerMetal;
        int humanEnergy;
        int computerEnergy;
        std::string meteorWeapon;
        int meteorRadius;
        float meteorDensity;
        int meteorDuration;
        int meteorInterval;

        std::vector<OtaFeature> features;
        std::vector<OtaSpecial> specials;

        /** Pre-placed mission units. Empty for multiplayer/skirmish maps. */
        std::vector<OtaUnit> units;
    };

    struct OtaRecord
    {
        std::string missionName;
        std::string missionDescription;
        std::string planet;
        std::string missionHint;
        std::string brief;
        std::string narration;
        std::string glamour;
        int lineOfSight;
        int mapping;
        int tidalStrength;
        int solarStrength;
        bool lavaWorld;
        int killMul;
        int timeMul;
        int minWindSpeed;
        int maxWindSpeed;
        int gravity;
        /**
         * OTA `nosealeveltrigger` (retail missionstate+0xd48). When set,
         * projectiles are not destroyed on crossing below sea level
         * (retail 0x49b3c0 gates the destroy path on it); retail also
         * uses it to suppress water-splash presentation, which RWE
         * does not model.
         */
        bool noSeaLevelTrigger;

        /**
         * OTA `waterdoesdamage`/`waterdamage` (retail missionstate
         * +0xd4c/+0xd50). When armed, every 30th sim tick applies
         * `waterDamage` to each live unit whose footprint terrain
         * height is at or below sea level; hovercraft are exempt
         * (retail 0x48aed3..0x48af32).
         */
        bool waterDoesDamage;
        int waterDamage;
        std::string numPlayers;
        std::string size;
        std::string memory;
        std::string useOnlyUnits;
        /**
         * OTA `maxunits` GlobalHeader key: the per-player live-unit
         * cap for this map/mission. Retail sources it from the
         * UnitLimit INI key (default 250) and clamps to [20,500];
         * stock maps author 200-500.
         */
        int maxUnits;
        int schemaCount;

        std::vector<OtaSchema> schemas;

        /**
         * Mission victory/defeat conditions declared in the GlobalHeader.
         * Empty for multiplayer/skirmish maps. Parsed and retained only;
         * no runtime evaluation exists yet.
         */
        std::vector<OtaMissionObjective> objectives;
    };

    OtaSchema parseOtaSchema(const TdfBlock& tdf);

    OtaRecord parseOta(const TdfBlock& tdf);

    OtaRecord parseOtaGlobalHeader(const TdfBlock& tdf);

    OtaFeature parseOtaFeature(const TdfBlock& tdf);

    OtaSpecial parseOtaSpecial(const TdfBlock& tdf);

    OtaUnit parseOtaUnit(const TdfBlock& tdf, unsigned int index);
}
