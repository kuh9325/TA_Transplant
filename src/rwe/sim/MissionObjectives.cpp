#include <rwe/sim/MissionObjectives.h>

#include <algorithm>
#include <cstdlib>

#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitState.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    namespace
    {
        std::optional<MissionObjectivePolarity> polarityForCanonicalKey(const std::string& key)
        {
            // Retail key set, grouped by the VictoryCondition_* /
            // DefeatCondition_* class each GlobalHeader key instantiates.
            static const std::vector<std::string> victoryKeys{
                "KillEnemyCommander",
                "DestroyAllUnits",
                "KillAllMobileUnits",
                "BuildUnitType",
                "CaptureUnitType",
                "KillAllOfType",
                "KillUnitType",
                "MoveUnitToRadius",
                "UnitTypePassesX",
                "UnitTypePassesZ",
                "VictoryTimerRunsOut",
            };
            static const std::vector<std::string> defeatKeys{
                "CommanderKilled",
                "AllUnitsKilled",
                "AllUnitsKilledOfType",
                "UnitTypeKilled",
                "DeathTimerRunsOut",
                "AnyUnitPassesX",
                "AnyUnitPassesZ",
            };
            for (const auto& k : victoryKeys)
            {
                if (key == k)
                {
                    return MissionObjectivePolarity::Victory;
                }
            }
            for (const auto& k : defeatKeys)
            {
                if (key == k)
                {
                    return MissionObjectivePolarity::Defeat;
                }
            }
            return std::nullopt;
        }

        std::string trimmedUpper(const std::string& s)
        {
            auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            auto last = s.find_last_not_of(" \t\r\n");
            return toUpper(s.substr(first, last - first + 1));
        }

        std::optional<int> parseObjectiveInt(const std::string& s)
        {
            auto trimmed = trimmedUpper(s);
            if (trimmed.empty())
            {
                return std::nullopt;
            }
            try
            {
                return std::stoi(trimmed);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        std::optional<std::string> objectiveTypeParam(const MissionObjectiveRuntime& objective)
        {
            if (objective.params.empty())
            {
                return std::nullopt;
            }
            auto type = trimmedUpper(objective.params.front());
            if (type.empty())
            {
                return std::nullopt;
            }
            return type;
        }

        bool typeMatches(const UnitState& unit, const std::string& typeName)
        {
            return toUpper(unit.unitType) == typeName;
        }

        bool isCommander(const GameSimulation& sim, const UnitState& unit)
        {
            auto it = sim.unitDefinitions.find(unit.unitType);
            return it != sim.unitDefinitions.end() && it->second.commander;
        }

        /**
         * Retail's "live" filter used by the list-scanning accepts
         * (the [+0x110]&0x20 / [+0x104]==0 / [+0xfb]==0 test): on-map,
         * not dead, not under construction, not carried, and not a
         * non-selectable scripted mission unit -- this is why AC01's
         * non-selectable ARMGATE does not satisfy MoveUnitToRadius.
         */
        bool isLiveUnit(const GameSimulation& sim, const UnitState& unit)
        {
            if (unit.isDead() || unit.carriedBy)
            {
                return false;
            }
            if (unit.missionState && !unit.missionState->selectable)
            {
                return false;
            }
            auto it = sim.unitDefinitions.find(unit.unitType);
            return it != sim.unitDefinitions.end() && !unit.isBeingBuilt(it->second);
        }

        /**
         * Retail cell-quantized coordinate compare for the *PassesX/Z
         * family: OTA parameters are top-left-origin world units and
         * unit fields are cells in that same space, so both are reduced
         * to >>4 cells; passing means within 2 cells (inclusive).
         */
        bool passesCoordinate(SimScalar position, int param, SimScalar mapSize)
        {
            auto unitTopLeft = simScalarToFloat(position) + (simScalarToFloat(mapSize) / 2.0f);
            auto unitCell = static_cast<int>(unitTopLeft) >> 4;
            auto targetCell = param >> 4;
            return std::abs(unitCell - targetCell) <= 2;
        }
    }

    std::optional<MissionObjectivePolarity> missionObjectivePolarity(const std::string& key)
    {
        return polarityForCanonicalKey(key);
    }

    MissionObjectiveEvaluator::MissionObjectiveEvaluator(const std::vector<MissionObjective>& objectives, PlayerId localPlayerId)
        : localPlayerId(localPlayerId)
    {
        for (const auto& o : objectives)
        {
            auto polarity = polarityForCanonicalKey(o.type);
            if (!polarity)
            {
                continue;
            }

            MissionObjectiveRuntime runtime;
            runtime.key = o.type;
            runtime.params = o.params;
            runtime.polarity = *polarity;

            if (runtime.key == "KillUnitType" || runtime.key == "UnitTypeKilled")
            {
                runtime.remainingCount = runtime.params.size() >= 2 ? parseObjectiveInt(runtime.params[1]).value_or(0) : 0;
            }

            (*polarity == MissionObjectivePolarity::Victory ? victoryObjectives : defeatObjectives).push_back(std::move(runtime));
        }

        // Retail appends default objectives when a side's list is empty.
        if (victoryObjectives.empty())
        {
            MissionObjectiveRuntime runtime;
            runtime.key = "DestroyAllUnits";
            runtime.polarity = MissionObjectivePolarity::Victory;
            victoryObjectives.push_back(std::move(runtime));
        }
        if (defeatObjectives.empty())
        {
            MissionObjectiveRuntime runtime;
            runtime.key = "AllUnitsKilled";
            runtime.polarity = MissionObjectivePolarity::Defeat;
            defeatObjectives.push_back(std::move(runtime));
        }
    }

    bool MissionObjectiveEvaluator::check(const GameSimulation& sim, MissionObjectiveRuntime& objective) const
    {
        const auto& key = objective.key;

        // Event-driven objectives report only their latch state.
        if (key == "KillEnemyCommander" || key == "KillAllMobileUnits" || key == "CaptureUnitType"
            || key == "KillAllOfType" || key == "KillUnitType"
            || key == "CommanderKilled" || key == "AllUnitsKilledOfType" || key == "UnitTypeKilled")
        {
            return objective.satisfied;
        }

        if (key == "DestroyAllUnits")
        {
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != localPlayerId && !unit.isDead())
                {
                    return false;
                }
            }
            return true;
        }

        if (key == "AllUnitsKilled")
        {
            // Retail recomputes this every eval rather than latching.
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner == localPlayerId && isLiveUnit(sim, unit))
                {
                    return false;
                }
            }
            return true;
        }

        if (key == "BuildUnitType")
        {
            if (objective.satisfied)
            {
                return true;
            }
            auto type = objectiveTypeParam(objective);
            if (!type)
            {
                return false;
            }
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner == localPlayerId && typeMatches(unit, *type))
                {
                    auto it = sim.unitDefinitions.find(unit.unitType);
                    if (it != sim.unitDefinitions.end() && !unit.isBeingBuilt(it->second))
                    {
                        objective.satisfied = true;
                        return true;
                    }
                }
            }
            return false;
        }

        if (key == "MoveUnitToRadius")
        {
            if (objective.satisfied || objective.params.size() < 4)
            {
                return objective.satisfied;
            }
            auto type = objectiveTypeParam(objective);
            auto x = parseObjectiveInt(objective.params[1]);
            auto z = parseObjectiveInt(objective.params[2]);
            auto r = parseObjectiveInt(objective.params[3]);
            if (!x || !z || !r)
            {
                return false;
            }
            // OTA params are top-left-origin; unit positions are
            // centered, so shift the target into world space. Retail
            // tests Euclidean distance against the radius.
            auto cx = *x - (simScalarToFloat(sim.terrain.getWidthInWorldUnits()) / 2.0f);
            auto cz = *z - (simScalarToFloat(sim.terrain.getHeightInWorldUnits()) / 2.0f);
            float r2 = static_cast<float>(*r) * static_cast<float>(*r);
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != localPlayerId || !isLiveUnit(sim, unit))
                {
                    continue;
                }
                // "ANYTYPE" is a wildcard in retail.
                if (type && *type != "ANYTYPE" && !typeMatches(unit, *type))
                {
                    continue;
                }
                auto dx = simScalarToFloat(unit.position.x) - cx;
                auto dz = simScalarToFloat(unit.position.z) - cz;
                if (dx * dx + dz * dz <= r2)
                {
                    objective.satisfied = true;
                    return true;
                }
            }
            return false;
        }

        if (key == "UnitTypePassesX" || key == "UnitTypePassesZ")
        {
            if (objective.satisfied || objective.params.size() < 2)
            {
                return objective.satisfied;
            }
            auto type = objectiveTypeParam(objective);
            auto coordinate = parseObjectiveInt(objective.params[1]);
            if (!coordinate)
            {
                return false;
            }
            bool useX = key == "UnitTypePassesX";
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != localPlayerId)
                {
                    continue;
                }
                if (type && *type != "ANYTYPE" && !typeMatches(unit, *type))
                {
                    continue;
                }
                auto mapSize = useX ? sim.terrain.getWidthInWorldUnits() : sim.terrain.getHeightInWorldUnits();
                if (passesCoordinate(useX ? unit.position.x : unit.position.z, *coordinate, mapSize))
                {
                    objective.satisfied = true;
                    return true;
                }
            }
            return false;
        }

        if (key == "AnyUnitPassesX" || key == "AnyUnitPassesZ")
        {
            if (objective.satisfied || objective.params.empty())
            {
                return objective.satisfied;
            }
            auto coordinate = parseObjectiveInt(objective.params[0]);
            if (!coordinate)
            {
                return false;
            }
            bool useX = key == "AnyUnitPassesX";
            auto mapSize = useX ? sim.terrain.getWidthInWorldUnits() : sim.terrain.getHeightInWorldUnits();
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != localPlayerId
                    && passesCoordinate(useX ? unit.position.x : unit.position.z, *coordinate, mapSize))
                {
                    objective.satisfied = true;
                    return true;
                }
            }
            return false;
        }

        if (key == "VictoryTimerRunsOut" || key == "DeathTimerRunsOut")
        {
            auto seconds = objective.params.empty() ? std::nullopt : parseObjectiveInt(objective.params[0]);
            if (!seconds)
            {
                return false;
            }
            // Retail stores seconds * 30 ticks and triggers when the
            // mission clock reaches it.
            return sim.gameTime.value >= static_cast<unsigned int>(*seconds) * SimTicksPerSecond;
        }

        return objective.satisfied;
    }

    void MissionObjectiveEvaluator::onUnitDied(const GameSimulation& sim, const UnitState& unit, MissionObjectiveRuntime& objective)
    {
        const auto& key = objective.key;
        // Retail event handlers test the unit's owner-index byte
        // directly: [unit+0xff] == 1 is the enemy player slot (OTA
        // Player=2) and == 0 is the local slot (OTA Player=1), matching
        // the player indices the campaign loader produces.
        bool enemy = unit.owner == PlayerId(1);

        if (key == "KillEnemyCommander")
        {
            if (enemy && isCommander(sim, unit))
            {
                objective.satisfied = true;
            }
        }
        else if (key == "CommanderKilled")
        {
            if (unit.owner == localPlayerId && isCommander(sim, unit))
            {
                objective.satisfied = true;
            }
        }
        else if (key == "KillAllMobileUnits")
        {
            if (enemy)
            {
                objective.satisfied = std::none_of(sim.units.begin(), sim.units.end(),
                    [&](const auto& p) {
                        const auto& u = p.second;
                        if (u.owner == localPlayerId || u.isDead())
                        {
                            return false;
                        }
                        auto it = sim.unitDefinitions.find(u.unitType);
                        return it != sim.unitDefinitions.end() && it->second.canMove;
                    });
            }
        }
        else if (key == "KillAllOfType" || key == "AllUnitsKilledOfType")
        {
            auto type = objectiveTypeParam(objective);
            if (!type)
            {
                return;
            }
            if (key == "KillAllOfType" && !enemy)
            {
                return;
            }
            if (!typeMatches(unit, *type))
            {
                return;
            }
            // The dying unit is already flagged dead, so a zero count
            // means it was the last one.
            objective.satisfied = std::none_of(sim.units.begin(), sim.units.end(),
                [&](const auto& p) {
                    const auto& u = p.second;
                    if (u.isDead() || !typeMatches(u, *type))
                    {
                        return false;
                    }
                    return key == "AllUnitsKilledOfType" || u.owner != localPlayerId;
                });
        }
        else if (key == "KillUnitType")
        {
            if (objective.remainingCount <= 0)
            {
                return;
            }
            auto type = objectiveTypeParam(objective);
            if (type && enemy && typeMatches(unit, *type))
            {
                if (--objective.remainingCount <= 0)
                {
                    objective.satisfied = true;
                }
            }
        }
        else if (key == "UnitTypeKilled")
        {
            auto type = objectiveTypeParam(objective);
            if (type && typeMatches(unit, *type))
            {
                if (--objective.remainingCount <= 0)
                {
                    objective.satisfied = true;
                }
            }
        }
    }

    void MissionObjectiveEvaluator::onUnitOwnerChanged(const GameSimulation& sim, const UnitState& unit, PlayerId previousOwner, MissionObjectiveRuntime& objective)
    {
        // Retail CaptureUnitType (TotalA.exe 0x48eeb0): the owner-change
        // event is dispatched before the transfer is applied, so the
        // tested owner index is the unit's *previous* owner; it must be
        // index 1 (the enemy slot). The new owner is never checked, the
        // unit's live/construction state is not inspected, and the
        // satisfied latch is never cleared by later changes.
        if (objective.key != "CaptureUnitType")
        {
            return;
        }
        auto type = objectiveTypeParam(objective);
        if (type && typeMatches(unit, *type) && previousOwner == PlayerId(1))
        {
            objective.satisfied = true;
        }
    }

    void MissionObjectiveEvaluator::onUnitOwnerChanged(const GameSimulation& sim, const UnitState& unit, PlayerId previousOwner)
    {
        if (missionResult != MissionResult::InProgress)
        {
            return;
        }
        for (auto& o : victoryObjectives)
        {
            onUnitOwnerChanged(sim, unit, previousOwner, o);
        }
        for (auto& o : defeatObjectives)
        {
            onUnitOwnerChanged(sim, unit, previousOwner, o);
        }
    }

    void MissionObjectiveEvaluator::onUnitDied(const GameSimulation& sim, const UnitState& unit)
    {
        if (missionResult != MissionResult::InProgress)
        {
            return;
        }
        for (auto& o : victoryObjectives)
        {
            onUnitDied(sim, unit, o);
        }
        for (auto& o : defeatObjectives)
        {
            onUnitDied(sim, unit, o);
        }
    }

    void MissionObjectiveEvaluator::tick(const GameSimulation& sim)
    {
        if (missionResult != MissionResult::InProgress)
        {
            return;
        }

        // Retail evaluates objectives once per 30 game ticks.
        if (sim.gameTime % GameTime(SimTicksPerSecond) != GameTime(0))
        {
            return;
        }

        // Victory first: when all victory objectives pass, the defeat
        // side is not evaluated this tick. Both sides share the same
        // debounce countdown.
        bool allVictory = std::all_of(victoryObjectives.begin(), victoryObjectives.end(),
            [&](auto& o) { return check(sim, o); });

        bool anyDefeat = false;
        if (!allVictory)
        {
            anyDefeat = std::any_of(defeatObjectives.begin(), defeatObjectives.end(),
                [&](auto& o) { return check(sim, o); });
        }

        if (!allVictory && !anyDefeat)
        {
            return;
        }

        if (resultCountdown < 0)
        {
            resultCountdown = 4;
            return;
        }
        if (--resultCountdown < 0)
        {
            missionResult = allVictory ? MissionResult::Victory : MissionResult::Defeat;
        }
    }
}
