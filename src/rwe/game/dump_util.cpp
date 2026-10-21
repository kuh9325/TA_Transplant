#include "dump_util.h"

namespace rwe
{
    nlohmann::json dumpJson(float f)
    {
        return f;
    }
    nlohmann::json dumpJson(bool b)
    {
        return b;
    }
    nlohmann::json dumpJson(uint32_t i)
    {
        return i;
    }
    nlohmann::json dumpJson(int32_t i)
    {
        return i;
    }
    nlohmann::json dumpJson(const std::string& s)
    {
        return s;
    }
    nlohmann::json dumpJson(const char* s)
    {
        return s;
    }
    nlohmann::json dumpJson(const GamePlayerInfo& p)
    {
        return nlohmann::json{
            {"type", dumpJson(p.type)},
            {"color", dumpJson(p.color)},
            {"status", dumpJson(p.status)},
            {"side", dumpJson(p.side)},
            {"metal", dumpJson(p.metal)},
            {"maxMetal", dumpJson(p.maxMetal)},
            {"energy", dumpJson(p.energy)},
            {"maxEnergy", dumpJson(p.maxEnergy)},
            {"metalStalled", dumpJson(p.metalStalled)},
            {"energyStalled", dumpJson(p.energyStalled)},
            {"desiredMetalConsumptionBuffer", dumpJson(p.desiredMetalConsumptionBuffer)},
            {"desiredEnergyConsumptionBuffer", dumpJson(p.desiredEnergyConsumptionBuffer)},
            {"previousDesiredMetalConsumptionBuffer", dumpJson(p.previousDesiredMetalConsumptionBuffer)},
            {"previousDesiredEnergyConsumptionBuffer", dumpJson(p.previousDesiredEnergyConsumptionBuffer)},
            {"actualMetalConsumptionBuffer", dumpJson(p.actualMetalConsumptionBuffer)},
            {"actualEnergyConsumptionBuffer", dumpJson(p.actualEnergyConsumptionBuffer)},
            {"metalProductionBuffer", dumpJson(p.metalProductionBuffer)},
            {"energyProductionBuffer", dumpJson(p.energyProductionBuffer)}};
    }
    nlohmann::json dumpJson(const UnitState& u)
    {
        return nlohmann::json{
            {"unitType", dumpJson(u.unitType)},
            {"position", dumpJson(u.position)},
            {"owner", dumpJson(u.owner)},
            {"rotation", dumpJson(u.rotation)},
            {"physics", dumpJson(u.physics)},
            {"hitPoints", dumpJson(u.hitPoints)},
            {"kills", dumpJson(static_cast<unsigned int>(u.kills))},
            {"lifeState", dumpJson(u.lifeState)},
            {"navigationState", dumpJson(u.navigationState)},
            {"behaviourState", dumpJson(u.behaviourState)},
            {"inBuildStance", dumpJson(u.inBuildStance)},
            {"yardOpen", dumpJson(u.yardOpen)},
            {"inCollision", dumpJson(u.inCollision)},
            {"fireOrders", dumpJson(u.fireOrders)},
            {"moveOrders", dumpJson(u.moveOrders)},
            {"orders", dumpJson(u.orders)},
            {"lastDamagedAt", dumpJson(u.lastDamagedAt)},
            {"lastDamagedBy", dumpJson(u.lastDamagedBy)},
            {"missionState", dumpJson(u.missionState)},
            {"buildTimeCompleted", dumpJson(u.buildTimeCompleted)},
            {"activated", dumpJson(u.activated)},
            {"isSufficientlyPowered", dumpJson(u.isSufficientlyPowered)},
            {"energyProductionBuffer", dumpJson(u.energyProductionBuffer)},
            {"metalProductionBuffer", dumpJson(u.metalProductionBuffer)},
            {"previousEnergyConsumptionBuffer", dumpJson(u.previousEnergyConsumptionBuffer)},
            {"previousMetalConsumptionBuffer", dumpJson(u.previousMetalConsumptionBuffer)},
            {"energyConsumptionBuffer", dumpJson(u.energyConsumptionBuffer)},
            {"metalConsumptionBuffer", dumpJson(u.metalConsumptionBuffer)},
            {"weapons", dumpJson(u.weapons)},
            {"backgroundOrders", dumpJson(u.backgroundOrders)},
            {"carriedBy", dumpJson(u.carriedBy)},
            {"attachSlot", dumpJson(u.attachSlot)},
            {"passengers", dumpJson(u.passengers)},
            {"nextKamikazeScan", dumpJson(u.nextKamikazeScan)},
            {"nextServiceScan", dumpJson(u.nextServiceScan)},
            {"aircraftService", u.aircraftService ? nlohmann::json{
                {"facility", u.aircraftService->facility.value}, {"piece", u.aircraftService->piece},
                {"phase", static_cast<unsigned int>(u.aircraftService->phase)},
                {"bearing", u.aircraftService->loiterBearing.value}, {"goal", dumpJson(u.aircraftService->goal)},
                {"automatic", u.aircraftService->automatic}} : nlohmann::json(nullptr)},
            {"bomberState", u.bomberState ? nlohmann::json{
                {"target", dumpJson(u.bomberState->target)},
                {"phase", static_cast<unsigned int>(u.bomberState->phase)},
                {"goal", dumpJson(u.bomberState->goal)},
                {"releaseTarget", dumpJson(u.bomberState->releaseTarget)},
                {"nextScan", dumpJson(u.bomberState->nextScan)}} : nlohmann::json(nullptr)},
            {"cloakRequested", dumpJson(u.cloakRequested)},
            {"cloaked", dumpJson(u.cloaked)},
            {"cloakBlockedUntil", dumpJson(u.cloakBlockedUntil)}};
    }

