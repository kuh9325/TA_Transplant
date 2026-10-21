#include "io.h"
#include <rwe/util/OpaqueId_io.h>

namespace rwe
{
    UnitFbi parseUnitFbi(const TdfBlock& tdf)
    {
        auto infoBlock = tdf.findBlock("UNITINFO");
        if (!infoBlock)
        {
            throw std::runtime_error("FBI missing UNITINFO block");
        }

        return parseUnitInfoBlock(*infoBlock);
    }

    UnitFbi parseUnitInfoBlock(const TdfBlock& tdf)
    {
        UnitFbi u;

        tdf.read("UnitName", u.unitName);
        tdf.read("Objectname", u.objectName);
        tdf.read("SoundCategory", u.soundCategory);

        tdf.readOrDefault("MovementClass", u.movementClass);

        tdf.readOrDefault("Name", u.name);
        tdf.readOrDefault("Description", u.description);

        tdf.readOrDefault("TurnRate", u.turnRate);
        tdf.readOrDefault("MaxVelocity", u.maxVelocity);
        tdf.readOrDefault("Acceleration", u.acceleration);
        tdf.readOrDefault("BrakeRate", u.brakeRate);

        tdf.readOrDefault("FootprintX", u.footprintX);
        tdf.readOrDefault("FootprintZ", u.footprintZ);

        tdf.readOrDefault("MaxSlope", u.maxSlope, 255u);
        tdf.readOrDefault("MaxWaterSlope", u.maxWaterSlope, 255u);
        tdf.readOrDefault("MinWaterDepth", u.minWaterDepth);
        tdf.readOrDefault("MaxWaterDepth", u.maxWaterDepth, 255u);
        tdf.readOrDefault("Waterline", u.waterline);

        tdf.readOrDefault("CanAttack", u.canAttack);
        tdf.readOrDefault("CanMove", u.canMove);
        tdf.readOrDefault("CanGuard", u.canGuard);
        tdf.readOrDefault("CanPatrol", u.canPatrol);
        tdf.readOrDefault("CanStop", u.canStop);
        tdf.readOrDefault("CanReclamate", u.canReclaim);
        tdf.readOrDefault("CanDGun", u.canDGun);

        tdf.readOrDefault("StandingFireOrder", u.standingFireOrder, 2);
        tdf.readOrDefault("StandingMoveOrder", u.standingMoveOrder, 1);

        tdf.readOrDefault("Commander", u.commander);

        tdf.readOrDefault("MaxDamage", u.maxDamage);

        tdf.readOrDefault("BMCode", u.bmCode);

        tdf.readOrDefault("Floater", u.floater);
        tdf.readOrDefault("CanHover", u.canHover);

        tdf.readOrDefault("CanFly", u.canFly);
        tdf.readOrDefault("IsAirBase", u.isAirBase);
        tdf.readOrDefault("AttackRunLength", u.attackRunLength);

        tdf.readOrDefault("CruiseAlt", u.cruiseAlt);

        tdf.readOrDefault("Weapon1", u.weapon1);
        tdf.readOrDefault("Weapon2", u.weapon2);
        tdf.readOrDefault("Weapon3", u.weapon3);

        tdf.readOrDefault("ExplodeAs", u.explodeAs);
        tdf.readOrDefault("SelfDestructAs", u.selfDestructAs);
        tdf.readOrDefault("Kamikaze", u.kamikaze);
        tdf.readOrDefault("KamikazeDistance", u.kamikazeDistance);
        tdf.readOrDefault("DefaultMissionType", u.defaultMissionType);
        tdf.readOrDefault("ShootMe", u.shootMe);
        tdf.readOrDefault("NoChaseCategory", u.noChaseCategory);

        tdf.readOrDefault("Builder", u.builder);

        tdf.readOrDefault("CanCapture", u.canCapture);

        tdf.readOrDefault("BuildTime", u.buildTime);
        tdf.readOrDefault("BuildCostEnergy", u.buildCostEnergy);
        tdf.readOrDefault("BuildCostMetal", u.buildCostMetal);

        tdf.readOrDefault("WorkerTime", u.workerTime);
        tdf.readOrDefault("Builddistance", u.buildDistance);

        tdf.readOrDefault("onoffable", u.onOffable);
        tdf.readOrDefault("ActivateWhenBuilt", u.activateWhenBuilt);

        tdf.readOrDefault("EnergyMake", u.energyMake);
        tdf.readOrDefault("MetalMake", u.metalMake);
        tdf.readOrDefault("EnergyUse", u.energyUse);
        tdf.readOrDefault("MetalUse", u.metalUse);
        tdf.readOrDefault("ExtractsMetal", u.extractsMetal);
        tdf.readOrDefault("EnergyStorage", u.energyStorage);
        tdf.readOrDefault("MetalStorage", u.metalStorage);
        tdf.readOrDefault("WindGenerator", u.windGenerator);
        tdf.readOrDefault("TidalGenerator", u.tidalGenerator);

        tdf.readOrDefault("MakesMetal", u.makesMetal);

        tdf.readOrDefault("TransportCapacity", u.transportCapacity);
        tdf.readOrDefault("CanLoad", u.canLoad);
        tdf.readOrDefault("TransportSize", u.transportSize);
        tdf.readOrDefault("CantBeTransported", u.cantBeTransported);
        tdf.readOrDefault("SelfDestructCountdown", u.selfDestructCountdown, 5u);

        tdf.readOrDefault("CloakCost", u.cloakCost);
        tdf.readOrDefault("CloakCostMoving", u.cloakCostMoving);

        tdf.readOrDefault("DamageModifier", u.damageModifier, 1.0f);
        tdf.readOrDefault("mincloakdistance", u.minCloakDistance);
        tdf.readOrDefault("init_cloaked", u.initCloaked);

        tdf.readOrDefault("HideDamage", u.hideDamage, false);
        tdf.readOrDefault("ShowPlayerName", u.showPlayerName, false);

        tdf.readOrDefault("SightDistance", u.sightDistance);
        tdf.readOrDefault("RadarDistance", u.radarDistance);
        tdf.readOrDefault("SonarDistance", u.sonarDistance);
        tdf.readOrDefault("RadarDistanceJam", u.radarDistanceJam);
        tdf.readOrDefault("SonarDistanceJam", u.sonarDistanceJam);
        tdf.readOrDefault("Stealth", u.stealth);
        tdf.readOrDefault("ImmuneToParalyzer", u.immuneToParalyzer);
        tdf.readOrDefault("antiweapons", u.antiWeapons);
        tdf.readOrDefault("istargetingupgrade", u.isTargetingUpgrade);

        tdf.readOrDefault("Category", u.category);
        tdf.readOrDefault("ai_limit", u.aiLimit);
        tdf.readOrDefault("ai_weight", u.aiWeight);

        tdf.readOrDefault("YardMap", u.yardMap);

        tdf.readOrDefault("Corpse", u.corpse);

        return u;
    }
}
