#include "ota.h"

#include <rwe/io/tdf/tdf.h>

namespace rwe
{
    OtaParseException::OtaParseException(const std::string& __arg) : runtime_error(__arg)
    {
    }

    OtaParseException::OtaParseException(const char* string) : runtime_error(string)
    {
    }

    OtaRecord parseOta(const TdfBlock& tdf)
    {
        auto block = tdf.findBlock("GlobalHeader");
        if (!block)
        {
            throw OtaParseException("Could not find GlobalHeader block");
        }

        return parseOtaGlobalHeader(*block);
    }

    OtaRecord parseOtaGlobalHeader(const TdfBlock& tdf)
    {
        OtaRecord r;

        tdf.read("missionname", r.missionName);

        tdf.readOrDefault("missiondescription", r.missionDescription);
        tdf.readOrDefault("planet", r.planet);
        tdf.readOrDefault("missionhint", r.missionHint);
        tdf.readOrDefault("brief", r.brief);
        tdf.readOrDefault("narration", r.narration);
        tdf.readOrDefault("glamour", r.glamour);
        tdf.readOrDefault("lineofsight", r.lineOfSight);
        tdf.readOrDefault("mapping", r.mapping);
        tdf.readOrDefault("tidalstrength", r.tidalStrength);
        tdf.readOrDefault("solarstrength", r.solarStrength);
        tdf.readOrDefault("lavaworld", r.lavaWorld);
        tdf.readOrDefault("killmul", r.killMul, 50);
        tdf.readOrDefault("timemul", r.timeMul);
        tdf.readOrDefault("minwindspeed", r.minWindSpeed);
        tdf.readOrDefault("maxwindspeed", r.maxWindSpeed);
        tdf.readOrDefault("gravity", r.gravity, 112);
        tdf.readOrDefault("nosealeveltrigger", r.noSeaLevelTrigger);
        tdf.readOrDefault("waterdoesdamage", r.waterDoesDamage);
        tdf.readOrDefault("waterdamage", r.waterDamage);
        tdf.readOrDefault("numplayers", r.numPlayers);
        tdf.readOrDefault("size", r.size);
        tdf.readOrDefault("memory", r.memory);
        tdf.readOrDefault("useonlyunits", r.useOnlyUnits);
        // retail UnitLimit handling: default 250, clamped [20,500]
        // (TotalA.exe 0x491644 + option clamp)
        tdf.readOrDefault("maxunits", r.maxUnits, 250);
        r.maxUnits = std::clamp(r.maxUnits, 20, 500);

        // Mission objectives. Campaign OTAs declare victory/defeat
        // conditions as GlobalHeader keys; we retain them verbatim.
        // The key set is fixed by retail: each key name maps to a
        // VictoryCondition_* or DefeatCondition_* implementation.
        for (const auto& key : {
                 // victory-side keys
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
                 // defeat-side keys
                 "CommanderKilled",
                 "AllUnitsKilled",
                 "AllUnitsKilledOfType",
                 "UnitTypeKilled",
                 "DeathTimerRunsOut",
                 "AnyUnitPassesX",
                 "AnyUnitPassesZ",
             })
        {
            if (auto value = tdf.findValue(key))
            {
                OtaMissionObjective objective;
                objective.type = key;
                for (auto param : utf8Split(*value, ','))
                {
                    utf8Trim(param);
                    objective.params.push_back(std::move(param));
                }
                r.objectives.push_back(std::move(objective));
            }
        }

        r.schemaCount = tdf.expectInt("SCHEMACOUNT");

        for (int i = 0; i < r.schemaCount; ++i)
        {
            auto schemaName = std::string("Schema ") + std::to_string(i);
            auto schemaBlock = tdf.findBlock(schemaName);
            if (!schemaBlock)
            {
                throw OtaParseException("Missing block: " + schemaName);
            }

            r.schemas.push_back(parseOtaSchema(*schemaBlock));
        }

        return r;
    }