    nlohmann::json dumpJson(const UnitWeapon& w)
    {
        return nlohmann::json{
            {"weaponType", dumpJson(w.weaponType)},
            {"readyTime", dumpJson(w.readyTime)},
            {"stockpileCount", dumpJson(w.stockpileCount)}};
    }

    nlohmann::json dumpJson(const UnitPhysicsInfoGround& p)
    {
        return nlohmann::json{
            {"steeringInfo", dumpJson(p.steeringInfo)},
            {"currentSpeed", dumpJson(p.currentSpeed)},
        };
    }

    nlohmann::json dumpJson(const UnitPhysicsInfoAir& p)
    {
        return nlohmann::json{
            {"movementState", dumpJson(p.movementState)},
        };
    }

    nlohmann::json dumpJson(const AirMovementStateTakingOff& p)
    {
        return nlohmann::json{};
    }

    nlohmann::json dumpJson(const AirMovementStateLanding& p)
    {
        return nlohmann::json{};
    }

    nlohmann::json dumpJson(const AirMovementStateFlying& p)
    {
        return nlohmann::json{};
    }

    nlohmann::json dumpJson(const SteeringInfo& s)
    {
        return nlohmann::json{
            {"targetAngle", dumpJson(s.targetAngle)},
            {"targetSpeed", dumpJson(s.targetSpeed)},
        };
    }

    nlohmann::json dumpJson(const Vector3f& v)
    {
        return nlohmann::json{
            {"x", v.x},
            {"y", v.y},
            {"z", v.z},
        };
    }
    nlohmann::json dumpJson(const Projectile& projectile)
    {
        nlohmann::json j{
            {"owner", dumpJson(projectile.owner)},
            {"position", dumpJson(projectile.position)},
            {"origin", dumpJson(projectile.origin)},
            {"velocity", dumpJson(projectile.velocity)},
            {"damageRadius", dumpJson(projectile.damageRadius)}};

        for (const auto& [t, damage] : projectile.damage)
        {
            j["damage"][t] = dumpJson(damage);
        }

        return j;
    }
    nlohmann::json dumpJson(const UnitBehaviorStateIdle&)
    {
        return nlohmann::json();
    }
    nlohmann::json dumpJson(const UnitBehaviorStateBuilding&)
    {
        return nlohmann::json();
    }
    nlohmann::json dumpJson(const UnitBehaviorStateRepairing& s)
    {
        return nlohmann::json{
            {"targetUnit", dumpJson(s.targetUnit)},
            {"repairProgress", dumpJson(s.repairProgress)},
        };
    }
    nlohmann::json dumpJson(const UnitBehaviorStateReclaiming& s)
    {
        return nlohmann::json{
            {"target", dumpJson(s.target)},
            {"targetPosition", dumpJson(s.targetPosition)},
        };
    }
    nlohmann::json dumpJson(const UnitBehaviorStateCapturing& s)
    {
        return nlohmann::json{
            {"target", dumpJson(s.target)},
            {"targetPosition", dumpJson(s.targetPosition)},
        };
    }
    nlohmann::json dumpJson(const MapFeature& f)
    {
        return nlohmann::json{
            {"featureName", dumpJson(f.featureName)},
            {"position", dumpJson(f.position)},
            {"rotation", dumpJson(f.rotation)},
            {"reclaimWorkDone", dumpJson(f.reclaimWorkDone)},
            {"reclaimMetalGiven", dumpJson(f.reclaimMetalGiven)},
            {"reclaimEnergyGiven", dumpJson(f.reclaimEnergyGiven)},
            {"damageAccumulated", f.damageAccumulated},
            {"event", static_cast<unsigned int>(f.event)},
            {"animationFrame", f.animationFrame}, {"animationDelay", f.animationDelay},
            {"shadowFrame", f.shadowFrame}, {"shadowDelay", f.shadowDelay},
            {"sparkCountdown", f.sparkCountdown},
        };
    }
    nlohmann::json dumpJson(const UnitBehaviorStateCreatingUnit& s)
    {
        return nlohmann::json{
            {"unitType", dumpJson(s.unitType)},
            {"owner", dumpJson(s.owner)},
            {"position", dumpJson(s.position)},
        };
    }
    nlohmann::json dumpJson(const UnitCreationStatusPending&)
    {
        return nlohmann::json();
    }
    nlohmann::json dumpJson(const UnitCreationStatusDone& s)
    {
        return nlohmann::json{{"unitId", dumpJson(s.unitId)}};
    }
    nlohmann::json dumpJson(const UnitCreationStatusFailed&)
    {
        return nlohmann::json();
    }

