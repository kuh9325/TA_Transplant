#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <rwe/sim/GameHash.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/util/match.h>

namespace rwe
{
    // Hash contract 2: ordered, presence-tagged composition and expanded dynamic
    // coverage. Peers must use the same build/assets; this is not a wire negotiation.
    constexpr std::uint32_t SimulationHashContractVersion = 2;
    inline std::uint32_t mixGameHash(std::uint32_t h, std::uint32_t value)
    { return (h ^ value) * 16777619u; }

    GameHash computeHashOf(GameHash hash);

    GameHash computeHashOf(float f);

    GameHash computeHashOf(bool b);

    GameHash computeHashOf(uint32_t i);

    GameHash computeHashOf(int32_t i);

    GameHash computeHashOf(const std::string& s);

    GameHash computeHashOf(const char* s);

    GameHash computeHashOf(const GamePlayerInfo& p);

    GameHash computeHashOf(const UnitState& u);
    GameHash computeHashOf(const BomberState& b);

    GameHash computeHashOf(const UnitPhysicsInfoGround& p);
    GameHash computeHashOf(const UnitPhysicsInfoAir& p);
    GameHash computeHashOf(const AirMovementStateTakingOff& p);
    GameHash computeHashOf(const AirMovementStateLanding& p);
    GameHash computeHashOf(const AirMovementStateFlying& p);

    GameHash computeHashOf(const SteeringInfo& s);

    GameHash computeHashOf(const Projectile& projectile);

    GameHash computeHashOf(const UnitBehaviorStateIdle&);

    GameHash computeHashOf(const UnitBehaviorStateBuilding& s);

    GameHash computeHashOf(const UnitBehaviorStateRepairing& s);

    GameHash computeHashOf(const UnitBehaviorStateReclaiming& s);

    GameHash computeHashOf(const UnitBehaviorStateCapturing& s);

    GameHash computeHashOf(const MapFeature& f);

    GameHash computeHashOf(const UnitBehaviorStateCreatingUnit&);


    GameHash computeHashOf(const UnitCreationStatusPending&);
    GameHash computeHashOf(const UnitCreationStatusDone&);
    GameHash computeHashOf(const UnitCreationStatusFailed&);

    GameHash computeHashOf(const UnitState::LifeStateAlive&);
    GameHash computeHashOf(const UnitState::LifeStateDead&);

    GameHash computeHashOf(const NavigationGoalLandingLocation&);

    GameHash computeHashOf(const NavigationStateIdle&);
    GameHash computeHashOf(const NavigationStateMoving& m);
    GameHash computeHashOf(const NavigationStateMovingToLandingSpot&);
    GameHash computeHashOf(const NavigationStateInfo& i);

    GameHash computeHashOf(const DiscreteRect& r);

    GameHash computeHashOf(const MoveOrder& o);
    GameHash computeHashOf(const AttackOrder& o);
    GameHash computeHashOf(const BuildOrder& o);
    GameHash computeHashOf(const BuggerOffOrder& o);
    GameHash computeHashOf(const CompleteBuildOrder& o);
    GameHash computeHashOf(const GuardOrder& o);
    GameHash computeHashOf(const RepairOrder& o);
    GameHash computeHashOf(const ReclaimOrder& o);
    GameHash computeHashOf(const PatrolOrder& o);
    GameHash computeHashOf(const WaitOrder& o);
    GameHash computeHashOf(const LandOrder& o);
    GameHash computeHashOf(const AircraftServiceState& s);
    GameHash computeHashOf(const ParalyzeOrder& o);
    GameHash computeHashOf(const WaitForAttackOrder& o);
    GameHash computeHashOf(const MakeSelectableOrder& o);
    GameHash computeHashOf(const AttackUnitTypeOrder& o);
    GameHash computeHashOf(const MissionBuildOrder& o);
    GameHash computeHashOf(const MissionFactoryBuildOrder& o);

    GameHash computeHashOf(const SelfDestructOrder& o);
    GameHash computeHashOf(const UnloadOrder& o);

    GameHash computeHashOf(const LoadOrder& o);
    GameHash computeHashOf(const BuildWeaponOrder& o);
    GameHash computeHashOf(const CommandFireOrder& o);
    GameHash computeHashOf(const CaptureOrder& o);

    GameHash computeHashOf(const UnitWeapon& w);

    GameHash computeHashOf(const MissionUnitState& s);

    GameHash computeHashOf(const CampaignAiPlayerState& st);
    GameHash computeHashOf(const CampaignAiPlayerState::DeadlockState& s);

    GameHash computeHashOf(const MeteorShower& m);