    OtaSchema parseOtaSchema(const TdfBlock& tdf)
    {
        OtaSchema s;
        tdf.read("Type", s.type);
        tdf.readOrDefault("aiprofile", s.aiProfile, std::string("DEFAULT"));
        tdf.readOrDefault("SurfaceMetal", s.surfaceMetal, 3);
        tdf.readOrDefault("MohoMetal", s.mohoMetal, 30);
        tdf.readOrDefault("HumanMetal", s.humanMetal, 1000);
        tdf.readOrDefault("ComputerMetal", s.computerMetal, 1000);
        tdf.readOrDefault("HumanEnergy", s.humanEnergy, 1000);
        tdf.readOrDefault("ComputerEnergy", s.computerEnergy, 1000);
        tdf.readOrDefault("MeteorWeapon", s.meteorWeapon);
        tdf.readOrDefault("MeteorRadius", s.meteorRadius);
        tdf.readOrDefault("MeteorDensity", s.meteorDensity);
        tdf.readOrDefault("MeteorDuration", s.meteorDuration);
        tdf.readOrDefault("MeteorInterval", s.meteorInterval);

        auto featuresBlockOption = tdf.findBlock("features");
        if (featuresBlockOption)
        {
            auto featuresBlock = &featuresBlockOption->get();
            int i = 0;
            auto block = featuresBlock->findBlock("feature" + std::to_string(i));
            while (block)
            {
                s.features.push_back(parseOtaFeature(*block));

                i += 1;
                block = featuresBlock->findBlock("feature" + std::to_string(i));
            }
        }

        auto specialsBlockOption = tdf.findBlock("specials");
        if (specialsBlockOption)
        {
            auto specialsBlock = &specialsBlockOption->get();
            int i = 0;
            auto block = specialsBlock->findBlock("special" + std::to_string(i));
            while (block)
            {
                s.specials.push_back(parseOtaSpecial(*block));

                i += 1;
                block = specialsBlock->findBlock("special" + std::to_string(i));
            }
        }

        auto unitsBlockOption = tdf.findBlock("units");
        if (unitsBlockOption)
        {
            auto unitsBlock = &unitsBlockOption->get();
            unsigned int i = 0;
            auto block = unitsBlock->findBlock("unit" + std::to_string(i));
            while (block)
            {
                s.units.push_back(parseOtaUnit(*block, i));

                i += 1;
                block = unitsBlock->findBlock("unit" + std::to_string(i));
            }
        }

        return s;
    }

    OtaUnit parseOtaUnit(const TdfBlock& tdf, unsigned int index)
    {
        OtaUnit u;
        u.index = index;
        tdf.read("Unitname", u.unitName);
        u.player = tdf.expectUint("Player");
        tdf.read("XPos", u.xPos);
        tdf.readOrDefault("YPos", u.yPos);
        tdf.read("ZPos", u.zPos);
        u.healthPercentage = tdf.extractUint("HealthPercentage");
        u.angle = tdf.extractUint("Angle");
        u.kills = tdf.extractUint("Kills");
        u.creationCountdown = tdf.extractUint("CreationCountdown");
        u.initialGroup = tdf.extractUint("InitialGroup");
        u.immunity = tdf.extractBool("Immunity").value_or(false);
        u.aiIgnore = tdf.extractBool("AiIgnore").value_or(false);

        // empty InitialMission entries are common in stock data;
        // retain whatever string is present verbatim
        u.initialMission = tdf.extract<std::string>("InitialMission").value_or("");

        u.ident = tdf.extract<std::string>("Ident");

        return u;
    }

    OtaFeature parseOtaFeature(const TdfBlock& tdf)
    {
        OtaFeature f;
        tdf.read("Featurename", f.featureName);
        tdf.read("XPos", f.xPos);
        tdf.read("ZPos", f.zPos);
        return f;
    }

    OtaSpecial parseOtaSpecial(const TdfBlock& tdf)
    {
        OtaSpecial s;
        tdf.read("specialwhat", s.specialWhat);
        tdf.read("XPos", s.xPos);
        tdf.read("ZPos", s.zPos);
        return s;
    }
}