    nlohmann::json dumpJson(const UnitState::LifeStateAlive&)
    {
        return nlohmann::json();
    }
    nlohmann::json dumpJson(const UnitState::LifeStateDead&)
    {
        return nlohmann::json();
    }

    nlohmann::json dumpJson(const NavigationGoalLandingLocation&)
    {
        return nlohmann::json();
    }

    nlohmann::json dumpJson(const NavigationStateIdle& m)
    {
        return nlohmann::json();
    }

    nlohmann::json dumpJson(const NavigationStateMoving& m)
    {
        return nlohmann::json{
            {"movementGoal", dumpJson(m.movementGoal)},
            {"pathDestination", dumpJson(m.pathDestination)},
            {"pathRequested", m.pathRequested},
            {"pathExhausted", m.pathExhausted}};
    }

    nlohmann::json dumpJson(const NavigationStateMovingToLandingSpot& m)
    {
        return nlohmann::json{
            {"landingLocation", dumpJson(m.landingLocation)}};
    }

    nlohmann::json dumpJson(const NavigationStateInfo& m)
    {
        return nlohmann::json{
            {"desiredDestination", dumpJson(m.desiredDestination)},
            {"state", dumpJson(m.state)}};
    }

    nlohmann::json dumpJson(const DiscreteRect& r)
    {
        return nlohmann::json{
            {"x", r.x},
            {"y", r.y},
            {"width", r.width},
            {"height", r.height}};
    }

    nlohmann::json dumpJson(const MoveOrder& o)
    {
        return nlohmann::json{
            {"destination", dumpJson(o.destination)}};
    }

    nlohmann::json dumpJson(const AttackOrder& o)
    {
        return nlohmann::json{{"target", dumpJson(o.target)}};
    }

    nlohmann::json dumpJson(const BuildOrder& o)
    {
        return nlohmann::json{
            {"unitType", dumpJson(o.unitType)},
            {"position", dumpJson(o.position)}};
    }

    nlohmann::json dumpJson(const BuggerOffOrder& o)
    {
        return nlohmann::json{{"rect", dumpJson(o.rect)}};
    }

    nlohmann::json dumpJson(const CompleteBuildOrder& o)
    {
        return nlohmann::json{{"target", dumpJson(o.target)}};
    }

    nlohmann::json dumpJson(const GuardOrder& o)
    {
        return nlohmann::json{{"target", dumpJson(o.target)}};
    }

    nlohmann::json dumpJson(const RepairOrder& o)
    {
        return nlohmann::json{{"target", dumpJson(o.target)}};
    }

    nlohmann::json dumpJson(const ReclaimOrder& o)
    {
        return nlohmann::json{{"target", dumpJson(o.target)}};
    }

    nlohmann::json dumpJson(const PatrolOrder& o)
    {
        return nlohmann::json{
            {"waypoints", dumpJson(o.waypoints)},
            {"nextWaypointIndex", dumpJson(o.nextWaypointIndex)}};
    }

