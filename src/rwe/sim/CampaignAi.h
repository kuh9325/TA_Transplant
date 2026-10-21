#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <rwe/grid/DiscreteRect.h>
#include <rwe/sim/CampaignAiProfile.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/MovementClassDefinition.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitId.h>

namespace rwe
{
    struct GameSimulation;
    struct UnitDefinition;
    struct UnitState;

    /**
     * Retail AI group slots (TotalA.exe aiCtx wiring at 0x408cb0:
     * nine group containers on each computer player). Slots that
     * stock campaign data never populates stay empty.
     */
    enum CampaignAiGroupIndex
    {
        /** Group 1: factories plus on/off-capable resource units. */
        AiGroupFactory = 0,
        /** Group 2: reserved (unused). */
        AiGroupReserved2 = 1,
        /** Group 3: combat reserve A (feeds the attack task). */
        AiGroupReserveA = 2,
        /** Group 4: mobile builders (including the commander). */
        AiGroupBuilder = 3,
        /** Group 5: reserved no-op (retail task 0x407380 is a bare ret). */
        AiGroupReserved5 = 4,
        /** Group 6: reserved (unused). */
        AiGroupReserved6 = 5,
        /** Group 7: combat reserve B (feeds the attack task). */
        AiGroupReserveB = 6,
        /** Group 8: wander group (unused in campaign). */
        AiGroupWander = 7,
        /** Group 9: scout/harass group (never populated by stock data). */
        AiGroupScout = 8,

        AiGroupCount = 9,
    };

    /**
     * Per-computer-player campaign AI state — the RWE analogue of the
     * retail aiBlock attached at player+0x1b63 (constructor 0x409160).
     *
     * The weight/limit tables are resolved at profile load and are
     * authoritative: they persist across Save/Load. `unitIndex` is the
     * deterministic table order (sorted unit names) rebuilt on restore.
     * Group membership is rebuilt every repartition tick (~30 ticks),
     * so it survives load only as a hint; it is serialized anyway to
     * keep post-load hashes identical.
     */
    struct CampaignAiPlayerState
    {
        PlayerId player{0};

        /** Sorted unit names; indexes the parallel tables. */
        std::vector<std::string> unitIndex;

        /** Retail +0xb1 byte weights (0..100, default 100). */
        std::vector<int> unitWeights;

        /** Retail +0xd1 dword caps (-1 = unlimited). */
        std::vector<int> unitLimits;

        GameTime nextRepartition{0};
        GameTime nextFactoryScan{0};
        GameTime nextBuilderScan{0};
        GameTime nextAttackScan{0};
        GameTime nextDeadlockScan{0};

        std::array<std::vector<UnitId>, AiGroupCount> groups;

        /** The unit the attack task last sent its squad against. */
        std::optional<UnitId> attackTarget;

        /**
         * Per-factory bookkeeping for the egress-deadlock recovery
         * task. Authoritative: `blockedSince` times how long the
         * sealed condition has persisted and `reclaimTarget`/
         * `reclaimBuilder` dedupe the recovery order across scans.
         * TA-TRANSPLANT ROBUSTNESS APPROXIMATION -- retail's site
         * finder (TotalA.exe 0x419b00) checks only footprint validity
         * and has no deadlock recovery at all.
         */
        struct DeadlockState
        {
            /** First scan at which sealed egress was observed. */
            std::optional<GameTime> blockedSince;
            /**
             * Own noncritical structure picked for removal. With
             * `reclaimBuilder` unset this target is under a
             * self-destruct last resort instead.
             */
            std::optional<UnitId> reclaimTarget;
            /** Builder issued the reclaim order. */
            std::optional<UnitId> reclaimBuilder;
            /**
             * Last tick at which the in-flight reclaim showed signs of
             * life: order issue, a measurably closer approach to the
             * target, or an active reclaim beam. The stall watchdog
             * counts from here.
             */
            std::optional<GameTime> reclaimIssuedAt;
            /** Reclaim orders issued; a stalled attempt opens the
             *  self-destruct last resort once the grace window lapses. */
            unsigned int reclaimAttempts = 0;
            /**
             * When the last reclaim attempt stalled (persistent since RWESAVE
             * v16). A stall verdict is not permanent truth --
             * base geometry evolves as units die and new structures go
             * up -- so after a cooldown the attempt budget refreshes
             * and recovery may try again.
             */
            std::optional<GameTime> reclaimStalledAt;
            /**
             * Builders whose reclaim attempt already stalled
             * (persistent since RWESAVE v16). A stall proves only
             * that THIS builder cannot reach the working-range anchor
             * -- congestion is local -- so the next pick must try a
             * different builder rather than abandoning the recovery
             * until the attempt budget is spent. Cleared whenever the
             * budget refreshes.
             */
            std::unordered_set<unsigned int> reclaimAvoid;
            /**
             * Smallest builder-to-target distance observed while the
             * current reclaim order is in flight (persistent since RWESAVE
             * v16). The reclaim watchdog measures lack of
             * approach rather than elapsed time: a builder still
             * closing on its target -- or one that has arrived and is
             * actively reclaiming -- is making progress no matter how
             * long the walk takes, while a builder pinned by
             * congestion stops improving and is convicted after the
             * watchdog window.
             */
            std::optional<SimScalar> reclaimBestDistance;
            /**
             * Absolute cap on the current reclaim approach (persistent since
             * RWESAVE v16). Progress refreshes keep the
             * no-approach watchdog alive while a builder genuinely
             * closes on its target, but a pinned unit can ratchet its
             * best distance indefinitely -- congestion nudges it a few
             * units at a time -- so the approach is also time-boxed by
             * the distance it started from. Initialized lazily on the
             * first in-flight check; its original deadline survives save/load.
             */
            std::optional<GameTime> reclaimDeadline;
            /**
             * Per-mobile shove count (persistent since RWESAVE v16).
             * A parked unit that stays put through several bugger-off
             * orders is nav-immobile -- it graduates from eviction to
             * reclaim candidacy so a builder can clear the cell it
             * will not leave. Persisted so save/load cannot delay
             * escalation by issuing redundant shove attempts.
             */
            std::unordered_map<unsigned int, unsigned int> evictAttempts;
        };

