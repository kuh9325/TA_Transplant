#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <cmath>
#include <rwe/save/SavedGame.h>
#include <rwe/CampaignProgress.h>
#include <rwe/EndMissionScene_util.h>
#include <rwe/game/dump_util.h>
#include <rwe/game/GameScene_util.h>
#include <rwe/game/PlayerCommandService.h>
#include <rwe/sim/GameHash_util.h>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <rwe/LoadingScene_util.h>
#include <rwe/io/_3do/_3do.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/gui/gui.h>
#include <rwe/io/moveinfotdf/io.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/tnt/TntArchive.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/mesh_util.h>
#include <rwe/sim/CampaignAi.h>
#include <rwe/sim/CampaignAiProfile.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/MissionObjectives.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vertex_height.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    namespace
    {
        /**
         * The media-free half of LoadingScene::loadDefinitions: parses
         * every retail FBI/3DO/COB/weapon/feature once so each mission's
         * GameSimulation can be built cheaply per schema.
         */
        struct DefinitionPack
        {
            MovementClassDatabase movementClassDatabase;
            std::unordered_map<std::string, UnitDefinition> unitDefinitions;
            std::unordered_map<std::string, UnitModelDefinition> unitModelDefinitions;
            std::unordered_map<std::string, CobScript> unitScriptDefinitions;
            std::unordered_map<std::string, WeaponDefinition> weaponDefinitions;
            std::unordered_map<std::string, FeatureTdf> featureTdfs;
        };

        DefinitionPack loadDefinitionPack(AbstractVirtualFileSystem& vfs)
        {
            DefinitionPack pack;

            {
                auto bytes = vfs.readFile("gamedata/MOVEINFO.TDF");
                REQUIRE(bytes);
                for (const auto& c : parseMoveInfoTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
                {
                    pack.movementClassDatabase.registerMovementClass(parseMovementClassDefinition(c.second));
                }
            }

            {
                for (const auto& fileName : vfs.getFileNames("weapons", ".tdf"))
                {
                    auto bytes = vfs.readFile("weapons/" + fileName);
                    REQUIRE(bytes);
                    for (const auto& pair : parseWeaponTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
                    {
                        pack.weaponDefinitions.insert({toUpper(pair.first), parseWeaponDefinition(pair.second)});
                    }
                }
            }

            {
                for (const auto& fbiName : vfs.getFileNames("units", ".fbi"))
                {
                    auto bytes = vfs.readFile("units/" + fbiName);
                    REQUIRE(bytes);
                    auto fbi = parseUnitFbi(parseTdfFromString(std::string(bytes->data(), bytes->size())));
                    auto unitDefinition = parseUnitDefinition(fbi, pack.movementClassDatabase);

                    auto objBytes = vfs.readFile("objects3d/" + fbi.objectName + ".3do");
                    REQUIRE(objBytes);
                    SpanStream objStream(objBytes->data(), objBytes->size());
                    auto objects = parse3doObjects(objStream, objStream.tellg());
                    REQUIRE(objects.size() == 1);
                    pack.unitModelDefinitions.insert({toUpper(fbi.objectName),
                        createUnitModelDefinition(simScalarFromFixed(findHighestVertex(objects.front()).y), unitMeshFrom3do(objects.front()))});

                    pack.unitDefinitions.insert({toUpper(fbi.unitName), std::move(unitDefinition)});
                }

                // match LoadingScene::loadDefinitions: the balance
                // override applies after all retail FBIs are loaded
                applyTransplantBalanceOverrides(pack.unitDefinitions);

                // builder GUI pages flatten into each builder's build
                // list after all unit defs exist, same as LoadingScene;
                // the useonly filter is a per-mission step, applied to
                // build menus, not needed for the AI's product table
                for (auto& [name, def] : pack.unitDefinitions)
                {
                    if (!def.builder)
                    {
                        continue;
                    }
                    std::unordered_set<std::string> seen;
                    for (int i = 1; auto rawGui = vfs.readFile("guis/" + name + std::to_string(i) + ".GUI"); ++i)
                    {
                        auto parsedGui = parseGuiFromBytes(*rawGui);
                        REQUIRE(parsedGui);
                        // index 0 is the page container, not a button;
                        // only BehaviorBuildButton gadgets are products
                        for (std::size_t j = 1; j < parsedGui->size(); ++j)
                        {
                            const auto& entry = (*parsedGui)[j];
                            if (!(entry.common.attribs & GuiButtonAttrib::BehaviorBuildButton))
                            {
                                continue;
                            }
                            auto n = toUpper(entry.common.name);
                            if (seen.insert(n).second
                                && pack.unitDefinitions.find(n) != pack.unitDefinitions.end())
                            {
                                def.buildOptions.push_back(n);
                            }
                        }
                    }
                }
            }

            pack.unitScriptDefinitions = loadCobScripts(vfs);

            {
                for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
                {
                    auto bytes = vfs.readFile("features/" + name);
                    REQUIRE(bytes);
                    auto tdfRoot = parseTdfFromString(std::string(bytes->data(), bytes->size()));
                    for (const auto& e : tdfRoot.blocks)
                    {
                        pack.featureTdfs.insert({toUpper(e.first), parseFeatureTdf(*e.second)});
                    }
                }
            }

            loadFeatureAnimations(vfs, pack.featureTdfs);
            return pack;
        }

        struct MissionRunResult
        {
            unsigned int unitsSpawned{0};
            unsigned int scriptedUnits{0};
            unsigned int parsedCommands{0};
            unsigned int installedOrders{0};
            unsigned int unsupportedCommands{0};
            unsigned int bootstrapErrors{0};
            unsigned int campaignAiPlayers{0};
            std::vector<std::string> spawnFailures;
            std::vector<std::string> errors;
            std::vector<std::string> unsupportedCommandStrings;
            unsigned int unresolvedFeatures{0};
            MissionResult result{MissionResult::InProgress};

            /** Meteor-shower hazard observation. */
            bool meteorEnabled{false};
            std::string meteorWeaponType;
            unsigned int meteorHits{0};

            /**
             * Corrosive-water observation: units at/below sea level
             * whose lastDamagedAt stamped on a corrosion-cadence
             * tick (gameTime % 30 == 0 inside the tick that ran).
             * Only meaningful when the OTA arms waterdoesdamage.
             */
            unsigned int corrosiveDamageApplications{0};
            unsigned int explosiveDeaths{0};
            unsigned int bombReleases{0};
            unsigned int bombImpacts{0};
            unsigned int serviceFacilities{0};
            unsigned int serviceAircraft{0};
            unsigned int serviceProbes{0};
            unsigned int serviceSupplementalAircraft{0};
            unsigned int serviceDocked{0};
            unsigned int serviceHealed{0};
            unsigned int distinctSelfDestructBlasts{0};
            unsigned int mapFeatures{0}, destructibleFeatures{0}, blockingFeatures{0}, reclaimableFeatures{0}, burnableFeatures{0};
            unsigned int featureProbes{0}, featureTransitions{0}, featureReclaims{0}, featureBurns{0};
            unsigned int initialExperienced{0}, initialVeterans{0}, initialKills{0}, experienceMismatches{0};
            uint64_t earnedKills{0}, promotions{0};
            MissionResult featureResultBefore{MissionResult::InProgress};
        };

        /**
         * One campaign schema run: terrain + defs + players +
         * objectives + spawn + InitialMission + CampaignAi + ticks.
         */
        MissionRunResult runSchema(
            const DefinitionPack& pack,
            const std::string& campaignName,
            const std::string& campaignSide,
            const CampaignMissionEntry& mission,
            const OtaRecord& ota,
            unsigned int schemaIndex,
            const std::string& stagedDifficulty,
            AbstractVirtualFileSystem& vfs,
            int tickCount = 120, bool exerciseService = false, bool exerciseFeatures = false, bool exerciseVeterancy = false,
            const std::function<void(GameSimulation&, unsigned int)>& observe = {}, bool faithfulAudit = false)
        {
            MissionRunResult out;

            const auto& schema = ota.schemas.at(schemaIndex);

            // terrain
            auto tntBytes = vfs.readFile("maps/" + missionFileToMapName(mission.missionFile) + ".tnt");
            REQUIRE(tntBytes);
            SpanStream tntStream(tntBytes->data(), tntBytes->size());
            TntArchive tnt(&tntStream);
            Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
            tnt.readMapAttributes(mapAttributes.getData());
            auto heightGrid = getHeightGrid(mapAttributes);
            auto featureNames = getFeatureNames(tnt);

            std::unordered_set<std::string> requiredFeatures;
            std::vector<std::pair<Point, std::string>> features;
            mapAttributes.forEachIndexed([&](auto c, const auto& e) {
                if (e.feature != TntTileAttributes::FeatureNone
                    && e.feature != TntTileAttributes::FeatureUnknown
                    && e.feature != TntTileAttributes::FeatureVoid)
                {
                    features.emplace_back(Point(c.x, c.y), featureNames.at(e.feature));
                }
            });
            for (const auto& f : schema.features)
            {
                features.emplace_back(Point(f.xPos, f.zPos), f.featureName);
            }
            for (const auto& [pos, name] : features)
            {
                requiredFeatures.insert(toUpper(name));
            }
            // unit corpses spawn as features when units die;
            // LoadingScene seeds them the same way
            for (const auto& [name, def] : pack.unitDefinitions)
            {
                if (!def.corpse.empty())
                {
                    requiredFeatures.insert(toUpper(def.corpse));
                }
            }

            GameSimulation sim(
                MapTerrain(std::move(heightGrid), SimScalar(tnt.getHeader().seaLevel)),
                static_cast<unsigned char>(schema.surfaceMetal),
                std::max(0, static_cast<int>(ota.minWindSpeed)),
                std::min(static_cast<int>(ota.maxWindSpeed), MaxUtilizableWindSpeed),
                static_cast<int>(ota.tidalStrength));

            sim.lineOfSightEnabled = ota.lineOfSight != 0;
            sim.lineOfSightMode = ota.lineOfSight;
            sim.mappingEnabled = ota.mapping != 0;
            sim.noSeaLevelTrigger = ota.noSeaLevelTrigger;
            sim.mapGravity = ota.gravity;
            sim.waterDoesDamage = ota.waterDoesDamage;
            sim.waterDamage = ota.waterDamage;
            sim.computerResourceScale = campaignDifficultyComputerResourceScale(stagedDifficulty);

            sim.unitDefinitions = pack.unitDefinitions;
            sim.weaponDefinitions = pack.weaponDefinitions;
            sim.movementClassDatabase = pack.movementClassDatabase;
            sim.unitModelDefinitions = pack.unitModelDefinitions;
            sim.unitScriptDefinitions = pack.unitScriptDefinitions;

            // OTA meteor-shower hazard (same path as LoadingScene)
            configureMeteorShower(sim, schema, loadMeteorShowerDefaults(vfs));

            for (const auto& name : requiredFeatures)
            {
                if (pack.featureTdfs.count(name) != 0)
                {
                    loadFeatureDefinitions(pack.featureTdfs, sim.featureDefinitions, sim.featureNameIndex, name);
                }
            }
            for (const auto& [pos, name] : features)
            {
                auto featureId = sim.tryGetFeatureDefinitionId(name);
                if (featureId)
                {
                    sim.addFeature(*featureId, pos.x, pos.y);
                }
                else
                {
                    ++out.unresolvedFeatures;
                }
            }

            sim.movementClassCollisionService = createMovementClassCollisionService(sim.terrain, sim.movementClassDatabase);

            // players: mission player 1 = human, others = computers
            auto campaignPlayers = buildCampaignPlayers(schema, campaignSide, oppositeSide(campaignSide));
            std::vector<PlayerId> missionPlayers;
            std::optional<PlayerId> localPlayerId;
            for (const auto& p : campaignPlayers)
            {
                auto playerId = addCampaignPlayer(sim, p);
                missionPlayers.push_back(playerId);
                if (p.type == GamePlayerType::Human)
                {
                    localPlayerId = playerId;
                }
            }
            REQUIRE(localPlayerId);

            // mission info + objectives
            auto useOnlyUnits = loadUseOnlyUnits(vfs, "camps/useonly", ota.useOnlyUnits);
            if (faithfulAudit)
            {
                // Match the interactive campaign loader for audit command runs.
                sim.maxUnitsPerPlayer = static_cast<unsigned int>(ota.maxUnits);
                if (!useOnlyUnits.empty())
                {
                    for (auto& [name, definition] : sim.unitDefinitions)
                    {
                        auto& options = definition.buildOptions;
                        options.erase(std::remove_if(options.begin(), options.end(), [&](const auto& option) {
                            return useOnlyUnits.count(option) == 0;
                        }), options.end());
                    }
                }
            }
            MissionInfo missionInfo;
            missionInfo.campaignName = campaignName;
            missionInfo.campaignSide = campaignSide;
            missionInfo.missionName = mission.missionName;
            missionInfo.missionIndex = mission.index;
            missionInfo.missionFile = mission.missionFile;
            missionInfo.schemaType = schema.type;
            missionInfo.aiProfile = schema.aiProfile;
            missionInfo.localPlayerId = *localPlayerId;
            for (const auto& o : ota.objectives)
            {
                missionInfo.objectives.push_back(MissionObjective{o.type, o.params});
            }
            missionInfo.useOnlyUnits = std::move(useOnlyUnits);
            missionInfo.brief = ota.brief;
            missionInfo.killMul = ota.killMul;
            missionInfo.timeMul = ota.timeMul;
            sim.missionInfo = std::move(missionInfo);
            sim.initMissionObjectives();
            REQUIRE(sim.missionObjectives);

            // pre-placed forces + scripted bootstrap;
            // retail authored at least one unit over a blocking map
            // feature (EXP1AC02 CORFRT over Coral02), so collect
            // failures rather than halting and let the caller audit them
            auto spawnStats = spawnMissionUnits(sim, schema.units, missionPlayers);
            out.unitsSpawned = spawnStats.spawned;
            out.spawnFailures = spawnStats.failures;

            if (exerciseVeterancy)
            {
                for (const auto& [id, u] : sim.units)
                {
                    // Retail mission placement ignores OTA Kills; live counters start at zero.
                    out.experienceMismatches += u.kills != 0;
                    out.initialExperienced += u.kills != 0;
                    out.initialVeterans += u.kills >= 5;
                    out.initialKills += u.kills;
                }
                sim.collectTickPhaseTimings = true;
            }

            auto bootstrap = installInitialMissionOrders(sim);
            out.scriptedUnits = bootstrap.scriptedUnits;
            out.parsedCommands = bootstrap.parsedCommands;
            out.installedOrders = bootstrap.installedOrders;
            out.unsupportedCommands = bootstrap.unsupportedCommands.size();
            out.bootstrapErrors = bootstrap.errors.size();
            out.errors = bootstrap.errors;
            out.unsupportedCommandStrings = bootstrap.unsupportedCommands;

            // campaign AI for computer players
            auto profileText = loadCampaignAiProfileText(vfs, "ai", schema.aiProfile);
            REQUIRE(profileText);
            auto profile = parseCampaignAiProfile(*profileText, campaignAiDifficultyIndex(stagedDifficulty));
            for (Index i = 0; i < getSize(sim.players); ++i)
            {
                if (sim.players[i].type == GamePlayerType::Computer)
                {
                    initCampaignAiPlayer(sim, PlayerId(i), profile);
                    ++out.campaignAiPlayers;
                }
            }

            std::map<UnitId, unsigned int> serviceProbeHp;
            std::map<UnitId, UnitId> serviceProbeBase;
            std::set<UnitId> serviced, healed;
            if (exerciseService)
            {
                for (const auto& [id, u] : sim.units)
                {
                    const auto& def = sim.unitDefinitions.at(u.unitType);
                    out.serviceFacilities += def.isAirBase;
                    out.serviceAircraft += def.canFly;
                }
                for (auto base : sim.airServiceFacilities)
                {
                    const auto& facility = sim.getUnitState(base);
                    const auto probesBefore = out.serviceProbes;
                    for (auto& [id, u] : sim.units)
                    {
                        const auto& def = sim.unitDefinitions.at(u.unitType);
                        if (def.canFly && u.owner == facility.owner && !u.carriedBy && !u.isBeingBuilt(def)
                            && u.passengers.empty() && !serviceProbeHp.count(id))
                        {
                            // A controlled probe on the loaded mission, explicitly
                            // distinct from its untouched authored command sequence.
                            u.hitPoints = def.maxHitPoints > 10 ? def.maxHitPoints - 10 : 1;
                            serviceProbeHp.emplace(id, u.hitPoints);
                            serviceProbeBase.emplace(id, base);
                            u.replaceOrders({LandOrder(base)});
                            ++out.serviceProbes;
                            break;
                        }
                    }
                    if (out.serviceProbes == probesBefore)
                    {
                        // Explicitly supplemental validation aircraft when the
                        // authored start has no eligible friendly aircraft.
                        const auto type = sim.players[facility.owner.value].side == "CORE" ? "CORVENG" : "ARMFIG";
                        auto id = sim.trySpawnUnit(type, facility.owner, facility.position, std::nullopt, true);
                        REQUIRE(id);
                        auto& u = sim.getUnitState(*id);
                        const auto& def = sim.unitDefinitions.at(type);
                        u.finishBuilding(def);
                        u.hitPoints = def.maxHitPoints - 10;
                        u.replaceOrders({LandOrder(base)});
                        serviceProbeHp.emplace(*id, u.hitPoints);
                        serviceProbeBase.emplace(*id, base);
                        ++out.serviceProbes;
                        ++out.serviceSupplementalAircraft;
                    }
                }
            }

            std::vector<FeatureId> featureProbeIds;
            if (exerciseFeatures)
            {
                std::optional<FeatureId> damage, reclaim, burn;
                for (const auto& [id, f] : sim.features)
                {
                    const auto& d = sim.getFeatureDefinition(f.featureName);
                    ++out.mapFeatures;
                    out.destructibleFeatures += !d.indestructible;
                    out.blockingFeatures += d.blocking;
                    out.reclaimableFeatures += d.reclaimable;
                    out.burnableFeatures += d.flamable;
                    if (!damage && !d.indestructible) { damage = id; }
                    else if (!reclaim && d.reclaimable && !d.indestructible) { reclaim = id; }
                    else if (!burn && d.flamable && !d.indestructible) { burn = id; }
                }
                out.featureResultBefore = sim.getMissionResult();
                if (damage)
                {
                    WeaponDefinition packet{}; packet.damage = {{"DEFAULT", 65535}};
                    sim.damageFeature(*damage, packet); featureProbeIds.push_back(*damage);
                }
                if (reclaim)
                {
                    const auto& d = sim.getFeatureDefinition(sim.getFeature(*reclaim).featureName);
                    auto credit = sim.applyReclaimProgress(*reclaim, d.metal + d.energy); REQUIRE(credit);
                    REQUIRE(credit->first == Energy(d.energy)); REQUIRE(credit->second == Metal(d.metal));
                    sim.transitionFeature(*reclaim, FeatureEvent::Reclaiming);
                    REQUIRE_FALSE(sim.applyReclaimProgress(*reclaim, d.metal + d.energy));
                    featureProbeIds.push_back(*reclaim); ++out.featureReclaims;
                }
                if (burn)
                {
                    REQUIRE(sim.igniteFeature(*burn)); featureProbeIds.push_back(*burn); ++out.featureBurns;
                }
                out.featureProbes = featureProbeIds.size();
            }

            // run a bounded slice: enough to cover spawn, first
            // objective eval windows, AI task passes and early script
            // orders (wa/w/m/p installs at tick 0)
            if (observe) { observe(sim, 0); }
            for (int i = 0; i < tickCount; ++i)
            {
                sim.tick();
                if (observe) { observe(sim, i + 1); }
                    for (const auto& [id, hp] : serviceProbeHp)
                {
                    auto u = sim.tryGetUnitState(id);
                    if (!u) { continue; }
                    if (u->get().aircraftService && u->get().aircraftService->docked()) { serviced.insert(id); }
                    if (serviced.count(id) && u->get().hitPoints > hp) { healed.insert(id); }
                }
                for (const auto& [_, p] : sim.projectiles)
                {
                    if (p.owner == PlayerIdNone)
                    {
                        ++out.meteorHits;
                    }
                }
                // corrosion fires during ticks where the pre-increment
                // gameTime was % 30 == 0, i.e. post-tick gameTime % 30
                // == 1
                if (sim.waterDoesDamage && sim.waterDamage != 0
                    && sim.gameTime % GameTime(30) == GameTime(1))
                {
                    for (const auto& [_, u] : sim.units)
                    {
                        if (u.lastDamagedAt + GameTime(1) == sim.gameTime
                            && sim.terrain.getHeightAt(u.position.x, u.position.z)
                                <= sim.terrain.getSeaLevel())
                        {
                            ++out.corrosiveDamageApplications;
                        }
                    }
                }
            }

            for (const auto& event : sim.events)
            {
                if (const auto* fire = std::get_if<FireWeaponEvent>(&event);
                    fire && std::holds_alternative<ProjectilePhysicsTypeDropped>(sim.weaponDefinitions.at(fire->weaponType).physicsType)) { ++out.bombReleases; }
                if (const auto* impact = std::get_if<ProjectileDiedEvent>(&event);
                    impact && std::holds_alternative<ProjectilePhysicsTypeDropped>(sim.weaponDefinitions.at(impact->weaponType).physicsType)) { ++out.bombImpacts; }
                if (const auto* death = std::get_if<UnitDiedEvent>(&event))
                {
                    const auto& def = sim.unitDefinitions.at(death->unitType);
                    if (def.kamikaze && !death->explosionWeapon.empty()) { ++out.explosiveDeaths; }
                    if (!death->explosionWeapon.empty() && death->explosionWeapon == toUpper(def.selfDestructAs)
                        && death->explosionWeapon != toUpper(def.explodeAs)) { ++out.distinctSelfDestructBlasts; }
                }
            }

            for (const auto& [id, hp] : serviceProbeHp)
            {
                if (healed.count(id)) { continue; }
                auto u = sim.tryGetUnitState(id);
                auto base = sim.tryGetUnitState(serviceProbeBase.at(id));
                LOG_INFO << "SERVICE incomplete mission=" << mission.missionFile << " aircraft=" << id.value
                    << " exists=" << static_cast<bool>(u)
                    << " alive=" << (u && u->get().isAlive())
                    << " hp=" << (u ? u->get().hitPoints : 0)
                    << " initial_hp=" << hp
                    << " type=" << (u ? u->get().unitType : "absent")
                    << " cargo=" << (u && u->get().carriedBy.has_value())
                    << " aircraft_unfinished=" << (u && u->get().isBeingBuilt(sim.unitDefinitions.at(u->get().unitType)))
                    << " base_exists=" << static_cast<bool>(base)
                    << " base_unfinished=" << (base && base->get().isBeingBuilt(sim.unitDefinitions.at(base->get().unitType)))
                    << " allied=" << (u && base && sim.areAllied(u->get().owner, base->get().owner))
                    << " phase=" << (u && u->get().aircraftService ? static_cast<int>(u->get().aircraftService->phase) : -1);
            }
            for (auto id : featureProbeIds)
            {
                if (!sim.tryGetFeature(id)) { ++out.featureTransitions; }
                REQUIRE_FALSE(sim.tryGetFeature(id));
            }
            out.serviceDocked = serviced.size();
            out.serviceHealed = healed.size();
            out.meteorEnabled = sim.meteorShower.enabled;
            out.meteorWeaponType = sim.meteorShower.weaponType;
            out.result = sim.getMissionResult();
            out.earnedKills = sim.tickPhaseTimings.veterancyAwards;
            out.promotions = sim.tickPhaseTimings.veterancyPromotions;
            return out;
        }
    }

    namespace
    {
        unsigned int totalFailuresAcross(const std::unordered_map<std::string, std::vector<std::string>>& m)
        {
            unsigned int n = 0;
            for (const auto& [k, v] : m)
            {
                n += static_cast<unsigned int>(v.size());
            }
            return n;
        }
    }

    /**
     * C9 closure smoke: every canonical campaign mission, every
     * schema, headlessly initialized and ticked.
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("canonical campaign missions initialize and tick")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_closure_smoke.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto allCampaigns = loadCampaignDescriptors(vfs, "camps");

        // Battle Tactics lives in the same camps directory as a
        // separate single-player corpus ("Battle Tactics - *"); the
        // canonical closure set is exactly the four campaign
        // descriptors plus the standalone Krogoth Encounter.
        static const std::unordered_set<std::string> canonicalNames{
            "Arm Campaign",
            "Core Campaign",
            "Arm Campaign - Core Contingency ",
            "Core Campaign - Core Contingency ",
            "Krogoth Encounter",
        };
        std::vector<CampaignDescriptorFile> campaigns;
        for (auto& c : allCampaigns)
        {
            if (canonicalNames.count(c.displayName))
            {
                campaigns.push_back(std::move(c));
            }
        }
        REQUIRE(campaigns.size() == 5);

        unsigned int canonicalMissions = 0;
        for (const auto& c : campaigns)
        {
            canonicalMissions += c.descriptor.missions.size();
        }
        REQUIRE(canonicalMissions == 75);

        auto pack = loadDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        static const std::vector<std::string> stagedDifficulties{"Easy", "Medium", "Hard"};

        unsigned int stagedResolutions = 0;
        unsigned int schemasRun = 0;
        unsigned int totalSpawned = 0;
        unsigned int totalScripted = 0;
        unsigned int totalCommands = 0;
        unsigned int totalInstalled = 0;
        unsigned int totalUnsupported = 0;
        unsigned int totalErrors = 0;
        unsigned int totalAiPlayers = 0;
        unsigned int hazardEnabledSchemas = 0;
        unsigned int hazardHitSchemas = 0;
        std::set<std::string> hazardWeaponTypes;
        std::set<std::string> profileNames;
        std::vector<std::string> earlyResults;
        std::unordered_map<std::string, std::vector<std::string>> spawnFailuresByMission;

        for (const auto& c : campaigns)
        {
            for (const auto& mission : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(mission.missionFile);
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                REQUIRE(otaRaw);
                auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));

                // each staged difficulty must resolve a schema
                std::set<unsigned int> resolved;
                for (const auto& difficulty : stagedDifficulties)
                {
                    INFO("mission " + mission.missionFile + " difficulty " + difficulty);
                    auto schemaIndex = findCampaignSchemaIndex(ota, difficulty);
                    REQUIRE(schemaIndex < ota.schemas.size());
                    resolved.insert(schemaIndex);
                    ++stagedResolutions;
                }

                for (auto schemaIndex : resolved)
                {
                    const auto& schema = ota.schemas.at(schemaIndex);
                    INFO("mission " + mission.missionFile + " schema " + std::to_string(schemaIndex)
                        + " (" + schema.type + ", ai=" + schema.aiProfile + ")");
                    MissionRunResult r;
                    REQUIRE_NOTHROW(r = runSchema(
                        pack, c.displayName, c.descriptor.campaignSide, mission,
                        ota, schemaIndex, schema.type, vfs));
                    profileNames.insert(schema.aiProfile);
                    ++schemasRun;
                    totalSpawned += r.unitsSpawned;
                    totalScripted += r.scriptedUnits;
                    totalCommands += r.parsedCommands;
                    totalInstalled += r.installedOrders;
                    totalUnsupported += r.unsupportedCommands;
                    totalErrors += r.bootstrapErrors;
                    totalAiPlayers += r.campaignAiPlayers;
                    if (r.meteorEnabled)
                    {
                        ++hazardEnabledSchemas;
                        hazardWeaponTypes.insert(r.meteorWeaponType);
                    }
                    if (r.meteorHits > 0)
                    {
                        ++hazardHitSchemas;
                    }
                    for (const auto& f : r.spawnFailures)
                    {
                        spawnFailuresByMission[mission.missionFile + "/" + schema.type].push_back(f);
                    }
                    if (r.result != MissionResult::InProgress)
                    {
                        earlyResults.push_back(mission.missionFile + "/" + schema.type);
                    }
                }
            }
        }

        LOG_INFO << "C9 closure smoke: " << stagedResolutions << " staged resolutions, "
                 << schemasRun << " schemas run, " << totalSpawned << " units spawned, "
                 << totalScripted << " scripted, " << totalCommands << " commands ("
                 << totalInstalled << " installed, " << totalUnsupported << " unsupported, "
                 << totalErrors << " errors), " << totalAiPlayers << " AI players";

        for (const auto& [m, failures] : spawnFailuresByMission)
        {
            for (const auto& f : failures)
            {
                UNSCOPED_INFO("spawn failure " << m << ": " << f);
            }
        }

        REQUIRE(stagedResolutions == 225);
        REQUIRE(schemasRun == 223);
        // authored units place unconditionally, matching retail;
        // the only tolerated failures are references to units that
        // exist in no retail archive (CORBVALK, ARMBUNK, CORLOOK),
        // which retail itself cannot spawn either
        for (const auto& [m, failures] : spawnFailuresByMission)
        {
            for (const auto& f : failures)
            {
                REQUIRE(f.find("missing unit definition") != std::string::npos);
            }
        }
        REQUIRE(totalFailuresAcross(spawnFailuresByMission) == 13);
        REQUIRE(totalSpawned == 21832 - 13);
        // five of the unspawnable dead references carried
        // InitialMission scripts
        REQUIRE(totalScripted == 11939);
        REQUIRE(totalInstalled > 0);
        // garbage/author-debris segments retail also ignores
        REQUIRE(totalUnsupported == 166);
        // unresolved ident/type references are data defects retail
        // tolerates; record but do not block on them here. The 12
        // `g ARMCOM` refs in CC25 are live in retail via the resolver's
        // Unitname fallback (0x487af0) and now resolve
        REQUIRE(totalErrors == 216);

        // C10: the 21 canonical hazard missions (63 schemas) must all
        // arm a resolved meteor-flagged weapon and produce hits inside
        // the smoke window (first strike begins at int(30/density),
        // worst case density 0.5 -> tick 60)
        REQUIRE(hazardEnabledSchemas == 63);
        REQUIRE(hazardHitSchemas == 63);
        REQUIRE(hazardWeaponTypes == std::set<std::string>({"METEOR", "HAILSTORM", "EARTHQUAKE"}));
        LOG_INFO << "C10 hazard coverage: " << hazardEnabledSchemas << " armed schemas, "
                 << hazardHitSchemas << " with hits in-window";

        // no mission should resolve during the 120-tick smoke window:
        // the earliest scripted wins/losses in stock data need minutes
        if (!earlyResults.empty())
        {
            for (const auto& e : earlyResults)
            {
                UNSCOPED_INFO("early mission result: " + e);
            }
            FAIL("mission resolved within smoke window");
        }
    }

    /**
     * C9 extended sample: risk-selected missions run ~1 minute of
     * simulation on their largest schema to expose crashes, AI
     * deadlock, pathological command growth and evaluator
     * exceptions that the 120-tick smoke cannot reach.
     *
     * Selection rationale:
     *  - AC25:      final ARM mission; KillEnemyCommander +
     *               MoveUnitToRadius; 524 authored units
     *  - AC05:      heaviest CaptureUnitType mission (human-QA'd)
     *  - CC21:      DeathTimerRunsOut + 's' select verbs
     *  - CC23:      largest unsupported-command corpus (79) +
     *               transport join/unload verbs; 667 units
     *  - EXP1AC12:  AirBattle profile, 'bw' build-weapon verbs,
     *               DeathTimerRunsOut, 727 units (largest)
     *  - EXP1CC12:  VictoryTimerRunsOut + KillEnemyCommander
     *  - EXP1CC10:  SeaBattle profile, 38 unsupported segments
     *  - Krogoth:   Krogoth profile, KillUnitType, 'bw' verbs
     *  - AC15:      first METEOR hazard mission (retail 0x418f50
     *               scheduler exercised over a full strike cycle)
     *  - EXP1CC07:  earthquake hazard (MeteorDensity=.5 -> hit every
     *               60 ticks; the EXP1AC07-09 records fall back to
     *               the METEOR defaults on their zero density field)
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("canonical campaign extended headless sample")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_closure_extended.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto allCampaigns = loadCampaignDescriptors(vfs, "camps");
        auto pack = loadDefinitionPack(vfs);

        static const std::unordered_set<std::string> sampleMissions{
            "ac25.ota", "ac05.ota", "cc21.ota", "cc23.ota",
            "exp1ac12.ota", "exp1cc12.ota", "exp1cc10.ota",
            "ac15.ota", "exp1cc07.ota",
            "krogoth encounter.ota",
            // CC25 authors 12 `g ARMCOM` refs that only resolve
            // through retail's Unitname fallback
            "cc25.ota",
        };
        static const std::unordered_set<std::string> canonicalNames{
            "Arm Campaign",
            "Core Campaign",
            "Arm Campaign - Core Contingency ",
            "Core Campaign - Core Contingency ",
            "Krogoth Encounter",
        };

        unsigned int runs = 0;
        for (const auto& c : allCampaigns)
        {
            if (!canonicalNames.count(c.displayName))
            {
                continue;
            }
            for (const auto& mission : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(mission.missionFile);
                if (!sampleMissions.count(toLower(mapName) + ".ota") && !sampleMissions.count(toLower(mission.missionFile)))
                {
                    continue;
                }
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                REQUIRE(otaRaw);
                auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));

                // largest authored schema exercises the most systems
                unsigned int schemaIndex = 0;
                for (unsigned int i = 1; i < ota.schemas.size(); ++i)
                {
                    if (ota.schemas[i].units.size() > ota.schemas[schemaIndex].units.size())
                    {
                        schemaIndex = i;
                    }
                }
                INFO("extended " + mission.missionFile + " schema " + std::string(ota.schemas[schemaIndex].type));
                MissionRunResult r;
                REQUIRE_NOTHROW(r = runSchema(
                    pack, c.displayName, c.descriptor.campaignSide, mission,
                    ota, schemaIndex, ota.schemas[schemaIndex].type, vfs, 1800));
                for (const auto& f : r.spawnFailures)
                {
                    REQUIRE(f.find("missing unit definition") != std::string::npos);
                }
                UNSCOPED_INFO("extended " << mission.missionFile
                    << " spawned=" << r.unitsSpawned
                    << " scripted=" << r.scriptedUnits
                    << " installed=" << r.installedOrders
                    << " unsupported=" << r.unsupportedCommands
                    << " errors=" << r.bootstrapErrors
                    << " result=" << static_cast<int>(r.result)
                    << " meteorWeapon=" << r.meteorWeaponType
                    << " meteorHits=" << r.meteorHits);
                ++runs;
            }
        }
        REQUIRE(runs == 11);
    }

    /**
     * C11 closure smoke: every Battle Tactics mission, every
     * difficulty, headlessly initialized and ticked.
     *
     * The eight "Battle Tactics - *" descriptors ship inside the
     * tactics1-8.hpi archives under the same camps directory as the
     * canonical campaigns, so they enumerate through the same
     * loadCampaignDescriptors path and the same OTA/schema/spawn/
     * InitialMission/objective/AI machinery.
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("battle tactics missions initialize and tick")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_bt_closure_smoke.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto allCampaigns = loadCampaignDescriptors(vfs, "camps");

        std::vector<CampaignDescriptorFile> campaigns;
        for (auto& c : allCampaigns)
        {
            if (c.displayName.rfind("Battle Tactics - ", 0) == 0)
            {
                campaigns.push_back(std::move(c));
            }
        }
        // tactics1-8.hpi: ARM and CORE x Very Short/Short/Medium/Long
        REQUIRE(campaigns.size() == 8);

        unsigned int btMissions = 0;
        for (const auto& c : campaigns)
        {
            btMissions += c.descriptor.missions.size();
        }
        REQUIRE(btMissions == 100);

        auto pack = loadDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        static const std::vector<std::string> stagedDifficulties{"Easy", "Medium", "Hard"};

        unsigned int stagedResolutions = 0;
        unsigned int schemasRun = 0;
        unsigned int totalSpawned = 0;
        unsigned int totalScripted = 0;
        unsigned int totalCommands = 0;
        unsigned int totalInstalled = 0;
        unsigned int totalUnsupported = 0;
        unsigned int totalErrors = 0;
        unsigned int totalAiPlayers = 0;
        unsigned int totalUnresolvedFeatures = 0;
        unsigned int hazardEnabledSchemas = 0;
        unsigned int hazardHitSchemas = 0;
        std::set<std::string> hazardWeaponTypes;
        std::set<std::string> profileNames;
        std::set<std::string> schemaTypes;
        std::vector<std::string> earlyResults;
        std::vector<std::string> allErrors;
        std::vector<std::string> allUnsupported;
        std::unordered_map<std::string, std::vector<std::string>> spawnFailuresByMission;

        for (const auto& c : campaigns)
        {
            for (const auto& mission : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(mission.missionFile);
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                REQUIRE(otaRaw);
                auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));

                // each staged difficulty must resolve a schema
                std::set<unsigned int> resolved;
                for (const auto& difficulty : stagedDifficulties)
                {
                    INFO("mission " + mission.missionFile + " difficulty " + difficulty);
                    auto schemaIndex = findCampaignSchemaIndex(ota, difficulty);
                    REQUIRE(schemaIndex < ota.schemas.size());
                    resolved.insert(schemaIndex);
                    ++stagedResolutions;
                }

                for (auto schemaIndex : resolved)
                {
                    const auto& schema = ota.schemas.at(schemaIndex);
                    INFO("mission " + mission.missionFile + " schema " + std::to_string(schemaIndex)
                        + " (" + schema.type + ", ai=" + schema.aiProfile + ")");
                    MissionRunResult r;
                    REQUIRE_NOTHROW(r = runSchema(
                        pack, c.displayName, c.descriptor.campaignSide, mission,
                        ota, schemaIndex, schema.type, vfs));
                    profileNames.insert(schema.aiProfile);
                    schemaTypes.insert(schema.type);
                    ++schemasRun;
                    totalSpawned += r.unitsSpawned;
                    totalScripted += r.scriptedUnits;
                    totalCommands += r.parsedCommands;
                    totalInstalled += r.installedOrders;
                    totalUnsupported += r.unsupportedCommands;
                    totalErrors += r.bootstrapErrors;
                    totalAiPlayers += r.campaignAiPlayers;
                    totalUnresolvedFeatures += r.unresolvedFeatures;
                    for (const auto& e : r.errors)
                    {
                        allErrors.push_back(mission.missionFile + "/" + schema.type + ": " + e);
                    }
                    for (const auto& u : r.unsupportedCommandStrings)
                    {
                        allUnsupported.push_back(mission.missionFile + "/" + schema.type + ": " + u);
                    }
                    if (r.meteorEnabled)
                    {
                        ++hazardEnabledSchemas;
                        hazardWeaponTypes.insert(r.meteorWeaponType);
                    }
                    if (r.meteorHits > 0)
                    {
                        ++hazardHitSchemas;
                    }
                    for (const auto& f : r.spawnFailures)
                    {
                        spawnFailuresByMission[mission.missionFile + "/" + schema.type].push_back(f);
                    }
                    if (r.result != MissionResult::InProgress)
                    {
                        earlyResults.push_back(mission.missionFile + "/" + schema.type);
                    }
                }
            }
        }

        LOG_INFO << "C11 BT closure smoke: " << stagedResolutions << " staged resolutions, "
                 << schemasRun << " schemas run, " << totalSpawned << " units spawned, "
                 << totalScripted << " scripted, " << totalCommands << " commands ("
                 << totalInstalled << " installed, " << totalUnsupported << " unsupported, "
                 << totalErrors << " errors), " << totalAiPlayers << " AI players, "
                 << totalUnresolvedFeatures << " unresolved features";

        for (const auto& [m, failures] : spawnFailuresByMission)
        {
            for (const auto& f : failures)
            {
                UNSCOPED_INFO("spawn failure " << m << ": " << f);
            }
        }
        for (const auto& e : allErrors)
        {
            LOG_INFO << "C11 BT error: " << e;
        }
        for (const auto& u : allUnsupported)
        {
            LOG_INFO << "C11 BT unsupported: " << u;
        }

        // 100 missions x 3 staged difficulties; every BT OTA ships
        // exactly one schema per difficulty
        REQUIRE(stagedResolutions == 300);
        REQUIRE(schemasRun == 300);
        REQUIRE(schemaTypes == std::set<std::string>({"Easy", "Medium", "Hard"}));

        // authored units place unconditionally, matching retail; the
        // only tolerated failures are dead unit references
        for (const auto& [m, failures] : spawnFailuresByMission)
        {
            for (const auto& f : failures)
            {
                REQUIRE(f.find("missing unit definition") != std::string::npos);
            }
        }
        UNSCOPED_INFO("BT spawn failures: " << totalFailuresAcross(spawnFailuresByMission));
        UNSCOPED_INFO("BT spawned=" << totalSpawned << " scripted=" << totalScripted
                                    << " commands=" << totalCommands << " installed=" << totalInstalled
                                    << " unsupported=" << totalUnsupported << " errors=" << totalErrors
                                    << " unresolvedFeatures=" << totalUnresolvedFeatures);
        // every BT computer player attaches a campaign-AI instance
        REQUIRE(totalAiPlayers == 300);

        // only A Shortage of Water (30s interval) and New Moon
        // (500s interval) arm the meteor hazard in the BT corpus;
        // retail fires the first strike one hit-step in, so both
        // produce hits inside the smoke window despite New Moon's
        // long interval
        REQUIRE(hazardWeaponTypes == std::set<std::string>({"METEOR"}));
        LOG_INFO << "C11 BT hazard coverage: " << hazardEnabledSchemas << " armed schemas, "
                 << hazardHitSchemas << " with hits in-window";

        // no mission should resolve during the 120-tick smoke window:
        // the earliest scripted wins/losses in stock data need minutes
        if (!earlyResults.empty())
        {
            for (const auto& e : earlyResults)
            {
                UNSCOPED_INFO("early mission result: " + e);
            }
            FAIL("mission resolved within smoke window");
        }
    }

    /**
     * C11 extended sample: risk-selected Battle Tactics missions run
     * ~1 minute of simulation on their largest schema to expose
     * crashes, AI deadlock, pathological command growth and evaluator
     * exceptions that the 120-tick smoke cannot reach.
     *
     * Selection rationale:
     *  - Intimidator Rising:       largest authored force (341 units),
     *                              MoveUnitToRadius
     *  - Dogfight over Grantham Deep: heaviest script volume (8384
     *                              segments), InitialGroup fields,
     *                              AirBattle profile
     *  - Brawler Buildup:          4205 segments, VictoryTimerRunsOut,
     *                              partial `p` args
     *  - Black Box:                most `i` transport-join verbs (207),
     *                              MoveUnitToRadius
     *  - Sink the Battleship:      MissionCriticalUnit + InitialGroup
     *                              fields, KillAllOfType ARMBATS,
     *                              DeathTimerRunsOut
     *  - Attack on Barrack:        BuildPriority field
     *  - Miniement Landing:        AiPriorityTarget field, 88 `i`
     *                              transport joins, amphibious assault
     *  - A Shortage of Water:      armed meteor hazard (30s interval),
     *                              DeathTimerRunsOut, 248 units
     *  - New Moon:                 armed meteor hazard (500s interval;
     *                              first strike fires one hit-step in,
     *                              later strikes exceed the window)
     *  - Capture Hanan Base:       VictoryTimerRunsOut + capture
     *                              objectives, ARM Long
     *  - Cutoff in Sector 6895:    the corpus' only UnitTypePassesZ
     *  - Surprise at Labrodorite:  the corpus' only KillEnemyCommander
     *                              + AnyUnitPassesX
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("battle tactics extended headless sample")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_bt_closure_extended.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto allCampaigns = loadCampaignDescriptors(vfs, "camps");
        auto pack = loadDefinitionPack(vfs);

        static const std::unordered_set<std::string> sampleMissions{
            "intimidator rising.ota",
            "dogfight over grantham deep.ota",
            "brawler buildup.ota",
            "black box.ota",
            "sink the battleship.ota",
            "attack on barrack.ota",
            "miniement landing.ota",
            "a shortage of water.ota",
            "new moon.ota",
            "capture hanan base.ota",
            "cutoff in sector 6895.ota",
            "surprise at labrodorite.ota",
            // C11 follow-up: `i CORVALK` Unitname-fallback attach,
            // plus the two corrosive-water missions
            "mesa run.ota",
            "cleanup on kral.ota",
            "less refreshing sea.ota",
        };

        unsigned int runs = 0;
        for (const auto& c : allCampaigns)
        {
            if (c.displayName.rfind("Battle Tactics - ", 0) != 0)
            {
                continue;
            }
            for (const auto& mission : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(mission.missionFile);
                if (!sampleMissions.count(toLower(mapName) + ".ota") && !sampleMissions.count(toLower(mission.missionFile)))
                {
                    continue;
                }
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                REQUIRE(otaRaw);
                auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));

                // largest authored schema exercises the most systems
                unsigned int schemaIndex = 0;
                for (unsigned int i = 1; i < ota.schemas.size(); ++i)
                {
                    if (ota.schemas[i].units.size() > ota.schemas[schemaIndex].units.size())
                    {
                        schemaIndex = i;
                    }
                }
                INFO("bt extended " + mission.missionFile + " schema " + std::string(ota.schemas[schemaIndex].type));
                MissionRunResult r;
                REQUIRE_NOTHROW(r = runSchema(
                    pack, c.displayName, c.descriptor.campaignSide, mission,
                    ota, schemaIndex, ota.schemas[schemaIndex].type, vfs, 1800));
                for (const auto& f : r.spawnFailures)
                {
                    REQUIRE(f.find("missing unit definition") != std::string::npos);
                }
                for (const auto& e : r.errors)
                {
                    LOG_INFO << "C11 BT extended error " << mission.missionFile << ": " << e;
                }
                for (const auto& u : r.unsupportedCommandStrings)
                {
                    LOG_INFO << "C11 BT extended unsupported " << mission.missionFile << ": " << u;
                }
                // corrosive-water missions must actually corrode the
                // units standing below sea level inside the window
                // (Cleanup on Kral waterdamage=100; Less Refreshing
                // Sea waterdamage=6)
                if (ota.waterDoesDamage && ota.waterDamage != 0)
                {
                    REQUIRE(r.corrosiveDamageApplications > 0);
                }
                UNSCOPED_INFO("bt extended " << mission.missionFile
                    << " spawned=" << r.unitsSpawned
                    << " scripted=" << r.scriptedUnits
                    << " installed=" << r.installedOrders
                    << " unsupported=" << r.unsupportedCommands
                    << " errors=" << r.bootstrapErrors
                    << " unresolvedFeatures=" << r.unresolvedFeatures
                    << " result=" << static_cast<int>(r.result)
                    << " meteorWeapon=" << r.meteorWeaponType
                    << " meteorHits=" << r.meteorHits
                    << " corrosion=" << r.corrosiveDamageApplications);
                LOG_INFO << "C11 BT extended " << mission.missionFile
                         << " schema=" << ota.schemas[schemaIndex].type
                         << " spawned=" << r.unitsSpawned
                         << " scripted=" << r.scriptedUnits
                         << " installed=" << r.installedOrders
                         << " unsupported=" << r.unsupportedCommands
                         << " errors=" << r.bootstrapErrors
                         << " unresolvedFeatures=" << r.unresolvedFeatures
                         << " result=" << static_cast<int>(r.result)
                         << " meteorWeapon=" << r.meteorWeaponType
                         << " corrosion=" << r.corrosiveDamageApplications
                         << " meteorHits=" << r.meteorHits;
                ++runs;
            }
        }
        REQUIRE(runs == 15);
    }
    TEST_CASE("all missions containing explosive units run a bounded native combat window", "[kamikaze-missions]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        if (!data || !std::filesystem::exists(data)) { SKIP("RWE_DATA_PATH required for mission gate"); }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_kamikaze_missions.log", false));
        auto vfs = constructVfs(data);
        auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        unsigned int runs = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                bool exposed = false;
                unsigned int largest = 0;
                for (unsigned int i = 0; i < ota.schemas.size(); ++i)
                {
                    if (ota.schemas[i].units.size() > ota.schemas[largest].units.size()) { largest = i; }
                    for (const auto& placed : ota.schemas[i].units)
                    {
                        auto def = pack.unitDefinitions.find(toUpper(placed.unitName));
                        if (def != pack.unitDefinitions.end() && def->second.kamikaze) { exposed = true; }
                    }
                }
                if (!exposed) { continue; }
                const auto& schema = ota.schemas[largest];
                INFO(mission.missionFile + "/" + schema.type);
                const int ticks = toLower(mission.missionFile) == "crawling destruction.ota" ? 1800 : 900;
                MissionRunResult result;
                REQUIRE_NOTHROW(result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, largest, schema.type, vfs, ticks));
                REQUIRE(result.unitsSpawned > 0);
                LOG_INFO << "KAM mission=" << mission.missionFile << " schema=" << schema.type << " ticks=" << ticks
                    << " spawned=" << result.unitsSpawned << " installed=" << result.installedOrders
                    << " errors=" << result.bootstrapErrors << " unsupported=" << result.unsupportedCommands
                    << " explosiveDeaths=" << result.explosiveDeaths << " distinctSelfBlasts=" << result.distinctSelfDestructBlasts
                    << " result=" << static_cast<int>(result.result);
                ++runs;
            }
        }
        REQUIRE(runs == 30);
    }

    // Opt-in diagnostics only: no production switch or changed mission assertion.
    TEST_CASE("EXP1CC12 Hard veterancy tick trace", "[.][veterancy-trace]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        const char* output = std::getenv("RWE_VETERANCY_TRACE_FILE");
        REQUIRE(data); REQUIRE(output); REQUIRE_FALSE(std::filesystem::exists(output));
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_veterancy_trace.log", false));
        auto vfs = constructVfs(data); auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        std::string mode = std::getenv("RWE_VETERANCY_TRACE_MODE") ? std::getenv("RWE_VETERANCY_TRACE_MODE") : "retail";
        REQUIRE((mode == "retail" || mode == "phase6" || mode == "zero" || mode == "only-krog" || mode == "zero-krog"));
        std::ofstream trace(output);
        REQUIRE(trace.is_open());
        std::map<unsigned int, nlohmann::json> last;
        std::size_t eventCursor = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                if (toLower(mission.missionFile) != "exp1cc12.ota") { continue; }
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                auto schema = findCampaignSchemaIndex(ota, "Hard");
                auto result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, schema, "Hard", vfs, 1800, false, false, false,
                    [&](GameSimulation& sim, unsigned int tick) {
                        if (tick == 0)
                        {
                            for (auto& [id, u] : sim.units)
                            {
                                // Reproduce the removed Phase 6 initialization only
                                // in this explicit diagnostic fixture, never gameplay.
                                const auto authored = static_cast<uint16_t>(u.missionState ? u.missionState->kills.value_or(0) : 0);
                                if (mode == "phase6") { u.kills = authored; }
                                if (mode == "zero") { u.kills = 0; }
                                if (mode == "only-krog") { u.kills = u.unitType == "CORKROG" ? authored : 0; }
                                if (mode == "zero-krog") { u.kills = u.unitType == "CORKROG" ? 0 : authored; }
                            }
                        }
                        nlohmann::json frame{{"tick", tick}, {"hash", sim.computeHash().value}};
                        auto rng = sim.rng; frame["rng_next"] = rng();
                        unsigned int totalKills = 0;
                        auto units = nlohmann::json::array();
                        std::set<unsigned int> present;
                        for (const auto& [id, u] : sim.units)
                        {
                            totalKills += u.kills; present.insert(id.value);
                            auto state = dumpJson(u);
                            state["velocity"] = dumpJson(u.position - u.previousPosition);
                            state["weapon_states"] = nlohmann::json::array();
                            for (const auto& weapon : u.weapons)
                            {
                                if (!weapon) { state["weapon_states"].push_back(nullptr); continue; }
                                nlohmann::json w{{"state", weapon->state.index()}};
                                if (const auto* attack = std::get_if<UnitWeaponStateAttacking>(&weapon->state))
                                {
                                    w["target"] = dumpJson(attack->target); w["phase"] = attack->attackInfo.index();
                                    if (const auto* aim = std::get_if<UnitWeaponStateAttacking::AimInfo>(&attack->attackInfo))
                                    {
                                        w["heading"] = aim->lastHeading.value; w["pitch"] = aim->lastPitch.value;
                                    }
                                    if (const auto* fire = std::get_if<UnitWeaponStateAttacking::FireInfo>(&attack->attackInfo))
                                    {
                                        w["heading"] = fire->heading.value; w["pitch"] = fire->pitch.value;
                                        w["aim"] = dumpJson(fire->targetPosition); w["bursts"] = fire->burstsFired;
                                        w["ready"] = fire->readyTime.value;
                                    }
                                }
                                state["weapon_states"].push_back(std::move(w));
                            }
                            if (last.find(id.value) == last.end() || last.at(id.value) != state)
                            {
                                units.push_back({{"id", id.value}, {"state", state}}); last[id.value] = std::move(state);
                            }
                        }
                        frame["without_kills_hash"] = sim.computeHash().value - totalKills;
                        frame["units"] = std::move(units); frame["removed"] = nlohmann::json::array();
                        for (auto i = last.begin(); i != last.end();)
                        {
                            if (!present.count(i->first)) { frame["removed"].push_back(i->first); i = last.erase(i); }
                            else { ++i; }
                        }
                        frame["projectiles"] = nlohmann::json::array();
                        for (const auto& [id, p] : sim.projectiles)
                        {
                            auto state = dumpJson(p); state["weaponType"] = p.weaponType;
                            state["firedBy"] = dumpJson(p.firedBy); state["targetUnit"] = dumpJson(p.targetUnit);
                            state["targetPosition"] = dumpJson(p.targetPosition); state["damage"] = p.damage;
                            frame["projectiles"].push_back({{"id", id.value}, {"state", state}});
                        }
                        frame["events"] = nlohmann::json::array();
                        while (eventCursor < sim.events.size())
                        {
                            const auto& event = sim.events[eventCursor++];
                            if (const auto* death = std::get_if<UnitDiedEvent>(&event))
                            {
                                frame["events"].push_back({{"death", death->unitId.value}, {"type", death->unitType},
                                    {"position", dumpJson(death->position)}, {"weapon", death->explosionWeapon}});
                            }
                            if (const auto* impact = std::get_if<ProjectileDiedEvent>(&event))
                            {
                                frame["events"].push_back({{"impact", impact->projectileId.value}, {"weapon", impact->weaponType},
                                    {"position", dumpJson(impact->position)}, {"cause", static_cast<int>(impact->deathType)}});
                            }
                        }
                        trace << frame.dump() << '\n';
                    });
                trace << nlohmann::json{{"result", static_cast<int>(result.result)}, {"releases", result.bombReleases}, {"impacts", result.bombImpacts}}.dump() << '\n';
                return;
            }
        }
        FAIL("EXP1CC12 missing from mounted corpus");
    }

    TEST_CASE("all missions containing bombers run a bounded native combat window", "[bomber-missions]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        if (!data || !std::filesystem::exists(data)) { SKIP("RWE_DATA_PATH required for mission gate"); }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_bomber_missions.log", false));
        auto vfs = constructVfs(data);
        auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        unsigned int runs = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                bool exposed = false;
                unsigned int largest = 0;
                for (unsigned int i = 0; i < ota.schemas.size(); ++i)
                {
                    if (ota.schemas[i].units.size() > ota.schemas[largest].units.size()) { largest = i; }
                    for (const auto& placed : ota.schemas[i].units)
                    {
                        auto def = pack.unitDefinitions.find(toUpper(placed.unitName));
                        if (def != pack.unitDefinitions.end() && def->second.canFly && !def->second.weapon1.empty()
                            && std::holds_alternative<ProjectilePhysicsTypeDropped>(pack.weaponDefinitions.at(toUpper(def->second.weapon1)).physicsType)) { exposed = true; }
                    }
                }
                if (!exposed) { continue; }
                const auto& schema = ota.schemas[largest];
                INFO(mission.missionFile + "/" + schema.type);
                const int ticks = 1800;
                MissionRunResult result;
                REQUIRE_NOTHROW(result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, largest, schema.type, vfs, ticks));
                REQUIRE(result.unitsSpawned > 0);
                // These untouched authored starts actually attack within this window.
                const auto missionName = toLower(mission.missionFile);
                if (missionName == "charging the line.ota" || missionName == "air assault.ota"
                    || missionName == "exp1cc12.ota" || missionName == "cc06.ota")
                {
                    REQUIRE(result.bombReleases > 0);
                    REQUIRE(result.bombImpacts > 0);
                }
                REQUIRE(result.bombImpacts <= result.bombReleases);
                LOG_INFO << "BOMBER mission=" << mission.missionFile << " schema=" << schema.type << " ticks=" << ticks
                    << " spawned=" << result.unitsSpawned << " installed=" << result.installedOrders
                    << " errors=" << result.bootstrapErrors << " unsupported=" << result.unsupportedCommands
                    << " releases=" << result.bombReleases << " impacts=" << result.bombImpacts
                    << " result=" << static_cast<int>(result.result);
                ++runs;
            }
        }
        REQUIRE(runs == 37);
    }

    TEST_CASE("all missions containing air service facilities run controlled landing probes", "[air-service-missions]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        if (!data || !std::filesystem::exists(data)) { SKIP("RWE_DATA_PATH required for mission gate"); }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_service_missions.log", false));
        auto vfs = constructVfs(data);
        auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        unsigned int runs = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                bool exposed = false;
                unsigned int largest = 0;
                for (unsigned int i = 0; i < ota.schemas.size(); ++i)
                {
                    if (ota.schemas[i].units.size() > ota.schemas[largest].units.size()) { largest = i; }
                    for (const auto& placed : ota.schemas[i].units)
                    {
                        auto def = pack.unitDefinitions.find(toUpper(placed.unitName));
                        if (def != pack.unitDefinitions.end() && def->second.isAirBase) { exposed = true; }
                    }
                }
                if (!exposed) { continue; }
                const auto& schema = ota.schemas[largest];
                INFO(mission.missionFile + "/" + schema.type);
                const int ticks = 2400;
                MissionRunResult result;
                REQUIRE_NOTHROW(result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, largest, schema.type, vfs, ticks, true));
                REQUIRE(result.unitsSpawned > 0);
                REQUIRE(result.serviceFacilities > 0);
                REQUIRE(result.serviceHealed <= result.serviceDocked);
                REQUIRE(result.serviceDocked > 0);
                REQUIRE(result.serviceHealed > 0);
                REQUIRE(result.serviceDocked == result.serviceProbes);
                REQUIRE(result.serviceHealed == result.serviceProbes);
                for (const auto& error : result.errors)
                {
                    LOG_INFO << "SERVICE bootstrap mission=" << mission.missionFile << " error=" << error;
                }
                LOG_INFO << "SERVICE mission=" << mission.missionFile << " schema=" << schema.type << " ticks=" << ticks
                    << " spawned=" << result.unitsSpawned << " errors=" << result.bootstrapErrors
                    << " spawn_failures=" << result.spawnFailures.size() << " unsupported=" << result.unsupportedCommands
                    << " facilities=" << result.serviceFacilities << " aircraft=" << result.serviceAircraft
                    << " probes=" << result.serviceProbes << " supplemental=" << result.serviceSupplementalAircraft << " docked=" << result.serviceDocked << " healed=" << result.serviceHealed
                    << " result=" << static_cast<int>(result.result);
                ++runs;
            }
        }
        REQUIRE(runs == 50);
    }

    TEST_CASE("all 175 mission environments initialize and exercise feature lifecycle probes", "[features][feature-missions]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        if (!data || !std::filesystem::exists(data)) { SKIP("RWE_DATA_PATH required for mission gate"); }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_feature_missions.log", false));
        auto vfs = constructVfs(data); auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        unsigned int missions = 0, exposed = 0, probes = 0, transitions = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                auto schema = findCampaignSchemaIndex(ota, "Medium");
                INFO(mission.missionFile);
                auto result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, schema, ota.schemas[schema].type, vfs, 400, false, true);
                REQUIRE(result.featureTransitions == result.featureProbes);
                REQUIRE(result.unitsSpawned > 0);
                ++missions; exposed += result.destructibleFeatures != 0;
                probes += result.featureProbes; transitions += result.featureTransitions;
                LOG_INFO << "FEATURE mission=" << mission.missionFile << " schema=" << ota.schemas[schema].type
                    << " placed=" << result.mapFeatures << " destructible=" << result.destructibleFeatures
                    << " blocking=" << result.blockingFeatures << " reclaimable=" << result.reclaimableFeatures
                    << " burnable=" << result.burnableFeatures << " probes=" << result.featureProbes
                    << " transitions=" << result.featureTransitions << " reclaims=" << result.featureReclaims
                    << " burns=" << result.featureBurns << " errors=" << result.bootstrapErrors
                    << " spawn_failures=" << result.spawnFailures.size() << " unsupported=" << result.unsupportedCommands
                    << " objective_before=" << static_cast<int>(result.featureResultBefore)
                    << " objective_after=" << static_cast<int>(result.result);
            }
        }
        REQUIRE(missions == 175); REQUIRE(exposed == 159); REQUIRE(transitions == probes);
    }

    TEST_CASE("all 175 missions start with zero live kills and execute dynamic veterancy", "[veterancy][veterancy-missions]")
    {
        const char* data = std::getenv("RWE_DATA_PATH");
        if (!data || !std::filesystem::exists(data)) { SKIP("RWE_DATA_PATH required for mission gate"); }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_veterancy_missions.log", false));
        auto vfs = constructVfs(data); auto pack = loadDefinitionPack(vfs);
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        unsigned int missions = 0, exposed = 0;
        for (const auto& campaign : campaigns)
        {
            for (const auto& mission : campaign.descriptor.missions)
            {
                auto bytes = vfs.readFile("maps/" + mission.missionFile); REQUIRE(bytes);
                auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
                bool hasKills = false;
                for (const auto& schema : ota.schemas)
                {
                    for (const auto& u : schema.units) { hasKills |= u.kills.value_or(0) != 0; }
                }
                auto schema = findCampaignSchemaIndex(ota, "Medium");
                const bool extended = std::getenv("RWE_VETERANCY_LONG_MISSIONS") != nullptr;
                if (extended && hasKills)
                {
                    // Exercise ignored Kills metadata even when it exists only on
                    // another difficulty; select the richest placement census.
                    auto countExperienced = [](const auto& candidate) {
                        return std::count_if(candidate.units.begin(), candidate.units.end(), [](const auto& u) { return u.kills.value_or(0) != 0; });
                    };
                    for (unsigned int i = 0; i < ota.schemas.size(); ++i)
                    {
                        if (countExperienced(ota.schemas[i]) > countExperienced(ota.schemas[schema])) { schema = i; }
                    }
                }
                INFO(mission.missionFile);
                auto result = runSchema(pack, campaign.displayName, campaign.descriptor.campaignSide,
                    mission, ota, schema, ota.schemas[schema].type, vfs,
                    hasKills && extended ? 1800 : 120, false, false, true);
                REQUIRE(result.experienceMismatches == 0); REQUIRE(result.unitsSpawned > 0);
                ++missions; exposed += hasKills;
                LOG_INFO << "VETERANCY mission=\"" << mission.missionFile << "\" schema=" << ota.schemas[schema].type
                    << " experienced=" << result.initialExperienced << " veterans=" << result.initialVeterans
                    << " initial_kills=" << result.initialKills << " earned=" << result.earnedKills << " promotions=" << result.promotions
                    << " errors=" << result.bootstrapErrors << " spawn_failures=" << result.spawnFailures.size()
                    << " unsupported=" << result.unsupportedCommands << " objective=" << static_cast<int>(result.result);
            }
        }
        REQUIRE(missions == 175); REQUIRE(exposed == 39);
    }

#include "MissionCompletionAudit.inc"
#include "MissionCompletionFollowup.inc"
#include "MissionCompletionPhase7C.inc"
}