    nlohmann::json dumpJson(const CaptureOrder& o)
    {
        return nlohmann::json{
            {"target", dumpJson(o.target)},
            {"progress", dumpJson(o.progress)},
            {"workRequired", dumpJson(o.workRequired)},
            {"workTicks", dumpJson(o.workTicks)}};
    }

    nlohmann::json dumpJson(const WaitOrder& o)
    {
        return nlohmann::json{{"ticksRemaining", dumpJson(o.ticksRemaining)}};
    }

    nlohmann::json dumpJson(const WaitForAttackOrder& o)
    {
        return nlohmann::json{
            {"target", dumpJson(o.target)},
            {"armed", dumpJson(o.armed)},
            {"observedDamagedAt", dumpJson(o.observedDamagedAt)}};
    }

    nlohmann::json dumpJson(const MakeSelectableOrder&)
    {
        return nlohmann::json{};
    }

    nlohmann::json dumpJson(const AttackUnitTypeOrder& o)
    {
        return nlohmann::json{
            {"unitType", dumpJson(o.unitType)},
            {"nextScanTime", dumpJson(o.nextScanTime)},
            {"currentTarget", dumpJson(o.currentTarget)}};
    }

    nlohmann::json dumpJson(const MissionBuildOrder& o)
    {
        return nlohmann::json{
            {"unitType", dumpJson(o.unitType)},
            {"position", dumpJson(o.position)},
            {"attemptsRemaining", dumpJson(o.attemptsRemaining)},
            {"retryAt", dumpJson(o.retryAt)}};
    }

    nlohmann::json dumpJson(const MissionFactoryBuildOrder& o)
    {
        return nlohmann::json{
            {"unitType", dumpJson(o.unitType)},
            {"count", dumpJson(o.count)},
            {"enqueued", dumpJson(o.enqueued)}};
    }

    nlohmann::json dumpJson(const SelfDestructOrder& o)
    {
        return nlohmann::json{
            {"countdownTicks", dumpJson(o.countdownTicks)},
            {"fuseTicks", dumpJson(o.fuseTicks)}};
    }

    nlohmann::json dumpJson(const UnloadOrder& o)
    {
        return nlohmann::json{
            {"destination", dumpJson(o.destination)},
            {"pops", dumpJson(o.pops)},
            {"claimActive", dumpJson(o.claimActive)},
            {"cycleTicks", dumpJson(o.cycleTicks)},
            {"dropRadius", dumpJson(o.dropRadius)}};
    }

    nlohmann::json dumpJson(const LandOrder& o) { return {{"type", "Land"}, {"target", o.target.value}}; }

    nlohmann::json dumpJson(const ParalyzeOrder& o)
    {
        return nlohmann::json{{"credit", o.credit}, {"resumeAt", dumpJson(o.resumeAt)}};
    }

    nlohmann::json dumpJson(const LoadOrder& o)
    {
        return nlohmann::json{
            {"target", dumpJson(o.target)},
            {"attempts", dumpJson(o.attempts)},
            {"waitTicks", dumpJson(o.waitTicks)},
            {"pickupThreadStarted", dumpJson(o.pickupThreadStarted)},
            {"phase", dumpJson(o.phase)},
            {"slot", dumpJson(o.slot)}};
    }

    nlohmann::json dumpJson(const BuildWeaponOrder& o)
    {
        return nlohmann::json{
            {"remaining", dumpJson(o.remaining)},
            {"progressTicks", dumpJson(o.progressTicks)}};
    }

    nlohmann::json dumpJson(const CommandFireOrder& o)
    {
        return nlohmann::json{
            {"target", dumpJson(o.target)},
            {"observedReadyTime", dumpJson(o.observedReadyTime)}};
    }

    nlohmann::json dumpJson(const MissionUnitState& s)
    {
        return nlohmann::json{
            {"initialMission", dumpJson(s.initialMission)},
            {"ident", dumpJson(s.ident)},
            {"selectable", dumpJson(s.selectable)}};
    }

    nlohmann::json dumpJson(const GameSimulation& simulation)
    {
        return nlohmann::json{
            {"gameTime", dumpJson(simulation.gameTime)},
            {"mapGravity", simulation.mapGravity},
            {"projectileWind", dumpJson(simulation.projectileWind)},
            {"players", dumpJson(simulation.players)},
            {"units", dumpJson(simulation.units)},
            {"projectiles", dumpJson(simulation.projectiles)},
            {"features", dumpJson(simulation.features)},
        };
    }
}
