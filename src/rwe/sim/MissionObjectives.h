#pragma once

#include <optional>
#include <string>
#include <vector>

#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/PlayerId.h>

namespace rwe
{
    struct GameSimulation;
    class UnitState;

    enum class MissionResult
    {
        InProgress,
        Victory,
        Defeat
    };

    enum class MissionObjectivePolarity
    {
        Victory,
        Defeat
    };

    /**
     * Retail encodes objective polarity in the GlobalHeader key name:
     * each key maps to a VictoryCondition_* or DefeatCondition_* class.
     * Returns nullopt for keys retail does not recognize.
     */
    std::optional<MissionObjectivePolarity> missionObjectivePolarity(const std::string& key);

    /**
     * Runtime state for one parsed mission objective.
     * `satisfied` mirrors the retail objective's latch flag: once set it
     * is never cleared (except that purely-polled objectives recompute
     * every evaluation and do not use the latch at all).
     */
    struct MissionObjectiveRuntime
    {
        std::string key;
        std::vector<std::string> params;
        MissionObjectivePolarity polarity;
        bool satisfied{false};

        /** Remaining kill count for KillUnitType / UnitTypeKilled. */
        int remainingCount{-1};
    };

    /**
     * Generic campaign objective evaluator implementing retail
     * TotalA.exe mission victory/defeat semantics.
     *
     * Retail structure (from TotalA.exe 0x48e010 factory, 0x490230/
     * 0x490360 evaluators):
     * - objectives split into victory and defeat lists by key name;
     * - empty victory list gains a default DestroyAllUnits objective;
     *   empty defeat list gains a default AllUnitsKilled objective;
     * - once per 30 game ticks: victory checked first (ALL must pass),
     *   then defeat (ANY suffices) only if victory did not pass;
     * - a shared countdown (TotalA.exe game object +0x39239, init -1)
     *   debounces the result: armed at 4 on the first passing eval,
     *   decremented per subsequent passing eval, result commits when it
     *   goes below 0 -- exactly 6 passing evals (~6s at the 30-tick
     *   cadence); a non-passing eval leaves the counter unchanged;
     * - unit-death events latch event-driven objectives immediately.
     */
    class MissionObjectiveEvaluator
    {
    public:
        MissionObjectiveEvaluator(const std::vector<MissionObjective>& objectives, PlayerId localPlayerId);

        /** Dispatch a unit-death event; mirrors retail slot-1 dispatch. */
        void onUnitDied(const GameSimulation& sim, const UnitState& unit);

        /**
         * Dispatch an ownership-change event; mirrors retail slot-2
         * dispatch (TotalA.exe 0x4885b1 -> 0x490520). Retail dispatches
         * before the transfer is applied, so the unit's owner field at
         * event time still reads the previous owner; `previousOwner`
         * carries that value explicitly.
         *
         * No production caller exists yet (RWE has no capture/ownership
         * transfer mechanic); the evaluator-side semantics are exercised
         * by tests.
         */
        void onUnitOwnerChanged(const GameSimulation& sim, const UnitState& unit, PlayerId previousOwner);

        /**
         * Run one evaluation pass (poll checks + combination +
         * debounce). Call once per simulation tick.
         */
        void tick(const GameSimulation& sim);

        MissionResult result() const { return missionResult; }

        PlayerId getLocalPlayerId() const { return localPlayerId; }

        const std::vector<MissionObjectiveRuntime>& getVictoryObjectives() const { return victoryObjectives; }
        const std::vector<MissionObjectiveRuntime>& getDefeatObjectives() const { return defeatObjectives; }

        int getResultCountdown() const { return resultCountdown; }

        /**
         * Save/load support: restore the evaluator's runtime state from a
         * snapshot. The original objective list is irrelevant -- the
         * snapshotted runtime objectives replace it wholesale.
         */
        void restoreRuntimeState(
            std::vector<MissionObjectiveRuntime>&& victory,
            std::vector<MissionObjectiveRuntime>&& defeat,
            int countdown,
            MissionResult result)
        {
            victoryObjectives = std::move(victory);
            defeatObjectives = std::move(defeat);
            resultCountdown = countdown;
            missionResult = result;
        }

    private:
        std::vector<MissionObjectiveRuntime> victoryObjectives;
        std::vector<MissionObjectiveRuntime> defeatObjectives;
        PlayerId localPlayerId;
        int resultCountdown{-1};
        MissionResult missionResult{MissionResult::InProgress};

        /** Equivalent of a retail objective's virtual check() call. */
        bool check(const GameSimulation& sim, MissionObjectiveRuntime& objective) const;

        void onUnitDied(const GameSimulation& sim, const UnitState& unit, MissionObjectiveRuntime& objective);

        void onUnitOwnerChanged(const GameSimulation& sim, const UnitState& unit, PlayerId previousOwner, MissionObjectiveRuntime& objective);
    };
}
