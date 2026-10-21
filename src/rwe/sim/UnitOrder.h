#pragma once

#include <rwe/grid/DiscreteRect.h>
#include <rwe/sim/FeatureId.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitId.h>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace rwe
{
    struct MoveOrder
    {
        SimVector destination;

        explicit MoveOrder(const SimVector& destination) : destination(destination) {}
    };

    using AttackTarget = std::variant<UnitId, SimVector>;

    struct AttackOrder
    {
        AttackTarget target;
        explicit AttackOrder(UnitId target) : target(target) {}
        explicit AttackOrder(const SimVector& target) : target(target) {}
    };

    struct BuildOrder
    {
        std::string unitType;
        SimVector position;
        BuildOrder(const std::string& unitType, const SimVector& position) : unitType(unitType), position(position) {}
    };

    struct BuggerOffOrder
    {
        DiscreteRect rect;
        explicit BuggerOffOrder(const DiscreteRect& r) : rect(r) {}
    };

    /** Finish building an already in-progress unit */
    struct CompleteBuildOrder
    {
        UnitId target;
        explicit CompleteBuildOrder(const UnitId& target) : target(target) {}
    };

    struct GuardOrder
    {
        UnitId target;
        explicit GuardOrder(const UnitId& target) : target(target) {}
    };

    /** Repair a damaged, completed friendly unit */
    struct RepairOrder
    {
        UnitId target;
        explicit RepairOrder(const UnitId& target) : target(target) {}
    };

    using ReclaimTarget = std::variant<UnitId, FeatureId>;

    /** Reclaim a map feature (wreckage, trees, rocks) or a completed live unit */
    struct ReclaimOrder
    {
        ReclaimTarget target;
        explicit ReclaimOrder(const FeatureId& target) : target(target) {}
        explicit ReclaimOrder(const UnitId& target) : target(target) {}
    };

    /** Repair-patrol diversion: assist an unfinished allied unit. */
    struct PatrolAssistTarget
    {
        UnitId target;
    };

    /** Repair-patrol diversion: repair a damaged allied unit. */
    struct PatrolRepairTarget
    {
        UnitId target;
    };

    /** Repair-patrol diversion: reclaim a unit corpse or feature. */
    using PatrolDivertTarget = std::variant<PatrolAssistTarget, PatrolRepairTarget, ReclaimTarget>;

    /**
     * Patrol continuously through a cycle of waypoints.
     * The unit walks to each waypoint in turn; on reaching the last it
     * loops back to the first. A single-point patrol between the clicked
     * point and the unit's position is represented by including the
     * unit's position as the final waypoint.
     * The order never completes on its own; it is only removed by
     * STOP or by being replaced with another order.
     *
     * Repair-capable builders run retail's RepairPatrol: on a 60-tick
     * cadence the builder scans for allied work inside its
     * buildDistance (TotalA.exe HelpBuild scan, 0x405980) and diverts
     * to assist/repair/reclaim, resuming this same route when the
     * diversion ends. The diversion state lives on the order so the
     * standing route survives it.
     */
    struct PatrolOrder
    {
        std::vector<SimVector> waypoints;
        unsigned int nextWaypointIndex{0};
        unsigned int scanTicks{0};
        std::optional<PatrolDivertTarget> divertTarget;
        explicit PatrolOrder(std::vector<SimVector>&& waypoints) : waypoints(std::move(waypoints)) {}
    };

    /**
     * Wait for a fixed duration before the next order runs.
     * Retail `w <seconds>` is converted to ticks at 30 ticks/second.
     */
    struct WaitOrder
    {
        unsigned int ticksRemaining;
        explicit WaitOrder(unsigned int ticks) : ticksRemaining(ticks) {}
    };

    /**
     * Wait until the target mission object takes damage.
     * Installed by the retail `wa <ident>` InitialMission command;
     * a nullopt target waits on the ordered unit itself (bare `wa`).
     * The wait is armed when the order reaches the front of the queue;
     * completes when the target takes damage after that point,
     * or when the target is destroyed.
     */
    struct WaitForAttackOrder
    {
        std::optional<UnitId> target;

        bool armed{false};

        /** The target's lastDamagedAt observed when the wait armed. */
        GameTime observedDamagedAt{0};

        explicit WaitForAttackOrder(std::optional<UnitId> target)
            : target(target) {}
    };

    /**
     * Marks the mission unit as selectable by its owner.
     * Installed by the retail `s` InitialMission command, and implicitly
     * appended by the retail interpreter after a unit's scripted queue.
     */
    struct MakeSelectableOrder
    {
    };

    /**
     * Attack enemy units of a fixed unit type (retail ATTACKUTYPE,
     * `a <type>`). Each activation scans the whole map for a matching
     * enemy, preferring nearby targets with simulation-RNG jitter like
     * retail, then drives the unit through the normal attack path.
     * The order completes when a scan finds no living matching enemy;
     * it does not persist waiting for future targets.
     */
    struct AttackUnitTypeOrder
    {
        std::string unitType;

        /**
         * Game time at which the first target scan may run.
         * Retail schedules the first activation at a random 1-90 tick
         * delay so scripted groups do not all acquire simultaneously.
         */
        GameTime nextScanTime{0};

        /**
         * The unit currently being attacked, if any. Retail pushes
         * concrete attack orders to a secondary queue which keeps
         * attacking until the target dies; this field is the
         * equivalent current target.
         */
        std::optional<UnitId> currentTarget;

        explicit AttackUnitTypeOrder(const std::string& unitType) : unitType(unitType) {}
    };

    /**
     * Build a unit at a position through normal mobile-constructor
     * mechanics (retail MOBILEBUILD). The builder walks to the site,
     * requests placement, and nanolathes the unit to completion.
     * Placement failures are retried a bounded number of times with
     * a delay before the order is abandoned, matching retail's
     * attempt counter.
     */
    struct MissionBuildOrder
    {
        std::string unitType;
        SimVector position;
        unsigned int attemptsRemaining{10};
        GameTime retryAt{0};

        MissionBuildOrder(const std::string& unitType, const SimVector& position) : unitType(unitType), position(position) {}
    };

    /**
     * Produce a run of units through the normal factory build queue
     * (retail BUILDINGBUILD). The order enqueues the requested count
     * once, then completes when the factory's build queue has fully
     * drained. Produced units receive no orders, matching retail's
     * GetBuilt behavior.
     */
    struct MissionFactoryBuildOrder
    {
        std::string unitType;
        unsigned int count;
        bool enqueued{false};

        MissionFactoryBuildOrder(const std::string& unitType, unsigned int count) : unitType(unitType), count(count) {}
    };

    /**
     * Detonate the unit after its definition's self-destruct countdown
     * (retail SELFDESTRUCTFG, used by the `d` InitialMission command).
     * Retail runs the countdown in one-second steps, then waits a random
     * fuse of 0-14 ticks before applying lethal self-damage through the
     * normal damage/explosion path.
     */
    struct SelfDestructOrder
    {
        /** Ticks remaining in the audible countdown phase. */
        unsigned int countdownTicks;

        /**
         * Fuse ticks once the countdown has expired.
         * Assigned when the countdown first reaches zero.
         */
        std::optional<unsigned int> fuseTicks;

        explicit SelfDestructOrder(unsigned int countdownTicks) : countdownTicks(countdownTicks) {}
    };

    /**
     * Unload carried units at the destination (retail UNLOAD family,
     * used by the `u` InitialMission command). The transport navigates
     * to the destination before each release and drops one passenger
     * per cycle; retail sleeps 15 ticks between cycles and abandons the
     * order once its pop counter reaches 3, so scripts repeat `u`
     * commands to unload larger groups or to spread them across
     * multiple drop points.
     */
    struct UnloadOrder
    {
        SimVector destination;

        /**
         * Member claims consumed by this order (retail order+0x36).
         * Ground_Unload increments the field in state 0 each time it
         * claims the head cargo member and aborts the order when a
         * third claim is reached in state 2, so a ground `u` releases
         * at most two passengers. The field is never reset; it is not
         * used by VTOL_Unload, which drops exactly one passenger.
         */
        unsigned int pops{0};

        /**
         * True while a claim is in flight: the head member has been
         * claimed and the order is running its approach + drop
         * phases. Cleared when the member's move job completes and
         * its single drop-search call has run, whether or not the
         * member was placed.
         */
        bool claimActive{false};

        /**
         * Ticks until the next claim cycle may start (retail sleeps
         * 15 ticks per cycle in Ground_Unload state 0).
         */
        unsigned int cycleTicks{15};

        /**
         * Ground drop-search radius in world units (retail
         * transport+0x109). Widens by 160 world units per search call
         * while a drop is pending, clamps to the map extents, and
         * resets to zero on a successful drop.
         */
        SimScalar dropRadius{0};

        explicit UnloadOrder(const SimVector& destination) : destination(destination) {}
    };

    /**
     * Stockpile ammunition for the unit's stockpile weapon (retail
     * BUILDWEAPON, used by the `bw` InitialMission command). Runs as a
     * background order in parallel with the unit's normal order queue.
     * Each round takes the weapon's reload time and drains its
     * energy-per-shot and metal-per-shot costs over that period;
     * progress stalls while resources are unavailable.
     */
    struct BuildWeaponOrder
    {
        unsigned int remaining;
        unsigned int progressTicks{0};

        explicit BuildWeaponOrder(unsigned int count) : remaining(count) {}
    };

    /**
     * Fire the unit's command-fire weapon at a target: the retail
     * `ATTACKSPECIAL` order issued by the BLAST (D-Gun) command.
     * Retail resolves the order onto the unit's `commandfire` weapon
     * (FBI `candgun` marks units carrying one) and drives it through
     * the unit's "Activate" state, during which the target scanner
     * admits command-fire weapons. The order is strictly one-shot:
     * it completes once the single shot launches, for both unit and
     * position targets.
     */
    struct CommandFireOrder
    {
        AttackTarget target;

        /**
         * The command-fire weapon's `readyTime` observed when the order
         * first came in range. Firing pushes `readyTime` forward past
         * the baseline, which is how position-targeted orders detect
         * that the single shot has launched.
         */
        std::optional<GameTime> observedReadyTime;

        explicit CommandFireOrder(UnitId target) : target(target) {}
        explicit CommandFireOrder(const SimVector& target) : target(target) {}
    };

    /**
     * Take ownership of a completed enemy or allied unit (retail
     * CAPTURE, orders-panel ARMCAPTURE/CORCAPTURE quickkey `c`).
     * Only units whose FBI sets `CanCapture=1` (stock: commanders
     * only) may issue the order; targets with the same flag
     * (commanders) are immune, as are unfinished nanoframes and
     * carried units (TotalA.exe 0x4042ab-0x404342).
     *
     * Capture work accrues while the capturer is in working range at
     * a fixed retail rate — 2 progress per 6 ticks — independent of
     * WorkerTime (0x4045dd). The required work is snapshotted when
     * the order first executes: `min(1800, 0.015*BuildCostEnergy +
     * (3/14)*BuildCostMetal + 150)`, scaled by `(hp + maxHp) /
     * (2*maxHp)` and by `(kills/25 + 2)` veterancy (0x404359-0x404407).
     * RWE tracks no per-unit kill count, so the veterancy factor is
     * the kills=0 retail value: 2.
     *
     * Progress lives on the order like retail, so it survives a
     * moving target (the order keeps chasing) but is lost when the
     * order is cancelled or replaced.
     */
    struct CaptureOrder
    {
        UnitId target;

        /** Work accumulated; `workRate` is added every `workPeriod` ticks. */
        unsigned int progress{0};

        /**
         * Work needed to complete the capture, computed once when the
         * order first reaches the unit — retail snapshots it at order
         * start, so damage taken mid-capture does not rescale it.
         * Nullopt until computed.
         */
        std::optional<unsigned int> workRequired;

        /** Ticks since the current work period began. */
        unsigned int workTicks{0};

        /**
         * Retail accrues 2 work per 6-tick cycle (TotalA.exe
         * 0x4045b3-0x4045dd).
         */
        static constexpr unsigned int workRate = 2;
        static constexpr unsigned int workPeriod = 6;

        explicit CaptureOrder(const UnitId& target) : target(target) {}
    };

    /**
     * Interactive player Load: board the target unit aboard this
     * transport (retail LOAD gadget, orders-panel ARMLOAD/CORLOAD
     * quickkey `l`, click-mode 6). The player order builder emits
     * `Ground_Pickup` for non-flying transports and `VTOL_Pickup` for
     * `canfly` transports (TotalA.exe 0x43fb6d); both order the
     * *transport*, which drives to the passenger and attaches it.
     *
     * Ground_Pickup (0x406780) fires `TransportPickup(passenger)` on
     * the carrier's COB script, then waits for the script's
     * ATTACH_UNIT to board the member, retrying the approach up to 3
     * times (retail order+0x36) before abandoning.
     *
     * VTOL_Pickup (0x4111b0) flies to the passenger, descends to
     * ground level, calls `QueryTransport` for an attach slot, fires
     * `BeginTransport(height)` and attaches the passenger at the slot
     * (retail 0x48aac0), then climbs back to cruise altitude.
     * `EndTransport` is fired on the abort path.
     */
    struct LoadOrder
    {
        UnitId target;

        /**
         * Approach cycles started (retail order+0x36). A ground
         * Pickup abandons after the third failed attempt; the VTOL
         * path makes a single attempt.
         */
        unsigned int attempts{0};

        /**
         * Ticks spent waiting for the carrier's TransportPickup
         * script to attach the member during the current attempt.
         */
        unsigned int waitTicks{0};

        /** True once TransportPickup has been fired this attempt. */
        bool pickupThreadStarted{false};

        /**
         * VTOL phases: 0 approach, 1 descend, 2 attach/query,
         * 3 attached-climb, 4 aborted-climb. Ground pickups use 0
         * (approach) and 1 (wait-for-script-attach).
         */
        unsigned int phase{0};

        /**
         * Attach slot returned by QueryTransport for VTOL loads.
         * -1 until queried (and permanently for ground loads, whose
         * scripts drive their own attach slots via ATTACH_UNIT).
         */
        int slot{-1};

        /**
         * Ticks a ground Pickup waits for the script attach before
         * the approach is retried. The stock TransportPickup boom
         * animations run a few seconds; retail re-approaches when
         * the member still isn't attached after the move job.
         */
        static constexpr unsigned int maxAttachWaitTicks = 300;

        static constexpr unsigned int maxAttempts = 3;

        explicit LoadOrder(const UnitId& target) : target(target) {}
    };

    /**
     * Internal primary-queue interrupt (TotalA.exe 0x489deb / 0x402d10).
     * Hits add unsigned packet credit here, including during a scheduled
     * wait. Each visit consumes at most 1800 ticks of positive credit;
     * excess is discarded, not carried into another wait. Further hits
     * are consumed only when resumeAt is reached. No health pool is used.
     */
    struct ParalyzeOrder
    {
        std::uint32_t credit{0};
        std::optional<GameTime> resumeAt;
    };

    struct LandOrder
    {
        UnitId target;
        explicit LandOrder(UnitId target) : target(target) {}
    };

    using UnitOrder = std::variant<MoveOrder, AttackOrder, BuildOrder, BuggerOffOrder, CompleteBuildOrder, GuardOrder, RepairOrder, ReclaimOrder, PatrolOrder, WaitOrder, WaitForAttackOrder, MakeSelectableOrder, AttackUnitTypeOrder, MissionBuildOrder, MissionFactoryBuildOrder, SelfDestructOrder, UnloadOrder, BuildWeaponOrder, CommandFireOrder, CaptureOrder, LoadOrder, ParalyzeOrder, LandOrder>;
}
