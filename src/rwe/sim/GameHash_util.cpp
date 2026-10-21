#include "GameHash_util.h"
#include <cctype>

#include <bit>
#include <cmath>
#include <limits>

namespace rwe
{
    GameHash computeHashOf(GameHash hash)
    {
        return hash;
    }

    GameHash computeHashOf(float f)
    {
        // Hash the IEEE-754 bit representation. Converting a float to an
        // integer is undefined when the value is out of range and produces
        // different results on arm64 vs x86 for negative values.
        // +0.0f and -0.0f are equivalent in the simulation, so -0.0f is
        // canonicalized to +0.0f. NaN payloads are platform-dependent, so
        // all NaNs hash to one canonical quiet-NaN pattern; a NaN in
        // authoritative state means the simulation has already diverged.
        if (std::isnan(f))
        {
            return GameHash(0x7FC00000u);
        }
        if (f == 0.0f)
        {
            f = 0.0f;
        }
        return GameHash(std::bit_cast<uint32_t>(f));
    }

    GameHash computeHashOf(bool b)
    {
        return b ? GameHash(1) : GameHash(0);
    }

    GameHash computeHashOf(uint32_t i)
    {
        return GameHash(i);
    }

    GameHash computeHashOf(int32_t i)
    {
        return GameHash(static_cast<uint32_t>(i));
    }

