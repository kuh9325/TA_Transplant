#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/sim/UnitBehaviorService_util.h>
#include <rwe/cob/CobExecutionContext.h>
#include <algorithm>
#include <cmath>

namespace rwe
{
    void GameSimulation::rebuildAirServiceIndexes()
    {
        airServiceFacilities.clear();
        airServiceOccupants.clear();
        for (const auto& [id, unit] : units)
        {
            if (unit.isDead()) { continue; }
            if (unitDefinitions.at(unit.unitType).isAirBase) { airServiceFacilities.insert(id); }
            if (unit.aircraftService && unit.aircraftService->docked())
            {
                const auto& s = *unit.aircraftService;
                if (!airServiceOccupants.emplace(std::make_pair(s.facility, s.piece), id).second)
                {
                    throw std::runtime_error("duplicate aircraft landing-pad occupant");
                }
            }
        }
    }

    void GameSimulation::releaseAirService(UnitId aircraft)
    {
        auto& unit = getUnitState(aircraft);
        if (!unit.aircraftService) { return; }
        const auto& s = *unit.aircraftService;
        auto it = airServiceOccupants.find({s.facility, s.piece});
        if (it != airServiceOccupants.end() && it->second == aircraft) { airServiceOccupants.erase(it); }
        unit.aircraftService.reset();
    }

    void GameSimulation::destroyAirServiceDependents(UnitId facility, UnitDeathCause cause)
    {
        airServiceFacilities.erase(facility);
        // Retail pad attachments participate in the carrier's death group.
        // Approaching aircraft have no attachment and remain alive.
        auto it = airServiceOccupants.lower_bound({facility, -1});
        while (it != airServiceOccupants.end() && it->first.first == facility)
        {
            const auto aircraft = it->second;
            airServiceOccupants.erase(it);
            if (auto patient = tryGetUnitState(aircraft); patient && patient->get().isAlive())
            {
                patient->get().lastDamagedBy = getUnitState(facility).lastDamagedBy;
                patient->get().lastDamagedByPlayer = getUnitState(facility).lastDamagedByPlayer;
                killUnit(aircraft, cause);
            }
            it = airServiceOccupants.lower_bound({facility, -1});
        }
    }

    bool UnitBehaviorService::validAirFacility(UnitInfo aircraft, UnitId facility) const
    {
        auto base = sim->tryGetUnitState(facility);
        if (!base || base->get().isDead() || base->get().carriedBy || facility == aircraft.id
            || !sim->areAllied(aircraft.state->owner, base->get().owner)) { return false; }
        const auto& def = sim->unitDefinitions.at(base->get().unitType);
        return def.isAirBase && def.builder && !base->get().isBeingBuilt(def);
    }

    bool UnitBehaviorService::startAircraftService(UnitInfo unit, UnitId facility, bool automatic)
    {
        if (!unit.definition->canFly || !unit.definition->isMobile || unit.state->carriedBy
            || !unit.state->passengers.empty() || unit.state->aircraftService
            || !validAirFacility(unit, facility)) { return false; }
        unit.state->aircraftService = std::make_unique<AircraftServiceState>();
        auto& s = *unit.state->aircraftService;
        s.facility = facility;
        s.automatic = automatic;
        s.loiterBearing = SimAngle(sim->rng() % 65536);
        s.goal = unit.state->position;
        if (isFlying(unit.state->physics) && !unit.state->activated) { unit.state->activate(); }
        unit.state->navigationState = NavigationStateInfo{};
        unit.state->clearWeaponTargets();
        return true;
    }

    bool UnitBehaviorService::seekAircraftService(UnitInfo unit)
    {
        if (!unit.definition->canFly || unit.state->isParalyzed() || unit.state->aircraftService
            || unit.state->hitPoints >= (unit.definition->maxHitPoints >> 2) * 3) { return false; }
        std::vector<UnitId> candidates;
        for (auto id : sim->airServiceFacilities)
        {
            if (!validAirFacility(unit, id)) { continue; }
            const auto& base = sim->getUnitState(id);
            auto delta = base.position - unit.state->position;
            delta.y = 0_ss;
            if (base.activated && delta.lengthSquared() <= 3840_ss * 3840_ss) { candidates.push_back(id); }
        }
        if (candidates.empty()) { return false; }
        const auto index = candidates.size() == 1 ? 0u : sim->rng() % candidates.size();
        return startAircraftService(unit, candidates[index], true);
    }

