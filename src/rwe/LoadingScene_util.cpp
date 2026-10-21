#include "LoadingScene_util.h"
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <sstream>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/gaf/GafArchive.h>

#include <rwe/util/Index.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/match.h>
#include <rwe/sim/CampaignAi.h>
#include <rwe/sim/MissionInitialCommand.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitState.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    MovementClassCollisionService createMovementClassCollisionService(const MapTerrain& terrain, const MovementClassDatabase& movementClassDatabase)
    {
        MovementClassCollisionService service;
        for (const auto& pair : movementClassDatabase)
        {
            service.registerMovementClass(pair.first, computeWalkableGrid(terrain, pair.second));
        }
        return service;
    }


    std::unordered_map<std::string, CobScript> loadCobScripts(AbstractVirtualFileSystem& vfs)
    {
        std::unordered_map<std::string, CobScript> output;

        auto scripts = vfs.getFileNames("scripts", ".cob");

        for (const auto& scriptName : scripts)
        {
            auto bytes = vfs.readFile("scripts/" + scriptName);
            if (!bytes)
            {
                throw std::runtime_error("File in listing could not be read: " + scriptName);
            }

            rwe::SpanStream s(bytes->data(), bytes->size());
            auto cob = parseCob(s);

            auto scriptNameWithoutExtension = scriptName.substr(0, scriptName.size() - 4);

            output.insert({toUpper(scriptNameWithoutExtension), std::move(cob)});
        }

        return output;
    }

    std::vector<std::string> getFeatureNames(TntArchive& tnt)
    {
        std::vector<std::string> features;

        tnt.readFeatures([&](const auto& featureName) {
            features.push_back(featureName);
        });

        return features;
    }

    Grid<std::size_t> getMapData(TntArchive& tnt)
    {
        auto mapWidthInTiles = tnt.getHeader().width / 2;
        auto mapHeightInTiles = tnt.getHeader().height / 2;
        std::vector<uint16_t> mapData(mapWidthInTiles * mapHeightInTiles);
        tnt.readMapData(mapData.data());
        std::vector<std::size_t> dataCopy;
        dataCopy.reserve(mapData.size());
        std::copy(mapData.begin(), mapData.end(), std::back_inserter(dataCopy));
        Grid<std::size_t> dataGrid(mapWidthInTiles, mapHeightInTiles, std::move(dataCopy));
        return dataGrid;
    }

    Grid<unsigned char> getHeightGrid(const Grid<TntTileAttributes>& attrs)
    {
        const auto& sourceData = attrs.getVector();

        std::vector<unsigned char> data;
        data.reserve(sourceData.size());

        std::transform(sourceData.begin(), sourceData.end(), std::back_inserter(data), [](const TntTileAttributes& e) {
            return e.height;
        });

        return Grid<unsigned char>(attrs.getWidth(), attrs.getHeight(), std::move(data));
    }

    Vector3f colorToVector(const Color& color)
    {
        return Vector3f(
            static_cast<float>(color.r) / 255.0f,
            static_cast<float>(color.g) / 255.0f,
            static_cast<float>(color.b) / 255.0f);
    }

    unsigned int colorDistance(const Color& a, const Color& b)
    {
        auto dr = a.r > b.r ? a.r - b.r : b.r - a.r;
        auto dg = a.g > b.g ? a.g - b.g : b.g - a.g;
        auto db = a.b > b.b ? a.b - b.b : b.b - a.b;
        return dr + dg + db;
    }

    Vector3f getLaserColorUtil(const std::vector<Color>& palette, const std::vector<Color>& guiPalette, unsigned int colorIndex)
    {
        // In TA, lasers use the GUIPAL colors,
        // but these must be mapped to a color available
        // in the in-game PALETTE.
        const auto& guiColor = guiPalette.at(colorIndex);

        auto elem = std::min_element(
            palette.begin(),
            palette.end(),
            [&guiColor](const auto& a, const auto& b) { return colorDistance(guiColor, a) < colorDistance(guiColor, b); });

        return colorToVector(*elem);
    }

    std::optional<std::string> getFxName(unsigned int code)
    {
        switch (code)
        {
            case 0:
                return "cannonshell";
            case 1:
                return "plasmasm";
            case 2:
                return "plasmamd";
            case 3:
                return "ultrashell";
            case 4:
                return "plasmasm";
            default:
                return std::nullopt;
        }
    }

    MovementClassDefinition parseMovementClassDefinition(const MovementClassTdf& tdf)
    {
        MovementClassDefinition mc;

        mc.name = tdf.name;
        mc.footprintX = tdf.footprintX;
        mc.footprintZ = tdf.footprintZ;
        mc.minWaterDepth = tdf.minWaterDepth;
        mc.maxWaterDepth = tdf.maxWaterDepth;
        mc.maxSlope = tdf.maxSlope;
        mc.maxWaterSlope = tdf.maxWaterSlope;

        return mc;
    }

    WeaponDefinition parseWeaponDefinition(const WeaponTdf& tdf)
    {
        WeaponDefinition weaponDefinition;

        weaponDefinition.maxRange = SimScalar(tdf.range);
        weaponDefinition.reloadTime = SimScalar(tdf.reloadTime);
        weaponDefinition.tolerance = SimAngle(tdf.tolerance);
        weaponDefinition.pitchTolerance = SimAngle(tdf.pitchTolerance);
        weaponDefinition.velocity = SimScalar(static_cast<float>(tdf.weaponVelocity) / 30.0f);

        weaponDefinition.burst = tdf.burst;
        weaponDefinition.burstInterval = SimScalar(tdf.burstRate);
        weaponDefinition.sprayAngle = SimAngle(tdf.sprayAngle);
        weaponDefinition.turret = tdf.turret;
        weaponDefinition.accuracy = static_cast<uint16_t>(tdf.accuracy);

        if (tdf.tracks)
        {
            weaponDefinition.physicsType = ProjectilePhysicsTypeTracking{SimScalar(tdf.turnRate)};
        }
        else if (tdf.lineOfSight)
        {
            weaponDefinition.physicsType = ProjectilePhysicsTypeLineOfSight();
        }
        else if (tdf.ballistic)
        {
            weaponDefinition.physicsType = ProjectilePhysicsTypeBallistic();
        }
        else if (tdf.dropped)
        {
            weaponDefinition.physicsType = ProjectilePhysicsTypeDropped();
        }

        weaponDefinition.commandFire = tdf.commandFire;
        weaponDefinition.paralyzer = tdf.paralyzer;
        weaponDefinition.unitsOnly = tdf.unitsOnly;
        weaponDefinition.fireStarter = tdf.fireStarter != 0;
        weaponDefinition.noExplode = tdf.noExplode;

        for (const auto& p : tdf.damage)
        {
            weaponDefinition.damage.insert_or_assign(toUpper(p.first), p.second);
        }

        weaponDefinition.damageRadius = SimScalar(static_cast<float>(tdf.areaOfEffect) / 2.0f);

        if (tdf.weaponTimer != 0.0f)
        {
            weaponDefinition.weaponTimer = GameTime(static_cast<unsigned int>(tdf.weaponTimer * 30.0f));
        }

        weaponDefinition.name = tdf.name;

        weaponDefinition.groundBounce = tdf.groundBounce;
        weaponDefinition.waterWeapon = tdf.waterWeapon;

        weaponDefinition.guidance = tdf.guidance;
        weaponDefinition.turnRate = SimScalar(tdf.turnRate);
        weaponDefinition.vLaunch = tdf.vLaunch;
        weaponDefinition.cruise = tdf.cruise;
        // TDF `flighttime` is already a frame count
        weaponDefinition.flightTime = GameTime(static_cast<unsigned int>(tdf.flightTime));
        weaponDefinition.acceleration = SimScalar(static_cast<float>(tdf.weaponAcceleration) / 30.0f);

        weaponDefinition.randomDecay = GameTime(static_cast<unsigned int>(tdf.randomDecay * 30.0f));

        weaponDefinition.stockpile = tdf.stockpile;
        weaponDefinition.interceptor = tdf.interceptor;
        weaponDefinition.coverage = SimScalar(tdf.coverage);
        weaponDefinition.targetable = tdf.targetable;
        weaponDefinition.isMeteor = tdf.meteor;
        weaponDefinition.energyPerShot = Energy(static_cast<float>(tdf.energyPerShot));
        weaponDefinition.metalPerShot = Metal(static_cast<float>(tdf.metalPerShot));

        return weaponDefinition;
    }

    std::optional<YardMapCell> parseYardMapCell(char c)
    {
        switch (c)
        {
            case 'c':
                return YardMapCell::GroundGeoPassableWhenOpen;
            case 'C':
                return YardMapCell::WaterPassableWhenOpen;
            case 'f':
                return YardMapCell::GroundNoFeature;
            case 'g':
                // retail's yardmap jump table has no 'g' arm:
                // it is skipped without consuming a cell
                return std::nullopt;
            case 'G':
                return YardMapCell::Geo;
            case 'o':
                return YardMapCell::Ground;
            case 'O':
                return YardMapCell::GroundPassableWhenClosed;
            case 'w':
                return YardMapCell::Water;
            case 'y':
                return YardMapCell::GroundPassable;
            case 'Y':
                return YardMapCell::WaterPassable;
            case '.':
                return YardMapCell::Passable;
            case ' ':
                return std::nullopt;
            case '\r':
                return std::nullopt;
            case '\n':
                return std::nullopt;
            case '\t':
                return std::nullopt;
            default:
                // any character without a retail jump-table arm is
                // skipped without consuming a footprint cell
                return std::nullopt;
        }
    }

    std::vector<YardMapCell> parseYardMapCells(const std::string& yardMap)
    {
        std::vector<YardMapCell> cells;
        for (const auto& c : yardMap)
        {
            auto cell = parseYardMapCell(c);
            if (cell)
            {
                cells.push_back(*cell);
            }
        }
        return cells;
    }

    Grid<YardMapCell> parseYardMap(unsigned int width, unsigned int height, const std::string& yardMap)
    {
        auto cells = parseYardMapCells(yardMap);
        // retail repeats the last parsed cell when the string
        // is shorter than the footprint (e.g. YardMap=w floods it)
        cells.resize(width * height, cells.empty() ? YardMapCell::Ground : cells.back());
        return Grid<YardMapCell>(width, height, std::move(cells));
    }

    UnitDefinition parseUnitDefinition(const UnitFbi& fbi, MovementClassDatabase& movementClassDatabase)
    {
        UnitDefinition u;

        u.unitName = fbi.name;
        u.unitDescription = fbi.description;
        u.objectName = fbi.objectName;

        u.turnRate = SimScalar(fbi.turnRate);
        u.maxVelocity = SimScalar(fbi.maxVelocity);
        u.acceleration = SimScalar(fbi.acceleration);
        u.brakeRate = SimScalar(fbi.brakeRate);

        u.canAttack = fbi.canAttack;
        u.canMove = fbi.canMove;
        u.canGuard = fbi.canGuard;
        u.canPatrol = fbi.canPatrol;
        u.canStop = fbi.canStop;
        u.canReclaim = fbi.canReclaim;
        u.canDGun = fbi.canDGun;
        u.standingFireOrder = fbi.standingFireOrder == 0
            ? UnitFireOrders::HoldFire
            : (fbi.standingFireOrder == 1 ? UnitFireOrders::ReturnFire : UnitFireOrders::FireAtWill);
        u.standingMoveOrder = fbi.standingMoveOrder == 0
            ? UnitMoveOrders::HoldPosition
            : (fbi.standingMoveOrder == 1 ? UnitMoveOrders::Maneuver : UnitMoveOrders::Roam);

        u.commander = fbi.commander;

        u.maxHitPoints = fbi.maxDamage;

        u.isMobile = fbi.bmCode;

        u.floater = fbi.floater;
        u.canHover = fbi.canHover;
        u.waterline = fbi.waterline;

        u.canFly = fbi.canFly;
        u.attackRunLength = SimScalar(fbi.attackRunLength & 0xffffu);

        u.cruiseAltitude = SimScalar(fbi.cruiseAlt);

        u.weapon1 = fbi.weapon1;
        u.weapon2 = fbi.weapon2;
        u.weapon3 = fbi.weapon3;

        u.explodeAs = fbi.explodeAs;
        u.selfDestructAs = fbi.selfDestructAs;
        u.kamikaze = fbi.kamikaze;
        // Local retail uses an unsigned word compare here (0x403364).
        u.kamikazeDistance = SimScalar(fbi.kamikazeDistance & 0xffffu);
        u.standbyMine = toUpper(fbi.defaultMissionType) == "STANDBY_MINE";
        u.shootMe = fbi.shootMe;
        for (const auto& token : split(fbi.noChaseCategory, {' ', '\t'}))
        {
            if (!token.empty()) { u.noChaseCategories.insert(toUpper(token)); }
        }

        u.builder = fbi.builder;
        u.isAirBase = fbi.isAirBase;
        u.canCapture = fbi.canCapture;
        u.buildTime = fbi.buildTime;
        u.buildCostEnergy = Energy(fbi.buildCostEnergy);
        u.buildCostMetal = Metal(fbi.buildCostMetal);

        u.workerTimePerTick = fbi.workerTime / 30;

        u.buildDistance = SimScalar(fbi.buildDistance);

        u.onOffable = fbi.onOffable;
        u.activateWhenBuilt = fbi.activateWhenBuilt;

        u.energyMake = Energy(fbi.energyMake);
        u.metalMake = Metal(fbi.metalMake);
        u.energyUse = Energy(fbi.energyUse);
        u.metalUse = Metal(fbi.metalUse);

        u.makesMetal = Metal(fbi.makesMetal);
        u.extractsMetal = Metal(fbi.extractsMetal);

        u.energyStorage = Energy(fbi.energyStorage);
        u.metalStorage = Metal(fbi.metalStorage);

        u.windGenerator = Energy(fbi.windGenerator);
        u.tidalGenerator = fbi.tidalGenerator;

        u.corpse = fbi.corpse;

        u.hideDamage = fbi.hideDamage;
        u.showPlayerName = fbi.showPlayerName;

        u.sightDistance = SimScalar(fbi.sightDistance);
        u.radarDistance = SimScalar(fbi.radarDistance);
        u.sonarDistance = SimScalar(fbi.sonarDistance);
        u.radarDistanceJam = SimScalar(fbi.radarDistanceJam);
        u.sonarDistanceJam = SimScalar(fbi.sonarDistanceJam);
        u.stealth = fbi.stealth;
        u.immuneToParalyzer = fbi.immuneToParalyzer;
        u.antiWeapons = fbi.antiWeapons;
        u.isTargetingUpgrade = fbi.isTargetingUpgrade;

        u.soundCategory = fbi.soundCategory;

        // FBI `Category` tags: whitespace-separated tokens, matched
        // case-insensitively by AI profile selectors
        for (const auto& token : split(fbi.category, {' ', '\t'}))
        {
            if (!token.empty())
            {
                u.categories.insert(toUpper(token));
            }
        }
        u.aiSelfDirective = fbi.aiLimit;

        u.transportCapacity = fbi.transportCapacity;
        u.canLoad = fbi.canLoad;
        u.transportSize = fbi.transportSize;
        u.cantBeTransported = fbi.cantBeTransported;
        u.selfDestructCountdown = fbi.selfDestructCountdown;

        u.cloakCost = fbi.cloakCost;
        u.cloakCostMoving = fbi.cloakCostMoving;

        // retail stores DamageModifier as 16.16 fixed-point
        // (unitdef+0x1aa); negative or absurd inputs are clamped to a
        // non-negative multiplier the same way the fixed-point store
        // would behave
        u.damageModifier = fbi.damageModifier > 0.0f
            ? static_cast<std::uint32_t>(fbi.damageModifier * 65536.0f + 0.5f)
            : 0u;
        u.minCloakDistance = SimScalar(fbi.minCloakDistance);
        u.initCloaked = fbi.initCloaked;

        u.yardMapContainsGeo = false;

        auto movementClassId = movementClassDatabase.resolveMovementClassByName(fbi.movementClass);

        if (movementClassId)
        {
            u.movementCollisionInfo = UnitDefinition::NamedMovementClass{*movementClassId};
        }
        else
        {
            u.movementCollisionInfo = UnitDefinition::AdHocMovementClass{
                fbi.footprintX,
                fbi.footprintZ,
                fbi.maxSlope,
                fbi.maxWaterSlope,
                fbi.minWaterDepth,
                fbi.maxWaterDepth,
            };

            u.yardMap = parseYardMap(fbi.footprintX, fbi.footprintZ, fbi.yardMap);
            if (u.yardMap->any(isWater))
            {
                u.floater = true;
            }
            if (u.yardMap->any(isGeo))
            {
                u.yardMapContainsGeo = true;
            }
        }

        return u;
    }

    WeaponMediaInfo parseWeaponMediaInfo(const std::vector<Color>& palette, const std::vector<Color>& guiPalette, const WeaponTdf& tdf)
    {
        WeaponMediaInfo mediaInfo;

        mediaInfo.soundTrigger = tdf.soundTrigger;

        if (!tdf.soundStart.empty())
        {
            mediaInfo.soundStart = tdf.soundStart;
        }
        if (!tdf.soundHit.empty())
        {
            mediaInfo.soundHit = tdf.soundHit;
        }
        if (!tdf.soundWater.empty())
        {
            mediaInfo.soundWater = tdf.soundWater;
        }

        mediaInfo.startSmoke = tdf.startSmoke;
        mediaInfo.endSmoke = tdf.endSmoke;
        if (tdf.smokeTrail)
        {
            mediaInfo.smokeTrail = GameTime(static_cast<unsigned int>(tdf.smokeDelay * 30.0f));
        }

        switch (tdf.renderType)
        {
            case 0:
            {
                mediaInfo.renderType = ProjectileRenderTypeLaser{
                    getLaserColorUtil(palette, guiPalette, tdf.color),
                    getLaserColorUtil(palette, guiPalette, tdf.color2),
                    SimScalar(tdf.duration * 30.0f * 2.0f), // duration seems to match better if doubled
                };
                break;
            }
            case 1:
            {
                if (tdf.model.empty())
                {
                    mediaInfo.renderType = ProjectileRenderTypeNone{};
                }
                else
                {
                    mediaInfo.renderType = ProjectileRenderTypeModel{
                        toUpper(tdf.model), ProjectileRenderTypeModel::RotationMode::HalfZ};
                }
                break;
            }
            case 3:
            {
                if (tdf.model.empty())
                {
                    mediaInfo.renderType = ProjectileRenderTypeNone{};
                }
                else
                {
                    mediaInfo.renderType = ProjectileRenderTypeModel{
                        toUpper(tdf.model), ProjectileRenderTypeModel::RotationMode::QuarterY};
                }
                break;
            }
            case 4:
            {
                auto fxName = getFxName(tdf.color);
                if (fxName)
                {

                    mediaInfo.renderType = ProjectileRenderTypeSprite{"fx", *fxName};
                }
                else
                {
                    mediaInfo.renderType = ProjectileRenderTypeNone{};
                }

                break;
            }
            case 5:
            {
                mediaInfo.renderType = ProjectileRenderTypeFlamethrower{};
                break;
            }
            case 6:
            {
                if (tdf.model.empty())
                {
                    mediaInfo.renderType = ProjectileRenderTypeNone{};
                }
                else
                {
                    mediaInfo.renderType = ProjectileRenderTypeModel{
                        toUpper(tdf.model), ProjectileRenderTypeModel::RotationMode::None};
                }
                break;
            }
            case 7:
            {
                mediaInfo.renderType = ProjectileRenderTypeLightning{
                    getLaserColorUtil(palette, guiPalette, tdf.color),
                    SimScalar(tdf.duration * 30.0f * 2.0f), // duration seems to match better if doubled
                };
                break;
            }
            default:
            {
                mediaInfo.renderType = ProjectileRenderTypeLaser{
                    Vector3f(0.0f, 0.0f, 0.0f),
                    Vector3f(0.0f, 0.0f, 0.0f),
                    SimScalar(4.0f)};
                break;
            }
        }

        if (!tdf.explosionGaf.empty() && !tdf.explosionArt.empty())
        {
            mediaInfo.explosionAnim = AnimLocation{tdf.explosionGaf, tdf.explosionArt};
        }

        if (!tdf.waterExplosionGaf.empty() && !tdf.waterExplosionArt.empty())
        {
            mediaInfo.waterExplosionAnim = AnimLocation{tdf.waterExplosionGaf, tdf.waterExplosionArt};
        }

        return mediaInfo;
    }

    FeatureDefinitionId getFeatureId(FeatureDefinitionId& nextId, const std::unordered_map<std::string, FeatureDefinitionId>& featureNameIndex, std::deque<std::string>& openQueue, std::unordered_map<std::string, FeatureDefinitionId>& openSet, const std::string& featureName)
    {
        if (auto existingId = featureNameIndex.find(featureName); existingId != featureNameIndex.end())
        {
            return existingId->second;
        }

        if (auto it = openSet.find(toUpper(featureName)); it != openSet.end())
        {
            return it->second;
        }

        auto id = nextId;
        openQueue.push_back(featureName);
        openSet.insert({toUpper(featureName), nextId});
        nextId = FeatureDefinitionId(nextId.value + 1);
        return id;
    }

    void loadFeatureAnimations(AbstractVirtualFileSystem& vfs, std::unordered_map<std::string, FeatureTdf>& tdfs)
    {
        using Bank = std::unordered_map<std::string, std::vector<uint16_t>>;
        std::unordered_map<std::string, Bank> banks;
        for (auto& [name, tdf] : tdfs)
        {
            if (!tdf.object.empty() || tdf.fileName.empty()) { continue; }
            auto file = toUpper(tdf.fileName);
            auto [it, inserted] = banks.try_emplace(file);
            if (inserted)
            {
                if (auto bytes = vfs.readFile("anims/" + file + ".GAF"))
                {
                    SpanStream stream(bytes->data(), bytes->size());
                    GafArchive gaf(&stream);
                    for (const auto& entry : gaf.entries())
                    {
                        // GafArchive::findEntry and texture loading select the first
                        // matching entry; duplicate names must not concatenate clocks.
                        auto [sequence, fresh] = it->second.try_emplace(toUpper(entry.name));
                        if (!fresh) { continue; }
                        auto& delays = sequence->second;
                        for (const auto& frame : entry.frameOffsets)
                        {
                            delays.push_back(static_cast<uint16_t>(frame.unknown1));
                        }
                    }
                }
            }
            const std::array<std::pair<std::string, std::string>, 3> sequences{{
                {tdf.seqNameDie, tdf.seqNameDieShad},
                {tdf.seqNameReclamate, tdf.seqNameReclamateShad},
                {tdf.seqNameBurn, tdf.seqNameBurnShad}}};
            for (unsigned int i = 0; i < sequences.size(); ++i)
            {
                auto read = [&](const std::string& seq) {
                    auto found = it->second.find(toUpper(seq));
                    return found == it->second.end() ? std::vector<uint16_t>{} : found->second;
                };
                tdf.animations[i] = {read(sequences[i].first), read(sequences[i].second)};
            }
        }
    }

    std::vector<const FeatureTdf*> loadFeatureDefinitions(
        const std::unordered_map<std::string, FeatureTdf>& tdfs,
        SimpleVectorMap<FeatureDefinition, FeatureDefinitionIdTag>& featureDefinitions,
        std::unordered_map<std::string, FeatureDefinitionId>& featureNameIndex,
        const std::string& initialFeatureName)
    {
        std::vector<const FeatureTdf*> loaded;
        auto nextId = featureDefinitions.getNextId();
        std::unordered_map<std::string, FeatureDefinitionId> openSet{{toUpper(initialFeatureName), nextId}};
        nextId = FeatureDefinitionId(nextId.value + 1);
        for (std::deque<std::string> featuresToLoad{{initialFeatureName}}; !featuresToLoad.empty(); featuresToLoad.pop_front())
        {
            const auto& featureName = featuresToLoad.front();

            auto tdfIt = tdfs.find(toUpper(featureName));
            if (tdfIt == tdfs.end())
            {
                continue;
            }
            const auto& tdf = tdfIt->second;
            loaded.push_back(&tdf);

            FeatureDefinition f{};
            f.object = !tdf.object.empty();
            f.animations = tdf.animations;

            f.name = featureName;

            f.footprintX = tdf.footprintX;
            f.footprintZ = tdf.footprintZ;
            f.height = SimScalar(tdf.height);

            f.reclaimable = tdf.reclaimable;
            f.autoreclaimable = tdf.autoreclaimable;
            if (!tdf.featureReclamate.empty())
            {
                f.featureReclamate = getFeatureId(nextId, featureNameIndex, featuresToLoad, openSet, tdf.featureReclamate);
            }
            f.metal = tdf.metal;
            f.energy = tdf.energy;

            f.flamable = tdf.flamable;
            if (!tdf.featureBurnt.empty())
            {
                f.featureBurnt = getFeatureId(nextId, featureNameIndex, featuresToLoad, openSet, tdf.featureBurnt);
            }
            f.burnMin = tdf.burnMin;
            f.burnMax = tdf.burnMax;
            f.sparkTime = tdf.sparkTime;
            f.spreadChance = tdf.spreadChance;
            f.burnWeapon = tdf.burnWeapon;

            f.geothermal = tdf.geothermal;

            f.hitDensity = tdf.hitDensity;

            f.reproduce = tdf.reproduce;
            f.reproduceArea = tdf.reproduceArea;

            f.noDisplayInfo = tdf.noDisplayInfo;

            f.permanent = tdf.permanent;

            f.blocking = tdf.blocking;

            f.indestructible = tdf.indestructible;
            f.damage = tdf.damage;
            if (!tdf.featureDead.empty())
            {
                f.featureDead = getFeatureId(nextId, featureNameIndex, featuresToLoad, openSet, tdf.featureDead);
            }

            auto id = featureDefinitions.insert(f);
            featureNameIndex.insert({toUpper(featureName), id});
        }
        return loaded;
    }
}