    GameHash computeHashOf(const GameSimulation& simulation);
    GameHash computeHashOf(const PathFollowingInfo& p);
    GameHash computeHashOf(const UnitPositionCache& p);
    GameHash computeHashOf(const FactoryBehaviorStateIdle& p);
    GameHash computeHashOf(const FactoryBehaviorStateCreatingUnit& p);
    GameHash computeHashOf(const FactoryBehaviorStateBuilding& p);
    GameHash computeHashOf(const UnitMesh& p);
    GameHash computeHashOf(const UnitMesh::MoveOperation& p);
    GameHash computeHashOf(const UnitMesh::TurnOperation& p);
    GameHash computeHashOf(const UnitMesh::SpinOperation& p);
    GameHash computeHashOf(const UnitMesh::StopSpinOperation& p);
    GameHash computeHashOf(const CobFunction& p);
    GameHash computeHashOf(const MissionInfo& p);
    GameHash computeHashOf(const MissionObjectiveRuntime& p);
    GameHash computeHashOf(const MissionObjectiveEvaluator& p);
    GameHash computeHashOf(const PathRequest& p);
    GameHash computeHashOf(const WinStatusUndecided& p);
    GameHash computeHashOf(const WinStatusWon& p);
    GameHash computeHashOf(const WinStatusDraw& p);


    template <typename... Ts>
    GameHash combineHashes(const Ts&... items);

    template <typename A, typename B> GameHash computeHashOf(const std::pair<A, B>& p);

    template <typename Val>
    GameHash computeHashOf(const Vector3x<Val>& v)
    {
        return combineHashes(v.x, v.y, v.z);
    }

    template <typename T, typename Tag>
    GameHash computeHashOf(const OpaqueId<T, Tag>& id)
    {
        return computeHashOf(id.value);
    }

    template <typename T>
    GameHash computeHashOf(const std::optional<T>& o)
    {
        return o ? combineHashes(1u, *o) : combineHashes(0u);
    }

    template <typename T>
    GameHash computeHashOf(const std::vector<T>& v)
    {
        std::uint32_t h = 2166136261u;
        std::uint32_t count = 0;
        for (const auto& x : v) { h = mixGameHash(h, computeHashOf(x).value); ++count; }
        return GameHash(mixGameHash(h, count));
    }

    template <typename T, std::size_t N>
    GameHash computeHashOf(const std::array<T, N>& v)
    {
        std::uint32_t h = 2166136261u;
        std::uint32_t count = 0;
        for (const auto& x : v) { h = mixGameHash(h, computeHashOf(x).value); ++count; }
        return GameHash(mixGameHash(h, count));
    }

    template <typename T>
    GameHash computeHashOf(const std::deque<T>& v)
    {
        std::uint32_t h = 2166136261u;
        std::uint32_t count = 0;
        for (const auto& x : v) { h = mixGameHash(h, computeHashOf(x).value); ++count; }
        return GameHash(mixGameHash(h, count));
    }

    template <typename T, typename Tag>
    GameHash computeHashOf(const VectorMap<T, Tag>& v)
    {
        std::uint32_t h = computeHashOf(v.firstFreeSlot()).value;
        v.forEachSlotState([&](auto index, auto id, const T* value, auto next) {
            h = mixGameHash(h, combineHashes(index, id, value != nullptr).value);
            h = mixGameHash(h, value ? computeHashOf(*value).value : computeHashOf(next).value);
        });
        return GameHash(h);
    }

    template <typename K, typename V>
    GameHash computeHashOf(const std::map<K, V>& m)
    {
        std::uint32_t h = 2166136261u;
        for (const auto& [key, value] : m) { h = mixGameHash(h, combineHashes(key, value).value); }
        return GameHash(mixGameHash(h, static_cast<uint32_t>(m.size())));
    }

    template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
    GameHash computeHashOf(const T& en)
    {
        return GameHash(static_cast<std::underlying_type_t<T>>(en));
    }

    template <typename... Ts>
    GameHash computeHashOf(const std::variant<Ts...>& v)
    {
        return combineHashes(static_cast<uint32_t>(v.index()),
            match(v, [](const auto& x) { return computeHashOf(x); }));
    }

    template <typename A, typename B>
    GameHash computeHashOf(const std::pair<A, B>& p)
    {
        return combineHashes(p.first, p.second);
    }

    template <typename... Ts>
    GameHash combineHashes(const Ts&... items)
    {
        uint32_t h = 2166136261u;
        ((h = mixGameHash(h, computeHashOf(items).value)), ...);
        return GameHash(h);
    }
}