    bool UnitBehaviorService::isAirPadFree(UnitId facility, int piece, UnitId aircraft) const
    {
        const auto& base = sim->getUnitState(facility);
        if (base.carriedBy || piece < 0
            || static_cast<std::size_t>(piece) >= base.cobEnvironment->_script->pieces.size()) { return false; }
        const auto occupant = sim->airServiceOccupants.find({facility, piece});
        if (occupant != sim->airServiceOccupants.end() && occupant->second != aircraft) { return false; }
        for (auto id : base.passengers)
        {
            auto passenger = sim->tryGetUnitState(id);
            if (passenger && passenger->get().isAlive() && passenger->get().attachSlot == piece) { return false; }
        }
        return true;
    }

    std::optional<int> UnitBehaviorService::queryFreeAirPad(UnitId facility, UnitId aircraft)
    {
        auto& base = sim->getUnitState(facility);
        if (base.carriedBy) { return std::nullopt; }
        auto thread = base.cobEnvironment->createNonScheduledThread("QueryLandingPad", {-1, -1, -1, -1});
        if (!thread) { return std::nullopt; }
        CobExecutionContext context(base.cobEnvironment.get(), &*thread);
        if (!std::holds_alternative<CobEnvironment::FinishedStatus>(context.execute()))
        {
            throw std::runtime_error("QueryLandingPad blocked before completion");
        }
        for (unsigned int i = 0; i < 4; ++i)
        {
            const auto piece = thread->returnLocals.at(i);
            if (isAirPadFree(facility, piece, aircraft)) { return piece; }
        }
        return std::nullopt;
    }

    bool UnitBehaviorService::updateAircraftService(UnitInfo unit, bool blocked)
    {
        auto& u = *unit.state;
        using Phase = AircraftServiceState::Phase;
        auto& s = *u.aircraftService;
        if (!validAirFacility(unit, s.facility))
        {
            sim->releaseAirService(unit.id);
            u.navigationState = NavigationStateInfo{};
            return false;
        }
        auto& base = sim->getUnitState(s.facility);
        if (s.docked())
        {
            // This is a live piece binding, including moving carriers and animation.
            u.previousPosition = u.position;
            u.position = sim->computeAttachPosition(base, s.piece);
            u.previousRotation = u.rotation;
            u.rotation = base.rotation;
        }
        if (blocked) { return true; }
        auto horizontalClose = [&](const SimVector& goal, SimScalar radius) {
            auto d = goal - u.position; d.y = 0_ss;
            return d.lengthSquared() < radius * radius;
        };
        auto restart = [&]() {
            s.phase = Phase::Waiting;
            s.piece = -1;
            s.goal = u.position;
        };
        switch (s.phase)
        {
            case Phase::Takeoff:
            {
                navigateTo(unit, base.position);
                auto air = std::get_if<UnitPhysicsInfoAir>(&u.physics);
                if (air && std::holds_alternative<AirMovementStateFlying>(air->movementState)) { restart(); }
                break;
            }
            case Phase::Waiting:
                if (!horizontalClose(s.goal, 128_ss)) { navigateTo(unit, s.goal); break; }
                if (queryFreeAirPad(s.facility, unit.id)) { s.phase = Phase::Approach; }
                else
                {
                    SimScalar range{0};
                    if (u.weapons[0]) { range = sim->weaponDefinitions.at(u.weapons[0]->weaponType).maxRange; }
                    s.goal = base.position + UnitState::toDirection(s.loiterBearing) * range;
                    s.loiterBearing += SimAngle(16384);
                    navigateTo(unit, s.goal);
                }
                break;
            case Phase::Approach:
                navigateTo(unit, base.position, 160_ss);
                if (horizontalClose(base.position, 160_ss))
                {
                    auto piece = queryFreeAirPad(s.facility, unit.id);
                    if (!piece) { restart(); break; }
                    s.piece = *piece;
                    s.phase = Phase::Align;
                }
                break;
            case Phase::Align:
            {
                auto pad = sim->computeAttachPosition(base, s.piece);
                s.goal = pad;
                s.goal.y += unit.definition->cruiseAltitude;
                if (horizontalClose(pad, 48_ss))
                {
                    if (!isAirPadFree(s.facility, s.piece, unit.id)) { restart(); break; }
                    s.phase = Phase::Descend;
                    u.cobEnvironment->createThread("EndTransport");
                }
                break;
            }
            case Phase::Descend:
            {
                // Revalidate at touchdown; approaching aircraft never reserve slots.
                if (!isAirPadFree(s.facility, s.piece, unit.id)) { restart(); break; }
                s.goal = sim->computeAttachPosition(base, s.piece);
                const auto delta = u.position - s.goal;
                // Retail follow-piece marker: half-unit horizontal tolerance,
                // exact heading, and explicit altitude within 0x10001 in 16.16.
                if (delta.x * delta.x + delta.z * delta.z <= 0.25_ssf
                    && rweAbs(delta.y) < simScalarFromFixed(0x10001)
                    && u.rotation == base.rotation)
                {
                    sim->airServiceOccupants.emplace(std::make_pair(s.facility, s.piece), unit.id);
                    u.position = s.goal;
                    u.physics = UnitPhysicsInfoAir{AirMovementStateFlying{}};
                    u.navigationState = NavigationStateInfo{};
                    s.phase = Phase::Repair;
                }
                break;
            }
            case Phase::Repair:
                if (u.hitPoints >= unit.definition->maxHitPoints) { s.phase = Phase::Parked; }
                else if (u.activated)
                {
                    u.cloakBlockedUntil = sim->gameTime + GameTime(150);
                    if (applyRepairStep(sim->getUnitInfo(s.facility), unit, nullptr))
                    {
                        u.activeNanolatheTarget = std::make_pair(UnitState::NanolatheTargetRef(unit.id), getNanoPoint(s.facility));
                    }
                }
                break;
            case Phase::Parked:
                if (s.automatic || !u.orders.empty())
                {
                    sim->airServiceOccupants.erase({s.facility, s.piece});
                    s.phase = Phase::Depart;
                    u.activate();
                    u.physics = UnitPhysicsInfoAir{AirMovementStateTakingOff{}};
                }
                break;
            case Phase::Depart:
                if (auto air = std::get_if<UnitPhysicsInfoAir>(&u.physics);
                    air && std::holds_alternative<AirMovementStateFlying>(air->movementState))
                {
                    sim->releaseAirService(unit.id);
                    return false;
                }
                break;
        }
        u.clearWeaponTargets();
        return true;
    }