namespace rwe
{
    unsigned int findSchemaIndexByType(const OtaRecord& ota, const std::string& schemaType)
    {
        for (Index i = 0; i < getSize(ota.schemas); ++i)
        {
            if (toUpper(ota.schemas[i].type) == toUpper(schemaType))
            {
                return i;
            }
        }

        throw OtaParseException("No schema with Type matching: " + schemaType);
    }

    unsigned int findCampaignSchemaIndex(const OtaRecord& ota, const std::string& difficulty)
    {
        // Retail TotalA.exe 0x436860 mode-1 candidate permutations,
        // indexed by the staged difficulty rank.
        static const char* const candidateOrders[3][3] = {
            {"Easy", "Medium", "Hard"},
            {"Medium", "Easy", "Hard"},
            {"Hard", "Medium", "Easy"},
        };

        // The staged name is tried first (first match in file order);
        // this also honours non-standard Types a mission might ship.
        for (Index i = 0; i < getSize(ota.schemas); ++i)
        {
            if (toUpper(ota.schemas[i].type) == toUpper(difficulty))
            {
                return i;
            }
        }

        // Then the remaining candidates in retail order for the staged
        // rank; an unrecognised staged value falls back to ascending
        // Easy/Medium/Hard.
        const auto* order = &candidateOrders[0];
        for (const auto& o : candidateOrders)
        {
            if (toUpper(o[0]) == toUpper(difficulty))
            {
                order = &o;
                break;
            }
        }
        for (const auto* candidate : *order)
        {
            if (toUpper(candidate) == toUpper(difficulty))
            {
                continue;
            }
            for (Index i = 0; i < getSize(ota.schemas); ++i)
            {
                if (toUpper(ota.schemas[i].type) == toUpper(candidate))
                {
                    return i;
                }
            }
        }

        throw OtaParseException("No suitable schema type in mission file: " + difficulty);
    }

