#include "UnitBehaviorService.h"
#include "Veterancy.h"
#include <rwe/cob/CobExecutionContext.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitBehaviorService_util.h>
#include <rwe/sim/cob.h>
#include <rwe/sim/movement.h>
#include <rwe/util/Index.h>
#include <rwe/util/match.h>

namespace rwe
{
    SimScalar getTargetAltitude(const MapTerrain& terrain, SimScalar x, SimScalar z, const UnitDefinition& unitDefinition)
    {
        return rweMax(terrain.getHeightAt(x, z), terrain.getSeaLevel()) + unitDefinition.cruiseAltitude;
    }

    UnitBehaviorService::UnitBehaviorService(GameSimulation* sim)
        : sim(sim)
    {
    }

    void UnitBehaviorService::onCreate(UnitId unitId)
    {
        auto& unit = sim->getUnitState(unitId);
        const auto& unitDefinition = sim->unitDefinitions.at(unit.unitType);

        unit.cobEnvironment->createThread("Create", std::vector<int>());

        // set speed for metal extractors
        if (unitDefinition.extractsMetal != Metal(0))
        {
            auto footprint = sim->computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
            auto metalValue = sim->metalGrid.accumulate(sim->metalGrid.clipRegion(footprint), 0u, std::plus<>());
            unit.cobEnvironment->createThread("SetSpeed", {static_cast<int>(metalValue)});
        }

        runUnitCobScripts(*sim, unitId);

        // measure z distances for ballistics
        for (int i = 0; i < getSize(unit.weapons); ++i)
        {
            auto& weapon = unit.weapons[i];
            if (!weapon)
            {
                continue;
            }
            auto localAimingPoint = getLocalAimingPoint(unitId, i);
            auto localFiringPoint = getLocalFiringPoint(unitId, i);
            weapon->ballisticZOffset = localFiringPoint.z - localAimingPoint.z;
        }
    }

    void UnitBehaviorService::updateWind(SimScalar windGenerationFactor, SimAngle windDirection)
    {
        int cobWindSpeed = static_cast<int>(toCobSpeed(windGenerationFactor).value);
        int cobWindDirection = toCobAngle(windDirection).value;

        for (auto& [id, unit] : sim->units)
        {
            const auto& unitDefinition = sim->unitDefinitions.at(unit.unitType);
            if (unitDefinition.windGenerator != Energy(0))
            {
                unit.cobEnvironment->createThread("SetSpeed", {cobWindSpeed});
                unit.cobEnvironment->createThread("SetDirection", {cobWindDirection});
            }
        }
    }

    void UnitBehaviorService::update(UnitId unitId)
    {
        auto unitInfo = sim->getUnitInfo(unitId);
        if (unitInfo.state->isDead() || unitInfo.state->carriedBy) { return; }

        bool primaryBlocked = false;
        if (!unitInfo.state->orders.empty())
        {
            if (auto* paralysis = std::get_if<ParalyzeOrder>(&unitInfo.state->orders.front()))
            {
                primaryBlocked = !handleParalyzeOrder(unitInfo, *paralysis);
                if (!primaryBlocked)
                {
                    unitInfo.state->orders.pop_front();
                }
            }
        }

        // Clear steering targets.
        match(
            unitInfo.state->physics,
            [&](UnitPhysicsInfoGround& p) {
                p.steeringInfo = SteeringInfo{
                    unitInfo.state->rotation,
                    0_ss,
                };
            },
            [&](UnitPhysicsInfoAir& p) {
                match(
                    p.movementState,
                    [&](AirMovementStateFlying& s) {
                        s.targetPosition = unitInfo.state->position;
                    },
                    [&](const AirMovementStateTakingOff&) {
                        // do nothing
                    },
                    [&](const AirMovementStateLanding&) {
                        // do nothing
                    });
            });

        // clear navigation targets
        if (!primaryBlocked)
        {
            unitInfo.state->navigationState.desiredDestination = std::nullopt;
        }

        // the nanolathe emitter only runs on ticks where a work handler
        // below actually applies work; clear last tick's marker so a stale
        // work state can never keep the beam alive on its own
        unitInfo.state->activeNanolatheTarget = std::nullopt;

        if (!primaryBlocked && unitInfo.definition->canFly && !unitInfo.state->aircraftService
            && !unitInfo.state->isBeingBuilt(*unitInfo.definition))
        {
            const auto& orders = unitInfo.state->orders;
            const bool opportunity = orders.empty() || std::holds_alternative<PatrolOrder>(orders.front())
                || std::holds_alternative<GuardOrder>(orders.front())
                || (!isBomber(unitInfo) && std::holds_alternative<AttackOrder>(orders.front()));
            if (opportunity && unitInfo.state->hitPoints < (unitInfo.definition->maxHitPoints >> 2) * 3
                && (!unitInfo.state->nextServiceScan || sim->gameTime >= *unitInfo.state->nextServiceScan))
            {
                unitInfo.state->nextServiceScan = sim->gameTime + GameTime(30);
                seekAircraftService(unitInfo);
            }
        }
        const bool servicing = unitInfo.state->aircraftService && updateAircraftService(unitInfo, primaryBlocked);

        // Run unit and weapon AI
        if (!unitInfo.state->isBeingBuilt(*unitInfo.definition))
        {
            if (!primaryBlocked && !servicing)
            {
                if (unitInfo.definition->kamikaze)
                {
                    updateKamikazeStandby(unitInfo);
                    if (unitInfo.state->isDead()) { return; }
                }

                if (isBomber(unitInfo)) { updateBomberStandby(unitInfo); }

                // check our build queue
                if (!unitInfo.state->buildQueue.empty())
                {
                    auto& entry = unitInfo.state->buildQueue.front();
                    if (handleBuild(unitInfo, entry.first))
                    {
                        if (entry.second > 1)
                        {
                            --entry.second;
                        }
                        else
                        {
                            unitInfo.state->buildQueue.pop_front();
                        }
                    }
                }
                else
                {
                    clearBuild(unitInfo);
                }

                // check our orders
                if (!unitInfo.state->orders.empty())
                {
                    auto& order = unitInfo.state->orders.front();

                    // process move orders
                    if (handleOrder(unitInfo, order))
                    {
                        unitInfo.state->orders.pop_front();
                        unitInfo.state->bomberState.reset();
                        unitInfo.state->buildOrderUnitId = std::nullopt;
                    }
                }
                else if (auto airPhysics = std::get_if<UnitPhysicsInfoAir>(&unitInfo.state->physics); airPhysics != nullptr)
                {
                    match(
                        airPhysics->movementState,
                        [&](AirMovementStateFlying& m) {
                            if (navigateTo(unitInfo, NavigationGoalLandingLocation()) == NavigationResult::Arrived)
                            {
                                m.shouldLand = true;
                            }
                        },
                        [&](const AirMovementStateLanding&) {
                            // do nothing
                        },
                        [&](const AirMovementStateTakingOff&) {
                            // do nothing
                        });
                }
                else
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                }

            }

            if (unitInfo.state->isDead()) { return; }

            // background orders (retail's parallel order slot, e.g. `bw`)
            // run alongside the foreground queue without blocking it
            if (!servicing && !unitInfo.state->backgroundOrders.empty())
            {
                auto& order = unitInfo.state->backgroundOrders.front();
                if (handleOrder(unitInfo, order))
                {
                    unitInfo.state->backgroundOrders.pop_front();
                }
            }

            // interactive self-destruct (retail SELFDESTRUCT unit
            // command, Ctrl/Alt+D) is a unit state, not a queue order:
            // it counts down in parallel with whatever the unit is
            // doing and survives order-queue changes
            updateSelfDestruct(unitId);

            if (unitInfo.state->isDead()) { return; }

            for (Index i = 0; !primaryBlocked && !unitInfo.state->aircraftService && i < getSize(unitInfo.state->weapons); ++i)
            {
                if (i != 0 && unitInfo.state->bomberState && unitInfo.state->bomberState->target) { continue; }
                updateWeapon(unitId, i);
            }
        }