    void UnitBehaviorService::steerAircraftService(UnitInfo unit)
    {
        const auto& s = *unit.state->aircraftService;
        using Phase = AircraftServiceState::Phase;
        if (s.phase != Phase::Align && s.phase != Phase::Descend) { return; }
        if (auto air = std::get_if<UnitPhysicsInfoAir>(&unit.state->physics))
        {
            if (auto flight = std::get_if<AirMovementStateFlying>(&air->movementState))
            {
                flight->targetPosition = s.goal;
                flight->shouldLand = false;
            }
        }
    }

    bool UnitBehaviorService::applyRepairStep(UnitInfo repairer, UnitInfo patient, unsigned int* progress)
    {
        auto& target = *patient.state;
        const auto& def = *patient.definition;
        if (target.hitPoints >= def.maxHitPoints) { return false; }
        if (progress)
        {
            // Preserve existing builder repair accounting, shared admission and
            // progress application. Aircraft SelfRepair has a distinct retail quantum.
            const auto worker = repairer.definition->workerTimePerTick;
            auto cost = target.getRepairCostInfo(def, *progress, worker);
            if (!sim->addScaledResourceDelta(repairer.id,
                -Energy(def.buildCostEnergy.value * static_cast<float>(worker) / def.buildTime),
                -Metal(def.buildCostMetal.value * static_cast<float>(worker) / def.buildTime),
                -cost.energyCost, -cost.metalCost)) { return false; }
            target.addRepairProgress(def, *progress, worker);
            return true;
        }
        // TotalA.exe 0x41bd10: truncate each term, cap positive integers at ONE.
        // Zero buildtime/worker performs no healing. Stock facilities author 200/30.
        auto term = [&](double amount) {
            if (def.buildTime == 0) { return 0; }
            return std::clamp(static_cast<int>(std::trunc(1.0 + (amount * repairer.definition->workerTimePerTick - 1.0) / def.buildTime)), 0, 1);
        };
        const auto hp = term(def.maxHitPoints);
        const auto energy = Energy(term(def.buildCostEnergy.value));
        if (!sim->addResourceDelta(repairer.id, -energy, Metal(0))) { return false; }
        target.hitPoints = std::min(def.maxHitPoints, target.hitPoints + hp);
        return true;
    }
}