    namespace
    {
        bool isStartPosSpecial(const OtaSpecial& s)
        {
            auto name = toUpper(s.specialWhat);
            return name.size() > 8 && name.compare(0, 8, "STARTPOS") == 0;
        }

        unsigned int startPosNumber(const OtaSpecial& s)
        {
            // StartPosN -> N; non-numeric suffixes sort last
            auto name = toUpper(s.specialWhat);
            unsigned int n = 0;
            for (std::size_t i = 8; i < name.size(); ++i)
            {
                if (name[i] < '0' || name[i] > '9')
                {
                    return 0;
                }
                n = n * 10 + static_cast<unsigned int>(name[i] - '0');
            }
            return n;
        }
    }

    unsigned int countStartPositions(const OtaSchema& schema)
    {
        unsigned int count = 0;
        for (const auto& s : schema.specials)
        {
            if (isStartPosSpecial(s))
            {
                ++count;
            }
        }
        return count;
    }

    std::vector<const OtaSpecial*> collectStartPositions(const OtaSchema& schema)
    {
        std::vector<const OtaSpecial*> result;
        for (const auto& s : schema.specials)
        {
            if (isStartPosSpecial(s))
            {
                result.push_back(&s);
            }
        }
        std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
            return startPosNumber(*a) < startPosNumber(*b);
        });
        return result;
    }

    unsigned int resolveSkirmishSchemaIndex(const OtaRecord& ota, unsigned int playerCount)
    {
        // Retail TotalA.exe 0x436860 mode 3: candidate types are
        // "Network 1".."Network 4" in ascending order; the inner loop
        // walks physical "Schema i" blocks in file order. Acceptance
        // rules per candidate (retail 0x436b23-0x436b3d):
        //   * zero StartPos count -> skip
        //   * count == requested (or requested == 0) -> accept
        //   * else skip when count <= best or best is an exact match
        //   * else (count > best) -> accept
        // Net effect: the last exact match wins; with no exact match
        // the first candidate holding the maximum count wins.
        std::optional<unsigned int> best;
        unsigned int bestCount = 0;
        bool exactSeen = playerCount == 0;

        for (unsigned int network = 1; network <= 4; ++network)
        {
            auto wanted = std::string("Network ").append(std::to_string(network));
            for (Index i = 0; i < getSize(ota.schemas); ++i)
            {
                if (toUpper(ota.schemas[i].type) != toUpper(wanted))
                {
                    continue;
                }
                auto count = countStartPositions(ota.schemas[i]);
                if (count == 0)
                {
                    continue;
                }
                if (playerCount == 0 || count == playerCount)
                {
                    best = i;
                    bestCount = count;
                    exactSeen = true;
                    continue;
                }
                if (exactSeen || count <= bestCount)
                {
                    continue;
                }
                best = i;
                bestCount = count;
            }
        }

        if (!best)
        {
            throw OtaParseException("No suitable schema type in mission file: Network");
        }
        return *best;
    }

    std::set<unsigned int> parseNumPlayers(const std::string& numPlayers)
    {
        std::set<unsigned int> result;
        std::istringstream stream(numPlayers);
        std::string token;
        bool sawToken = false;
        while (std::getline(stream, token, ','))
        {
            sawToken = true;
            try
            {
                auto value = std::stoi(token);
                if (value > 0)
                {
                    result.insert(static_cast<unsigned int>(value));
                }
            }
            catch (const std::exception&)
            {
                // an unparseable entry invalidates the whole list;
                // the callers fall back to unrestricted counts
                return {};
            }
        }
        return sawToken ? result : std::set<unsigned int>{};
    }

    const OtaSpecial* findStartPosSpecial(const OtaSchema& schema, unsigned int posNum)
    {
        // retail ident assignment (0x437123-0x437160): a StartPos
        // special whose suffix begins with a digit carries
        // atoi(suffix) - 1 when that value is positive and the raw
        // atoi result otherwise; a non-numeric suffix consumes a
        // sequential fallback counter shared across such names.
        // The spawn lookup (0x437320) scans in file order and takes
        // the first ident match.
        unsigned int nonNumeric = 0;
        for (const auto& s : schema.specials)
        {
            if (!isStartPosSpecial(s))
            {
                continue;
            }
            auto suffix = toUpper(s.specialWhat).substr(8);
            unsigned int ident;
            if (suffix.front() >= '0' && suffix.front() <= '9')
            {
                auto value = std::strtoul(suffix.c_str(), nullptr, 10);
                ident = value > 0 ? static_cast<unsigned int>(value - 1) : static_cast<unsigned int>(value);
            }
            else
            {
                ident = ++nonNumeric;
            }
            if (ident == posNum)
            {
                return &s;
            }
        }
        return nullptr;
    }

    std::array<std::optional<unsigned int>, 10> assignSkirmishStartPosNums(
        const std::array<std::optional<PlayerInfo>, 10>& players,
        SkirmishStartLocation location,
        std::minstd_rand& rng)
    {
        std::array<std::optional<unsigned int>, 10> result;
        std::vector<unsigned int> occupied;
        for (unsigned int i = 0; i < players.size(); ++i)
        {
            if (players[i])
            {
                occupied.push_back(i);
            }
        }

        std::vector<unsigned int> pool(occupied);
        if (location == SkirmishStartLocation::Random)
        {
            // retail 0x497a10-0x497a71 shuffle: for each entry i >= 1,
            // swap pool[i] with pool[rand() % i] -- the divisor is i
            // itself (j is strictly < i; entry 1 always swaps with 0),
            // retail's slightly biased inside-out variant. Draw source
            // is RWE's seeded rng; bit-exact retail PRNG parity is not
            // claimed.
            for (std::size_t i = 1; i < pool.size(); ++i)
            {
                std::uniform_int_distribution<std::size_t> dist(0, i - 1);
                std::swap(pool[i], pool[dist(rng)]);
            }
        }

        for (std::size_t k = 0; k < occupied.size(); ++k)
        {
            result[occupied[k]] = pool[k];
        }
        return result;
    }

    std::optional<std::string> checkSkirmishStart(
        const std::array<std::optional<PlayerInfo>, 10>& players,
        const OtaRecord& ota)
    {
        // retail order: player mix, then seat capacity, then alliance
        // uniformity (0x47af91-0x47b0cb)
        unsigned int humans = 0;
        unsigned int computers = 0;
        std::set<unsigned int> allianceKeys;
        for (Index i = 0; i < getSize(players); ++i)
        {
            if (!players[i])
            {
                continue;
            }
            humans += std::holds_alternative<PlayerControllerTypeHuman>(players[i]->controller) ? 1 : 0;
            computers += std::holds_alternative<PlayerControllerTypeComputer>(players[i]->controller) ? 1 : 0;
            // retail ally field defaults to sentinel 5 = no group;
            // an unset group is an independent alliance per slot,
            // never "the same" as another
            allianceKeys.insert(players[i]->allyGroup ? *players[i]->allyGroup : 0x100u + i);
        }
        if (humans == 0 || computers == 0)
        {
            return "There must be at least one player and one computer opponent";
        }

        const unsigned int occupied = humans + computers;
        unsigned int startPosCount = 0;
        try
        {
            startPosCount = countStartPositions(ota.schemas.at(resolveSkirmishSchemaIndex(ota, occupied)));
        }
        catch (const std::exception&)
        {
            // no resolvable Network schema contributes zero seats
        }
        if (occupied > startPosCount)
        {
            return "There are too many players enabled for this map";
        }

        if (allianceKeys.size() < 2)
        {
            return "All players may not be in the same allied group.";
        }
        return std::nullopt;
    }

    float skirmishDifficultyComputerResourceScale(SkirmishDifficulty difficulty)
    {
        // same ctx+0x37eee global as the campaign path: the staged
        // difficulty indexes retail's economy coefficient table
        switch (difficulty)
        {
            case SkirmishDifficulty::Easy:
                return 0.5f;
            case SkirmishDifficulty::Medium:
                return 0.7f;
            case SkirmishDifficulty::Hard:
                return 1.0f;
        }
        return 1.0f;
    }

    PlayerId addSkirmishPlayer(GameSimulation& simulation, const GamePlayerInfo& info)
    {
        return addCampaignPlayer(simulation, info);
    }

    std::string oppositeSide(const std::string& side)
    {
        return toUpper(side) == "ARM" ? "CORE" : "ARM";
    }

    std::vector<GamePlayerInfo> buildCampaignPlayers(const OtaSchema& schema, const std::string& humanSide, const std::string& enemySide)
    {
        unsigned int maxPlayer = 0;
        for (const auto& u : schema.units)
        {
            maxPlayer = std::max(maxPlayer, u.player);
        }

        std::vector<GamePlayerInfo> players;
        players.reserve(maxPlayer);

        for (unsigned int i = 0; i < maxPlayer; ++i)
        {
            bool isHuman = (i == 0);
            auto metal = Metal(isHuman ? schema.humanMetal : schema.computerMetal);
            auto energy = Energy(isHuman ? schema.humanEnergy : schema.computerEnergy);
            players.push_back(GamePlayerInfo{
                std::nullopt,
                isHuman ? GamePlayerType::Human : GamePlayerType::Computer,
                PlayerColorIndex(i),
                GamePlayerStatus::Alive,
                isHuman ? humanSide : enemySide,
                std::nullopt,
                metal,
                energy,
                metal,
                energy,
                metal,
                energy});
        }

        return players;
    }

    SimAngle otaAngleToSimAngle(unsigned int degrees)
    {
        return SimAngle(static_cast<uint16_t>((static_cast<unsigned long long>(degrees % 360) * 65536) / 360));
    }

    MissionSpawnStats spawnMissionUnits(GameSimulation& sim, const std::vector<OtaUnit>& units, const std::vector<PlayerId>& missionPlayers)
    {
        MissionSpawnStats stats;

        for (const auto& u : units)
        {
            auto unitType = toUpper(u.unitName);

            if (u.player == 0 || u.player > missionPlayers.size())
            {
                stats.failures.push_back(unitType + ": mission player " + std::to_string(u.player) + " has no player slot");
                continue;
            }

            if (sim.unitDefinitions.find(unitType) == sim.unitDefinitions.end())
            {
                stats.failures.push_back(unitType + ": missing unit definition");
                continue;
            }

            if (sim.unitScriptDefinitions.find(unitType) == sim.unitScriptDefinitions.end())
            {
                stats.failures.push_back(unitType + ": missing unit script");
                continue;
            }

            auto worldPos = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(u.xPos), 0_ss, SimScalar(u.zPos)));
            worldPos.y = sim.terrain.getHeightAt(worldPos.x, worldPos.z);

            std::optional<SimAngle> rotation;
            if (u.angle)
            {
                rotation = otaAngleToSimAngle(*u.angle);
            }

            // retail places authored units unconditionally; several
            // missions stack units on blocking features/each other.
            // They also bypass the maxunits cap: retail populates the
            // per-player slot ranges directly at mission load.
            auto unitId = sim.trySpawnUnit(unitType, missionPlayers[u.player - 1], worldPos, rotation, true, std::nullopt, true);
            if (!unitId)
            {
                stats.failures.push_back(unitType + ": blocked at (" + std::to_string(u.xPos) + "," + std::to_string(u.zPos) + ")");
                continue;
            }

            auto& unit = sim.getUnitState(*unitId);
            const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);

            // mission units are placed already constructed
            unit.finishBuilding(unitDefinition);

            // a placed unit adopts the same activation state it would have
            // had it just finished construction: ActivateWhenBuilt units
            // come online, everything else stays off
            if (unitDefinition.activateWhenBuilt)
            {
                unit.activate();
            }

            if (u.healthPercentage && *u.healthPercentage < 100)
            {
                unit.hitPoints = std::max(1u, static_cast<unsigned int>((static_cast<unsigned long long>(unitDefinition.maxHitPoints) * *u.healthPercentage) / 100));
            }

            unit.missionState = MissionUnitState{u.initialMission, u.ident, true, u.initialGroup, u.creationCountdown, u.kills, u.immunity, u.aiIgnore};
            // Retail 3.1's mission placement parser (0x436c30) never reads
            // OTA Kills. Its spawn path (0x488310 -> 0x485f50 -> 0x485a40)
            // starts the live counter at zero. Retain the authored metadata,
            // but earn runtime veterancy through combat; saves restore their
            // independently persisted live counter through applySavedGame.

            ++stats.spawned;
        }

        return stats;
    }

    MissionBootstrapStats installInitialMissionOrders(GameSimulation& sim)
    {
        MissionBootstrapStats stats;

        // retail's ident resolver (TotalA.exe 0x487af0) scans the
        // authored unit-record table in order and, per record, compares
        // the reference case-insensitively against `Ident` and then
        // `Unitname` (the unit type); the first record with a live unit
        // that matches either wins. `g`, `i` and `wa` all share it.
        auto resolveIdent = [&](const std::string& ident) -> std::optional<UnitId> {
            auto upper = toUpper(ident);
            for (const auto& [candidateId, candidate] : sim.units)
            {
                if (!candidate.missionState)
                {
                    continue;
                }
                if (candidate.missionState->ident && toUpper(*candidate.missionState->ident) == upper)
                {
                    return candidateId;
                }
                if (candidate.unitType == upper)
                {
                    return candidateId;
                }
            }
            return std::nullopt;
        };

        auto toWorld = [&](const SimVector& otaPos) {
            auto world = sim.terrain.topLeftCoordinateToWorld(otaPos);
            world.y = sim.terrain.getHeightAt(world.x, world.z);
            return world;
        };

        // unit type references resolve case-insensitively against the
        // loaded definitions, matching retail's name lookup
        auto resolveUnitType = [&](const std::string& name) -> std::optional<std::string> {
            auto upper = toUpper(name);
            if (sim.unitDefinitions.find(upper) != sim.unitDefinitions.end())
            {
                return upper;
            }
            return std::nullopt;
        };

        for (auto& [unitId, unit] : sim.units)
        {
            if (!unit.missionState || unit.missionState->initialMission.empty())
            {
                continue;
            }

            ++stats.scriptedUnits;

            auto parsed = parseInitialMission(unit.missionState->initialMission);
            stats.parsedCommands += parsed.commands.size();
            for (const auto& e : parsed.errors)
            {
                stats.errors.push_back(unit.unitType + ": " + e);
            }
            for (const auto& u : parsed.unsupportedCommands)
            {
                stats.unsupportedCommands.push_back(unit.unitType + ": " + u);
            }

            // retail only clears the selectable bit in the epilogue
            // when the script issued at least one real order (edi at
            // 0x487e5b); scripts that issue no orders (`i`-only,
            // entirely malformed) leave the spawn default true, so
            // transport cargo stays owner-controllable once unloaded

            // retail shares one coordinate slot pair across the
            // coordinate commands: a `b` without explicit coordinates
            // builds at the position left by the previous coordinate
            // command, or the unit's spawn position before any ran
            SimVector scriptPosition = unit.position;

            // retail tracks whether the script issued any real order
            // (edi flag in the dispatch loop); `i`, `o` and malformed
            // commands do not count
            bool issuedOrder = false;

            for (const auto& command : parsed.commands)
            {
                match(
                    command,
                    [&](const MissionCommandPatrol& c) {
                        std::vector<SimVector> waypoints;
                        if (c.destination)
                        {
                            waypoints.push_back(toWorld(*c.destination));
                            scriptPosition = waypoints.front();
                        }
                        // patrols loop back through the unit's own position
                        waypoints.push_back(unit.position);
                        unit.addOrder(PatrolOrder(std::move(waypoints)));
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandMove& c) {
                        scriptPosition = toWorld(c.destination);
                        unit.addOrder(createMoveOrder(scriptPosition));
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandAttackPosition& c) {
                        scriptPosition = toWorld(c.target);
                        unit.addOrder(createAttackGroundOrder(scriptPosition));
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandAttackType& c) {
                        auto resolvedType = resolveUnitType(c.unitType);
                        if (!resolvedType)
                        {
                            stats.errors.push_back(unit.unitType + ": a references unknown unit type " + c.unitType);
                            return;
                        }
                        unit.addOrder(AttackUnitTypeOrder{*resolvedType});
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandGuard& c) {
                        auto target = resolveIdent(c.targetIdent);
                        if (target)
                        {
                            unit.addOrder(GuardOrder(*target));
                            issuedOrder = true;
                            ++stats.installedOrders;
                        }
                        else
                        {
                            stats.errors.push_back(unit.unitType + ": g references unknown ident " + c.targetIdent);
                        }
                    },
                    [&](const MissionCommandWait& c) {
                        unit.addOrder(WaitOrder(c.ticks));
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandWaitForAttack& c) {
                        std::optional<UnitId> target;
                        if (c.targetIdent)
                        {
                            target = resolveIdent(*c.targetIdent);
                            if (!target)
                            {
                                stats.errors.push_back(unit.unitType + ": wa references unknown ident " + *c.targetIdent);
                                return;
                            }
                        }
                        unit.addOrder(WaitForAttackOrder(target));
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandSetStance& c) {
                        // retail applies `o` at parse time, not via the
                        // queue; absent arguments keep the current value
                        if (c.moveOrder)
                        {
                            unit.moveOrders = *c.moveOrder;
                        }
                        if (c.fireOrder)
                        {
                            unit.fireOrders = *c.fireOrder;
                        }
                        ++stats.stanceCommands;
                    },
                    [&](const MissionCommandMakeSelectable&) {
                        unit.addOrder(MakeSelectableOrder{});
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandBuild& c) {
                        auto resolvedType = resolveUnitType(c.unitType);
                        if (!resolvedType)
                        {
                            stats.errors.push_back(unit.unitType + ": b references unknown unit type " + c.unitType);
                            return;
                        }

                        const auto& producerDefinition = sim.unitDefinitions.at(unit.unitType);
                        if (!producerDefinition.builder)
                        {
                            stats.errors.push_back(unit.unitType + ": b on non-builder for " + *resolvedType);
                            return;
                        }

                        if (producerDefinition.isMobile)
                        {
                            // mobile scripts write `b TYPE x z`; the
                            // retail sscanf then holds x in the count
                            // slot and z in the first float slot
                            SimVector buildPosition;
                            if (c.coordX && c.coordZ)
                            {
                                buildPosition = toWorld(SimVector(SimScalar(*c.coordX), 0_ss, SimScalar(*c.coordZ)));
                            }
                            else if (c.coordX)
                            {
                                buildPosition = toWorld(SimVector(SimScalar(static_cast<float>(c.count)), 0_ss, SimScalar(*c.coordX)));
                            }
                            else
                            {
                                buildPosition = scriptPosition;
                            }
                            scriptPosition = buildPosition;
                            unit.addOrder(MissionBuildOrder{*resolvedType, buildPosition});
                        }
                        else
                        {
                            // factories use `b TYPE [count]`; any
                            // coordinates are ignored
                            auto count = c.count > 0 ? static_cast<unsigned int>(c.count) : 0u;
                            unit.addOrder(MissionFactoryBuildOrder{*resolvedType, count});
                        }
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandBuildWeapon& c) {
                        // `bw` installs a background order that does
                        // not block the rest of the script
                        unit.backgroundOrders.emplace_back(BuildWeaponOrder{c.count > 0 ? static_cast<unsigned int>(c.count) : 0u});
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandJoinGroup& c) {
                        // `i` joins the target's group immediately at
                        // interpretation time; in practice every stock
                        // target is a transport, so this boards the unit
                        auto target = resolveIdent(c.targetIdent);
                        if (!target)
                        {
                            stats.errors.push_back(unit.unitType + ": i references unknown ident " + c.targetIdent);
                            return;
                        }
                        if (!sim.tryBoardTransport(unitId, *target))
                        {
                            stats.errors.push_back(unit.unitType + ": i could not board " + c.targetIdent);
                            return;
                        }
                        // not a queued order: does not count toward the
                        // implicit make-selectable gate
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandUnload& c) {
                        // retail shares the coordinate slots: a later
                        // bare `b` inherits these coords
                        if (c.destination)
                        {
                            scriptPosition = toWorld(*c.destination);
                        }
                        unit.addOrder(UnloadOrder{scriptPosition});
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandSelfDestruct&) {
                        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
                        unit.addOrder(SelfDestructOrder{unitDefinition.selfDestructCountdown * SimTicksPerSecond});
                        issuedOrder = true;
                        ++stats.installedOrders;
                    },
                    [&](const MissionCommandUnsupported&) {
                        // already recorded above
                    });
            }

            // retail clears the selectable flag when the script issued
            // at least one order, then appends an implicit trailing
            // make-selectable unless `p`, `a <pos>`, `d` or `s` ran;
            // the queued make-selectable order is what finally marks
            // the unit selectable when it executes
            if (issuedOrder)
            {
                unit.missionState->selectable = false;
                if (!parsed.hasExplicitSelectable && !parsed.suppressImplicitSelectable)
                {
                    unit.addOrder(MakeSelectableOrder{});
                    ++stats.installedOrders;
                }
            }
        }

        return stats;
    }

    std::optional<std::string> loadCampaignAiProfileText(
        AbstractVirtualFileSystem& vfs,
        const std::string& aiDirectory,
        const std::string& profileName)
    {
        for (const auto& candidate : {profileName, profileName + ".TXT", std::string("DEFAULT"), std::string("DEFAULT.TXT")})
        {
            if (auto bytes = vfs.readFile(aiDirectory + "/" + candidate))
            {
                return std::string(bytes->begin(), bytes->end());
            }
        }
        return std::nullopt;
    }

    CampaignAiDifficulty campaignAiDifficultyIndex(const std::string& difficulty)
    {
        auto name = toUpper(difficulty);
        if (name == "EASY")
        {
            return CampaignAiDifficulty::Easy;
        }
        if (name == "HARD")
        {
            return CampaignAiDifficulty::Hard;
        }
        return CampaignAiDifficulty::Medium;
    }

    CampaignAiDifficulty skirmishAiDifficultyIndex(SkirmishDifficulty difficulty)
    {
        switch (difficulty)
        {
            case SkirmishDifficulty::Easy:
                return CampaignAiDifficulty::Easy;
            case SkirmishDifficulty::Medium:
                return CampaignAiDifficulty::Medium;
            case SkirmishDifficulty::Hard:
                return CampaignAiDifficulty::Hard;
        }
        return CampaignAiDifficulty::Medium;
    }

    void attachCampaignAiPlayers(GameSimulation& simulation, const CampaignAiProfile& profile)
    {
        for (Index i = 0; i < getSize(simulation.players); ++i)
        {
            if (simulation.players[i].type != GamePlayerType::Computer)
            {
                continue;
            }
            auto alreadyLoaded = std::any_of(simulation.campaignAi.begin(), simulation.campaignAi.end(), [&](const auto& st) { return st.player == PlayerId(i); });
            if (!alreadyLoaded)
            {
                initCampaignAiPlayer(simulation, PlayerId(i), profile);
            }
        }
    }

    float campaignDifficultyComputerResourceScale(const std::string& difficulty)
    {
        // retail reads ctx+0x37eee as a raw index: 0 (Easy) -> x0.5,
        // 1 (Medium) -> x0.7, anything else -> unscaled x1.0
        auto name = toUpper(difficulty);
        if (name == "EASY")
        {
            return 0.5f;
        }
        if (name == "MEDIUM")
        {
            return 0.7f;
        }
        return 1.0f;
    }

    PlayerId addCampaignPlayer(GameSimulation& simulation, const GamePlayerInfo& info)
    {
        auto playerId = simulation.addPlayer(info);
        if (info.type == GamePlayerType::Computer)
        {
            auto& player = simulation.getPlayer(playerId);
            player.metal = Metal(player.metal.value * simulation.computerResourceScale);
            player.energy = Energy(player.energy.value * simulation.computerResourceScale);
        }
        return playerId;
    }

    std::optional<MeteorShowerParams> loadMeteorShowerDefaults(AbstractVirtualFileSystem& vfs)
    {
        auto bytes = vfs.readFile("gamedata/meteor.tdf");
        if (!bytes)
        {
            return std::nullopt;
        }

        std::string tdfString(bytes->data(), bytes->size());
        auto block = parseTdfFromString(tdfString);
        auto defaults = block.findBlock("Default");
        if (!defaults)
        {
            return std::nullopt;
        }

        MeteorShowerParams params;
        defaults->get().readOrDefault("MeteorWeapon", params.weaponName);
        defaults->get().readOrDefault("MeteorRadius", params.radius);
        defaults->get().readOrDefault("MeteorDensity", params.density);
        defaults->get().readOrDefault("MeteorDuration", params.duration);
        defaults->get().readOrDefault("MeteorInterval", params.interval);
        return params;
    }

    void configureMeteorShower(GameSimulation& simulation, const OtaSchema& schema, const std::optional<MeteorShowerParams>& defaults)
    {
        MeteorShowerParams schemaParams;
        schemaParams.weaponName = schema.meteorWeapon;
        schemaParams.radius = schema.meteorRadius;
        schemaParams.density = schema.meteorDensity;
        schemaParams.duration = static_cast<float>(schema.meteorDuration);
        schemaParams.interval = static_cast<float>(schema.meteorInterval);

        auto resolved = resolveMeteorShowerParams(schemaParams, defaults.value_or(MeteorShowerParams{}));
        simulation.configureMeteorShower(resolved.params, resolved.enabled);
    }

    void applyTransplantBalanceOverrides(std::unordered_map<std::string, UnitDefinition>& unitDefinitions)
    {
        static const std::unordered_map<std::string, unsigned int> transportCapacityOverrides{
            {"ARMATLAS", 1},
            {"CORVALK", 1},
            {"ARMTHOVR", 5},
            {"CORTHOVR", 5},
            {"ARMTSHIP", 24},
            {"CORTSHIP", 24},
        };

        for (const auto& [unitName, capacity] : transportCapacityOverrides)
        {
            auto it = unitDefinitions.find(unitName);
            if (it == unitDefinitions.end())
            {
                LOG_WARN << "TA-Transplant balance override skipped: unit definition not loaded: " << unitName;
                continue;
            }
            it->second.transportCapacity = capacity;
        }
    }
}
