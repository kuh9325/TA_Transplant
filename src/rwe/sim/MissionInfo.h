#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <rwe/sim/PlayerId.h>

namespace rwe
{
    /**
     * A mission victory/defeat condition retained from the mission's
     * OTA GlobalHeader, e.g. MoveUnitToRadius=ANYTYPE, 992, 656, 64.
     * Params are the verbatim comma-separated fields from the OTA.
     * Evaluated by MissionObjectiveEvaluator during the mission.
     */
    struct MissionObjective
    {
        std::string type;
        std::vector<std::string> params;
    };

    /**
     * Per-unit mission data retained from a campaign schema's [unitN] block.
     * Only present on units spawned by a campaign mission.
     * InitialMission is parsed once at mission bootstrap and installed
     * as real simulation orders; the raw string is kept for diagnostics.
     */
    struct MissionUnitState
    {
        /** The verbatim TA InitialMission command string ("" if absent). */
        std::string initialMission;

        /**
         * The unit's mission-object identifier (OTA Ident=), used to
         * resolve named targets in InitialMission commands.
         */
        std::optional<std::string> ident;

        /**
         * Whether the unit may be selected by its owner. The retail
         * interpreter makes scripted units selectable via an explicit
         * `s` command or an implicitly appended one once their mission
         * queue drains; until then they cannot be commanded.
         */
        bool selectable{true};

        std::optional<unsigned int> initialGroup;
        std::optional<unsigned int> creationCountdown;
        std::optional<unsigned int> kills;

        bool immunity{false};
        bool aiIgnore{false};
    };

    /**
     * Marks a GameSimulation as running a campaign mission rather than
     * a skirmish. Presence of this value suppresses skirmish victory
     * semantics (commander-death kill, last-player-standing exit).
     */
    struct MissionInfo
    {
        std::string campaignName;
        std::string campaignSide;

        std::string missionName;
        unsigned int missionIndex;
        std::string missionFile;

        /** Schema Type selected for this run, e.g. "Medium". */
        std::string schemaType;

        /**
         * The schema's aiprofile identifier, e.g. "MISSIONS".
         * Retained for the campaign AI phase; the existing
         * computer controller does not interpret it.
         */
        std::string aiProfile;

        /** The human player the mission objectives are evaluated for. */
        PlayerId localPlayerId;

        /** Parsed mission conditions; see MissionObjectiveEvaluator. */
        std::vector<MissionObjective> objectives;

        /**
         * The OTA's brief entry, naming the briefing text file under
         * camps/briefs/. Empty when the mission has no briefing file.
         */
        std::string brief;

        /**
         * Uppercase unit names the human is allowed to build in this
         * mission, loaded from the OTA's useonlyunits file.
         * Empty means unrestricted.
         */
        std::unordered_set<std::string> useOnlyUnits;

        /**
         * OTA GlobalHeader score multipliers consumed by the ENDMSN
         * debrief score (retail ctx+0xd54/+0xd58):
         *   score = int(kills*killMul) + int(gameSeconds*timeMul), >= 0.
         */
        int killMul{0};
        int timeMul{0};
    };
}
