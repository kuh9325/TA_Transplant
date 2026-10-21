#pragma once

#include <optional>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitFireOrders.h>
#include <rwe/sim/UnitMoveOrders.h>
#include <string>
#include <variant>
#include <vector>

namespace rwe
{
    /**
     * Typed representation of a retail InitialMission command.
     * Names describe semantics, not the raw one-letter verb.
     * Coordinate values are stored in the OTA top-left frame
     * (the same frame as OTA unit positions) and must be converted
     * via MapTerrain::topLeftCoordinateToWorld before use.
     */

    /**
     * Patrol continuously between the unit's position and the destination.
     * A missing destination (bare `p`) patrols in place.
     * Retail accepts a third numeric argument interpreted as a dwell
     * time in seconds, converted at 30 ticks/second; extra arguments
     * beyond that are swallowed by its sscanf call.
     */
    struct MissionCommandPatrol
    {
        std::optional<SimVector> destination;
        std::optional<unsigned int> dwellTicks;
    };

    /** Move to a position, then continue with the next command. */
    struct MissionCommandMove
    {
        SimVector destination;
    };

    /** Attack a ground position. */
    struct MissionCommandAttackPosition
    {
        SimVector target;
    };

    /**
     * Attack units of a unit type (retail ATTACKUTYPE).
     * The argument is a unit type name, not a mission-object Ident.
     */
    struct MissionCommandAttackType
    {
        std::string unitType;
    };

    /** Guard the mission object carrying the given Ident. */
    struct MissionCommandGuard
    {
        std::string targetIdent;
    };

    /** Wait for a duration, in simulation ticks (30 ticks per second). */
    struct MissionCommandWait
    {
        unsigned int ticks;
    };

    /**
     * Wait until a mission object is attacked.
     * A missing ident (bare `wa`) watches the ordered unit itself.
     */
    struct MissionCommandWaitForAttack
    {
        std::optional<std::string> targetIdent;
    };

    /**
     * Immediately apply standing fire/move orders.
     * Retail `o <fire> <move>`; takes effect at parse time,
     * it is not queued behind earlier commands. Arguments that are
     * absent retain the unit's current standing order.
     */
    struct MissionCommandSetStance
    {
        std::optional<UnitFireOrders> fireOrder;
        std::optional<UnitMoveOrders> moveOrder;
    };

    /**
     * Build or train units of a type (retail `b`).
     * Mobile units produce a MOBILEBUILD order, factories a build order.
     * Arguments map to the retail sscanf slots ` %[name] %d %f %f`:
     * `count` defaults to 1 when absent, `coordX`/`coordZ` hold the two
     * float arguments actually present. In stock data factories use
     * `b TYPE [count]` while mobile builders use `b TYPE x z`, which
     * the retail parse stores as count=x, coordX=z; the installer
     * reinterprets that shape as build coordinates.
     */
    struct MissionCommandBuild
    {
        std::string unitType;
        int count;
        std::optional<float> coordX;
        std::optional<float> coordZ;
    };

    /**
     * Background weapon-production directive (retail `bw`, BUILDWEAPON).
     * Stockpiles `count` rounds for the unit's stockpile weapon as a
     * background order alongside the normal order queue.
     */
    struct MissionCommandBuildWeapon
    {
        int count{0};
    };

    /**
     * Attach the unit to the named mission object (retail `i <ident>`).
     * Retail resolves the ident and joins the target's group chain
     * immediately at script-interpretation time; in practice every
     * stock target is a transport, making the unit carried cargo.
     */
    struct MissionCommandJoinGroup
    {
        std::string targetIdent;
    };

    /**
     * Unload one carried unit (retail `u`, order type 5).
     * A missing destination inherits the shared script position,
     * which defaults to the unit's spawn position.
     */
    struct MissionCommandUnload
    {
        std::optional<SimVector> destination;
    };

    /**
     * Queue the unit's self-destruct (retail `d`, SELFDESTRUCTFG):
     * the unit counts down its SelfDestructCountdown seconds, waits a
     * random short fuse, then detonates through the normal damage path.
     * Also suppresses the implicit trailing make-selectable, matching
     * retail.
     */
    struct MissionCommandSelfDestruct
    {
    };

    /** Make the unit selectable by its owner (retail `s`). */
    struct MissionCommandMakeSelectable
    {
    };

    /**
     * A command the parser recognized but this build does not execute,
     * or an unrecognized verb. Retained for diagnostics so nothing
     * is silently discarded.
     */
    struct MissionCommandUnsupported
    {
        std::string raw;
    };

    using MissionInitialCommand = std::variant<
        MissionCommandPatrol,
        MissionCommandMove,
        MissionCommandAttackPosition,
        MissionCommandAttackType,
        MissionCommandGuard,
        MissionCommandWait,
        MissionCommandWaitForAttack,
        MissionCommandSetStance,
        MissionCommandMakeSelectable,
        MissionCommandBuild,
        MissionCommandBuildWeapon,
        MissionCommandJoinGroup,
        MissionCommandUnload,
        MissionCommandSelfDestruct,
        MissionCommandUnsupported>;

    struct ParsedInitialMission
    {
        /**
         * Commands in source order. Unsupported commands appear here as
         * MissionCommandUnsupported, and are also listed raw in
         * `unsupportedCommands` for reporting.
         */
        std::vector<MissionInitialCommand> commands;

        /** Raw text of each segment that produced MissionCommandUnsupported. */
        std::vector<std::string> unsupportedCommands;

        /** Human-readable descriptions of malformed segments. */
        std::vector<std::string> errors;

        /**
         * True when the source contained a `d` command. Retail suppresses
         * its implicit trailing make-selectable when `s` or `d` is present.
         */
        bool suppressImplicitSelectable{false};

        /** True when the source contained an explicit `s` command. */
        bool hasExplicitSelectable{false};
    };

    /**
     * Parses a retail InitialMission string into typed commands.
     *
     * Grammar (decoded from the retail executable):
     * - commands are separated by commas; whitespace around segments
     *   is ignored; a trailing comma is permitted;
     * - each segment is exactly one command; verbs are case-insensitive;
     * - `w` followed by `a` is WaitForAttack, otherwise Wait;
     * - wait durations are seconds, converted to ticks at 30/sec;
     * - `o <fire> <move>` sets standing orders; missing arguments
     *   retain the current value;
     * - `p [x z [dwell]]` patrols to the point (or in place without
     *   coords); the optional third argument is a dwell in seconds;
     * - `m x z` moves; `a x z` attacks a position;
     * - `a <type>` attacks units of that type (ATTACKUTYPE);
     * - `g <ident>`/`wa <ident>`/`i <ident>` reference mission-object
     *   Idents;
     * - `b <type> [count] [x z]` builds/trains; `bw` is BuildWeapon;
     * - `u [x z]` unloads; `d` self-destructs and suppresses the
     *   implicit trailing make-selectable;
     * - `s` makes the unit selectable;
     * - unknown verbs are retained as unsupported commands.
     */
    ParsedInitialMission parseInitialMission(const std::string& initialMission);
}