        /** Factories whose egress is currently sealed (keyed by factory). */
        std::map<UnitId, DeadlockState> deadlockedFactories;
    };

    /**
     * Resolve `profile` for `player` against the simulation's unit
     * definitions and append the resulting state to
     * `sim.campaignAi`. No-op for non-computer players.
     *
     * Application order (TotalA.exe ReloadAIProfiles 0x464990):
     *   1. profile weight directives (multiplicative, first-wins,
     *      clamped 0..100)
     *   2. profile limit directives (first-wins, -1 = unlimited)
     *   3. each unit's own FBI `ai_limit` directive line, gated on the
     *      matching lock (the unit's text may set either kind)
     */
    void initCampaignAiPlayer(GameSimulation& sim, PlayerId player, const CampaignAiProfile& profile);

    /**
     * The per-tick AI driver (retail dispatches the ten task slots
     * from the main game loop). Runs repartition (~30t), factory scan
     * (~30t), builder scan (~90t) and attack scan (~300t) for every
     * live computer player that has a state.
     */
    void updateCampaignAi(GameSimulation& sim);

    /**
     * Rebuild the deterministic unit index and group structure after a
     * save restore repopulated the serialized tables. Group lists are
     * left as restored; the next repartition regenerates them.
     */
    void rebuildCampaignAiIndex(GameSimulation& sim);

    /**
     * TA-TRANSPLANT ROBUSTNESS APPROXIMATION -- retail's site finder
     * (TotalA.exe 0x419b00) validates only the candidate footprint; it
     * never checks that a placement preserves factory egress, which is
     * exactly how stock AI walls itself in. The probe and the
     * prevention check below are exposed for the placement regression
     * tests.
     */
    struct EgressProbeResult
    {
        /** The flood fill reached open terrain beyond the base. */
        bool open{false};
        /** Own structures sitting on the pocket's boundary. */
        std::vector<UnitId> boundaryBuildings;
        /** Cells the flood fill reached (heightmap y*width+x). */
        std::unordered_set<unsigned int> pocket;
    };

    /**
     * Flood-fills from the factory's yard ring over cells a produced
     * unit of movement class `mc` can stand on, reporting whether the
     * fill escapes the base region. `extraBlocked` simulates a
     * hypothetical footprint; `extraOpenId` simulates removing that
     * blocker's cells. Mobile units never block -- they are traffic,
     * not walls.
     */
    EgressProbeResult probeFactoryEgress(
        const GameSimulation& sim,
        const DiscreteRect& factoryRect,
        const MovementClassDefinition& mc,
        const DiscreteRect* extraBlocked,
        const UnitId* extraOpenId);

    /**
     * True when hypothetically occupying `candidateRect` would
     * disconnect some own factory's yard exit.
     */
    bool placementSealsFactoryEgress(
        GameSimulation& sim,
        const CampaignAiPlayerState& st,
        const DiscreteRect& candidateRect);

    /**
     * Retail build-site spiral (candidate rings around the builder,
     * footprint-validated) extended with the egress prevention check
     * above: sites that would seal an own factory are rejected.
     */
    std::optional<SimVector> findBuildSite(
        GameSimulation& sim,
        const CampaignAiPlayerState& st,
        const UnitState& builder,
        const UnitDefinition& builderDef,
        const UnitDefinition& targetDef);
}