    GameHash computeHashOf(const std::string& s)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) { h = mixGameHash(h, c); }
        return GameHash(h);
    }

    GameHash computeHashOf(const char* s)
    {
        uint32_t h = 2166136261u;
        for (; *s; ++s) { h = mixGameHash(h, static_cast<unsigned char>(*s)); }
        return GameHash(h);
    }

    GameHash computeHashOf(const GamePlayerInfo& p)
    {
        return combineHashes(
            p.type,
            p.status,
            p.side,
            p.allyGroup,
            p.metal,
            p.maxMetal,
            p.energy,
            p.maxEnergy,
            p.startingMetal, p.startingEnergy,
            p.metalStalled,
            p.energyStalled,
            p.desiredMetalConsumptionBuffer,
            p.desiredEnergyConsumptionBuffer,
            p.previousDesiredMetalConsumptionBuffer,
            p.previousDesiredEnergyConsumptionBuffer,
            p.actualMetalConsumptionBuffer,
            p.actualEnergyConsumptionBuffer,
            p.metalProductionBuffer,
            p.energyProductionBuffer);
    }

    GameHash computeHashOf(const BomberState& b)
    {
        return combineHashes(b.target.has_value(), b.target, static_cast<unsigned int>(b.phase),
            b.goal, b.releaseTarget, b.nextScan.has_value(), b.nextScan);
    }

    namespace
    {
        // Per-call scratch only: canonical key order, no copies of payloads,
        // no cache/invalidation contract. Large containers use one allocation.
        template <typename Container, typename Key, typename Hash>
        GameHash hashCanonicalEntries(const Container& values, Key key, Hash hash)
        {
            using Entry = typename Container::value_type;
            std::array<const Entry*, 64> local;
            std::vector<const Entry*> overflow;
            auto entries = local.data();
            if (values.size() > local.size())
            { overflow.resize(values.size()); entries = overflow.data(); }
            std::size_t i = 0;
            for (const auto& value : values) { entries[i++] = &value; }
            std::sort(entries, entries + i, [&](auto a, auto b) { return key(*a) < key(*b); });
            uint32_t h = 2166136261u;
            for (std::size_t j = 0; j < i; ++j) { h = mixGameHash(h, hash(*entries[j]).value); }
            return GameHash(mixGameHash(h, static_cast<uint32_t>(i)));
        }

        template <typename Map> GameHash hashCanonicalMap(const Map& values)
        {
            return hashCanonicalEntries(values,
                [](const auto& v) -> const auto& { return v.first; },
                [](const auto& v) { return combineHashes(v.first, v.second); });
        }
        template <typename Set> GameHash hashCanonicalSet(const Set& values)
        {
            return hashCanonicalEntries(values,
                [](const auto& v) -> const auto& { return v; },
                [](const auto& v) { return computeHashOf(v); });
        }

        unsigned int threadIndex(const CobEnvironment& env, const CobThread* thread)
        {
            for (unsigned int i = 0; i < env.threads.size(); ++i)
            { if (env.threads[i].get() == thread) { return i; } }
            // Invalid references are represented without dereferencing them.
            return std::numeric_limits<unsigned int>::max();
        }

        GameHash hashCob(const CobEnvironment& env)
        {
            uint32_t h = computeHashOf(env._statics).value;
            auto add = [&](const auto& value) { h = mixGameHash(h, computeHashOf(value).value); };
            add(static_cast<uint32_t>(env.threads.size()));
            for (const auto& t : env.threads)
            {
                add(t->signalMask); add(t->stack.elements()); add(t->callStack.elements());
                // Only a finished result is consumed. Old saves may contain
                // unspecified unused returnValue bytes for running threads.
                const bool finished = std::find(env.finishedQueue.begin(), env.finishedQueue.end(), t.get()) != env.finishedQueue.end();
                add(finished); if (finished) { add(t->returnValue); add(t->returnLocals); }
            }
            add(static_cast<uint32_t>(env.readyQueue.size()));
            for (auto* t : env.readyQueue) { add(threadIndex(env, t)); }
            add(static_cast<uint32_t>(env.blockedQueue.size()));
            for (const auto& [status, t] : env.blockedQueue)
            {
                add(threadIndex(env, t)); add(static_cast<uint32_t>(status.condition.index()));
                std::visit([&](const auto& c) { add(c.object); add(c.axis); }, status.condition);
            }
            add(static_cast<uint32_t>(env.sleepingQueue.size()));
            for (const auto& [time, t] : env.sleepingQueue) { add(time); add(threadIndex(env, t)); }
            add(static_cast<uint32_t>(env.finishedQueue.size()));
            for (auto* t : env.finishedQueue) { add(threadIndex(env, t)); }
            return GameHash(h);
        }

        GameHash hashWeaponReferences(const UnitState& u)
        {
            uint32_t h = 2166136261u;
            for (const auto& weapon : u.weapons)
            {
                if (!weapon) { continue; }
                const auto* attack = std::get_if<UnitWeaponStateAttacking>(&weapon->state);
                const auto* aim = attack ? std::get_if<UnitWeaponStateAttacking::AimInfo>(&attack->attackInfo) : nullptr;
                if (aim) { h = mixGameHash(h, threadIndex(*u.cobEnvironment, aim->thread)); }
            }
            return GameHash(h);
        }
    }

    GameHash computeHashOf(const UnitState& u)
    {
        return combineHashes(
            u.unitType,
            u.pieces, u.cobEnvironment ? hashCob(*u.cobEnvironment) : GameHash(0),
            u.cobEnvironment ? hashWeaponReferences(u) : GameHash(0),
            u.buildOrderUnitId, u.buildQueue, u.factoryState,
            u.position,
            u.owner,
            u.rotation,
            u.physics,
            u.hitPoints,
            static_cast<uint32_t>(u.kills),
            u.lifeState,
            u.navigationState,
            u.behaviourState,
            u.inBuildStance,
            u.yardOpen,
            u.inCollision,
            u.armored,
            u.busy,
            u.buggerOff,
            u.fireOrders,
            u.moveOrders,
            u.orders,
            u.lastDamagedAt,
            u.lastDamagedBy,
            u.lastDamagedByPlayer,
            u.missionState,
            u.buildTimeCompleted,
            u.activated,
            u.isSufficientlyPowered,
            u.energyProductionBuffer,
            u.metalProductionBuffer,
            u.previousEnergyConsumptionBuffer,
            u.previousMetalConsumptionBuffer,
            u.energyConsumptionBuffer,
            u.metalConsumptionBuffer,
            u.weapons,
            u.backgroundOrders,
            u.carriedBy,
            u.attachSlot,
            u.passengers,
            u.selfDestruct,
            u.nextServiceScan.has_value(), u.nextServiceScan,
            static_cast<bool>(u.aircraftService), u.aircraftService ? computeHashOf(*u.aircraftService) : GameHash(0),
            u.nextKamikazeScan.has_value(),
            u.nextKamikazeScan,
            static_cast<bool>(u.bomberState),
            u.bomberState ? computeHashOf(*u.bomberState) : GameHash(0),
            u.cloakRequested,
            u.cloaked,
            u.cloakBlockedUntil);
    }

    GameHash computeHashOf(const UnitWeapon& w)
    {
        GameHash state = std::visit([](const auto& attack) -> GameHash {
            using T = std::decay_t<decltype(attack)>;
            if constexpr (std::is_same_v<T, UnitWeaponStateIdle>) { return GameHash(0); }
            else
            {
                auto info = std::visit([](const auto& i) -> GameHash {
                    using I = std::decay_t<decltype(i)>;
                    if constexpr (std::is_same_v<I, UnitWeaponStateAttacking::IdleInfo>) { return GameHash(0); }
                    else if constexpr (std::is_same_v<I, UnitWeaponStateAttacking::AimInfo>)
                    { return combineHashes(i.lastHeading, i.lastPitch); } // thread identity is unit-relative
                    else { return combineHashes(i.heading, i.pitch, i.targetPosition, i.firingPiece, i.burstsFired, i.readyTime); }
                }, attack.attackInfo);
                return combineHashes(attack.target, static_cast<uint32_t>(attack.attackInfo.index()), info);
            }
        }, w.state);
        return combineHashes(w.weaponType, w.readyTime, w.stockpileCount, w.ballisticZOffset,
            static_cast<uint32_t>(w.state.index()), state);
    }

    GameHash computeHashOf(const UnitPhysicsInfoGround& p)
    {
        return combineHashes(p.steeringInfo, p.currentSpeed);
    }

    GameHash computeHashOf(const UnitPhysicsInfoAir& p)
    {
        return combineHashes(p.movementState);
    }

    GameHash computeHashOf(const AirMovementStateTakingOff& p)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const AirMovementStateLanding& p)
    {
        return combineHashes(p.landingFailed, p.shouldAbort);
    }

    GameHash computeHashOf(const AirMovementStateFlying& p)
    {
        return combineHashes(p.targetPosition, p.shouldLand, p.currentVelocity);
    }

    GameHash computeHashOf(const SteeringInfo& s)
    {
        return combineHashes(s.targetAngle, s.targetSpeed, s.shouldTakeOff);
    }

    GameHash computeHashOf(const Vector3f& v)
    {
        return combineHashes(v.x, v.y, v.z);
    }

    GameHash computeHashOf(const Projectile& projectile)
    {
        GameHash h = combineHashes(
            projectile.owner,
            projectile.groundBounce, projectile.createdAt,
            projectile.weaponType,
            projectile.firedBy,
            projectile.targetUnit,
            projectile.isDead,
            projectile.dieOnFrame,
            projectile.position,
            projectile.origin,
            projectile.velocity,
            projectile.damageRadius,
            projectile.targetProjectile,
            projectile.targetPosition);

        return combineHashes(h, hashCanonicalMap(projectile.damage));
    }

    GameHash computeHashOf(const UnitBehaviorStateIdle&)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const UnitBehaviorStateBuilding& s)
    {
        return computeHashOf(s.targetUnit);
    }

    GameHash computeHashOf(const UnitBehaviorStateRepairing& s)
    {
        return combineHashes(
            s.targetUnit,
            s.repairProgress);
    }

    GameHash computeHashOf(const UnitBehaviorStateReclaiming& s)
    {
        return combineHashes(
            s.target,
            s.targetPosition, s.nanoParticleOrigin.has_value());
    }

    GameHash computeHashOf(const UnitBehaviorStateCapturing& s)
    {
        return combineHashes(
            s.target,
            s.targetPosition);
    }

    GameHash computeHashOf(const MapFeature& f)
    {
        uint32_t h = 2166136261u;
        auto add = [&](const auto& v) { h = (h ^ computeHashOf(v).value) * 16777619u; };
        add(f.position); add(f.rotation);
        add(f.reclaimWorkDone); add(f.reclaimMetalGiven); add(f.reclaimEnergyGiven);
        add(static_cast<uint32_t>(f.damageAccumulated)); add(static_cast<uint32_t>(f.event));
        // Main animation completion changes feature lifecycle; the independent
        // shadow cursor is presentation only and must not desync peers.
        add(f.animationFrame); add(f.animationDelay);
        add(static_cast<uint32_t>(f.sparkCountdown));
        return GameHash(h);
    }

    GameHash computeHashOf(const UnitBehaviorStateCreatingUnit& s)
    {
        return combineHashes(
            s.position,
            s.owner,
            s.unitType,
            s.status);
    }

    GameHash computeHashOf(const UnitCreationStatusPending&)
    {
        return GameHash(0);
    }
    GameHash computeHashOf(const UnitCreationStatusDone& s)
    {
        return combineHashes(s.unitId);
    }

    GameHash computeHashOf(const UnitCreationStatusFailed&)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const UnitState::LifeStateAlive&)
    {
        return GameHash(0);
    }
    GameHash computeHashOf(const UnitState::LifeStateDead& s)
    {
        return computeHashOf(s.leaveCorpse);
    }

    GameHash computeHashOf(const NavigationGoalLandingLocation&)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const NavigationStateIdle&)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const NavigationStateMoving& m)
    {
        return combineHashes(
            m.movementGoal, m.path,
            m.pathDestination,
            m.pathRequested,
            m.pathExhausted,
            m.pathExhaustedAt);
    }

    GameHash computeHashOf(const NavigationStateMovingToLandingSpot& m)
    {
        return combineHashes(m.landingLocation);
    }

    GameHash computeHashOf(const NavigationStateInfo& i)
    {
        return combineHashes(i.desiredDestination, i.unitPositionCache, i.state);
    }

    GameHash computeHashOf(const DiscreteRect& r)
    {
        return combineHashes(r.x, r.y, r.width, r.height);
    }

    GameHash computeHashOf(const MoveOrder& o)
    {
        return combineHashes(o.destination);
    }

    GameHash computeHashOf(const AttackOrder& o)
    {
        return combineHashes(o.target);
    }

    GameHash computeHashOf(const BuildOrder& o)
    {
        return combineHashes(o.unitType, o.position);
    }

    GameHash computeHashOf(const BuggerOffOrder& o)
    {
        return combineHashes(o.rect);
    }

    GameHash computeHashOf(const CompleteBuildOrder& o)
    {
        return combineHashes(o.target);
    }

    GameHash computeHashOf(const GuardOrder& o)
    {
        return combineHashes(o.target);
    }

    GameHash computeHashOf(const RepairOrder& o)
    {
        return combineHashes(o.target);
    }

    GameHash computeHashOf(const ReclaimOrder& o)
    {
        return combineHashes(o.target);
    }

    GameHash computeHashOf(const PatrolAssistTarget& t)
    {
        return combineHashes(t.target);
    }

    GameHash computeHashOf(const PatrolRepairTarget& t)
    {
        return combineHashes(t.target);
    }

    GameHash computeHashOf(const PatrolOrder& o)
    {
        return combineHashes(o.waypoints, o.nextWaypointIndex, o.scanTicks, o.divertTarget);
    }

    GameHash computeHashOf(const CaptureOrder& o)
    {
        return combineHashes(o.target, o.progress, o.workRequired, o.workTicks);
    }

    GameHash computeHashOf(const LandOrder& o) { return computeHashOf(o.target); }

    GameHash computeHashOf(const AircraftServiceState& s)
    {
        std::uint32_t h = 2166136261u;
        for (auto value : {s.facility.value, static_cast<unsigned int>(s.piece), static_cast<unsigned int>(s.phase),
            static_cast<unsigned int>(s.loiterBearing.value), computeHashOf(s.goal.x).value,
            computeHashOf(s.goal.y).value, computeHashOf(s.goal.z).value, static_cast<unsigned int>(s.automatic)})
        {
            h = (h ^ value) * 16777619u;
        }
        return GameHash(h);
    }

    GameHash computeHashOf(const ParalyzeOrder& o)
    {
        // Credit and the active deadline have different meanings. Avoid
        // the additive hash's credit/deadline swaps and null/zero alias.
        std::uint32_t h = 2166136261u;
        for (auto value : {o.credit, static_cast<std::uint32_t>(o.resumeAt.has_value()), o.resumeAt.value_or(GameTime(0)).value})
        {
            h = (h ^ value) * 16777619u;
        }
        return GameHash(h);
    }

    GameHash computeHashOf(const WaitOrder& o)
    {
        return combineHashes(o.ticksRemaining);
    }

    GameHash computeHashOf(const WaitForAttackOrder& o)
    {
        return combineHashes(o.target, o.armed, o.observedDamagedAt);
    }

    GameHash computeHashOf(const MakeSelectableOrder&)
    {
        return GameHash(0);
    }

    GameHash computeHashOf(const AttackUnitTypeOrder& o)
    {
        return combineHashes(o.unitType, o.nextScanTime, o.currentTarget);
    }

    GameHash computeHashOf(const MissionBuildOrder& o)
    {
        return combineHashes(o.unitType, o.position, o.attemptsRemaining, o.retryAt);
    }

    GameHash computeHashOf(const MissionFactoryBuildOrder& o)
    {
        return combineHashes(o.unitType, o.count, o.enqueued);
    }

    GameHash computeHashOf(const SelfDestructOrder& o)
    {
        return combineHashes(o.countdownTicks, o.fuseTicks);
    }

    GameHash computeHashOf(const UnloadOrder& o)
    {
        return combineHashes(o.destination, o.pops, o.claimActive, o.cycleTicks, o.dropRadius);
    }

    GameHash computeHashOf(const BuildWeaponOrder& o)
    {
        return combineHashes(o.remaining, o.progressTicks);
    }

    GameHash computeHashOf(const CommandFireOrder& o)
    {
        return combineHashes(o.target, o.observedReadyTime);
    }

    GameHash computeHashOf(const LoadOrder& o)
    {
        return combineHashes(o.target, o.attempts, o.waitTicks, o.pickupThreadStarted, o.phase, o.slot);
    }

    GameHash computeHashOf(const MissionUnitState& s)
    {
        return combineHashes(s.selectable, s.immunity, s.aiIgnore);
    }

    GameHash computeHashOf(const CampaignAiPlayerState& st)
    {
        return combineHashes(
            st.player,
            st.unitWeights,
            st.unitLimits,
            st.nextRepartition,
            st.nextFactoryScan,
            st.nextBuilderScan,
            st.nextAttackScan,
            st.nextDeadlockScan,
            st.groups,
            st.attackTarget,
            st.deadlockedFactories);
    }

    GameHash computeHashOf(const CampaignAiPlayerState::DeadlockState& s)
    {
        return combineHashes(
            s.blockedSince,
            s.reclaimTarget,
            s.reclaimBuilder,
            s.reclaimIssuedAt,
            s.reclaimAttempts,
            s.reclaimStalledAt, s.reclaimBestDistance, s.reclaimDeadline,
            hashCanonicalSet(s.reclaimAvoid),
            hashCanonicalMap(s.evictAttempts));
    }

    GameHash computeHashOf(const MeteorShower& m)
    {
        // resolved config is constant for a running sim but is hashed
        // too so a config divergence cannot hide behind equal state
        return combineHashes(
            m.weaponType,
            m.radius,
            m.hitStepTicks,
            m.durationTicks,
            m.intervalTicks,
            m.enabled,
            m.active,
            m.nextStrikeTime,
            m.timeStrikeEnds,
            m.nextHitTime,
            m.originX,
            m.originZ,
            m.targetX,
            m.targetZ);
    }

    // Same unsigned bytes and locale conversion as toUpper, without a copy.
    GameHash hashUppercase(const std::string& text)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : text)
        { h = mixGameHash(h, static_cast<unsigned char>(std::toupper(c))); }
        return GameHash(h);
    }

    GameHash hashFeatureIdentities(const GameSimulation& sim)
    {
        uint32_t h = 2166136261u;
        // Bounded per-call memoization of repeated definition names. Nothing
        // survives computeHash(), so edits/load/ID reuse need no invalidation.
        // A slot collision only recomputes a name; it never substitutes one.
        struct NameHash { bool valid{false}; FeatureDefinitionId id{}; GameHash hash{0}; };
        std::array<NameHash, 64> names{};
        for (const auto& [id, f] : sim.features)
        {
            auto& entry = names[f.featureName.value % names.size()];
            if (!entry.valid || entry.id != f.featureName)
            {
                entry = {true, f.featureName, hashUppercase(sim.getFeatureDefinition(f.featureName).name)};
            }
            h = (h ^ id.value) * 16777619u;
            h = (h ^ entry.hash.value) * 16777619u;
        }
        for (auto id : sim.activeFeatures) { h = (h ^ id.value) * 16777619u; }
        // The saved RNG drives ignition/spread; different streams must desync
        // immediately, even before the next spark event consumes a draw.
        // minstd_rand's next state is a bijection of its valid current state.
        // A local copy avoids both mutation and per-tick stream allocation.
        auto rng = sim.rng;
        return combineHashes(GameHash(h), static_cast<uint32_t>(rng()));
    }

    GameHash computeHashOf(const GameSimulation& simulation)
    {
        return combineHashes(
            simulation.gameTime,
            SimulationHashContractVersion,
            simulation.gameStatus, simulation.pathRequests, simulation.unitCreationRequests,
            simulation.missionInfo, simulation.missionObjectives,
            simulation.currentWindGenerationFactor,
            simulation.noSeaLevelTrigger, simulation.waterDoesDamage, simulation.waterDamage,
            simulation.minWindSpeed, simulation.maxWindSpeed, simulation.tidalStrength,
            simulation.occupiedGrid.stateHash(), simulation.metalGrid.stateHash(), simulation.geoGrid.stateHash(),
            simulation.players,
            simulation.units,
            simulation.projectiles,
            simulation.features,
            hashFeatureIdentities(simulation),
            simulation.campaignAi,
            simulation.meteorShower,
            // skirmish/campaign settings that steer future sim state
            simulation.commanderDeathContinues,
            simulation.computerResourceScale,
            simulation.lineOfSightEnabled,
            simulation.maxUnitsPerPlayer,
            simulation.mapGravity,
            simulation.projectileWind,
            simulation.nextWindSpeedChange);
    }
    GameHash computeHashOf(const PathFollowingInfo& p)
    {
        return combineHashes(p.path.waypoints, p.path.isPartial, p.pathCreationTime,
            static_cast<uint32_t>(std::distance(p.path.waypoints.cbegin(), p.currentWaypoint)));
    }
    GameHash computeHashOf(const UnitPositionCache& p) { return combineHashes(p.unitId, p.position, p.cachedAtTime); }
    GameHash computeHashOf(const FactoryBehaviorStateIdle&) { return GameHash(0); }
    GameHash computeHashOf(const FactoryBehaviorStateCreatingUnit& p) { return combineHashes(p.unitType, p.owner, p.position, p.rotation, p.status); }
    GameHash computeHashOf(const FactoryBehaviorStateBuilding& p)
    {
        // The beam origin is presentation; target identity controls production.
        return combineHashes(p.targetUnit.has_value(), p.targetUnit ? computeHashOf(p.targetUnit->first) : GameHash(0));
    }
    GameHash computeHashOf(const UnitMesh& p)
    {
        // Pose and pending operations drive QueryBuildInfo, aiming, docking,
        // attachment and WAIT_FOR_MOVE/TURN; interpolation and shading do not.
        if (!p.xMoveOperation && !p.yMoveOperation && !p.zMoveOperation
            && !p.xTurnOperation && !p.yTurnOperation && !p.zTurnOperation)
        {
            // All six absent optionals have the same contract-2 presence hash.
            // No pose or pending operation is omitted. This common path avoids
            // instantiating/evaluating six nested optional/variant visitors.
            uint32_t h = combineHashes(p.offset, p.rotationX, p.rotationY, p.rotationZ).value;
            constexpr auto absent = (2166136261u ^ 0u) * 16777619u;
            for (unsigned i = 0; i < 6; ++i) { h = mixGameHash(h, absent); }
            return GameHash(h);
        }
        return combineHashes(p.offset, p.rotationX, p.rotationY, p.rotationZ,
            p.xMoveOperation, p.yMoveOperation, p.zMoveOperation,
            p.xTurnOperation, p.yTurnOperation, p.zTurnOperation);
    }
    GameHash computeHashOf(const UnitMesh::MoveOperation& p) { return combineHashes(p.targetPosition, p.speed); }
    GameHash computeHashOf(const UnitMesh::TurnOperation& p) { return combineHashes(p.targetAngle, p.speed); }
    GameHash computeHashOf(const UnitMesh::SpinOperation& p) { return combineHashes(p.currentSpeed, p.targetSpeed, p.acceleration); }
    GameHash computeHashOf(const UnitMesh::StopSpinOperation& p) { return combineHashes(p.currentSpeed, p.deceleration); }
    GameHash computeHashOf(const CobFunction& p) { return combineHashes(p.instructionIndex, p.localCount, p.locals); }
    GameHash computeHashOf(const MissionInfo& p)
    {
        return combineHashes(p.localPlayerId, hashCanonicalSet(p.useOnlyUnits));
    }
    GameHash computeHashOf(const MissionObjectiveRuntime& p)
    { return combineHashes(p.key, p.params, p.polarity, p.satisfied, p.remainingCount); }
    GameHash computeHashOf(const MissionObjectiveEvaluator& p)
    {
        return combineHashes(p.getLocalPlayerId(), p.getVictoryObjectives(), p.getDefeatObjectives(), p.getResultCountdown(), p.result());
    }
    GameHash computeHashOf(const PathRequest& p) { return computeHashOf(p.unitId); }
    GameHash computeHashOf(const WinStatusUndecided&) { return GameHash(0); }
    GameHash computeHashOf(const WinStatusWon& p) { return computeHashOf(p.winner); }
    GameHash computeHashOf(const WinStatusDraw&) { return GameHash(0); }

}