        if (unitInfo.definition->isMobile)
        {
            if (unitInfo.state->aircraftService && unitInfo.state->aircraftService->docked()) { return; }
            updateNavigation(unitInfo);
            if (unitInfo.state->aircraftService) { steerAircraftService(unitInfo); }

            applyUnitSteering(unitInfo);

            auto previouslyWasMoving = !areCloserThan(unitInfo.state->previousPosition, unitInfo.state->position, 0.1_ssf);

            updateUnitPosition(unitInfo);

            auto currentlyIsMoving = !areCloserThan(unitInfo.state->previousPosition, unitInfo.state->position, 0.1_ssf);

            if (currentlyIsMoving && !previouslyWasMoving)
            {
                unitInfo.state->cobEnvironment->createThread("StartMoving");
            }
            else if (!currentlyIsMoving && previouslyWasMoving)
            {
                unitInfo.state->cobEnvironment->createThread("StopMoving");
            }

            // do physics transitions
            match(
                unitInfo.state->physics,
                [&](const UnitPhysicsInfoGround& p) {
                    if (p.steeringInfo.shouldTakeOff)
                    {
                        transitionFromGroundToAir(unitInfo);
                    }
                },
                [&](UnitPhysicsInfoAir& p) {
                    match(
                        p.movementState,
                        [&](const AirMovementStateTakingOff&) {
                            auto targetHeight = getTargetAltitude(sim->terrain, unitInfo.state->position.x, unitInfo.state->position.z, *unitInfo.definition);
                            if (unitInfo.state->position.y == targetHeight)
                            {
                                p.movementState = AirMovementStateFlying();
                            }
                        },
                        [&](AirMovementStateLanding& m) {
                            if (m.shouldAbort)
                            {
                                unitInfo.state->activate();
                                p.movementState = AirMovementStateFlying();
                            }
                            else
                            {
                                auto targetHeight = sim->terrain.getHeightAt(unitInfo.state->position.x, unitInfo.state->position.z);
                                if (unitInfo.state->position.y == targetHeight)
                                {
                                    if (!tryTransitionFromAirToGround(unitInfo))
                                    {
                                        m.landingFailed = true;
                                    }
                                }
                            }
                        },
                        [&](const AirMovementStateFlying& m) {
                            if (m.shouldLand)
                            {
                                p.movementState = AirMovementStateLanding();
                                unitInfo.state->deactivate();
                            }
                        });
                });
        }
    }

    bool UnitBehaviorService::handleParalyzeOrder(UnitInfo unitInfo, ParalyzeOrder& order)
    {
        if (order.resumeAt && sim->gameTime < *order.resumeAt)
        {
            return false;
        }
        if (order.credit == 0)
        {
            return true;
        }
        // Retail uses a signed cap and unsigned storage/addition.
        // No decrement scan: additional hits wait in credit until expiry.
        const auto ticks = order.credit <= 0x7fffffffu ? std::min(order.credit, 1800u) : order.credit;
        unitInfo.state->clearWeaponTargets();
        order.resumeAt = sim->gameTime + GameTime(ticks);
        order.credit = 0;
        // A fresh retail paralyze node owns no movement payload; releasing
        // it does not destroy a goal owned by the interrupted order. Keep
        // RWE's existing navigation/physics, scripts and independent queues.
        // The weapon pass above is withheld while the head wait owns the
        // slots; RWE has no separate retail slot-autonomy bit to release.
        return false;
    }

    SimVector UnitBehaviorService::getUnitPositionWithCache(UnitState& s, UnitId unitId)
    {
        if (s.navigationState.unitPositionCache && s.navigationState.unitPositionCache->unitId == unitId)
        {
            const auto& pos = s.navigationState.unitPositionCache->position;
            const auto& time = s.navigationState.unitPositionCache->cachedAtTime;
            if (sim->gameTime - time < GameTime(SimTicksPerSecond))
            {
                return pos;
            }
        }

        const auto& pos = sim->getUnitState(unitId).position;
        s.navigationState.unitPositionCache = UnitPositionCache{
            unitId,
            pos,
            sim->gameTime,
        };

        return pos;
    }


    void UnitBehaviorService::updateNavigation(UnitInfo unitInfo)
    {
        const auto& goal = unitInfo.state->navigationState.desiredDestination;

        if (!goal)
        {
            unitInfo.state->navigationState.state = NavigationStateIdle();
            return;
        }

        auto resolvedGoal = match(
            *goal,
            [&](const NavigationGoalLandingLocation&) {
                const auto llState = std::get_if<NavigationStateMovingToLandingSpot>(&unitInfo.state->navigationState.state);
                if (llState)
                {
                    return std::make_optional<MovingStateGoal>(llState->landingLocation);
                }
                else
                {
                    auto landingLocation = findLandingLocation(*sim, unitInfo);
                    if (!landingLocation)
                    {
                        return std::optional<MovingStateGoal>();
                    }
                    unitInfo.state->navigationState.state = NavigationStateMovingToLandingSpot{*landingLocation};
                    return std::make_optional<MovingStateGoal>(*landingLocation);
                }
            },
            [&](const SimVector& v) {
                return std::make_optional<MovingStateGoal>(v);
            },
            [&](const DiscreteRect& r) {
                return std::make_optional<MovingStateGoal>(r);
            },
            [&](const UnitId& u) {
                return std::make_optional<MovingStateGoal>(u);
            });

        if (!resolvedGoal)
        {
            unitInfo.state->navigationState.state = NavigationStateIdle();
            return;
        }

        moveTo(unitInfo, *resolvedGoal);
    }

    bool followPath(UnitInfo unitInfo, UnitPhysicsInfoGround& physics, PathFollowingInfo& path)
    {
        const auto& destination = *path.currentWaypoint;
        SimVector xzPosition(unitInfo.state->position.x, 0_ss, unitInfo.state->position.z);
        SimVector xzDestination(destination.x, 0_ss, destination.z);
        auto distanceSquared = xzPosition.distanceSquared(xzDestination);

        auto isFinalDestination = path.currentWaypoint == (path.path.waypoints.end() - 1);

        if (isFinalDestination)
        {
            if (distanceSquared < (8_ss * 8_ss))
            {
                return true;
            }

            physics.steeringInfo = arrive(*unitInfo.state, *unitInfo.definition, physics, destination);
            return false;
        }

        if (distanceSquared < (16_ss * 16_ss))
        {
            ++path.currentWaypoint;
            return false;
        }

        physics.steeringInfo = seek(*unitInfo.state, *unitInfo.definition, destination);
        return false;
    }

    void UnitBehaviorService::updateWeapon(UnitId id, unsigned int weaponIndex)
    {
        auto& unit = sim->getUnitState(id);
        auto& weapon = unit.weapons[weaponIndex];
        if (!weapon)
        {
            return;
        }

        const auto& weaponDefinition = sim->weaponDefinitions.at(weapon->weaponType);

        if (std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinition.physicsType))
        {
            updateDroppedWeapon(id, weaponIndex);
            return;
        }

        if (weaponDefinition.paralyzer)
        {
            if (auto* attacking = std::get_if<UnitWeaponStateAttacking>(&weapon->state))
            {
                if (auto* targetId = std::get_if<UnitId>(&attacking->target))
                {
                    const auto target = sim->tryGetUnitState(*targetId);
                    bool explicitlyTargeted = false;
                    if (!unit.orders.empty())
                    {
                        if (auto* order = std::get_if<AttackOrder>(&unit.orders.front()))
                        {
                            explicitlyTargeted = std::get_if<UnitId>(&order->target)
                                && std::get<UnitId>(order->target) == *targetId;
                        }
                        if (auto* order = std::get_if<CommandFireOrder>(&unit.orders.front()))
                        {
                            explicitlyTargeted = std::get_if<UnitId>(&order->target)
                                && std::get<UnitId>(order->target) == *targetId;
                        }
                    }
                    if (target && target->get().isParalyzed() && !explicitlyTargeted)
                    {
                        unit.clearWeaponTarget(weaponIndex);
                        return;
                    }
                }
            }
        }

        if (auto idleState = std::get_if<UnitWeaponStateIdle>(&weapon->state); idleState != nullptr)
        {
            // attempt to acquire a target
            if (weaponDefinition.interceptor)
            {
                // retail interceptor weapons never scan units: they
                // engage live enemy `targetable` projectiles inside
                // the weapon's coverage box, with one defender per
                // missile (TotalA.exe 0x408b38 -> 0x49d120). With no
                // claimable missile the weapon stays idle.
                if (!weaponDefinition.commandFire && unit.fireOrders == UnitFireOrders::FireAtWill)
                {
                    if (auto target = sim->findInterceptorTarget(unit, weaponDefinition); target)
                    {
                        weapon->state = UnitWeaponStateAttacking(*target);
                    }
                }
            }
            else if (!weaponDefinition.commandFire && unit.fireOrders == UnitFireOrders::FireAtWill)
            {
                // the per-tick candidate list holds this owner's
                // eligible enemies in `units` iteration order, so the
                // first passing candidate is the same one a full
                // `units` scan would pick; every predicate below is
                // still re-evaluated per candidate
                for (const auto otherUnitId : sim->getWeaponTargetCandidates(unit.owner))
                {
                    const auto& otherUnit = sim->getUnitState(otherUnitId);

                    if (otherUnit.isDead() || (weaponDefinition.paralyzer && otherUnit.isParalyzed()))
                    {
                        continue;
                    }

                    if (otherUnit.carriedBy)
                    {
                        // cargo aboard a transport is not a valid target
                        continue;
                    }

                    if (otherUnit.missionState && otherUnit.missionState->immunity)
                    {
                        // retail OTA `Immunity=1`: skipped by the
                        // nearest-enemy acquisition scan (TotalA.exe
                        // 0x407297: unit flags & 0x8000)
                        continue;
                    }

                    if (otherUnit.isOwnedBy(unit.owner)
                        || sim->areAllied(otherUnit.owner, unit.owner))
                    {
                        // allied units are never auto-acquired
                        // (retail gates acquisition on the +0x108
                        // alliedWith table)
                        continue;
                    }

                    // cheap range reject before the detected-contact
                    // set lookup -- the conjunction is unchanged
                    if (unit.position.distanceSquared(otherUnit.position) > weaponDefinition.maxRange * weaponDefinition.maxRange)
                    {
                        continue;
                    }

                    // retail auto-acquire collects targets from the
                    // attacker's detected-contact set; an undetected
                    // enemy cannot be engaged (TotalA.exe 0x40ad80).
                    // Radar-only contacts count when the owner has an
                    // active targeting facility (istargetingupgrade).
                    if (!sim->isWeaponTargetableByPlayer(otherUnitId, unit.owner))
                    {
                        continue;
                    }

                    weapon->state = UnitWeaponStateAttacking(otherUnitId);
                    break;
                }
            }
            else if (!weaponDefinition.commandFire && unit.fireOrders == UnitFireOrders::ReturnFire && unit.lastDamagedBy)
            {
                // return fire only at the unit that most recently damaged us
                auto attackerRef = sim->tryGetUnitState(*unit.lastDamagedBy);
                if (attackerRef)
                {
                    const auto& attacker = attackerRef->get();
                    if (attacker.isAlive()
                        && !(weaponDefinition.paralyzer && attacker.isParalyzed())
                        && !(attacker.missionState && attacker.missionState->immunity)
                        && !attacker.isOwnedBy(unit.owner)
                        && !sim->areAllied(attacker.owner, unit.owner)
                        && sim->isWeaponTargetableByPlayer(*unit.lastDamagedBy, unit.owner)
                        && unit.position.distanceSquared(attacker.position) <= weaponDefinition.maxRange * weaponDefinition.maxRange)
                    {
                        weapon->state = UnitWeaponStateAttacking(*unit.lastDamagedBy);
                    }
                }
            }
        }
        else if (auto aimingState = std::get_if<UnitWeaponStateAttacking>(&weapon->state); aimingState != nullptr)
        {
            // interceptor weapons only engage projectile targets; a
            // unit or ground lock is never valid, however it arose
            if (weaponDefinition.interceptor && !std::holds_alternative<ProjectileId>(aimingState->target))
            {
                unit.clearWeaponTarget(weaponIndex);
                return;
            }

            if (std::holds_alternative<UnitWeaponStateAttacking::FireInfo>(aimingState->attackInfo))
            {
                tryFireWeapon(id, weaponIndex);
                return;
            }

            // If we are not fire-at-will, the target is a unit,
            // and we don't have an explicit order to attack that unit,
            // drop the target.
            // This can happen if we acquired the target ourselves while in fire-at-will,
            // but then the player switched us to another firing mode.
            if (unit.fireOrders != UnitFireOrders::FireAtWill)
            {
                if (auto targetUnit = std::get_if<UnitId>(&aimingState->target); targetUnit != nullptr)
                {
                    // a target acquired under return-fire stays valid
                    // while it remains our most recent attacker
                    bool isReturnFireTarget = unit.fireOrders == UnitFireOrders::ReturnFire
                        && unit.lastDamagedBy && *unit.lastDamagedBy == *targetUnit;

                    if (!isReturnFireTarget)
                    {
                        if (unit.orders.empty())
                        {
                            unit.clearWeaponTarget(weaponIndex);
                            return;
                        }
                        else if (auto attackOrder = std::get_if<AttackOrder>(&unit.orders.front()); attackOrder != nullptr)
                        {
                            if (auto attackTarget = std::get_if<UnitId>(&attackOrder->target); attackTarget == nullptr || *attackTarget != *targetUnit)
                            {
                                unit.clearWeaponTarget(weaponIndex);
                                return;
                            }
                        }
                        else if (auto commandFireOrder = std::get_if<CommandFireOrder>(&unit.orders.front()); commandFireOrder != nullptr)
                        {
                            if (auto commandFireTarget = std::get_if<UnitId>(&commandFireOrder->target); commandFireTarget == nullptr || *commandFireTarget != *targetUnit)
                            {
                                unit.clearWeaponTarget(weaponIndex);
                                return;
                            }
                        }
                    }
                }
            }

            // a unit target that has become undetectable through cloak
            // breaks the weapon lock -- retail's acquisition scans draw
            // only from the owner's detected-contact set (0x40729c),
            // and effective cloak removes the target from it. A
            // radar-visible cloaked unit stays locked while the owner
            // has an active targeting facility.
            if (auto targetUnit = std::get_if<UnitId>(&aimingState->target); targetUnit != nullptr)
            {
                if (auto target = sim->tryGetUnitState(*targetUnit);
                    target
                    && target->get().cloaked
                    && !target->get().isOwnedBy(unit.owner)
                    && !sim->isWeaponTargetableByPlayer(*targetUnit, unit.owner))
                {
                    unit.clearWeaponTarget(weaponIndex);
                    return;
                }
            }

            auto targetPosition = getTargetPosition(aimingState->target);
            const bool targetInRange = targetPosition
                && unit.position.distanceSquared(*targetPosition) <= weaponDefinition.maxRange * weaponDefinition.maxRange;
            if (targetPosition && unit.kills > 5 && !weaponDefinition.cruise && weaponDefinition.velocity != 0_ss)
            {
                if (auto targetId = std::get_if<UnitId>(&aimingState->target))
                {
                    if (auto target = sim->tryGetUnitState(*targetId);
                        target && sim->unitDefinitions.at(target->get().unitType).isMobile)
                    {
                        // Retail 0x48a324: 6 kills, 3D flight time, fixed 0xcccc
                        // lead factor. Guidance after release still pursues normally.
                        *targetPosition = veteranAimPoint(unit, target->get(), weaponDefinition, *targetPosition);
                    }
                }
            }

            if (!targetInRange)
            {
                unit.clearWeaponTarget(weaponIndex);
            }
            else if (std::holds_alternative<UnitWeaponStateAttacking::IdleInfo>(aimingState->attackInfo))
            {
                auto aimFromPosition = getAimingPoint(id, weaponIndex);

                auto headingAndPitch = computeHeadingAndPitch(unit.rotation, aimFromPosition, *targetPosition, weaponDefinition.velocity, (112_ss / (30_ss * 30_ss)), weapon->ballisticZOffset, weaponDefinition.physicsType);
                auto heading = headingAndPitch.first;
                auto pitch = headingAndPitch.second;

                auto threadId = unit.cobEnvironment->createThread(getAimScriptName(weaponIndex), {toCobAngle(heading).value, toCobAngle(pitch).value});

                if (threadId)
                {
                    aimingState->attackInfo = UnitWeaponStateAttacking::AimInfo{*threadId, heading, pitch};
                }
                else
                {
                    // We couldn't launch an aiming script (there isn't one),
                    // just go straight to firing.
                    if (sim->gameTime >= weapon->readyTime && (!weaponDefinition.stockpile || weapon->stockpileCount > 0))
                    {
                        aimingState->attackInfo = UnitWeaponStateAttacking::FireInfo{heading, pitch, *targetPosition, std::nullopt, 0, GameTime(0)};
                        tryFireWeapon(id, weaponIndex);
                    }
                }
            }
            else if (auto aimInfo = std::get_if<UnitWeaponStateAttacking::AimInfo>(&aimingState->attackInfo))
            {
                auto returnValue = unit.cobEnvironment->tryReapThread(aimInfo->thread);
                if (returnValue)
                {
                    const auto lastHeading = aimInfo->lastHeading;
                    const auto lastPitch = aimInfo->lastPitch;
                    // we successfully reaped, clear the thread.
                    aimingState->attackInfo = UnitWeaponStateAttacking::IdleInfo{};

                    if (*returnValue)
                    {
                        // aiming was successful, check the target again for drift
                        auto aimFromPosition = getAimingPoint(id, weaponIndex);

                        auto headingAndPitch = computeHeadingAndPitch(unit.rotation, aimFromPosition, *targetPosition, weaponDefinition.velocity, (112_ss / (30_ss * 30_ss)), weapon->ballisticZOffset, weaponDefinition.physicsType);
                        auto heading = headingAndPitch.first;
                        auto pitch = headingAndPitch.second;

                        // if the target is close enough, try to fire
                        if (angleBetweenIsLessOrEqual(heading, lastHeading, weaponDefinition.tolerance) && angleBetweenIsLessOrEqual(pitch, lastPitch, weaponDefinition.pitchTolerance))
                        {

                            if (sim->gameTime >= weapon->readyTime && (!weaponDefinition.stockpile || weapon->stockpileCount > 0))
                            {
                                aimingState->attackInfo = UnitWeaponStateAttacking::FireInfo{heading, pitch, *targetPosition, std::nullopt, 0, GameTime(0)};
                                tryFireWeapon(id, weaponIndex);
                            }
                        }
                    }
                }
            }
        }
    }

    SimVector UnitBehaviorService::changeDirectionByRandomAngle(const SimVector& direction, SimAngle maxAngle)
    {
        std::uniform_int_distribution dist(SimAngle(0).value, maxAngle.value);
        std::uniform_int_distribution dist2(0, 1);
        auto& rng = sim->rng;
        auto angle = SimAngle(dist(rng));
        if (dist2(rng))
        {
            angle = SimAngle(0) - angle;
        }

        return rotateDirectionXZ(direction, angle);
    }

    void UnitBehaviorService::tryFireWeapon(UnitId id, unsigned int weaponIndex)
    {
        auto& unit = sim->getUnitState(id);
        auto& weapon = unit.weapons[weaponIndex];

        if (!weapon)
        {
            return;
        }

        const auto& weaponDefinition = sim->weaponDefinitions.at(weapon->weaponType);

        auto attackInfo = std::get_if<UnitWeaponStateAttacking>(&weapon->state);
        if (!attackInfo)
        {
            return;
        }

        auto fireInfo = std::get_if<UnitWeaponStateAttacking::FireInfo>(&attackInfo->attackInfo);
        if (!fireInfo)
        {
            return;
        }

        // wait for burst reload
        auto gameTime = sim->gameTime;
        if (gameTime < fireInfo->readyTime)
        {
            return;
        }

        // retail gates fire eligibility on the weapon slot's ammo byte:
        // a stockpile weapon cannot launch without a built round
        if (weaponDefinition.stockpile && weapon->stockpileCount == 0)
        {
            attackInfo->attackInfo = UnitWeaponStateAttacking::IdleInfo{};
            return;
        }

        // spawn a projectile from the firing point
        if (!fireInfo->firingPiece)
        {
            auto scriptName = getQueryScriptName(weaponIndex);
            fireInfo->firingPiece = runCobQuery(id, scriptName).value_or(0);
        }

        auto firingPoint = unit.getTransform() * getPieceLocalPosition(id, *fireInfo->firingPiece);

        auto direction = match(
            weaponDefinition.physicsType,
            [&](const ProjectilePhysicsTypeLineOfSight&) {
                return (fireInfo->targetPosition - firingPoint).normalized();
            },
            [&](const ProjectilePhysicsTypeTracking&) {
                return (fireInfo->targetPosition - firingPoint).normalized();
            },
            [&](const ProjectilePhysicsTypeBallistic&) {
                return toDirection(fireInfo->heading + unit.rotation, -fireInfo->pitch);
            },
            [&](const ProjectilePhysicsTypeDropped&) { return SimVector(0_ss, 0_ss, 0_ss); });


        const bool dropped = std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinition.physicsType);
        if (!dropped && !weaponDefinition.vLaunch && weaponDefinition.turret)
        {
            auto bound = veteranSpread(weaponDefinition.accuracy, unit.hitPoints,
                sim->unitDefinitions.at(unit.unitType).maxHitPoints, unit.kills);
            if (bound != 0)
            {
                auto draw = [&]() { return bound < 2 ? 0u : static_cast<unsigned int>(sim->rng() % bound); };
                auto yaw = static_cast<int>(draw()) - (bound >> 1);
                auto pitch = static_cast<int>(draw()) - (bound >> 1);
                // Ordinary creators re-solve from the aim point; their spread
                // only reaches recoil. Only the ballistic creator copies the
                // scattered slot angles into authoritative launch velocity.
                if (std::holds_alternative<ProjectilePhysicsTypeBallistic>(weaponDefinition.physicsType))
                {
                    direction = toDirection(fireInfo->heading + unit.rotation + SimAngle(yaw), -fireInfo->pitch - SimAngle(pitch));
                }
            }
        }
        if (!dropped && weaponDefinition.sprayAngle != SimAngle(0))
        {
            direction = changeDirectionByRandomAngle(direction, weaponDefinition.sprayAngle);
        }

        auto targetUnit = std::get_if<UnitId>(&attackInfo->target);
        auto targetUnitOption = targetUnit == nullptr ? std::optional<UnitId>() : std::make_optional(*targetUnit);
        auto targetProjectile = std::get_if<ProjectileId>(&attackInfo->target);
        auto targetProjectileOption = targetProjectile == nullptr ? std::optional<ProjectileId>() : std::make_optional(*targetProjectile);
        // the computed aim point rides on the projectile so tracking
        // guidance can steer a ground-target shot onto it (vlaunch
        // missiles rise then guide down onto the clicked point); a
        // unit target still steers by its live position first
        sim->spawnProjectile(unit.owner, *weapon, firingPoint, direction, (fireInfo->targetPosition - firingPoint).length(), targetUnitOption, id, targetProjectileOption, fireInfo->targetPosition);

        // a launched stockpiled shot consumes one built round
        if (weaponDefinition.stockpile)
        {
            --weapon->stockpileCount;
        }
        else
        {
            // retail spends the weapon's per-shot energy and metal when
            // the projectile launches (all-or-nothing TrySpendBoth);
            // the spend result does not gate the launch, a shortfall
            // still fires the shot
            auto& player = sim->getPlayer(unit.owner);
            if (player.energy >= weaponDefinition.energyPerShot && player.metal >= weaponDefinition.metalPerShot)
            {
                sim->addResourceDelta(id, -weaponDefinition.energyPerShot, -weaponDefinition.metalPerShot);
            }
        }

        sim->events.push_back(FireWeaponEvent{weapon->weaponType, fireInfo->burstsFired, firingPoint});

        // If we just started the burst, set the reload timer
        if (fireInfo->burstsFired == 0)
        {
            if (!dropped) { unit.cobEnvironment->createThread(getFireScriptName(weaponIndex)); }
            auto reload = deltaSecondsToTicks(weaponDefinition.reloadTime);
            // Stockpile assembly and its existing launch cadence are independent
            // of the retail ordinary-shot veterancy reload multiplier.
            if (!weaponDefinition.stockpile) { reload = GameTime(veteranReloadTicks(reload.value, unit.kills)); }
            weapon->readyTime = gameTime + reload;
        }

        ++fireInfo->burstsFired;
        fireInfo->readyTime = gameTime + deltaSecondsToTicks(weaponDefinition.burstInterval);
        if (dropped || fireInfo->burstsFired >= weaponDefinition.burst)
        {
            // we finished our burst, we are reloading now
            attackInfo->attackInfo = UnitWeaponStateAttacking::IdleInfo{};
        }
    }

    void UnitBehaviorService::applyUnitSteering(UnitInfo unitInfo)
    {
        updateUnitRotation(unitInfo);
        updateUnitSpeed(unitInfo);
    }

    void UnitBehaviorService::updateUnitRotation(UnitInfo unitInfo)
    {
        auto turnRateThisFrame = SimAngle(unitInfo.definition->turnRate.value);
        unitInfo.state->previousRotation = unitInfo.state->rotation;
        if (unitInfo.state->aircraftService
            && unitInfo.state->aircraftService->phase == AircraftServiceState::Phase::Descend)
        {
            auto base = sim->tryGetUnitState(unitInfo.state->aircraftService->facility);
            if (base) { unitInfo.state->rotation = turnTowards(unitInfo.state->rotation, base->get().rotation, turnRateThisFrame); }
            return;
        }

        match(
            unitInfo.state->physics,
            [&](const UnitPhysicsInfoGround& p) {
                unitInfo.state->rotation = turnTowards(unitInfo.state->rotation, p.steeringInfo.targetAngle, turnRateThisFrame);
            },
            [&](const UnitPhysicsInfoAir& p) {
                match(
                    p.movementState,
                    [&](const AirMovementStateTakingOff&) {
                        // do nothing
                    },
                    [&](const AirMovementStateLanding&) {
                        // do nothing
                    },
                    [&](const AirMovementStateFlying& m) {
                        if (!m.targetPosition)
                        {
                            // keep rotation as-is if not trying to go anywhere
                            return;
                        }
                        auto direction = *m.targetPosition - unitInfo.state->position;
                        auto targetAngle = UnitState::toRotation(direction);
                        unitInfo.state->rotation = turnTowards(unitInfo.state->rotation, targetAngle, turnRateThisFrame);
                    });
            });
    }

    void UnitBehaviorService::updateUnitSpeed(UnitInfo unitInfo)
    {
        match(
            unitInfo.state->physics,
            [&](UnitPhysicsInfoGround& p) {
                p.currentSpeed = computeNewGroundUnitSpeed(sim->terrain, *unitInfo.state, *unitInfo.definition, p);
            },
            [&](UnitPhysicsInfoAir& p) {
                match(
                    p.movementState,
                    [&](AirMovementStateFlying& m) {
                        m.currentVelocity = computeNewAirUnitVelocity(*unitInfo.state, *unitInfo.definition, m);
                    },
                    [&](const AirMovementStateTakingOff&) {
                        // do nothing
                    },
                    [&](const AirMovementStateLanding&) {
                        // do nothing
                    });
            });
    }

    void UnitBehaviorService::updateGroundUnitPosition(UnitInfo unitInfo, const UnitPhysicsInfoGround& physics)
    {
        auto direction = UnitState::toDirection(unitInfo.state->rotation);

        if (physics.currentSpeed > 0_ss)
        {
            auto newPosition = unitInfo.state->position + (direction * physics.currentSpeed);
            newPosition.y = sim->terrain.getHeightAt(newPosition.x, newPosition.z);
            if (unitInfo.definition->floater || unitInfo.definition->canHover)
            {
                newPosition.y = rweMax(newPosition.y, sim->terrain.getSeaLevel());
            }

            if (!tryApplyMovementToPosition(unitInfo, newPosition))
            {
                unitInfo.state->inCollision = true;

                // if we failed to move, try in each axis separately
                // to see if we can complete a "partial" movement
                const SimVector maskX(0_ss, 1_ss, 1_ss);
                const SimVector maskZ(1_ss, 1_ss, 0_ss);

                SimVector newPos1;
                SimVector newPos2;
                if (direction.x > direction.z)
                {
                    newPos1 = unitInfo.state->position + (direction * maskZ * physics.currentSpeed);
                    newPos2 = unitInfo.state->position + (direction * maskX * physics.currentSpeed);
                }
                else
                {
                    newPos1 = unitInfo.state->position + (direction * maskX * physics.currentSpeed);
                    newPos2 = unitInfo.state->position + (direction * maskZ * physics.currentSpeed);
                }
                newPos1.y = sim->terrain.getHeightAt(newPos1.x, newPos1.z);
                newPos2.y = sim->terrain.getHeightAt(newPos2.x, newPos2.z);

                if (unitInfo.definition->floater || unitInfo.definition->canHover)
                {
                    newPos1.y = rweMax(newPos1.y, sim->terrain.getSeaLevel());
                    newPos2.y = rweMax(newPos2.y, sim->terrain.getSeaLevel());
                }

                if (!tryApplyMovementToPosition(unitInfo, newPos1))
                {
                    tryApplyMovementToPosition(unitInfo, newPos2);
                }
            }
        }
    }

    void UnitBehaviorService::updateUnitPosition(UnitInfo unitInfo)
    {
        unitInfo.state->previousPosition = unitInfo.state->position;
        unitInfo.state->inCollision = false;

        match(
            unitInfo.state->physics,
            [&](const UnitPhysicsInfoGround& p) {
                updateGroundUnitPosition(unitInfo, p);
            },
            [&](const UnitPhysicsInfoAir& p) {
                match(
                    p.movementState,
                    [&](const AirMovementStateFlying& m) {
                        auto newPosition = unitInfo.state->position + m.currentVelocity;
                        tryApplyMovementToPosition(unitInfo, newPosition);
                    },
                    [&](const AirMovementStateTakingOff&) {
                        climbToCruiseAltitude(unitInfo);
                    },
                    [&](const AirMovementStateLanding&) {
                        descendToGroundLevel(unitInfo);
                    });
            });
    }

    bool UnitBehaviorService::tryApplyMovementToPosition(UnitInfo unitInfo, const SimVector& newPosition)
    {
        // No collision for flying units.
        if (isFlying(unitInfo.state->physics))
        {
            unitInfo.state->position = newPosition;
            return true;
        }

        // check for collision at the new position
        auto newFootprintRegion = sim->computeFootprintRegion(newPosition, unitInfo.definition->movementCollisionInfo);

        if (sim->isCollisionAt(newFootprintRegion, unitInfo.id))
        {
            return false;
        }

        // Unlike for pathfinding, TA doesn't care about the unit's actual movement class for collision checks,
        // it only cares about the attributes defined directly on the unitInfo.state->
        // Jam these into an ad-hoc movement class to pass into our walkability check.
        if (!isGridPointWalkable(sim->terrain, sim->getAdHocMovementClass(unitInfo.definition->movementCollisionInfo), newFootprintRegion.x, newFootprintRegion.y))
        {
            return false;
        }

        // we passed all collision checks, update accordingly
        auto footprintRegion = sim->computeFootprintRegion(unitInfo.state->position, unitInfo.definition->movementCollisionInfo);
        sim->moveUnitOccupiedArea(footprintRegion, newFootprintRegion, unitInfo.id);

        auto seaLevel = sim->terrain.getSeaLevel();
        auto oldTerrainHeight = sim->terrain.getHeightAt(unitInfo.state->position.x, unitInfo.state->position.z);
        auto oldPosBelowSea = oldTerrainHeight < seaLevel;

        unitInfo.state->position = newPosition;

        auto newTerrainHeight = sim->terrain.getHeightAt(unitInfo.state->position.x, unitInfo.state->position.z);
        auto newPosBelowSea = newTerrainHeight < seaLevel;

        if (oldPosBelowSea && !newPosBelowSea)
        {
            unitInfo.state->cobEnvironment->createThread("setSFXoccupy", std::vector<int>{4});
        }
        else if (!oldPosBelowSea && newPosBelowSea)
        {
            unitInfo.state->cobEnvironment->createThread("setSFXoccupy", std::vector<int>{2});
        }

        return true;
    }

    std::optional<int> UnitBehaviorService::runCobQuery(UnitId id, const std::string& name)
    {
        auto& unit = sim->getUnitState(id);
        auto thread = unit.cobEnvironment->createNonScheduledThread(name, {0});
        if (!thread)
        {
            return std::nullopt;
        }
        CobExecutionContext context(unit.cobEnvironment.get(), &*thread);
        auto status = context.execute();
        if (std::get_if<CobEnvironment::FinishedStatus>(&status) == nullptr)
        {
            throw std::runtime_error("Synchronous cob query thread blocked before completion");
        }

        auto result = thread->returnLocals[0];
        return result;
    }

    SimVector UnitBehaviorService::getAimingPoint(UnitId id, unsigned int weaponIndex)
    {
        const auto& unit = sim->getUnitState(id);
        return unit.getTransform() * getLocalAimingPoint(id, weaponIndex);
    }

    SimVector UnitBehaviorService::getLocalAimingPoint(UnitId id, unsigned int weaponIndex)
    {
        auto scriptName = getAimFromScriptName(weaponIndex);
        auto pieceId = runCobQuery(id, scriptName);
        if (!pieceId)
        {
            return getLocalFiringPoint(id, weaponIndex);
        }

        return getPieceLocalPosition(id, *pieceId);
    }

    SimVector UnitBehaviorService::getLocalFiringPoint(UnitId id, unsigned int weaponIndex)
    {

        auto scriptName = getQueryScriptName(weaponIndex);
        auto pieceId = runCobQuery(id, scriptName);
        if (!pieceId)
        {
            return SimVector(0_ss, 0_ss, 0_ss);
        }

        return getPieceLocalPosition(id, *pieceId);
    }

    SimVector UnitBehaviorService::getSweetSpot(UnitId id)
    {
        auto pieceId = runCobQuery(id, "SweetSpot");
        if (!pieceId)
        {
            return sim->getUnitState(id).position;
        }

        return getPiecePosition(id, *pieceId);
    }

    std::optional<SimVector> UnitBehaviorService::tryGetSweetSpot(UnitId id)
    {
        if (!sim->unitExists(id))
        {
            return std::nullopt;
        }

        return getSweetSpot(id);
    }

    bool UnitBehaviorService::handleOrder(UnitInfo unitInfo, UnitOrder& order)
    {
        return match(
            order,
            [&](LandOrder& o) {
                // Completion means the landing request has been admitted; the
                // patient-side service state blocks subsequent orders until ready.
                startAircraftService(unitInfo, o.target, false);
                return true;
            },
            [&](MoveOrder& o) {
                return handleMoveOrder(unitInfo, o);
            },
            [&](AttackOrder& o) {
                return handleAttackOrder(unitInfo, o);
            },
            [&](BuildOrder& o) {
                return handleBuildOrder(unitInfo, o);
            },
            [&](BuggerOffOrder& o) {
                return handleBuggerOffOrder(unitInfo, o);
            },
            [&](CompleteBuildOrder& o) {
                return handleCompleteBuildOrder(unitInfo, o);
            },
            [&](RepairOrder& o) {
                return handleRepairOrder(unitInfo, o);
            },
            [&](ReclaimOrder& o) {
                return handleReclaimOrder(unitInfo, o);
            },
            [&](CaptureOrder& o) {
                return handleCaptureOrder(unitInfo, o);
            },
            [&](GuardOrder& o) {
                return handleGuardOrder(unitInfo, o);
            },
            [&](PatrolOrder& o) {
                return handlePatrolOrder(unitInfo, o);
            },
            [&](WaitOrder& o) {
                return handleWaitOrder(unitInfo, o);
            },
            [&](ParalyzeOrder& o) {
                return handleParalyzeOrder(unitInfo, o);
            },
            [&](WaitForAttackOrder& o) {
                return handleWaitForAttackOrder(unitInfo, o);
            },
            [&](MakeSelectableOrder& o) {
                return handleMakeSelectableOrder(unitInfo, o);
            },
            [&](AttackUnitTypeOrder& o) {
                return handleAttackUnitTypeOrder(unitInfo, o);
            },
            [&](MissionBuildOrder& o) {
                return handleMissionBuildOrder(unitInfo, o);
            },
            [&](MissionFactoryBuildOrder& o) {
                return handleMissionFactoryBuildOrder(unitInfo, o);
            },
            [&](SelfDestructOrder& o) {
                return handleSelfDestructOrder(unitInfo, o);
            },
            [&](UnloadOrder& o) {
                return handleUnloadOrder(unitInfo, o);
            },
            [&](LoadOrder& o) {
                return handleLoadOrder(unitInfo, o);
            },
            [&](BuildWeaponOrder& o) {
                return handleBuildWeaponOrder(unitInfo, o);
            },
            [&](CommandFireOrder& o) {
                return handleCommandFireOrder(unitInfo, o);
            });
    }

    bool UnitBehaviorService::handleMoveOrder(UnitInfo unitInfo, MoveOrder& moveOrder)
    {
        if (!unitInfo.definition->isMobile)
        {
            return false;
        }

        // retail ends the move job when the mover reaches the goal or
        // its best-effort (partial) path is exhausted at the closest
        // reachable point; navigateTo reports both. Temporary
        // congestion keeps the job -- and this order -- alive.
        if (navigateTo(unitInfo, moveOrder.destination) != NavigationResult::InProgress)
        {
            sim->events.push_back(UnitArrivedEvent{unitInfo.id});
            return true;
        }

        return false;
    }

    bool UnitBehaviorService::handlePatrolOrder(UnitInfo unitInfo, PatrolOrder& patrolOrder)
    {
        if (!unitInfo.definition->isMobile || patrolOrder.waypoints.empty())
        {
            // a unit that can't move can never patrol; drop the order
            return true;
        }

        // retail maps a repair-capable builder's Patrol to the
        // RepairPatrol state (VTOL_REPAIRPATROL for air builders):
        // the builder diverts to autonomous allied work found by the
        // 60-tick HelpBuild scan and resumes this route afterward
        // (TotalA.exe 0x405980)
        if (unitInfo.definition->builder)
        {
            if (patrolOrder.divertTarget)
            {
                bool done = match(
                    *patrolOrder.divertTarget,
                    [&](const PatrolAssistTarget& t) { return buildExistingUnit(unitInfo, t.target); },
                    [&](const PatrolRepairTarget& t) { return repairExistingUnit(unitInfo, t.target); },
                    [&](const ReclaimTarget& t) {
                        return match(
                            t,
                            [&](const FeatureId& featureId) { return reclaimFeature(unitInfo, featureId); },
                            [&](const UnitId& unitId) { return reclaimUnit(unitInfo, unitId); });
                    });

                // the diversion is bounded by the patrol area: a
                // dragged-away target does not pull the builder
                // further than one buildDistance off the route
                if (!done)
                {
                    auto bound = unitInfo.definition->buildDistance;
                    auto boundSquared = bound * bound;
                    const auto& position = unitInfo.state->position;
                    auto segmentDistanceSquared = [&](const SimVector& a, const SimVector& b) {
                        auto abx = b.x - a.x;
                        auto abz = b.z - a.z;
                        auto lengthSquared = abx * abx + abz * abz;
                        SimScalar t{0};
                        if (lengthSquared > SimScalar(0))
                        {
                            t = ((position.x - a.x) * abx + (position.z - a.z) * abz) / lengthSquared;
                            if (t < SimScalar(0))
                            {
                                t = SimScalar(0);
                            }
                            else if (t > SimScalar(1))
                            {
                                t = SimScalar(1);
                            }
                        }
                        auto cx = a.x + abx * t - position.x;
                        auto cz = a.z + abz * t - position.z;
                        return cx * cx + cz * cz;
                    };
                    SimScalar best{0};
                    bool haveBest = false;
                    const auto waypointCount = patrolOrder.waypoints.size();
                    for (std::size_t i = 0; i < waypointCount; ++i)
                    {
                        auto d = segmentDistanceSquared(patrolOrder.waypoints[i], patrolOrder.waypoints[(i + 1) % waypointCount]);
                        if (!haveBest || d < best)
                        {
                            best = d;
                            haveBest = true;
                        }
                    }
                    done = haveBest && best > boundSquared;
                }

                if (done)
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    patrolOrder.divertTarget = std::nullopt;
                }
                else
                {
                    return false;
                }
            }
            else
            {
                if (patrolOrder.scanTicks > 0)
                {
                    --patrolOrder.scanTicks;
                }
                else
                {
                    patrolOrder.scanTicks = 60;
                    patrolOrder.divertTarget = scanPatrolWork(unitInfo);
                    if (patrolOrder.divertTarget)
                    {
                        return false;
                    }
                }
            }
        }

        if (patrolOrder.nextWaypointIndex >= patrolOrder.waypoints.size())
        {
            patrolOrder.nextWaypointIndex = 0;
        }

        if (navigateTo(unitInfo, patrolOrder.waypoints[patrolOrder.nextWaypointIndex]) != NavigationResult::InProgress)
        {
            patrolOrder.nextWaypointIndex = (patrolOrder.nextWaypointIndex + 1) % patrolOrder.waypoints.size();
        }

        // patrol is a standing order; it only ends when replaced or stopped
        return false;
    }

    std::optional<PatrolDivertTarget> UnitBehaviorService::scanPatrolWork(UnitInfo unitInfo)
    {
        const auto& player = sim->getPlayer(unitInfo.state->owner);

        // retail resource preconditions: reclaim work is scanned only
        // while a resource class sits below 20% of capacity, and
        // repair/assist only while the player holds at least 20% of
        // their metal capacity
        auto metalLow = player.metal.value < player.maxMetal.value * 0.2f;
        auto energyLow = player.energy.value < player.maxEnergy.value * 0.2f;

        std::vector<PatrolDivertTarget> candidates;

        if (!metalLow)
        {
            // assist unfinished allied construction; repair damaged
            // allied units; the scan radius is the builder's working
            // reach measured to the candidate's footprint, the same
            // distance the nano check uses -- center-to-center distance
            // misses genuinely adjacent targets on real footprints
            for (const auto& [otherId, other] : sim->units)
            {
                if (otherId == unitInfo.id || other.isDead() || !sim->areAllied(other.owner, unitInfo.state->owner))
                {
                    continue;
                }
                const auto& otherDefinition = sim->unitDefinitions.at(other.unitType);
                if (!inWorkingRange(unitInfo, other, otherDefinition))
                {
                    continue;
                }
                if (other.isBeingBuilt(otherDefinition))
                {
                    candidates.push_back(PatrolAssistTarget{otherId});
                }
                else if (other.hitPoints < otherDefinition.maxHitPoints)
                {
                    candidates.push_back(PatrolRepairTarget{otherId});
                }
            }
        }

        if (metalLow || energyLow)
        {
            // reclaim the resource class the player is low on;
            // wrecks and map features both live in the feature set
            for (const auto& [featureId, feature] : sim->features)
            {
                const auto& featureDefinition = sim->getFeatureDefinition(feature.featureName);
                if (!featureDefinition.reclaimable || feature.event != FeatureEvent::None || sim->reclaimComplete(feature))
                {
                    continue;
                }
                if (!inWorkingRange(unitInfo, feature, featureDefinition))
                {
                    continue;
                }
                auto remainingMetal = featureDefinition.metal - std::min(featureDefinition.metal, feature.reclaimMetalGiven);
                auto remainingEnergy = featureDefinition.energy - std::min(featureDefinition.energy, feature.reclaimEnergyGiven);
                if (metalLow && remainingMetal == 0)
                {
                    continue;
                }
                if (!metalLow && energyLow && remainingEnergy == 0)
                {
                    continue;
                }
                candidates.push_back(ReclaimTarget(featureId));
            }
        }

        if (candidates.empty())
        {
            return std::nullopt;
        }

        // retail picks a random eligible candidate, not the nearest
        std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
        return candidates[dist(sim->rng)];
    }

    bool UnitBehaviorService::handleWaitOrder(UnitInfo unitInfo, WaitOrder& waitOrder)
    {
        if (waitOrder.ticksRemaining == 0)
        {
            return true;
        }

        --waitOrder.ticksRemaining;
        return waitOrder.ticksRemaining == 0;
    }

    bool UnitBehaviorService::handleWaitForAttackOrder(UnitInfo unitInfo, WaitForAttackOrder& waitOrder)
    {
        auto targetId = waitOrder.target.value_or(unitInfo.id);
        auto targetRef = sim->tryGetUnitState(targetId);
        if (!targetRef)
        {
            // the watched unit is gone entirely; release the wait
            return true;
        }

        const auto& target = targetRef->get();
        if (target.isDead())
        {
            // destruction counts as having been attacked
            return true;
        }

        // the wait begins when the order becomes current: remember the
        // target's damage timestamp and release once it advances
        if (!waitOrder.armed)
        {
            waitOrder.armed = true;
            waitOrder.observedDamagedAt = target.lastDamagedAt;
        }

        return target.lastDamagedAt > waitOrder.observedDamagedAt;
    }

    bool UnitBehaviorService::handleMakeSelectableOrder(UnitInfo unitInfo, const MakeSelectableOrder&)
    {
        if (unitInfo.state->missionState)
        {
            unitInfo.state->missionState->selectable = true;
        }
        return true;
    }

    bool UnitBehaviorService::handleAttackUnitTypeOrder(UnitInfo unitInfo, AttackUnitTypeOrder& order)
    {
        if (!unitInfo.definition->canAttack)
        {
            return true;
        }

        // retail schedules the first scan at a random 1-90 tick delay
        if (order.nextScanTime == GameTime(0))
        {
            order.nextScanTime = sim->gameTime + GameTime(1) + GameTime(static_cast<unsigned int>(sim->rng() % 90));
            return false;
        }

        // keep attacking the current target while it lives; retail
        // commits each scan's choice to its secondary action queue
        if (order.currentTarget)
        {
            auto target = sim->tryGetUnitState(*order.currentTarget);
            if (target && !target->get().isDead())
            {
                return attackTarget(unitInfo, *order.currentTarget);
            }
            order.currentTarget = std::nullopt;
        }

        if (sim->gameTime < order.nextScanTime)
        {
            return false;
        }

        auto best = findBestUnitTypeTarget(unitInfo, order.unitType);
        if (!best)
        {
            // no living enemy of this type anywhere: the order is done
            return true;
        }

        order.currentTarget = *best;
        return attackTarget(unitInfo, *best);
    }

    std::optional<UnitId> UnitBehaviorService::findBestUnitTypeTarget(const UnitInfo& unitInfo, const std::string& unitType)
    {
        std::optional<UnitId> best;
        long long bestScore = 0;
        for (const auto& [candidateId, candidate] : sim->units)
        {
            if (candidate.unitType != unitType
                || candidate.isOwnedBy(unitInfo.state->owner)
                || sim->areAllied(candidate.owner, unitInfo.state->owner)
                || candidate.isDead()
                || candidate.carriedBy
                || (candidate.missionState && candidate.missionState->immunity))
            {
                continue;
            }

            // retail scores distance squared in whole world units,
            // minus a sim-RNG jitter of up to half that score
            auto distanceSquared = static_cast<long long>(simScalarToFloat(unitInfo.state->position.distanceSquared(candidate.position)));
            long long score = distanceSquared;
            auto halfDistance = distanceSquared / 2;
            if (halfDistance > 0)
            {
                score -= static_cast<long long>(sim->rng() % static_cast<unsigned int>(halfDistance));
            }

            // later candidates win ties, matching retail's scan
            if (!best || score <= bestScore)
            {
                best = candidateId;
                bestScore = score;
            }
        }
        return best;
    }

    bool UnitBehaviorService::handleMissionBuildOrder(UnitInfo unitInfo, MissionBuildOrder& order)
    {
        if (!unitInfo.definition->builder)
        {
            // not a builder: the scripted build cannot run
            return true;
        }

        if (sim->gameTime < order.retryAt)
        {
            return false;
        }

        if (!unitInfo.state->buildOrderUnitId)
        {
            auto result = createNewUnit(unitInfo, order.unitType, order.position);
            return match(
                result,
                [&](const UnitCreationStatusPending&) { return false; },
                [&](const UnitCreationStatusFailed&) {
                    // retail retries blocked placement a bounded number
                    // of times before abandoning the order
                    if (--order.attemptsRemaining == 0)
                    {
                        return true;
                    }
                    order.retryAt = sim->gameTime + GameTime(30);
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return false;
                },
                [&](const UnitCreationStatusDone& d) {
                    unitInfo.state->buildOrderUnitId = d.unitId;
                    return deployBuildArm(unitInfo, d.unitId);
                });
        }

        return deployBuildArm(unitInfo, *unitInfo.state->buildOrderUnitId);
    }

    bool UnitBehaviorService::handleMissionFactoryBuildOrder(UnitInfo unitInfo, MissionFactoryBuildOrder& order)
    {
        if (unitInfo.definition->isMobile || !unitInfo.definition->builder)
        {
            // not a factory: the scripted build cannot run
            return true;
        }

        if (order.count == 0)
        {
            return true;
        }

        if (!order.enqueued)
        {
            unitInfo.state->buildQueue.emplace_back(order.unitType, static_cast<int>(order.count));
            order.enqueued = true;
        }

        // the order completes once the factory's queue has fully drained
        return unitInfo.state->buildQueue.empty();
    }

    bool UnitBehaviorService::handleSelfDestructOrder(UnitInfo unitInfo, SelfDestructOrder& order)
    {
        return tickSelfDestruct(unitInfo, order);
    }

    void UnitBehaviorService::updateSelfDestruct(UnitId unitId)
    {
        auto unitInfo = sim->getUnitInfo(unitId);
        if (!unitInfo.state->isDead() && unitInfo.state->selfDestruct
            && tickSelfDestruct(unitInfo, *unitInfo.state->selfDestruct))
        {
            unitInfo.state->selfDestruct = std::nullopt;
        }
    }

    bool UnitBehaviorService::tickSelfDestruct(UnitInfo unitInfo, SelfDestructOrder& selfDestruct)
    {
        if (!selfDestruct.fuseTicks)
        {
            if (selfDestruct.countdownTicks > 0)
            {
                --selfDestruct.countdownTicks;

                // retail announces the remaining countdown in one-second
                // steps (count5..count0 unit sounds)
                if (selfDestruct.countdownTicks % SimTicksPerSecond == 0)
                {
                    sim->events.push_back(UnitSelfDestructCountdownEvent{
                        unitInfo.id,
                        static_cast<int>(selfDestruct.countdownTicks / SimTicksPerSecond)});
                }
                return false;
            }

            // countdown expired: retail lights a fuse of RNG(15) ticks
            selfDestruct.fuseTicks = static_cast<unsigned int>(sim->rng() % 15);
        }

        if (*selfDestruct.fuseTicks > 0)
        {
            --(*selfDestruct.fuseTicks);
            return false;
        }

        // detonate through the normal damage/explosion path,
        // matching retail's 30000-point self-damage. Retail applies it
        // as a direct damage call, which the immunity bit cannot reach
        // (immunity is a targeting/sweep property), so mission units
        // with OTA Immunity=1 (e.g. CC01's CORCOM) still detonate.
        sim->selfDestructUnit(unitInfo.id);
        return true;
    }

    std::optional<SimVector> UnitBehaviorService::findUnloadPosition(UnitInfo unitInfo, const UnitDefinition& passengerDefinition, UnloadOrder& order)
    {
        auto movementClass = sim->getAdHocMovementClass(passengerDefinition.movementCollisionInfo);
        const auto& transportPosition = unitInfo.state->position;

        // retail's drop picker (0x40bfe0): the search radius carried
        // in transport+0x109 grows by 160 world units per call while a
        // drop is pending, clamps to the map extents, and is cleared
        // back to zero on a successful drop
        order.dropRadius += SimScalar(160);
        SimScalar maxRadius = SimScalar(std::max(
            sim->occupiedGrid.getWidth() * MapTerrain::HeightTileWidthInWorldUnits.value,
            sim->occupiedGrid.getHeight() * MapTerrain::HeightTileHeightInWorldUnits.value));
        if (order.dropRadius > maxRadius)
        {
            order.dropRadius = maxRadius;
        }

        // candidates jitter around the scripted destination clamped
        // to within the current search radius of the transport
        SimVector base = order.destination;
        SimVector offset(order.destination.x - transportPosition.x, 0_ss, order.destination.z - transportPosition.z);
        auto distance = offset.length();
        if (distance > order.dropRadius)
        {
            base = transportPosition + offset * (order.dropRadius / distance);
        }

        // up to 30 candidates per call, uniform polar jitter:
        // amplitude ~ rand(radius), angle ~ rand(65536) drawn from
        // the simulation RNG (retail 0x4b6c30) through the turn-table
        // trig helpers 0x4b70ef/0x4b7123
        auto maxAmplitude = simScalarToUInt(order.dropRadius);
        std::uniform_int_distribution<unsigned int> amplitudeDist(0, maxAmplitude);
        std::uniform_int_distribution<unsigned int> angleDist(0, 65535);
        for (unsigned int i = 0; i < 30; ++i)
        {
            auto amplitude = SimScalar(amplitudeDist(sim->rng));
            SimAngle angle(angleDist(sim->rng));
            SimVector candidate(base.x + amplitude * cos(angle), 0_ss, base.z + amplitude * sin(angle));

            auto rect = sim->computeFootprintRegion(candidate, passengerDefinition.movementCollisionInfo);
            if (rect.x < 0 || rect.y < 0
                || rect.x + rect.width > static_cast<int>(sim->occupiedGrid.getWidth())
                || rect.y + rect.height > static_cast<int>(sim->occupiedGrid.getHeight()))
            {
                continue;
            }

            // retail validates each candidate for both passability
            // (0x47db70) and occupancy (0x47c770)
            if (!sim->isCollisionAt(rect)
                && isGridPointWalkable(sim->terrain, movementClass, rect.x, rect.y))
            {
                // retail clears the widening radius on a successful drop
                order.dropRadius = SimScalar(0);
                // the drop point is the footprint rect's own world
                // center so the disembark placement reproduces the
                // validated rect exactly
                auto corner = sim->terrain.heightmapIndexToWorldCorner(rect.x, rect.y);
                SimVector world(
                    corner.x + (SimScalar(rect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss,
                    0_ss,
                    corner.z + (SimScalar(rect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss);
                world.y = sim->terrain.getHeightAt(world.x, world.z);
                return world;
            }
        }
        return std::nullopt;
    }

    bool UnitBehaviorService::handleUnloadOrder(UnitInfo unitInfo, UnloadOrder& order)
    {
        auto& unit = *unitInfo.state;

        // discard stale chain entries: dead or detached passengers
        while (!unit.passengers.empty())
        {
            auto passengerId = unit.passengers.front();
            auto passenger = sim->tryGetUnitState(passengerId);
            if (passenger
                && !passenger->get().isDead()
                && passenger->get().carriedBy == std::optional<UnitId>(unitInfo.id))
            {
                break;
            }
            unit.passengers.pop_front();
        }
        if (unit.passengers.empty())
        {
            // cargo chain drained: the order completes (retail return 5)
            return true;
        }

        // VTOL_Unload @0x411560 is a separate order handler: it flies a
        // drop job to the scripted destination, then detaches the head
        // member at its carried position (scripted `i` attaches with
        // slot -1, so the member rides at the carrier's position).
        // Exactly one drop per `u`, no jittered search and no claim
        // cap. The move job ends only on arrival or path exhaustion --
        // retail movement jobs have no timeout.
        if (unitInfo.definition->canFly)
        {
            if (navigateTo(unitInfo, order.destination) == NavigationResult::InProgress)
            {
                return false;
            }

            // the member rides at the carrier's position; retail
            // releases it there, letting the normal placement check
            // decide whether the cell is clear
            auto passengerId = unit.passengers.front();
            if (sim->tryDisembarkUnit(passengerId, sim->getUnitState(passengerId).position)
                && unit.cobEnvironment)
            {
                // raise the link arm / shut the transport flames
                unit.cobEnvironment->createThread("EndTransport");
            }
            return true;
        }

        // Ground_Unload @0x406900 state machine: claim the head
        // member (order+0x36++), run a move job to the scripted
        // destination, then run the jittered drop picker once when the
        // move job completes. The counter is checked on each new
        // claim -- the third claim aborts the order -- so a ground `u`
        // releases at most two passengers; the rest stay aboard for
        // the next `u`. Retail sleeps 15 ticks between claims and has
        // no movement-job timeout: the move job ends only on arrival
        // or path exhaustion.
        if (order.cycleTicks > 0)
        {
            --order.cycleTicks;
            return false;
        }

        if (!order.claimActive)
        {
            ++order.pops;
            if (order.pops >= 3)
            {
                return true;
            }
            order.claimActive = true;
        }

        // e.g. a sea transport ordered to a beach point stops at the
        // shoreline on path exhaustion; the drop search then runs
        // around the scripted destination regardless of where the
        // transport stopped
        if (unitInfo.definition->isMobile && navigateTo(unitInfo, order.destination) == NavigationResult::InProgress)
        {
            return false;
        }

        // move job done: the drop picker runs each tick for the
        // claimed member with a radius that widens toward the map
        // extents, matching retail's per-tick drop job
        auto passengerId = unit.passengers.front();
        const auto& passengerDefinition = sim->unitDefinitions.at(sim->getUnitState(passengerId).unitType);
        if (auto position = findUnloadPosition(unitInfo, passengerDefinition, order))
        {
            // retail fires TransportDrop on the carrier script when
            // the drop is committed (the script's own drop-unit then
            // finds the member already released and no-ops)
            if (unit.cobEnvironment)
            {
                unit.cobEnvironment->createThread("TransportDrop", {static_cast<int>(passengerId.value)});
            }
            sim->tryDisembarkUnit(passengerId, *position);
            order.claimActive = false;
            order.cycleTicks = 15;
            return false;
        }

        // once the search radius has grown to cover the whole map and
        // a sweep still found no cell the member cannot be placed, so
        // the claim is released and the cycle moves to the next member
        SimScalar maxRadius = SimScalar(std::max(
            sim->occupiedGrid.getWidth() * MapTerrain::HeightTileWidthInWorldUnits.value,
            sim->occupiedGrid.getHeight() * MapTerrain::HeightTileHeightInWorldUnits.value));
        if (order.dropRadius >= maxRadius)
        {
            order.claimActive = false;
            order.dropRadius = SimScalar(0);
            order.cycleTicks = 15;
        }
        return false;
    }

    /**
     * Re-validates the interactive Load target each tick the way
     * retail's Pickup handlers do at state 0 (0x406780/0x4111b0):
     * the passenger must be a live, own-side, mobile, transportable
     * unit that is not already carried, not carrying units itself,
     * and small enough for this transport.
     */
    bool UnitBehaviorService::isLoadTargetValid(UnitInfo unitInfo, const UnitState& target) const
    {
        if (target.isDead() || !target.isOwnedBy(unitInfo.state->owner))
        {
            return false;
        }

        const auto& targetDefinition = sim->unitDefinitions.at(target.unitType);
        if (!targetDefinition.isMobile
            || targetDefinition.cantBeTransported
            || target.carriedBy
            || !target.passengers.empty()
            || target.isBeingBuilt(targetDefinition))
        {
            return false;
        }

        // retail compares the member's footprint X against the
        // carrier's TransportSize (unitdef byte +0x22a)
        auto footprintX = sim->getFootprintXZ(targetDefinition.movementCollisionInfo).first;
        if (footprintX > unitInfo.definition->transportSize)
        {
            return false;
        }

        // VTOL_Pickup rejects a fully submerged member
        // (0x411230-0x411278): bottom + height below sea level
        if (unitInfo.definition->canFly)
        {
            const auto& targetModel = sim->unitModelDefinitions.at(targetDefinition.objectName);
            if (target.position.y + targetModel.height < sim->terrain.getSeaLevel())
            {
                return false;
            }
        }

        return true;
    }

    bool UnitBehaviorService::handleLoadOrder(UnitInfo unitInfo, LoadOrder& order)
    {
        auto& unit = *unitInfo.state;

        // retail re-validates carrier and member at state 0
        if (!unitInfo.definition->canLoad
            || unit.carriedBy
            || unit.passengers.size() >= unitInfo.definition->transportCapacity
            || order.target == unitInfo.id)
        {
            return true;
        }

        auto target = sim->tryGetUnitState(order.target);
        if (!target || !isLoadTargetValid(unitInfo, target->get()))
        {
            // abort: a VTOL pickup already in flight raises the arm
            // and climbs back to cruise altitude (retail fires
            // EndTransport and flies off)
            if (unitInfo.definition->canFly && order.phase >= 1 && order.phase < 4)
            {
                if (unit.cobEnvironment)
                {
                    unit.cobEnvironment->createThread("EndTransport");
                }
                order.phase = 4;
                return false;
            }
            return true;
        }

        if (unitInfo.definition->canFly)
        {
            return handleVtolLoadOrder(unitInfo, order, target->get());
        }
        return handleGroundLoadOrder(unitInfo, order, target->get());
    }

    /**
     * Ground_Pickup (retail 0x406780): drive to the member, fire
     * `TransportPickup(member)` and wait for the script's
     * attach-unit to board it. The approach + wait is retried up
     * to three times (retail order+0x36) before the order abandons.
     * Transports without a TransportPickup entry point board the
     * member directly at slot -1.
     */
    bool UnitBehaviorService::handleGroundLoadOrder(UnitInfo unitInfo, LoadOrder& order, const UnitState& target)
    {
        auto& unit = *unitInfo.state;

        if (order.phase == 0)
        {
            if (!unitInfo.definition->isMobile)
            {
                return true;
            }
            // the retail move job steers to the member's footprint and
            // completes inside reach, or on best-effort path exhaustion
            const auto& targetDefinition = sim->unitDefinitions.at(target.unitType);
            auto targetRect = sim->computeFootprintRegion(target.position, targetDefinition.movementCollisionInfo);
            if (navigateTo(unitInfo, targetRect) == NavigationResult::InProgress)
            {
                return false;
            }

            // arrived: fire the pickup script once per attempt
            order.attempts += 1;
            order.phase = 1;
            order.waitTicks = 0;
            order.pickupThreadStarted = false;
            return false;
        }

        // phase 1: wait for the TransportPickup script to attach
        if (target.carriedBy == std::optional<UnitId>(unitInfo.id))
        {
            return true;
        }

        if (!order.pickupThreadStarted)
        {
            order.pickupThreadStarted = true;
            auto thread = unit.cobEnvironment
                ? unit.cobEnvironment->createThread("TransportPickup", {static_cast<int>(order.target.value)})
                : std::optional<const CobThread*>(std::nullopt);
            if (!thread)
            {
                // no pickup script: board directly at the invalid
                // slot so the member rides at the carrier origin
                sim->tryBoardTransport(order.target, unitInfo.id, -1);
                return true;
            }
        }

        ++order.waitTicks;
        if (order.waitTicks > LoadOrder::maxAttachWaitTicks)
        {
            // member never attached: re-approach like retail, or
            // abandon after the third attempt (order+0x36 >= 3)
            if (order.attempts >= LoadOrder::maxAttempts)
            {
                return true;
            }
            order.phase = 0;
            order.waitTicks = 0;
        }
        return false;
    }

    /**
     * VTOL_Pickup (retail 0x4111b0): fly to the member, descend to
     * ground level, ask the script for a slot via QueryTransport,
     * fire BeginTransport(height) and attach the member at that
     * slot (retail 0x48aac0), then climb back to cruise altitude.
     * A script that offers no slot aborts the order (retail's
     * "Transport mission failed" path), as does an invalid member.
     */
    bool UnitBehaviorService::handleVtolLoadOrder(UnitInfo unitInfo, LoadOrder& order, const UnitState& target)
    {
        auto& unit = *unitInfo.state;

        switch (order.phase)
        {
            case 0:
            {
                const auto& targetDefinition = sim->unitDefinitions.at(target.unitType);
                auto targetRect = sim->computeFootprintRegion(target.position, targetDefinition.movementCollisionInfo);
                if (navigateTo(unitInfo, targetRect) == NavigationResult::InProgress)
                {
                    return false;
                }
                order.phase = 1;
                return false;
            }

            case 1:
                if (!descendToGroundLevel(unitInfo))
                {
                    return false;
                }
                order.phase = 2;
                return false;

            case 2:
            {
                auto slot = runCobQuery(unitInfo.id, "QueryTransport").value_or(-1);
                if (slot < 0)
                {
                    // no free slot: retail aborts ("Transport
                    // mission failed"), raises the arm and climbs
                    if (unit.cobEnvironment)
                    {
                        unit.cobEnvironment->createThread("EndTransport");
                    }
                    order.phase = 4;
                    return false;
                }
                order.slot = slot;

                const auto& targetDefinition = sim->unitDefinitions.at(target.unitType);
                const auto& targetModel = sim->unitModelDefinitions.at(targetDefinition.objectName);
                if (unit.cobEnvironment)
                {
                    unit.cobEnvironment->createThread("BeginTransport", {simScalarToCobPosition(targetModel.height).value});
                }

                if (!sim->tryBoardTransport(order.target, unitInfo.id, slot))
                {
                    if (unit.cobEnvironment)
                    {
                        unit.cobEnvironment->createThread("EndTransport");
                    }
                    order.phase = 4;
                    return false;
                }
                order.phase = 3;
                return false;
            }

            default:
                // phases 3 (attached) and 4 (aborted) both climb to
                // cruise altitude before the order pops
                return climbToCruiseAltitude(unitInfo);
        }
    }

    bool UnitBehaviorService::handleBuildWeaponOrder(UnitInfo unitInfo, BuildWeaponOrder& order)
    {
        auto& unit = *unitInfo.state;

        // find the unit's stockpile weapon slot
        std::optional<Index> weaponIndex;
        for (Index i = 0; i < getSize(unit.weapons); ++i)
        {
            if (!unit.weapons[i])
            {
                continue;
            }
            const auto& weaponDefinition = sim->weaponDefinitions.at(unit.weapons[i]->weaponType);
            if (weaponDefinition.stockpile)
            {
                weaponIndex = i;
                break;
            }
        }
        if (!weaponIndex)
        {
            // no stockpile weapon: the command completes immediately
            return true;
        }

        auto& weapon = *unit.weapons[*weaponIndex];
        const auto& weaponDefinition = sim->weaponDefinitions.at(weapon.weaponType);

        // the ammo field is a byte in retail; when full, BuildWeapon
        // sleeps 300 ticks instead of producing
        if (weapon.stockpileCount >= 255)
        {
            return false;
        }

        // each round takes one reload period to assemble
        auto buildTicks = deltaSecondsToTicks(weaponDefinition.reloadTime).value;
        if (buildTicks == 0)
        {
            buildTicks = 1;
        }

        // drain the per-shot resource cost across the build period;
        // progress stalls while resources are unavailable
        auto energyPerTick = weaponDefinition.energyPerShot.value / static_cast<float>(buildTicks);
        auto metalPerTick = weaponDefinition.metalPerShot.value / static_cast<float>(buildTicks);
        if (!sim->addResourceDelta(unitInfo.id, -Energy(energyPerTick), -Metal(metalPerTick)))
        {
            return false;
        }

        ++order.progressTicks;
        if (order.progressTicks >= buildTicks)
        {
            ++weapon.stockpileCount;
            --order.remaining;
            order.progressTicks = 0;
        }

        return order.remaining == 0;
    }

    bool UnitBehaviorService::handleAttackOrder(UnitInfo unitInfo, const AttackOrder& attackOrder)
    {
        return attackTarget(unitInfo, attackOrder.target);
    }

    NavigationGoal attackTargetToNavigationGoal(const AttackTarget& target)
    {
        return match(
            target,
            [&](const UnitId& t) -> NavigationGoal {
                return t;
            },
            [&](const SimVector& t) -> NavigationGoal {
                return t;
            });
    }

    void UnitBehaviorService::updateKamikazeStandby(UnitInfo unitInfo)
    {
        auto& unit = *unitInfo.state;
        const auto& def = *unitInfo.definition;
        if (!def.kamikaze || std::any_of(unit.weapons.begin(), unit.weapons.end(), [](const auto& w) { return w.has_value(); }))
        {
            return;
        }
        if (!unit.orders.empty() || !unit.buildQueue.empty()
            || (def.standbyMine ? def.isMobile : !def.isMobile))
        {
            unit.nextKamikazeScan = std::nullopt;
            return;
        }
        if (!unit.nextKamikazeScan)
        {
            unit.nextKamikazeScan = sim->gameTime + GameTime(1);
            return;
        }
        if (sim->gameTime < *unit.nextKamikazeScan) { return; }

        auto eligible = [&](UnitId id, bool opportunity) {
            auto candidate = sim->tryGetUnitState(id);
            if (!candidate) { return false; }
            const auto& other = candidate->get();
            if (!other.isAlive() || other.carriedBy || sim->areAllied(unit.owner, other.owner)
                || (other.missionState && other.missionState->immunity)
                || !sim->isWeaponTargetableByPlayer(id, unit.owner)) { return false; }
            if (def.standbyMine && (!sim->unitDefinitions.at(other.unitType).isMobile || isFlying(other.physics))) { return false; }
            if (opportunity)
            {
                const auto& otherDef = sim->unitDefinitions.at(other.unitType);
                if (!otherDef.shootMe && sim->players.at(unit.owner.value).type != GamePlayerType::Computer) { return false; }
                for (const auto& category : def.noChaseCategories)
                {
                    if (otherDef.categories.count(category)) { return false; }
                }
                const auto dx = other.position.x - unit.position.x;
                const auto dz = other.position.z - unit.position.z;
                if (dx * dx + dz * dz > def.sightDistance * def.sightDistance) { return false; }
            }
            return true;
        };

        std::optional<UnitId> target;
        if (unit.fireOrders == UnitFireOrders::FireAtWill)
        {
            // Shared per-owner candidate cache, only on a standby wake (30-59 ticks).
            // Keep the engine's stable candidate order; retail random sampling is
            // a separate targeting-parity boundary, as for ordinary weapons.
            for (auto id : sim->getWeaponTargetCandidates(unit.owner))
            {
                if (eligible(id, true)) { target = id; break; }
            }
        }
        else if (!def.standbyMine && unit.fireOrders == UnitFireOrders::ReturnFire
            && unit.lastDamagedBy && eligible(*unit.lastDamagedBy, false))
        {
            target = unit.lastDamagedBy;
        }
        if (target)
        {
            unit.nextKamikazeScan = std::nullopt;
            if (def.standbyMine) { sim->selfDestructUnit(unitInfo.id); }
            else if (def.canAttack) { unit.addOrder(AttackOrder(*target)); }
        }
        else
        {
            unit.nextKamikazeScan = sim->gameTime + GameTime(30 + sim->rng() % 30);
        }
    }

    bool UnitBehaviorService::attackKamikazeTarget(UnitInfo unitInfo, const AttackTarget& target)
    {
        if (!unitInfo.definition->canAttack || !unitInfo.definition->isMobile) { return true; }
        auto position = match(target,
            [](const SimVector& p) { return std::make_optional(p); },
            [&](UnitId id) -> std::optional<SimVector> {
                auto other = sim->tryGetUnitState(id);
                if (!other || !other->get().isAlive() || other->get().carriedBy) { return std::nullopt; }
                return other->get().position;
            });
        if (!position) { return true; }
        // Same point-goal arrival predicate as movement, with the authored radius.
        // PathExhausted is deliberately NOT arrival and never causes a blast.
        auto result = navigateTo(unitInfo, attackTargetToNavigationGoal(target),
            rweMax(16_ss, unitInfo.definition->kamikazeDistance), position);
        if (result == NavigationResult::Arrived)
        {
            sim->selfDestructUnit(unitInfo.id);
            return true;
        }
        return result == NavigationResult::PathExhausted;
    }

    bool UnitBehaviorService::isBomber(UnitInfo unitInfo) const
    {
        return unitInfo.definition->canFly && unitInfo.state->weapons[0]
            && std::holds_alternative<ProjectilePhysicsTypeDropped>(sim->weaponDefinitions.at(unitInfo.state->weapons[0]->weaponType).physicsType);
    }

    void UnitBehaviorService::updateBomberStandby(UnitInfo unitInfo)
    {
        auto& unit = *unitInfo.state;
        if (!unit.orders.empty() || !unit.buildQueue.empty()) { return; }
        if (!unit.bomberState) { unit.bomberState = std::make_unique<BomberState>(); }
        auto& state = *unit.bomberState;
        state.target.reset();
        unit.clearWeaponTarget(0);
        if (!state.nextScan)
        {
            state.nextScan = sim->gameTime + GameTime(1);
            return;
        }
        if (sim->gameTime < *state.nextScan) { return; }
        auto eligible = [&](UnitId id) {
            auto other = sim->tryGetUnitState(id);
            if (!other || !other->get().isAlive() || other->get().carriedBy
                || sim->areAllied(unit.owner, other->get().owner)
                || (other->get().missionState && other->get().missionState->immunity)
                || !sim->isWeaponTargetableByPlayer(id, unit.owner)) { return false; }
            const auto& def = sim->unitDefinitions.at(other->get().unitType);
            if (def.canFly || (!def.shootMe && sim->players.at(unit.owner.value).type != GamePlayerType::Computer)) { return false; }
            for (const auto& category : unitInfo.definition->noChaseCategories)
            {
                if (def.categories.count(category)) { return false; }
            }
            auto delta = other->get().position - unit.position;
            delta.y = 0_ss;
            return delta.lengthSquared() <= unitInfo.definition->sightDistance * unitInfo.definition->sightDistance;
        };
        std::optional<UnitId> target;
        if (unit.fireOrders == UnitFireOrders::FireAtWill)
        {
            // Reuse the per-owner cache only at standby wakeups, not every tick.
            for (auto id : sim->getWeaponTargetCandidates(unit.owner))
            {
                if (eligible(id)) { target = id; break; }
            }
        }
        else if (unit.fireOrders == UnitFireOrders::ReturnFire && unit.lastDamagedBy && eligible(*unit.lastDamagedBy))
        {
            target = unit.lastDamagedBy;
        }
        if (target && unitInfo.definition->canAttack)
        {
            state.nextScan.reset();
            unit.addOrder(AttackOrder(*target));
        }
        else { state.nextScan = sim->gameTime + GameTime(30 + sim->rng() % 30); }
    }

    bool UnitBehaviorService::attackBomberTarget(UnitInfo unitInfo, const AttackTarget& target)
    {
        auto& unit = *unitInfo.state;
        // Background mission orders also reach this handler while the
        // foreground Paralyze interrupt owns movement/combat progression.
        if (unit.isParalyzed()) { return false; }
        auto position = getTargetPosition(target);
        if (const auto* id = std::get_if<UnitId>(&target))
        {
            auto other = sim->tryGetUnitState(*id);
            if (!other || !other->get().isAlive() || other->get().carriedBy
                || sim->unitDefinitions.at(other->get().unitType).canFly
                || (other->get().cloaked && !sim->isWeaponTargetableByPlayer(*id, unit.owner)))
            {
                position.reset();
            }
            else { position = other->get().position; }
        }
        if (!position)
        {
            unit.bomberState.reset();
            unit.clearWeaponTargets();
            return true;
        }
        if (!unit.bomberState) { unit.bomberState = std::make_unique<BomberState>(); }
        auto& state = *unit.bomberState;
        if (!state.target || *state.target != target)
        {
            state = BomberState{};
            state.target = target;
        }
        using Phase = BomberState::Phase;
        if (state.phase != Phase::Overfly) { unit.clearWeaponTargets(); }
        auto delta = *position - unit.position;
        delta.y = 0_ss;
        auto distance = delta.length();
        auto direction = distance > 0_ss ? delta / distance : UnitState::toDirection(unit.rotation);
        auto startSwing = [&]() {
            // Retail's half-distance setup leg includes +/-45 degree jitter.
            auto angle = SimAngle(static_cast<int>(sim->rng() % 0x4000) - 0x2000);
            state.goal = unit.position + rotateDirectionXZ(direction, angle) * (distance / 2_ss);
            state.phase = Phase::Swing;
        };
        switch (state.phase)
        {
            case Phase::Takeoff:
            {
                navigateTo(unitInfo, *position);
                auto* air = std::get_if<UnitPhysicsInfoAir>(&unit.physics);
                if (!air || !std::holds_alternative<AirMovementStateFlying>(air->movementState)) { break; }
                if (distance < 480_ss)
                {
                    state.goal = unit.position + direction * 2240_ss;
                    state.phase = Phase::Reposition;
                }
                else { startSwing(); }
                break;
            }
            case Phase::Reposition:
                if (navigateTo(unitInfo, state.goal, 960_ss) == NavigationResult::Arrived) { startSwing(); }
                break;
            case Phase::Swing:
                if (navigateTo(unitInfo, state.goal, 480_ss) == NavigationResult::Arrived) { state.phase = Phase::Approach; }
                break;
            case Phase::Approach:
            {
                if (sim->mapGravity == 0)
                {
                    // Retail tests raw OTA gravity on the release-point leg.
                    unit.clearOrders();
                    return false;
                }
                SimScalar speed{0};
                if (auto* air = std::get_if<UnitPhysicsInfoAir>(&unit.physics))
                {
                    if (auto* flying = std::get_if<AirMovementStateFlying>(&air->movementState))
                    {
                        auto velocity = flying->currentVelocity;
                        velocity.y = 0_ss;
                        speed = SimScalar(std::floor(velocity.length().value));
                    }
                }
                const auto lead = sim->mapGravity > 0
                    ? SimScalar(std::trunc(std::sqrt(2.0 * unitInfo.definition->cruiseAltitude.value / sim->mapGravity) * 30.0 * speed.value)) : 0_ss;
                if (navigateTo(unitInfo, attackTargetToNavigationGoal(target), lead + 1_ss + unitInfo.definition->attackRunLength, *position) == NavigationResult::Arrived)
                {
                    state.releaseTarget = *position;
                    state.goal = unit.position + direction * (unitInfo.definition->attackRunLength + 960_ss);
                    state.phase = Phase::Overfly;
                    unit.setWeaponTarget(0, state.releaseTarget);
                }
                break;
            }
            case Phase::Overfly:
                unit.setWeaponTarget(0, state.releaseTarget);
                if (navigateTo(unitInfo, state.goal, 960_ss) == NavigationResult::Arrived)
                {
                    unit.clearWeaponTargets();
                    state.goal = unit.position + UnitState::toDirection(unit.rotation) * 1440_ss;
                    state.phase = Phase::BreakAway;
                    if (seekAircraftService(unitInfo)) { state.phase = Phase::Takeoff; }
                }
                break;
            case Phase::BreakAway:
                if (navigateTo(unitInfo, state.goal, 128_ss) == NavigationResult::Arrived) { state.phase = Phase::Approach; }
                break;
        }
        return false;
    }

    void UnitBehaviorService::updateDroppedWeapon(UnitId id, unsigned int weaponIndex)
    {
        auto& unit = sim->getUnitState(id);
        if (weaponIndex != 0 || !unit.bomberState || !unit.bomberState->target
            || unit.bomberState->phase != BomberState::Phase::Overfly) { return; }
        auto& weapon = *unit.weapons[weaponIndex];
        const auto& def = sim->weaponDefinitions.at(weapon.weaponType);
        if (sim->gameTime < weapon.readyTime) { return; }
        auto delta = unit.bomberState->releaseTarget - unit.position;
        delta.y = 0_ss;
        const auto& unitDef = sim->unitDefinitions.at(unit.unitType);
        if (delta.lengthSquared() > def.maxRange * def.maxRange
            || (!def.waterWeapon && unit.position.y + sim->unitModelDefinitions.at(unitDef.objectName).height <= sim->terrain.getSeaLevel())) { return; }
        weapon.state = UnitWeaponStateAttacking(unit.bomberState->releaseTarget);
        auto& attack = std::get<UnitWeaponStateAttacking>(weapon.state);
        attack.attackInfo = UnitWeaponStateAttacking::FireInfo{SimAngle(0), SimAngle(0), unit.bomberState->releaseTarget, std::nullopt, 0, sim->gameTime};
        // Dropped roots query the emitter but never run Aim/Fire callbacks.
        tryFireWeapon(id, weaponIndex);
    }

    bool UnitBehaviorService::attackTarget(UnitInfo unitInfo, const AttackTarget& target)
    {
        if (isBomber(unitInfo)) { return attackBomberTarget(unitInfo, target); }
        if (unitInfo.definition->kamikaze
            && std::none_of(unitInfo.state->weapons.begin(), unitInfo.state->weapons.end(), [](const auto& w) { return w.has_value(); }))
        {
            return attackKamikazeTarget(unitInfo, target);
        }
        if (!unitInfo.state->weapons[0])
        {
            return true;
        }

        const auto& weaponDefinition = sim->weaponDefinitions.at(unitInfo.state->weapons[0]->weaponType);

        auto targetPosition = getTargetPosition(target);
        if (!targetPosition)
        {
            // target has gone away, throw away this order
            return true;
        }

        auto maxRangeSquared = weaponDefinition.maxRange * weaponDefinition.maxRange;
        if (unitInfo.state->position.distanceSquared(*targetPosition) > maxRangeSquared)
        {
            navigateTo(unitInfo, attackTargetToNavigationGoal(target));
        }
        else
        {
            // we're in range, aim weapons -- a unit target that is
            // currently undetectable (cloaked, with no targeting
            // facility radar fallback) cannot be locked; the order
            // stays and re-arms if the target decloaks
            for (unsigned int i = 0; i < 2; ++i)
            {
                match(
                    target,
                    [&](const UnitId& u) {
                        auto targetUnit = sim->tryGetUnitState(u);
                        if (targetUnit
                            && targetUnit->get().cloaked
                            && !targetUnit->get().isOwnedBy(unitInfo.state->owner)
                            && !sim->isWeaponTargetableByPlayer(u, unitInfo.state->owner))
                        {
                            return;
                        }
                        unitInfo.state->setWeaponTarget(i, u);
                    },
                    [&](const SimVector& v) { unitInfo.state->setWeaponTarget(i, v); });
            }
        }

        return false;
    }

    std::optional<unsigned int> UnitBehaviorService::findCommandFireWeaponIndex(const UnitState& unit) const
    {
        for (Index i = 0; i < getSize(unit.weapons); ++i)
        {
            if (unit.weapons[i] && sim->weaponDefinitions.at(unit.weapons[i]->weaponType).commandFire)
            {
                return i;
            }
        }

        return std::nullopt;
    }

    bool UnitBehaviorService::handleCommandFireOrder(UnitInfo unitInfo, CommandFireOrder& order)
    {
        if (isBomber(unitInfo)) { return attackBomberTarget(unitInfo, order.target); }
        // retail ATTACKSPECIAL puts the unit into its "Activate" state,
        // during which the target scanner admits command-fire weapons;
        // the order ends after the single shot launches
        auto weaponIndex = findCommandFireWeaponIndex(*unitInfo.state);
        if (!weaponIndex)
        {
            // no command-fire weapon, the order can never be satisfied
            return true;
        }

        auto& weapon = *unitInfo.state->weapons[*weaponIndex];
        const auto& weaponDefinition = sim->weaponDefinitions.at(weapon.weaponType);

        auto targetPosition = getTargetPosition(order.target);
        if (!targetPosition)
        {
            // target has gone away, throw away this order
            return true;
        }

        auto maxRangeSquared = weaponDefinition.maxRange * weaponDefinition.maxRange;
        if (unitInfo.state->position.distanceSquared(*targetPosition) > maxRangeSquared)
        {
            navigateTo(unitInfo, attackTargetToNavigationGoal(order.target));
            return false;
        }

        // In range: snapshot the reload state so the launched shot
        // (which pushes readyTime forward) can be detected.
        if (!order.observedReadyTime)
        {
            order.observedReadyTime = weapon.readyTime;
        }

        match(
            order.target,
            [&](const UnitId& u) { unitInfo.state->setWeaponTarget(*weaponIndex, u); },
            [&](const SimVector& v) { unitInfo.state->setWeaponTarget(*weaponIndex, v); });

        // Command fire is a one-shot order for both position and unit
        // targets: once the projectile launches, release the weapon and
        // complete.
        if (weapon.readyTime > *order.observedReadyTime)
        {
            unitInfo.state->clearWeaponTarget(*weaponIndex);
            return true;
        }

        return false;
    }

    bool UnitBehaviorService::handleBuildOrder(UnitInfo unitInfo, const BuildOrder& buildOrder)
    {
        return buildUnit(unitInfo, buildOrder.unitType, buildOrder.position);
    }

    bool UnitBehaviorService::handleBuggerOffOrder(UnitInfo unitInfo, const BuggerOffOrder& buggerOffOrder)
    {
        auto [footprintX, footprintZ] = sim->getFootprintXZ(unitInfo.definition->movementCollisionInfo);
        return navigateTo(unitInfo, buggerOffOrder.rect.expand((footprintX * 3) - 4, (footprintZ * 3) - 4)) != NavigationResult::InProgress;
    }

    bool UnitBehaviorService::handleCompleteBuildOrder(UnitInfo unitInfo, const rwe::CompleteBuildOrder& buildOrder)
    {
        return buildExistingUnit(unitInfo, buildOrder.target);
    }

    bool UnitBehaviorService::handleRepairOrder(UnitInfo unitInfo, const RepairOrder& repairOrder)
    {
        return repairExistingUnit(unitInfo, repairOrder.target);
    }

    bool UnitBehaviorService::handleReclaimOrder(UnitInfo unitInfo, const ReclaimOrder& reclaimOrder)
    {
        return match(
            reclaimOrder.target,
            [&](const FeatureId& featureId) { return reclaimFeature(unitInfo, featureId); },
            [&](const UnitId& unitId) { return reclaimUnit(unitInfo, unitId); });
    }

    bool UnitBehaviorService::reclaimUnit(UnitInfo unitInfo, UnitId targetUnitId)
    {
        // only builders can reclaim; a unit cannot reclaim itself
        if (!unitInfo.definition->builder || targetUnitId == unitInfo.id)
        {
            return true;
        }

        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);
        if (!targetUnitRef || targetUnitRef->get().isDead())
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        // commanders cannot be reclaimed; unfinished units can be
        // reclaimed but yield no resources
        if (targetUnitDefinition.commander)
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }

        if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
        {
            // the nanolathe is only active while actually in range
            if (auto rs = std::get_if<UnitBehaviorStateReclaiming>(&unitInfo.state->behaviourState))
            {
                rs->nanoParticleOrigin = std::nullopt;
            }
            navigateTo(unitInfo, targetUnitId);
            return false;
        }

        return deployReclaimArm(unitInfo, targetUnitId);
    }

    bool UnitBehaviorService::reclaimFeature(UnitInfo unitInfo, FeatureId targetFeatureId)
    {
        // only builders can reclaim
        if (!unitInfo.definition->builder)
        {
            return true;
        }

        auto featureRef = sim->tryGetFeature(targetFeatureId);

        // target must exist and still be reclaimable
        if (!featureRef || featureRef->get().event != FeatureEvent::None || !sim->getFeatureDefinition(featureRef->get().featureName).reclaimable)
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& feature = featureRef->get();
        const auto& featureDefinition = sim->getFeatureDefinition(feature.featureName);

        // nothing left to take
        if (sim->reclaimComplete(feature))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }

        if (!inWorkingRange(unitInfo, feature, featureDefinition))
        {
            if (auto rs = std::get_if<UnitBehaviorStateReclaiming>(&unitInfo.state->behaviourState))
            {
                rs->nanoParticleOrigin = std::nullopt;
            }
            navigateTo(unitInfo, sim->computeFootprintRegion(feature.position, featureDefinition.footprintX, featureDefinition.footprintZ));
            return false;
        }

        return deployReclaimArm(unitInfo, targetFeatureId);
    }

    bool UnitBehaviorService::inWorkingRange(UnitInfo unitInfo, const MapFeature& targetFeature, const FeatureDefinition& targetFeatureDefinition)
    {
        auto footprintRect = sim->computeFootprintRegion(targetFeature.position, targetFeatureDefinition.footprintX, targetFeatureDefinition.footprintZ);
        auto nearestPoint = findClosestPointToFootprintXZ(sim->terrain, footprintRect, unitInfo.state->position);
        auto distanceSquared = unitInfo.state->position.distanceSquared(nearestPoint);
        auto buildDistance = unitInfo.definition->buildDistance;
        return distanceSquared <= buildDistance * buildDistance;
    }

    bool UnitBehaviorService::deployReclaimArm(UnitInfo unitInfo, const ReclaimTarget& target)
    {
        // resolve the target's current position, validating that the
        // target is still present and reclaimable
        auto targetPosition = match(
            target,
            [&](const FeatureId& featureId) -> std::optional<SimVector> {
                auto featureRef = sim->tryGetFeature(featureId);
                if (!featureRef || featureRef->get().event != FeatureEvent::None || !sim->getFeatureDefinition(featureRef->get().featureName).reclaimable)
                {
                    return std::nullopt;
                }
                return featureRef->get().position;
            },
            [&](const UnitId& unitId) -> std::optional<SimVector> {
                auto targetUnitRef = sim->tryGetUnitState(unitId);
                if (!targetUnitRef || targetUnitRef->get().isDead())
                {
                    return std::nullopt;
                }
                const auto& targetUnit = targetUnitRef->get();
                const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);
                if (targetUnitDefinition.commander)
                {
                    return std::nullopt;
                }
                return targetUnit.position;
            });

        if (!targetPosition)
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }

        return match(
            unitInfo.state->behaviourState,
            [&](UnitBehaviorStateReclaiming& reclaimingState) {
                if (target != reclaimingState.target)
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return match(
                        target,
                        [&](const FeatureId& featureId) { return reclaimFeature(unitInfo, featureId); },
                        [&](const UnitId& unitId) { return reclaimUnit(unitInfo, unitId); });
                }

                if (!unitInfo.state->inBuildStance)
                {
                    // We are not in the correct stance to reclaim the target yet, wait.
                    reclaimingState.nanoParticleOrigin = std::nullopt;
                    return false;
                }

                return match(
                    target,
                    [&](const FeatureId& featureId) {
                        if (!reclaimingState.nanoParticleOrigin)
                        {
                            sim->events.push_back(UnitStartedReclaimingEvent{unitInfo.id});
                        }
                        reclaimingState.nanoParticleOrigin = getNanoPoint(unitInfo.id);
                        unitInfo.state->activeNanolatheTarget = std::make_pair(
                            UnitState::NanolatheTargetRef(featureId), *reclaimingState.nanoParticleOrigin);

                        auto credits = sim->applyReclaimProgress(featureId, unitInfo.definition->workerTimePerTick);
                        if (!credits)
                        {
                            // target disappeared mid-tick
                            changeState(*unitInfo.state, UnitBehaviorStateIdle());
                            return true;
                        }
                        sim->addScaledResourceDelta(unitInfo.id, credits->first, credits->second);

                        if (sim->reclaimComplete(sim->getFeature(featureId)))
                        {
                            finishReclaim(featureId);
                            changeState(*unitInfo.state, UnitBehaviorStateIdle());
                            return true;
                        }
                        return false;
                    },
                    [&](const UnitId& unitId) {
                        auto targetUnitRef = sim->tryGetUnitState(unitId);
                        if (!targetUnitRef || targetUnitRef->get().isDead())
                        {
                            changeState(*unitInfo.state, UnitBehaviorStateIdle());
                            return true;
                        }
                        auto& targetUnit = targetUnitRef->get();
                        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

                        // a moving target can leave working range; chase it
                        if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
                        {
                            reclaimingState.nanoParticleOrigin = std::nullopt;
                            navigateTo(unitInfo, unitId);
                            return false;
                        }
                        reclaimingState.targetPosition = targetUnit.position;

                        if (!reclaimingState.nanoParticleOrigin)
                        {
                            sim->events.push_back(UnitStartedReclaimingEvent{unitInfo.id});
                        }
                        reclaimingState.nanoParticleOrigin = getNanoPoint(unitInfo.id);
                        unitInfo.state->activeNanolatheTarget = std::make_pair(
                            UnitState::NanolatheTargetRef(unitId), *reclaimingState.nanoParticleOrigin);

                        // Live-unit reclaim drains hit points at the builder's
                        // work rate. Reclaim duration therefore scales with the
                        // target's current health, matching documented TA
                        // behavior where damaging a target shortens reclaim.
                        auto work = unitInfo.definition->workerTimePerTick * ((static_cast<unsigned int>(unitInfo.state->kills) + 5) / 5);
                        auto drained = std::min<unsigned int>(work, targetUnit.hitPoints);
                        targetUnit.hitPoints -= drained;

                        if (targetUnit.hitPoints == 0)
                        {
                            // A completed live unit refunds its full metal
                            // cost as a lump sum at completion: the amount is
                            // independent of the unit's remaining health
                            // (SDA KB: "reclaiming a unit at any health does
                            // not change the amount of metal received").
                            // Energy is never refunded and units still under
                            // construction yield nothing.
                            if (!targetUnit.isBeingBuilt(targetUnitDefinition))
                            {
                                sim->addScaledResourceDelta(unitInfo.id, Energy(0), targetUnitDefinition.buildCostMetal);
                            }
                            // Cause-5 enemy reclaim earns one kill. Preserve the
                            // existing native payout/cadence; do not credit features.
                            targetUnit.lastDamagedBy = unitInfo.id;
                            targetUnit.lastDamagedByPlayer = unitInfo.state->owner;
                            finishReclaim(unitId);
                            changeState(*unitInfo.state, UnitBehaviorStateIdle());
                            return true;
                        }
                        return false;
                    });
            },
            [&](const auto&) {
                auto nanoFromPosition = getNanoPoint(unitInfo.id);
                auto headingAndPitch = computeLineOfSightHeadingAndPitch(unitInfo.state->rotation, nanoFromPosition, *targetPosition);
                auto heading = headingAndPitch.first;
                auto pitch = headingAndPitch.second;

                changeState(*unitInfo.state, UnitBehaviorStateReclaiming{target, *targetPosition, std::nullopt});
                unitInfo.state->cobEnvironment->createThread("StartBuilding", {toCobAngle(heading).value, toCobAngle(pitch).value});
                return false;
            });
    }

    bool UnitBehaviorService::handleCaptureOrder(UnitInfo unitInfo, CaptureOrder& order)
    {
        return captureUnit(unitInfo, order);
    }

    bool UnitBehaviorService::captureUnit(UnitInfo unitInfo, CaptureOrder& order)
    {
        // only units with FBI CanCapture may capture; a unit cannot
        // capture itself (TotalA.exe 0x4042cf checks unitdef flag
        // 0x1000 on the capturer)
        if (!unitInfo.definition->canCapture || order.target == unitInfo.id)
        {
            return true;
        }

        auto targetUnitRef = sim->tryGetUnitState(order.target);
        if (!targetUnitRef || targetUnitRef->get().isDead())
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        // retail order-start checks (TotalA.exe 0x4042e3-0x404342):
        // capture-capable targets (commanders) are immune; unfinished
        // nanoframes and carried units cannot be captured; a
        // same-owner transfer early-outs in the transfer routine, so
        // an order on a friendly unit completes as a no-op. Allied
        // targets are likewise ineligible — capture is enemy-only.
        if (targetUnitDefinition.canCapture
            || targetUnit.isBeingBuilt(targetUnitDefinition)
            || targetUnit.carriedBy
            || sim->areAllied(targetUnit.owner, unitInfo.state->owner)
            || (targetUnit.missionState && targetUnit.missionState->immunity))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }

        if (!order.workRequired)
        {
            // retail snapshots the required work when the order first
            // executes (0x404359-0x404407); later damage does not
            // rescale it:
            //   base = min(1800, E*0.015 + M*(3/14) + 150)
            //   scaled = base * (hp + maxHp) / (2 * maxHp)
            // Preserve native base work/cadence, applying the target's uncapped
            // retail factor ((kills / 5) + 10) / 10 (0x4043d8).
            const auto base = static_cast<long long>(
                targetUnitDefinition.buildCostEnergy.value * 0.015f
                + targetUnitDefinition.buildCostMetal.value * (3.0f / 14.0f)
                + 150.0f);
            const auto clamped = std::min<long long>(base, 1800);
            const auto maxHp = std::max(1u, targetUnitDefinition.maxHitPoints);
            const auto scaled = static_cast<unsigned int>(
                static_cast<unsigned long long>(clamped) * (targetUnit.hitPoints + maxHp) / (2 * maxHp));
            order.workRequired = static_cast<unsigned int>(static_cast<uint64_t>(scaled) * 2 * (targetUnit.kills / 5 + 10) / 10);
        }

        if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
        {
            // the nanolathe is only active while actually in range
            if (auto cs = std::get_if<UnitBehaviorStateCapturing>(&unitInfo.state->behaviourState))
            {
                cs->nanoParticleOrigin = std::nullopt;
            }
            navigateTo(unitInfo, order.target);
            return false;
        }

        return deployCaptureArm(unitInfo, order);
    }

    bool UnitBehaviorService::deployCaptureArm(UnitInfo unitInfo, CaptureOrder& order)
    {
        auto targetUnitRef = sim->tryGetUnitState(order.target);
        if (!targetUnitRef || targetUnitRef->get().isDead())
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        if (auto cs = std::get_if<UnitBehaviorStateCapturing>(&unitInfo.state->behaviourState))
        {
            if (cs->target != order.target)
            {
                changeState(*unitInfo.state, UnitBehaviorStateIdle());
                return captureUnit(unitInfo, order);
            }

            // a moving target can leave working range; chase it
            if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
            {
                cs->nanoParticleOrigin = std::nullopt;
                navigateTo(unitInfo, order.target);
                return false;
            }

            cs->targetPosition = targetUnit.position;
            cs->nanoParticleOrigin = getNanoPoint(unitInfo.id);
            unitInfo.state->activeNanolatheTarget = std::make_pair(
                UnitState::NanolatheTargetRef(cs->target), *cs->nanoParticleOrigin);

            // retail accrues 2 work per 6-tick cycle while the beam is
            // on the target (TotalA.exe 0x4045b3-0x4045dd); progress is
            // independent of WorkerTime and consumes no resources
            if (++order.workTicks >= CaptureOrder::workPeriod)
            {
                order.workTicks = 0;
                order.progress += CaptureOrder::workRate;
            }

            if (order.progress >= *order.workRequired)
            {
                sim->transferUnitOwnership(order.target, unitInfo.state->owner, unitInfo.id);
                changeState(*unitInfo.state, UnitBehaviorStateIdle());
                return true;
            }
            return false;
        }

        // enter the capturing state; retail fires a "Capturing" COB
        // thread (0x404342) — a stub entry point absent from stock
        // scripts — and drives the nanolathe FX exe-side, so capture
        // work does not wait on inBuildStance
        auto nanoFromPosition = getNanoPoint(unitInfo.id);
        auto headingAndPitch = computeLineOfSightHeadingAndPitch(unitInfo.state->rotation, nanoFromPosition, targetUnit.position);
        changeState(*unitInfo.state, UnitBehaviorStateCapturing{order.target, targetUnit.position, std::nullopt});
        unitInfo.state->cobEnvironment->createThread("Capturing", {toCobAngle(headingAndPitch.first).value, toCobAngle(headingAndPitch.second).value});
        sim->events.push_back(UnitStartedCapturingEvent{unitInfo.id});
        return false;
    }

    void UnitBehaviorService::finishReclaim(FeatureId targetFeatureId)
    {
        auto feature = sim->tryGetFeature(targetFeatureId);
        if (!feature)
        {
            return;
        }

        sim->transitionFeature(targetFeatureId, FeatureEvent::Reclaiming);
    }

    void UnitBehaviorService::finishReclaim(UnitId targetUnitId)
    {
        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);
        if (!targetUnitRef || targetUnitRef->get().isDead())
        {
            return;
        }

        // Reclaim consumes the unit: it dies silently with no corpse and
        // no explosion, so it must not spawn a wreck or trigger
        // combat-death effects. The Deleted event lets listeners
        // (selection, hovered unit, GUI info) drop their references.
        auto& targetUnit = targetUnitRef->get();
        targetUnit.markAsDeadNoCorpse();
        sim->creditUnitVeterancy(targetUnit);
        sim->events.push_back(UnitDiedEvent{
            targetUnitId,
            targetUnit.unitType,
            targetUnit.position,
            UnitDiedEvent::DeathType::Deleted});
    }

    bool UnitBehaviorService::handleGuardOrder(UnitInfo unitInfo, const GuardOrder& guardOrder)
    {
        auto target = sim->tryGetUnitState(guardOrder.target);
        // retail gates guard on the player's alliance table
        // (+0x108 alliedWith), not ownership: a builder may guard and
        // assist an allied player's unit
        if (!target || !sim->areAllied(target->get().owner, unitInfo.state->owner))
        {
            // unit is dead or a traitor, abandon order
            return true;
        }
        auto& targetUnit = target->get();


        // assist building
        if (auto bs = std::get_if<UnitBehaviorStateBuilding>(&targetUnit.behaviourState); unitInfo.definition->builder && bs)
        {
            buildExistingUnit(unitInfo, bs->targetUnit);
            return false;
        }

        // assist repairing
        if (auto rs = std::get_if<UnitBehaviorStateRepairing>(&targetUnit.behaviourState); unitInfo.definition->builder && rs)
        {
            repairExistingUnit(unitInfo, rs->targetUnit);
            return false;
        }

        // assist reclaiming
        if (auto rs = std::get_if<UnitBehaviorStateReclaiming>(&targetUnit.behaviourState); unitInfo.definition->builder && rs)
        {
            match(
                rs->target,
                [&](const FeatureId& featureId) { reclaimFeature(unitInfo, featureId); },
                [&](const UnitId& unitId) { reclaimUnit(unitInfo, unitId); });
            return false;
        }

        // assist factory building
        if (auto fs = std::get_if<FactoryBehaviorStateBuilding>(&targetUnit.factoryState); unitInfo.definition->builder && fs)
        {
            if (fs->targetUnit)
            {
                buildExistingUnit(unitInfo, fs->targetUnit->first);
                return false;
            }
        }

        // stay close
        if (unitInfo.definition->canMove && unitInfo.state->position.distanceSquared(targetUnit.position) > SimScalar(200 * 200))
        {
            navigateTo(unitInfo, guardOrder.target);
            return false;
        }

        return false;
    }

    bool UnitBehaviorService::handleBuild(UnitInfo unitInfo, const std::string& unitType)
    {
        return match(
            unitInfo.state->factoryState,
            [&](const FactoryBehaviorStateIdle&) {
                sim->activateUnit(unitInfo.id);
                unitInfo.state->factoryState = FactoryBehaviorStateBuilding();
                return false;
            },
            [&](FactoryBehaviorStateCreatingUnit& state) {
                return match(
                    state.status,
                    [&](const UnitCreationStatusPending&) {
                        return false;
                    },
                    [&](const UnitCreationStatusDone& s) {
                        unitInfo.state->cobEnvironment->createThread("StartBuilding");
                        unitInfo.state->factoryState = FactoryBehaviorStateBuilding{std::make_pair(s.unitId, std::optional<SimVector>())};
                        return false;
                    },
                    [&](const UnitCreationStatusFailed&) {
                        unitInfo.state->factoryState = FactoryBehaviorStateBuilding();
                        return false;
                    });
            },
            [&](FactoryBehaviorStateBuilding& state) {
                if (!unitInfo.state->inBuildStance)
                {
                    if (state.targetUnit)
                    {
                        state.targetUnit->second = std::nullopt;
                    }
                    return false;
                }

                auto buildPieceInfo = getBuildPieceInfo(unitInfo.id);
                // buildPieceInfo.position.y = sim->terrain.getHeightAt(buildPieceInfo.position.x, buildPieceInfo.position.z);
                if (!state.targetUnit)
                {
                    unitInfo.state->factoryState = FactoryBehaviorStateCreatingUnit{unitType, unitInfo.state->owner, buildPieceInfo.position, buildPieceInfo.rotation};
                    sim->unitCreationRequests.push_back(unitInfo.id);
                    return false;
                }

                auto targetUnitOption = sim->tryGetUnitState(state.targetUnit->first);
                if (!targetUnitOption)
                {
                    unitInfo.state->factoryState = FactoryBehaviorStateCreatingUnit{unitType, unitInfo.state->owner, buildPieceInfo.position, buildPieceInfo.rotation};
                    sim->unitCreationRequests.push_back(unitInfo.id);
                    return false;
                }

                auto& targetUnit = targetUnitOption->get();
                const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

                if (targetUnit.unitType != unitType)
                {
                    if (targetUnit.isBeingBuilt(targetUnitDefinition) && !targetUnit.isDead())
                    {
                        sim->quietlyKillUnit(state.targetUnit->first);
                    }
                    state.targetUnit = std::nullopt;
                    return false;
                }

                if (targetUnit.isDead())
                {
                    unitInfo.state->cobEnvironment->createThread("StopBuilding");
                    sim->deactivateUnit(unitInfo.id);
                    unitInfo.state->factoryState = FactoryBehaviorStateIdle();
                    return true;
                }

                if (!targetUnit.isBeingBuilt(targetUnitDefinition))
                {
                    // produced units inherit the factory's orders as a
                    // rally mechanism -- but never its mission script:
                    // retail gives scripted production only a GetBuilt
                    if (unitInfo.state->orders.empty()
                        || std::holds_alternative<MissionFactoryBuildOrder>(unitInfo.state->orders.front()))
                    {
                        auto footprintRect = sim->computeFootprintRegion(unitInfo.state->position, unitInfo.definition->movementCollisionInfo);
                        targetUnit.addOrder(BuggerOffOrder(footprintRect));
                    }
                    else
                    {
                        targetUnit.replaceOrders(unitInfo.state->orders);
                    }
                    unitInfo.state->cobEnvironment->createThread("StopBuilding");
                    sim->deactivateUnit(unitInfo.id);
                    unitInfo.state->factoryState = FactoryBehaviorStateIdle();
                    return true;
                }

                if (targetUnitDefinition.floater || targetUnitDefinition.canHover)
                {
                    buildPieceInfo.position.y = rweMax(buildPieceInfo.position.y, sim->terrain.getSeaLevel());
                }

                tryApplyMovementToPosition(sim->getUnitInfo(state.targetUnit->first), buildPieceInfo.position);
                targetUnit.rotation = buildPieceInfo.rotation;

                auto costs = targetUnit.getBuildCostInfo(targetUnitDefinition, unitInfo.definition->workerTimePerTick);
                auto gotResources = sim->addScaledResourceDelta(
                    unitInfo.id,
                    -Energy(targetUnitDefinition.buildCostEnergy.value * static_cast<float>(unitInfo.definition->workerTimePerTick) / static_cast<float>(targetUnitDefinition.buildTime)),
                    -Metal(targetUnitDefinition.buildCostMetal.value * static_cast<float>(unitInfo.definition->workerTimePerTick) / static_cast<float>(targetUnitDefinition.buildTime)),
                    -costs.energyCost,
                    -costs.metalCost);

                if (!gotResources)
                {
                    // we don't have resources available to build -- wait
                    state.targetUnit->second = std::nullopt;
                    return false;
                }
                state.targetUnit->second = getNanoPoint(unitInfo.id);
                unitInfo.state->activeNanolatheTarget = std::make_pair(
                    UnitState::NanolatheTargetRef(state.targetUnit->first), *state.targetUnit->second);

                if (targetUnit.addBuildProgress(targetUnitDefinition, unitInfo.definition->workerTimePerTick))
                {
                    sim->events.push_back(UnitCompleteEvent{state.targetUnit->first});

                    if (targetUnitDefinition.activateWhenBuilt)
                    {
                        sim->activateUnit(state.targetUnit->first);
                    }
                }

                return false;
            });
    }

    void UnitBehaviorService::clearBuild(UnitInfo unitInfo)
    {
        match(
            unitInfo.state->factoryState,
            [&](const FactoryBehaviorStateIdle&) {
                // do nothing
            },
            [&](const FactoryBehaviorStateCreatingUnit& state) {
                match(
                    state.status,
                    [&](const UnitCreationStatusDone& d) {
                        sim->quietlyKillUnit(d.unitId);
                    },
                    [&](const auto&) {
                        // do nothing
                    });
                sim->deactivateUnit(unitInfo.id);
                unitInfo.state->factoryState = FactoryBehaviorStateIdle();
            },
            [&](const FactoryBehaviorStateBuilding& state) {
                if (state.targetUnit)
                {
                    sim->quietlyKillUnit(state.targetUnit->first);
                    unitInfo.state->cobEnvironment->createThread("StopBuilding");
                }
                sim->deactivateUnit(unitInfo.id);
                unitInfo.state->factoryState = FactoryBehaviorStateIdle();
            });
    }

    SimVector UnitBehaviorService::getNanoPoint(UnitId id)
    {
        auto pieceId = runCobQuery(id, "QueryNanoPiece");
        if (!pieceId)
        {
            return sim->getUnitState(id).position;
        }

        return getPiecePosition(id, *pieceId);
    }

    SimVector UnitBehaviorService::getPieceLocalPosition(UnitId id, unsigned int pieceId)
    {
        auto& unit = sim->getUnitState(id);

        const auto& pieceName = unit.cobEnvironment->_script->pieces.at(pieceId);
        auto pieceTransform = sim->getUnitPieceLocalTransform(id, pieceName);

        return pieceTransform * SimVector(0_ss, 0_ss, 0_ss);
    }

    SimVector UnitBehaviorService::getPiecePosition(UnitId id, unsigned int pieceId)
    {
        auto& unit = sim->getUnitState(id);

        return unit.getTransform() * getPieceLocalPosition(id, pieceId);
    }

    SimAngle UnitBehaviorService::getPieceXZRotation(UnitId id, unsigned int pieceId)
    {
        auto& unit = sim->getUnitState(id);

        const auto& pieceName = unit.cobEnvironment->_script->pieces.at(pieceId);
        auto pieceTransform = sim->getUnitPieceLocalTransform(id, pieceName);

        auto mat = unit.getTransform() * pieceTransform;

        auto a = Vector2x<SimScalar>(0_ss, 1_ss);
        auto b = mat.mult3x3(SimVector(0_ss, 0_ss, 1_ss)).xz();
        if (b.lengthSquared() == 0_ss)
        {
            return SimAngle(0);
        }

        // angleTo is computed in a space where Y points up,
        // but in our XZ space (Z is our Y here), Z points down.
        // This means we need to negate (and rewrap) the rotation value.
        return -angleTo(a, b);
    }

    UnitBehaviorService::BuildPieceInfo UnitBehaviorService::getBuildPieceInfo(UnitId id)
    {
        auto pieceId = runCobQuery(id, "QueryBuildInfo");
        if (!pieceId)
        {
            const auto& unit = sim->getUnitState(id);
            return BuildPieceInfo{unit.position, unit.rotation};
        }

        return BuildPieceInfo{getPiecePosition(id, *pieceId), getPieceXZRotation(id, *pieceId)};
    }

    std::optional<SimVector> UnitBehaviorService::getTargetPosition(const UnitWeaponAttackTarget& target)
    {
        return match(
            target,
            [](const SimVector& v) { return std::make_optional(v); },
            [this](UnitId id) { return tryGetSweetSpot(id); },
            [this](ProjectileId id) -> std::optional<SimVector> {
                auto projectile = sim->projectiles.tryGet(id);
                if (!projectile || projectile->get().isDead)
                {
                    return std::nullopt;
                }
                return projectile->get().position;
            });
    }

    std::optional<SimVector> UnitBehaviorService::getTargetPosition(const AttackTarget& target)
    {
        return match(
            target,
            [](const SimVector& v) { return std::make_optional(v); },
            [this](UnitId id) { return tryGetSweetSpot(id); });
    }

    PathDestination UnitBehaviorService::resolvePathDestination(UnitState& s, const MovingStateGoal& goal)
    {
        return match(
            goal,
            [&](const SimVector& v) -> PathDestination {
                return v;
            },
            [&](const DiscreteRect& r) -> PathDestination {
                return r;
            },
            [&](const UnitId& u) -> PathDestination {
                return getUnitPositionWithCache(s, u);
            });
    }

    void UnitBehaviorService::groundUnitMoveTo(UnitInfo unitInfo, const MovingStateGoal& goal)
    {
        auto movingState = std::get_if<NavigationStateMoving>(&unitInfo.state->navigationState.state);

        if (!movingState || movingState->movementGoal != goal)
        {
            // request a path to follow
            unitInfo.state->navigationState.state = NavigationStateMoving{goal, resolvePathDestination(*unitInfo.state, goal), std::nullopt, true};
            sim->requestPath(unitInfo.id);
            return;
        }

        // check to see if our goal has moved from its original location
        auto resolvedDestination = resolvePathDestination(*unitInfo.state, goal);
        if (resolvedDestination != movingState->pathDestination)
        {
            // The resolved position of our goal has changed.
            // We'll assume that this change isn't too big
            // i.e. we don't need to throw away our previous path,
            // we can still continue following it
            // while we wait for a new path to be computed.
            movingState->pathDestination = resolvedDestination;
            sim->requestPath(unitInfo.id);
            movingState->pathRequested = true;
        }

        // if we are colliding, request a new path
        if (unitInfo.state->inCollision && !movingState->pathRequested)
        {
            // only request a new path if we don't have one yet,
            // or we've already had our current one for a bit
            if (!movingState->path || (sim->gameTime - movingState->path->pathCreationTime) >= GameTime(30))
            {
                sim->requestPath(unitInfo.id);
                movingState->pathRequested = true;
            }
        }

        // if a path is available, attempt to follow it
        if (movingState->path)
        {
            auto groundPhysics = std::get_if<UnitPhysicsInfoGround>(&unitInfo.state->physics);
            if (groundPhysics == nullptr)
            {
                throw std::logic_error("ground unit does not have ground physics");
            }
            if (followPath(unitInfo, *groundPhysics, *movingState->path))
            {
                // We finished following the path.
                if (movingState->path->path.isPartial)
                {
                    // The path was a best-effort partial: the unit has
                    // reached the closest reachable point and the move
                    // job is done (retail clears the job-active flag in
                    // unit+0x10f here). Don't request another path --
                    // there is no better one.
                    movingState->pathExhausted = true;
                }
                else if (!movingState->path || (sim->gameTime - movingState->path->pathCreationTime) >= GameTime(30))
                {
                    // This doesn't necessarily mean we are at the goal.
                    // Request a new path to get us the rest of the way there.
                    sim->requestPath(unitInfo.id);
                    movingState->pathRequested = true;
                }
            }
        }
    }

    bool UnitBehaviorService::flyingUnitMoveTo(UnitInfo unitInfo, const MovingStateGoal& goal)
    {
        return match(
            unitInfo.state->physics,
            [&](UnitPhysicsInfoGround& p) {
                p.steeringInfo.shouldTakeOff = true;
                return false;
            },
            [&](const UnitPhysicsInfoAir& p) {
                return flyTowardsGoal(unitInfo, goal);
            });
    }

    NavigationResult UnitBehaviorService::navigateTo(UnitInfo unitInfo, const NavigationGoal& goal, SimScalar arrivalRadius, std::optional<SimVector> arrivalPosition)
    {
        unitInfo.state->navigationState.desiredDestination = goal;

        // Unit-following paths retain their stable ID/cache while arrival may
        // use the target's current point. Moving targets must not discard a
        // usable path on every tick by changing the path-goal variant payload.
        if (hasReachedGoal(*sim, sim->terrain, *unitInfo.state, *unitInfo.definition,
                arrivalPosition ? NavigationGoal(*arrivalPosition) : goal, arrivalRadius))
        {
            return NavigationResult::Arrived;
        }

        // retail's move job also completes when the mover's best-effort
        // path is exhausted at the closest reachable point (the job-done
        // flag the order poll waits on); report it distinctly so
        // range-gated orders don't mistake it for arrival
        if (auto movingState = std::get_if<NavigationStateMoving>(&unitInfo.state->navigationState.state);
            movingState != nullptr && movingState->pathExhausted && sim->gameTime >= movingState->pathExhaustedAt)
        {
            return NavigationResult::PathExhausted;
        }

        return NavigationResult::InProgress;
    }

    void UnitBehaviorService::moveTo(UnitInfo unitInfo, const MovingStateGoal& goal)
    {
        if (unitInfo.definition->canFly)
        {
            flyingUnitMoveTo(unitInfo, goal);
        }
        else
        {
            groundUnitMoveTo(unitInfo, goal);
        }
    }

    UnitCreationStatus UnitBehaviorService::createNewUnit(UnitInfo unitInfo, const std::string& unitType, const SimVector& position)
    {
        if (auto s = std::get_if<UnitBehaviorStateCreatingUnit>(&unitInfo.state->behaviourState))
        {
            if (s->unitType == unitType && s->position == position)
            {
                return s->status;
            }
        }

        const auto& targetUnitDefinition = sim->unitDefinitions.at(unitType);
        auto footprintRect = sim->computeFootprintRegion(position, targetUnitDefinition.movementCollisionInfo);

        // structures are validated against the same placement rules the
        // build cursor uses: terrain, water depth, slope, geo and occupancy
        if (!targetUnitDefinition.isMobile)
        {
            auto mc = sim->getAdHocMovementClass(targetUnitDefinition.movementCollisionInfo);
            auto placement = sim->computeBuildingPlacement(mc, targetUnitDefinition.yardMap, targetUnitDefinition.yardMapContainsGeo, targetUnitDefinition.waterline, footprintRect.x, footprintRect.y);
            if (!placement || !placement->valid)
            {
                return UnitCreationStatusFailed();
            }
        }

        // only actual arrival may begin construction: a best-effort
        // path exhausted out of build range must not start work, and
        // with no job timeout the order simply stays pending like retail
        if (navigateTo(unitInfo, footprintRect) == NavigationResult::Arrived)
        {
            changeState(*unitInfo.state, UnitBehaviorStateCreatingUnit{unitType, unitInfo.state->owner, position});
            sim->unitCreationRequests.push_back(unitInfo.id);
        }

        return UnitCreationStatusPending();
    }

    bool UnitBehaviorService::buildUnit(UnitInfo unitInfo, const std::string& unitType, const SimVector& position)
    {
        auto& unit = sim->getUnitState(unitInfo.id);
        if (!unit.buildOrderUnitId)
        {
            auto result = createNewUnit(unitInfo, unitType, position);
            return match(
                result,
                [&](const UnitCreationStatusPending&) { return false; },
                [&](const UnitCreationStatusFailed&) { return true; },
                [&](const UnitCreationStatusDone& d) {
                    unit.buildOrderUnitId = d.unitId;
                    return deployBuildArm(unitInfo, d.unitId);
                });
        }

        return deployBuildArm(unitInfo, *unit.buildOrderUnitId);
    }

    bool UnitBehaviorService::buildExistingUnit(UnitInfo unitInfo, UnitId targetUnitId)
    {
        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);

        if (!targetUnitRef
            || targetUnitRef->get().isDead()
            || !targetUnitRef->get().isBeingBuilt(sim->unitDefinitions.at(targetUnitRef->get().unitType)))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
        {
            if (auto bs = std::get_if<UnitBehaviorStateBuilding>(&unitInfo.state->behaviourState))
            {
                bs->nanoParticleOrigin = std::nullopt;
            }
            navigateTo(unitInfo, targetUnitId);
            return false;
        }

        // we're close enough -- actually build the unit
        return deployBuildArm(unitInfo, targetUnitId);
    }

    bool UnitBehaviorService::inWorkingRange(UnitInfo unitInfo, const UnitState& targetUnit, const UnitDefinition& targetUnitDefinition)
    {
        auto footprintRect = sim->computeFootprintRegion(targetUnit.position, targetUnitDefinition.movementCollisionInfo);
        auto nearestPoint = findClosestPointToFootprintXZ(sim->terrain, footprintRect, unitInfo.state->position);
        auto distanceSquared = unitInfo.state->position.distanceSquared(nearestPoint);
        auto buildDistance = unitInfo.definition->buildDistance;
        return distanceSquared <= buildDistance * buildDistance;
    }

    void UnitBehaviorService::changeState(UnitState& unit, const UnitBehaviorState& newState)
    {
        if (std::holds_alternative<UnitBehaviorStateBuilding>(unit.behaviourState)
            || std::holds_alternative<UnitBehaviorStateRepairing>(unit.behaviourState)
            || std::holds_alternative<UnitBehaviorStateReclaiming>(unit.behaviourState))
        {
            unit.cobEnvironment->createThread("StopBuilding");
        }
        unit.behaviourState = newState;
    }
    bool UnitBehaviorService::deployBuildArm(UnitInfo unitInfo, UnitId targetUnitId)
    {
        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);
        if (!targetUnitRef || targetUnitRef->get().isDead() || !targetUnitRef->get().isBeingBuilt(sim->unitDefinitions.at(targetUnitRef->get().unitType)))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        return match(
            unitInfo.state->behaviourState,
            [&](UnitBehaviorStateBuilding& buildingState) {
                if (targetUnitId != buildingState.targetUnit)
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return buildExistingUnit(unitInfo, targetUnitId);
                }

                if (!unitInfo.state->inBuildStance)
                {
                    // We are not in the correct stance to build the unit yet, wait.
                    buildingState.nanoParticleOrigin = std::nullopt;
                    return false;
                }

                auto costs = targetUnit.getBuildCostInfo(targetUnitDefinition, unitInfo.definition->workerTimePerTick);
                auto gotResources = sim->addScaledResourceDelta(
                    unitInfo.id,
                    -Energy(targetUnitDefinition.buildCostEnergy.value * static_cast<float>(unitInfo.definition->workerTimePerTick) / static_cast<float>(targetUnitDefinition.buildTime)),
                    -Metal(targetUnitDefinition.buildCostMetal.value * static_cast<float>(unitInfo.definition->workerTimePerTick) / static_cast<float>(targetUnitDefinition.buildTime)),
                    -costs.energyCost,
                    -costs.metalCost);

                if (!gotResources)
                {
                    // we don't have resources available to build -- wait
                    buildingState.nanoParticleOrigin = std::nullopt;
                    return false;
                }
                buildingState.nanoParticleOrigin = getNanoPoint(unitInfo.id);
                unitInfo.state->activeNanolatheTarget = std::make_pair(
                    UnitState::NanolatheTargetRef(buildingState.targetUnit), *buildingState.nanoParticleOrigin);

                if (targetUnit.addBuildProgress(targetUnitDefinition, unitInfo.definition->workerTimePerTick))
                {
                    sim->events.push_back(UnitCompleteEvent{buildingState.targetUnit});

                    if (targetUnitDefinition.activateWhenBuilt)
                    {
                        sim->activateUnit(buildingState.targetUnit);
                    }

                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return true;
                }
                return false;
            },
            [&](const auto&) {
                auto nanoFromPosition = getNanoPoint(unitInfo.id);
                auto headingAndPitch = computeLineOfSightHeadingAndPitch(unitInfo.state->rotation, nanoFromPosition, targetUnit.position);
                auto heading = headingAndPitch.first;
                auto pitch = headingAndPitch.second;

                changeState(*unitInfo.state, UnitBehaviorStateBuilding{targetUnitId, std::nullopt});
                unitInfo.state->cobEnvironment->createThread("StartBuilding", {toCobAngle(heading).value, toCobAngle(pitch).value});
                return false;
            });
    }

    bool UnitBehaviorService::repairExistingUnit(UnitInfo unitInfo, UnitId targetUnitId)
    {
        // only builders can repair
        if (!unitInfo.definition->builder || targetUnitId == unitInfo.id)
        {
            return true;
        }

        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);

        // retail repair accepts allied targets: the repair-patrol
        // candidate scan (TotalA.exe 0x410c70) collects units via the
        // player's alliedWith table, so repair is gated on alliance
        if (!targetUnitRef
            || targetUnitRef->get().isDead()
            || !sim->areAllied(targetUnitRef->get().owner, unitInfo.state->owner)
            || targetUnitRef->get().isBeingBuilt(sim->unitDefinitions.at(targetUnitRef->get().unitType)))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        if (!inWorkingRange(unitInfo, targetUnit, targetUnitDefinition))
        {
            if (auto rs = std::get_if<UnitBehaviorStateRepairing>(&unitInfo.state->behaviourState))
            {
                rs->nanoParticleOrigin = std::nullopt;
            }
            navigateTo(unitInfo, targetUnitId);
            return false;
        }

        return deployRepairArm(unitInfo, targetUnitId);
    }

    bool UnitBehaviorService::deployRepairArm(UnitInfo unitInfo, UnitId targetUnitId)
    {
        auto targetUnitRef = sim->tryGetUnitState(targetUnitId);
        if (!targetUnitRef
            || targetUnitRef->get().isDead()
            || !sim->areAllied(targetUnitRef->get().owner, unitInfo.state->owner)
            || targetUnitRef->get().isBeingBuilt(sim->unitDefinitions.at(targetUnitRef->get().unitType)))
        {
            changeState(*unitInfo.state, UnitBehaviorStateIdle());
            return true;
        }
        auto& targetUnit = targetUnitRef->get();
        const auto& targetUnitDefinition = sim->unitDefinitions.at(targetUnit.unitType);

        return match(
            unitInfo.state->behaviourState,
            [&](UnitBehaviorStateRepairing& repairingState) {
                if (targetUnitId != repairingState.targetUnit)
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return repairExistingUnit(unitInfo, targetUnitId);
                }

                if (!unitInfo.state->inBuildStance)
                {
                    // We are not in the correct stance to repair the unit yet, wait.
                    repairingState.nanoParticleOrigin = std::nullopt;
                    return false;
                }

                if (targetUnit.hitPoints >= targetUnitDefinition.maxHitPoints)
                {
                    // nothing left to repair
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return true;
                }

                const auto gotResources = applyRepairStep(unitInfo, sim->getUnitInfo(targetUnitId), &repairingState.repairProgress);

                if (!gotResources)
                {
                    // we don't have resources available to repair -- wait
                    repairingState.nanoParticleOrigin = std::nullopt;
                    return false;
                }

                if (!repairingState.nanoParticleOrigin)
                {
                    sim->events.push_back(UnitStartedRepairingEvent{unitInfo.id});
                }
                repairingState.nanoParticleOrigin = getNanoPoint(unitInfo.id);
                unitInfo.state->activeNanolatheTarget = std::make_pair(
                    UnitState::NanolatheTargetRef(repairingState.targetUnit), *repairingState.nanoParticleOrigin);

                if (targetUnit.hitPoints >= targetUnitDefinition.maxHitPoints)
                {
                    changeState(*unitInfo.state, UnitBehaviorStateIdle());
                    return true;
                }
                return false;
            },
            [&](const auto&) {
                auto nanoFromPosition = getNanoPoint(unitInfo.id);
                auto headingAndPitch = computeLineOfSightHeadingAndPitch(unitInfo.state->rotation, nanoFromPosition, targetUnit.position);
                auto heading = headingAndPitch.first;
                auto pitch = headingAndPitch.second;

                changeState(*unitInfo.state, UnitBehaviorStateRepairing{targetUnitId, std::nullopt, targetUnit.getRepairProgressForHitPoints(targetUnitDefinition)});
                unitInfo.state->cobEnvironment->createThread("StartBuilding", {toCobAngle(heading).value, toCobAngle(pitch).value});
                return false;
            });
    }

    bool UnitBehaviorService::climbToCruiseAltitude(UnitInfo unitInfo)
    {
        auto targetHeight = getTargetAltitude(sim->terrain, unitInfo.state->position.x, unitInfo.state->position.z, *unitInfo.definition);

        unitInfo.state->position.y = rweMin(unitInfo.state->position.y + 1_ss, targetHeight);

        return unitInfo.state->position.y == targetHeight;
    }

    bool UnitBehaviorService::descendToGroundLevel(UnitInfo unitInfo)
    {
        auto terrainHeight = sim->terrain.getHeightAt(unitInfo.state->position.x, unitInfo.state->position.z);

        unitInfo.state->position.y = rweMax(unitInfo.state->position.y - 1_ss, terrainHeight);

        return unitInfo.state->position.y == terrainHeight;
    }

    void UnitBehaviorService::transitionFromGroundToAir(UnitInfo unitInfo)
    {
        unitInfo.state->activate();

        unitInfo.state->physics = UnitPhysicsInfoAir();
        auto footprintRect = sim->computeFootprintRegion(unitInfo.state->position, unitInfo.definition->movementCollisionInfo);
        auto footprintRegion = sim->occupiedGrid.tryToRegion(footprintRect);
        assert(!!footprintRegion);
        sim->occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
            if (cell.mobileUnitId == unitInfo.id) { cell.mobileUnitId = std::nullopt; }
        });
        sim->flyingUnitsSet.insert(unitInfo.id);
    }

    bool UnitBehaviorService::tryTransitionFromAirToGround(UnitInfo unitInfo)
    {
        auto footprintRect = sim->computeFootprintRegion(unitInfo.state->position, unitInfo.definition->movementCollisionInfo);
        auto footprintRegion = sim->occupiedGrid.tryToRegion(footprintRect);
        assert(!!footprintRegion);

        if (sim->isCollisionAt(*footprintRegion))
        {
            return false;
        }

        sim->occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
            cell.mobileUnitId = unitInfo.id;
        });
        sim->flyingUnitsSet.erase(unitInfo.id);

        unitInfo.state->physics = UnitPhysicsInfoGround();

        return true;
    }

    bool UnitBehaviorService::flyTowardsGoal(UnitInfo unitInfo, const MovingStateGoal& goal)
    {
        auto destination = match(
            goal,
            [&](const SimVector& pos) {
                return pos;
            },
            [&](const DiscreteRect& rect) {
                return findClosestPointToFootprintXZ(sim->terrain, rect, unitInfo.state->position);
            },
            [&](const UnitId& u) {
                const auto& unit = sim->getUnitState(u);
                return unit.position;
            });

        SimVector xzPosition(unitInfo.state->position.x, 0_ss, unitInfo.state->position.z);
        SimVector xzDestination(destination.x, 0_ss, destination.z);
        auto distanceSquared = xzPosition.distanceSquared(xzDestination);

        if (distanceSquared < (8_ss * 8_ss))
        {
            return true;
        }

        auto airPhysics = std::get_if<UnitPhysicsInfoAir>(&unitInfo.state->physics);
        if (airPhysics == nullptr)
        {
            throw std::logic_error("cannot fly towards goal because unit does not have air physics");
        }

        match(
            airPhysics->movementState,
            [&](AirMovementStateFlying& m) {
                auto targetHeight = getTargetAltitude(sim->terrain, destination.x, destination.z, *unitInfo.definition);
                SimVector destinationAtAltitude(destination.x, targetHeight, destination.z);

                m.targetPosition = destinationAtAltitude;
            },
            [&](const AirMovementStateTakingOff&) {
                // do nothing
            },
            [&](AirMovementStateLanding& m) {
                m.shouldAbort = true;
            });

        return false;
    }
}
