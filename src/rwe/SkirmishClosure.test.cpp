#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <rwe/LoadingScene_util.h>
#include <rwe/SkirmishSettings.h>
#include <rwe/game/GameScene_util.h>
#include <rwe/io/_3do/_3do.h>
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
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/movement.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/UnitOrder.h>
#include <rwe/sim/UnitState_util.h>
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
         * The media-free half of LoadingScene::loadDefinitions, same
         * shape as the campaign closure harness: parse every retail
         * FBI/3DO/COB/weapon/feature once so each map's GameSimulation
         * can be built cheaply per schema.
         */
        struct SkirmishDefinitionPack
        {
            MovementClassDatabase movementClassDatabase;
            std::unordered_map<std::string, UnitDefinition> unitDefinitions;
            std::unordered_map<std::string, UnitModelDefinition> unitModelDefinitions;
            std::unordered_map<std::string, CobScript> unitScriptDefinitions;
            std::unordered_map<std::string, WeaponDefinition> weaponDefinitions;
            std::unordered_map<std::string, FeatureTdf> featureTdfs;
        };

        SkirmishDefinitionPack loadSkirmishDefinitionPack(AbstractVirtualFileSystem& vfs)
        {
            SkirmishDefinitionPack pack;

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
            }

            // match LoadingScene::loadDefinitions: the balance
            // override applies after all retail FBIs are loaded
            applyTransplantBalanceOverrides(pack.unitDefinitions);

            // match LoadingScene: builder GUI pages flatten into each
            // builder's build list (unitdef +0x156); the AI's product
            // table is resolved against these names. Skirmish has no
            // useonly pass -- campaign applies that per-mission.
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
                    // index 0 is the page container, not a button; only
                    // BehaviorBuildButton gadgets are products --
                    // order-panel buttons can collide with unit names
                    // (CORBUILD is the build-mode radio)
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

        struct SkirmishInitResult
        {
            unsigned int startPosCount{0};
            unsigned int playersSpawned{0};
            unsigned int schemaUnitsDeclared{0};
            unsigned int unresolvedFeatures{0};
            bool commanderSpawnOk{true};
            bool resourcesOk{true};
            bool teamsOk{true};
            std::string firstFailure;
            // C13B: profile resolution and per-computer AI attachment
            bool aiProfileResolved{false};
            std::string aiProfileResolvedTo;
            unsigned int aiPlayersExpected{0};
            unsigned int aiStatesAttached{0};
            unsigned int aiPlayersActive{0};
            std::string aiInactiveDetail;
            // latched during ticking: per-player bit i set once any
            // owned unit carries an order -- a roam move that completes
            // before the snapshot must not read as "never acted"
            std::unordered_set<int> aiPlayersEverOrdered;
            // latched during ticking: an AI combat squad received an
            // AttackOrder at least once (the target may be dead and
            // the order gone by the end-of-run snapshot)
            bool aiAttackOrderSeen{false};
        };

        /**
         * One skirmish schema init following the C13A LoadingScene
         * path: terrain + defs + skirmish LOS/mapping/commanderDeath/
         * difficulty runtime settings + a player table carrying ally
         * groups + commander spawn at each occupied slot's declared
         * StartPos with retail's unconditional overlap placement,
         * the C13B per-computer AI attach, then a short tick slice.
         */
        /**
         * Which ai/ file the schema's aiprofile actually resolves to
         * through the retail fallback chain, for census reporting.
         */
        std::string resolveAiProfileName(AbstractVirtualFileSystem& vfs, const std::string& profileName)
        {
            for (const auto& candidate : {profileName, profileName + ".TXT", std::string("DEFAULT"), std::string("DEFAULT.TXT")})
            {
                if (vfs.readFile("ai/" + candidate))
                {
                    return candidate;
                }
            }
            return "";
        }

        /**
         * A built skirmish sim plus its run bookkeeping. Returned by
         * value -- the deep smoke test inspects post-run unit state.
         */
        struct SkirmishRun
        {
            GameSimulation sim;
            SkirmishInitResult result;
            std::vector<PlayerId> playerIds;
        };

        SkirmishRun buildAndTickSkirmishSchema(
            const SkirmishDefinitionPack& pack,
            const std::string& mapName,
            const OtaRecord& ota,
            unsigned int schemaIndex,
            unsigned int playerCount,
            AbstractVirtualFileSystem& vfs,
            int tickCount,
            SkirmishDifficulty difficulty,
            const std::function<void(GameSimulation&)>& postSpawnHook = {})
        {
            SkirmishInitResult out;

            const auto& schema = ota.schemas.at(schemaIndex);

            auto tntBytes = vfs.readFile("maps/" + mapName + ".tnt");
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

            // C13A skirmish settings path: the frontend toggles drive
            // the live fog/mapping/commanderDeath globals, and the
            // Difficulty stage drives the computer economy scale
            sim.lineOfSightEnabled = true;
            sim.lineOfSightMode = static_cast<int>(SkirmishLineOfSight::True);
            sim.mappingEnabled = true;
            sim.commanderDeathContinues = true;
            sim.computerResourceScale = skirmishDifficultyComputerResourceScale(difficulty);

            sim.noSeaLevelTrigger = ota.noSeaLevelTrigger;
            sim.waterDoesDamage = ota.waterDoesDamage;
            sim.waterDamage = ota.waterDamage;

            sim.unitDefinitions = pack.unitDefinitions;
            sim.weaponDefinitions = pack.weaponDefinitions;
            sim.movementClassDatabase = pack.movementClassDatabase;
            sim.unitModelDefinitions = pack.unitModelDefinitions;
            sim.unitScriptDefinitions = pack.unitScriptDefinitions;

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

            auto startPosList = collectStartPositions(schema);
            out.startPosCount = static_cast<unsigned int>(startPosList.size());
            out.schemaUnitsDeclared = static_cast<unsigned int>(schema.units.size());

            playerCount = std::min<unsigned int>(playerCount, out.startPosCount);
            REQUIRE(playerCount >= 2);

            // C13A: ally groups propagate through the player table.
            // Alternate groups so roughly half the table shares an
            // alliance; slot 0 stays ungrouped.
            std::vector<PlayerId> playerIds;
            for (unsigned int i = 0; i < playerCount; ++i)
            {
                bool isHuman = (i == 0);
                auto metal = Metal(isHuman ? schema.humanMetal : schema.computerMetal);
                auto energy = Energy(isHuman ? schema.humanEnergy : schema.computerEnergy);
                std::optional<unsigned int> allyGroup;
                if (i >= 2)
                {
                    allyGroup = i % 2;
                }
                GamePlayerInfo gpi{
                    std::nullopt,
                    isHuman ? GamePlayerType::Human : GamePlayerType::Computer,
                    PlayerColorIndex(i),
                    GamePlayerStatus::Alive,
                    isHuman ? "ARM" : "CORE",
                    allyGroup,
                    metal,
                    energy,
                    metal,
                    energy,
                    metal,
                    energy};
                playerIds.push_back(addSkirmishPlayer(sim, gpi));
            }

            // the difficulty-scaled grant must have landed on every
            // computer slot; the human slot stays unscaled
            for (unsigned int i = 0; i < playerCount; ++i)
            {
                const auto& p = sim.players[i];
                if (i >= 2 && p.allyGroup != std::optional<unsigned int>(i % 2))
                {
                    out.teamsOk = false;
                }
                if (p.type == GamePlayerType::Computer)
                {
                    auto expectedMetal = Metal(schema.computerMetal * 0.7f);
                    auto expectedEnergy = Energy(schema.computerEnergy * 0.7f);
                    if (p.metal != expectedMetal || p.energy != expectedEnergy)
                    {
                        out.resourcesOk = false;
                    }
                }
                else if (p.metal != Metal(schema.humanMetal) || p.energy != Energy(schema.humanEnergy))
                {
                    out.resourcesOk = false;
                }
            }

            // C13B: retail bootstrap path -- resolve the schema's
            // aiprofile (ai\<name>[.txt], ai\default.txt fallback) and
            // attach one AI state per computer player. Humans never
            // receive one; allies/enemies ride the C13A ally groups.
            out.aiProfileResolvedTo = resolveAiProfileName(vfs, schema.aiProfile);
            if (auto profileText = loadCampaignAiProfileText(vfs, "ai", schema.aiProfile))
            {
                out.aiProfileResolved = true;
                attachCampaignAiPlayers(sim, parseCampaignAiProfile(*profileText, skirmishAiDifficultyIndex(difficulty)));
            }
            for (Index i = 0; i < getSize(sim.players); ++i)
            {
                if (sim.players[i].type == GamePlayerType::Computer)
                {
                    ++out.aiPlayersExpected;
                }
            }
            out.aiStatesAttached = static_cast<unsigned int>(sim.campaignAi.size());

            // exercise the same start-assignment path LoadingScene
            // uses: a dense physical slot table resolved through
            // assignSkirmishStartPosNums + findStartPosSpecial
            std::array<std::optional<PlayerInfo>, 10> lobbySlots;
            for (unsigned int i = 0; i < playerCount; ++i)
            {
                lobbySlots[i] = PlayerInfo{
                    std::nullopt,
                    PlayerControllerType(i == 0
                            ? PlayerControllerType(PlayerControllerTypeHuman())
                            : PlayerControllerType(PlayerControllerTypeComputer())),
                    "ARM",
                    PlayerColorIndex(i),
                    Metal(0),
                    Energy(0)};
            }
            auto startPosNums = assignSkirmishStartPosNums(
                lobbySlots, SkirmishStartLocation::Fixed, sim.rng);

            for (unsigned int i = 0; i < playerCount; ++i)
            {
                if (!startPosNums[i])
                {
                    out.commanderSpawnOk = false;
                    out.firstFailure = "no start assignment for slot " + std::to_string(i);
                    return {std::move(sim), std::move(out), std::move(playerIds)};
                }
                const auto* startPos = findStartPosSpecial(schema, *startPosNums[i]);
                if (startPos == nullptr)
                {
                    out.commanderSpawnOk = false;
                    out.firstFailure = "missing StartPos" + std::to_string(i + 1);
                    return {std::move(sim), std::move(out), std::move(playerIds)};
                }
                auto worldPos = sim.terrain.topLeftCoordinateToWorld(
                    SimVector(SimScalar(startPos->xPos), SimScalar(0), SimScalar(startPos->zPos)));
                worldPos.y = sim.terrain.getHeightAt(worldPos.x, worldPos.z);

                const char* commander = (i == 0) ? "ARMCOM" : "CORCOM";
                // retail spawns start units unconditionally; blocking
                // decorative features overlapping the footprint do not
                // reject the placement
                auto spawned = sim.trySpawnUnit(commander, playerIds[i], worldPos, std::nullopt, true);
                if (!spawned)
                {
                    out.commanderSpawnOk = false;
                    out.firstFailure = "commander spawn rejected at StartPos" + std::to_string(i + 1)
                        + " ota(" + std::to_string(startPos->xPos) + "," + std::to_string(startPos->zPos) + ")";
                    int sx = startPos->xPos / 16;
                    int sz = startPos->zPos / 16;
                    out.firstFailure += " height=" + std::to_string(static_cast<int>(simScalarToFloat(worldPos.y)))
                        + " sealevel=" + std::to_string(static_cast<int>(simScalarToFloat(sim.terrain.getSeaLevel())));
                    for (const auto& [pos, name] : features)
                    {
                        auto dx = static_cast<int>(pos.x) - sx;
                        auto dy = static_cast<int>(pos.y) - sz;
                        if (std::abs(dx) <= 6 && std::abs(dy) <= 6)
                        {
                            auto fid = sim.tryGetFeatureDefinitionId(name);
                            bool blocking = false;
                            if (fid)
                            {
                                blocking = sim.getFeatureDefinition(*fid).blocking;
                            }
                            out.firstFailure += " feat(" + name + (blocking ? " blocking" : " passable")
                                + " @" + std::to_string(pos.x) + "," + std::to_string(pos.y) + ")";
                        }
                    }
                    return {std::move(sim), std::move(out), std::move(playerIds)};
                }
                auto& unit = sim.getUnitState(*spawned);
                unit.finishBuilding(sim.unitDefinitions.at(unit.unitType));
                ++out.playersSpawned;
            }

            if (postSpawnHook)
            {
                postSpawnHook(sim);
            }

            for (int i = 0; i < tickCount; ++i)
            {
                sim.tick();

                for (const auto& st : sim.campaignAi)
                {
                    if (!out.aiPlayersEverOrdered.count(st.player.value))
                    {
                        for (const auto& [id, unit] : sim.units)
                        {
                            if (unit.owner == st.player && !unit.orders.empty())
                            {
                                out.aiPlayersEverOrdered.insert(st.player.value);
                                break;
                            }
                        }
                    }
                }

                if (!out.aiAttackOrderSeen && i % 60 == 0)
                {
                    for (const auto& st : sim.campaignAi)
                    {
                        if (st.attackTarget)
                        {
                            out.aiAttackOrderSeen = true;
                            break;
                        }
                        for (const auto& [id, unit] : sim.units)
                        {
                            if (unit.owner != st.player)
                            {
                                continue;
                            }
                            if (std::any_of(unit.orders.begin(), unit.orders.end(), [](const auto& o) {
                                    return std::holds_alternative<AttackOrder>(o);
                                }))
                            {
                                out.aiAttackOrderSeen = true;
                                break;
                            }
                        }
                    }
                }
            }

            // C13B evidence: an AI player counts as active once any
            // owned unit carried an order during the run (latched
            // above) or it already owns a completed second unit --
            // a commander between builder scans would otherwise read
            // as inactive on a short horizon
            for (const auto& st : sim.campaignAi)
            {
                unsigned int owned = 0;
                std::string ownedTypes;
                for (const auto& [id, unit] : sim.units)
                {
                    if (unit.owner != st.player || unit.isDead())
                    {
                        continue;
                    }
                    ++owned;
                    ownedTypes += unit.unitType + " ";
                }
                if (out.aiPlayersEverOrdered.count(st.player.value) || owned > 1)
                {
                    ++out.aiPlayersActive;
                }
                else
                {
                    out.aiInactiveDetail += " p" + std::to_string(st.player.value)
                        + " owned=" + std::to_string(owned)
                        + " (" + ownedTypes + ")";
                }
            }

            return {std::move(sim), std::move(out), std::move(playerIds)};
        }

        SkirmishInitResult runSkirmishSchema(
            const SkirmishDefinitionPack& pack,
            const std::string& mapName,
            const OtaRecord& ota,
            unsigned int schemaIndex,
            unsigned int playerCount,
            AbstractVirtualFileSystem& vfs,
            int tickCount = 60,
            SkirmishDifficulty difficulty = SkirmishDifficulty::Medium)
        {
            auto run = buildAndTickSkirmishSchema(pack, mapName, ota, schemaIndex, playerCount, vfs, tickCount, difficulty);
            return run.result;
        }
    }

    /**
     * C13A closure harness: every retail skirmish-eligible map (any
     * schema whose Type is `Network N`) must resolve a Network schema
     * per legal player count, initialize a full player table with
     * ally groups and difficulty-scaled computer resources, spawn
     * each player's commander at its StartPos (unconditionally, as
     * retail does), and tick without error.
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("retail skirmish maps resolve, initialize and tick")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_skirmish_smoke.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        unsigned int networkMaps = 0;
        unsigned int schemasRun = 0;
        std::vector<std::string> failures;
        std::unordered_map<std::string, unsigned int> startPosHistogram;
        unsigned int mapsWithSchemaUnits = 0;
        unsigned int totalSchemaUnits = 0;
        unsigned int resolutionChecks = 0;
        // C13B profile census: aiprofile value -> resolved ai/ file
        std::map<std::string, std::map<std::string, unsigned int>> profileCensus;

        auto otaNames = vfs.getFileNames("maps", ".ota");
        REQUIRE(!otaNames.empty());
        for (const auto& fileName : otaNames)
        {
            auto otaRaw = vfs.readFile("maps/" + fileName);
            if (!otaRaw)
            {
                failures.push_back(fileName + ": unreadable OTA");
                continue;
            }
            auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));

            bool hasNetworkSchema = false;
            unsigned int maxStartPos = 0;
            for (const auto& schema : ota.schemas)
            {
                if (schema.type.compare(0, 7, "Network") == 0)
                {
                    hasNetworkSchema = true;
                    maxStartPos = std::max(maxStartPos, countStartPositions(schema));
                }
            }
            if (!hasNetworkSchema)
            {
                continue;
            }
            ++networkMaps;

            std::string mapName = fileName.substr(0, fileName.size() - 4);

            // every feasible occupied count must resolve to a
            // Network schema that can seat it (or the largest
            // available); numplayers is not a legality list
            std::set<unsigned int> resolvedIndices;
            for (unsigned int n = 2; n <= 10; ++n)
            {
                unsigned int resolved;
                try
                {
                    resolved = resolveSkirmishSchemaIndex(ota, n);
                }
                catch (const std::exception& e)
                {
                    failures.push_back(mapName + ": resolver threw for " + std::to_string(n)
                        + " players: " + e.what());
                    continue;
                }
                const auto& resolvedSchema = ota.schemas.at(resolved);
                if (resolvedSchema.type.compare(0, 7, "Network") != 0)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(resolved)
                        + ": resolver picked non-Network schema " + resolvedSchema.type);
                }
                auto count = countStartPositions(resolvedSchema);
                auto needed = std::min(n, maxStartPos);
                if (count < needed)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(resolved)
                        + ": resolved schema seats " + std::to_string(count)
                        + " < " + std::to_string(needed) + " for " + std::to_string(n) + " players");
                }
                if (n <= maxStartPos)
                {
                    resolvedIndices.insert(resolved);
                }
                ++resolutionChecks;
            }

            auto legalCounts = parseNumPlayers(ota.numPlayers);
            for (auto n : legalCounts)
            {
                if (n > maxStartPos)
                {
                    LOG_INFO << "note: " << mapName << " numplayers declares " << n
                             << " but max StartPos is " << maxStartPos;
                }
            }

            // human-QA regression: a sparse lobby (Open, Human,
            // Computer) must pass Start validation on every map
            // whose resolved schema seats two players -- numplayers
            // never gates Start (TotalA.exe 0x47ae60-0x47b0f7)
            {
                std::array<std::optional<PlayerInfo>, 10> qaSlots;
                qaSlots[1] = PlayerInfo{
                    std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(0), Energy(0)};
                qaSlots[2] = PlayerInfo{
                    std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(0), Energy(0)};
                bool seatsTwo = countStartPositions(ota.schemas.at(resolveSkirmishSchemaIndex(ota, 2))) >= 2;
                auto rejection = checkSkirmishStart(qaSlots, ota);
                if (seatsTwo == static_cast<bool>(rejection))
                {
                    failures.push_back(mapName + ": sparse H+C Start: rejection "
                        + (rejection ? "'" + *rejection + "'" : "absent")
                        + " but the resolved schema " + (seatsTwo ? "seats 2" : "seats <2"));
                }
            }

            // full init once per distinct resolved schema: spawn the
            // largest legal Fixed table it can seat
            for (auto schemaIndex : resolvedIndices)
            {
                INFO("map " + mapName + " schema " + std::to_string(schemaIndex)
                    + " (" + ota.schemas[schemaIndex].type + ")");
                auto result = runSkirmishSchema(pack, mapName, ota, schemaIndex, 10, vfs);
                startPosHistogram[std::to_string(result.startPosCount)]++;
                totalSchemaUnits += result.schemaUnitsDeclared;
                profileCensus[ota.schemas[schemaIndex].aiProfile.empty() ? "<empty>" : ota.schemas[schemaIndex].aiProfile]
                             [result.aiProfileResolvedTo.empty() ? "<none>" : result.aiProfileResolvedTo]++;
                if (result.schemaUnitsDeclared != 0)
                {
                    ++mapsWithSchemaUnits;
                }
                if (!result.commanderSpawnOk)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": " + result.firstFailure);
                }
                if (!result.resourcesOk)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": scaled starting resources incorrect");
                }
                if (!result.teamsOk)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": ally groups did not propagate");
                }
                // C13B: retail bootstrap semantics -- the aiprofile
                // always resolves through the default.txt fallback and
                // every computer slot receives exactly one AI state
                if (!result.aiProfileResolved)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": aiprofile '" + ota.schemas[schemaIndex].aiProfile + "' unresolved");
                }
                if (result.aiStatesAttached != result.aiPlayersExpected)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": AI states " + std::to_string(result.aiStatesAttached)
                        + " != computer players " + std::to_string(result.aiPlayersExpected));
                }
                if (result.aiPlayersActive != result.aiStatesAttached)
                {
                    failures.push_back(mapName + "/schema" + std::to_string(schemaIndex)
                        + ": only " + std::to_string(result.aiPlayersActive)
                        + " of " + std::to_string(result.aiStatesAttached) + " AI players acted:"
                        + result.aiInactiveDetail);
                }
                ++schemasRun;
            }
        }

        LOG_INFO << "skirmish corpus: " << networkMaps << " network-schema maps, "
                 << schemasRun << " resolved schemas initialized, "
                 << resolutionChecks << " resolver checks, "
                 << mapsWithSchemaUnits << " schemas declaring pre-placed units ("
                 << totalSchemaUnits << " units)";
        for (const auto& [count, n] : startPosHistogram)
        {
            LOG_INFO << "  startpos " << count << ": " << n << " schemas";
        }
        // C13B profile census: every declared aiprofile value and the
        // ai/ file it actually resolves to through the fallback chain
        for (const auto& [declared, resolved] : profileCensus)
        {
            for (const auto& [file, n] : resolved)
            {
                LOG_INFO << "  aiprofile '" << declared << "' -> ai/" << file << " x" << n;
            }
        }
        for (const auto& f : failures)
        {
            LOG_INFO << "FAIL " << f;
            WARN(f);
        }

        REQUIRE(networkMaps == 112);
        // C13A: retail places start commanders unconditionally, so the
        // C12 blocking-feature rejection set is closed -- zero failures
        REQUIRE(failures.empty());
    }

    /**
     * Regression for C13B factory-spawn work: a nanoframe spawns
     * inside its parent's open yard, so spawn collision checks must
     * exclude the factory's own cells or every product request is
     * rejected. A lone CORLAB on real retail terrain must complete a
     * queued CORAK and release it off the pad.
     */
    TEST_CASE("skirmish factory produces a mobile unit on real terrain")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_skirmish_ai_smoke.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        auto otaRaw = vfs.readFile("maps/Starfish Isle.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        auto run = buildAndTickSkirmishSchema(pack, "Starfish Isle", ota, schemaIndex, 2, vfs, 30, SkirmishDifficulty::Medium);
        auto& sim = run.sim;

        // drop a fresh CORLAB on open ground near the AI start
        const auto& schema = ota.schemas.at(schemaIndex);
        PlayerId aiPlayer = run.playerIds.back();
        const auto* startPos = findStartPosSpecial(schema, 2);
        REQUIRE(startPos);
        auto worldPos = sim.terrain.topLeftCoordinateToWorld(
            SimVector(SimScalar(startPos->xPos), SimScalar(0), SimScalar(startPos->zPos)));
        auto cell = sim.terrain.worldToHeightmapCoordinateNearest(worldPos);
        auto labPos = sim.terrain.heightmapIndexToWorldCenter(cell.x + 8, cell.y);
        auto labId = sim.trySpawnUnit("CORLAB", aiPlayer, labPos, std::nullopt);
        REQUIRE(labId);
        auto& lab = sim.getUnitState(*labId);
        lab.finishBuilding(sim.unitDefinitions.at("CORLAB"));
        lab.inBuildStance = true;
        lab.buildQueue.emplace_back("CORAK", 1);

        int produced = 0;
        for (int i = 0; i < 3000 && produced == 0; ++i)
        {
            sim.tick();
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.unitType == "CORAK")
                {
                    ++produced;
                }
            }
        }
        REQUIRE(produced >= 1);
    }

    /**
     * C13B Tier-2: a representative slice of the corpus gets a deep
     * run. Each map starts a 2-player skirmish (1 human + 1 computer)
     * at Medium; the computer must demonstrate the retail AI path
     * end-to-end: attach -> commander builds -> economy -> factory ->
     * mobile production -> combat orders. Long horizons because real
     * build times apply (~300 ticks/sourced-second at commander
     * workerTime). Skips without RWE_DATA_PATH.
     */
    TEST_CASE("skirmish AI becomes operational on representative maps")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_skirmish_ai_smoke.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        struct MapSpec
        {
            const char* otaName;
            const char* note;
            int ticks;
            /**
             * Seed each computer player with a complete armed squad at
             * spawn, so the run deterministically exercises repartition
             * -> combat reserves -> attackScan -> pickAttackTarget ->
             * AttackOrder within a short horizon. Organic production to
             * the retail muster band is map- and stall-dependent, so it
             * is left to the unseeded maps as a progress diagnostic.
             */
            bool seedSquad;
        };
        const std::vector<MapSpec> maps = {
            // ordinary land map, default profile
            {"Coast To Coast", "land/DEFAULT", 12000, false},
            // water map with the SeaBattle profile
            {"Metal Isles", "naval/SeaBattle", 12000, false},
            // multi-schema map; schema0 exercises the DEFAULT profile
            // on a metal-poor naval map (organic production is slow)
            {"Starfish Isle", "multi-schema/DEFAULT", 24000, false},
            // >2-player capable map with omitted aiprofile (fallback)
            {"Yerrot Mountains", "fallback/<empty>", 9000, false},
            // expansion .ufo provenance; dead `AIR` profile reference
            // exercises the default fallback end-to-end
            {"Metal Gridlock", "dead-ref->default", 9000, false},
            // seeded combat squad: proves the repartition -> reserves
            // -> attackScan -> AttackOrder path end-to-end on a real
            // map without depending on organic build pacing
            {"Greenhaven", "seeded-squad-attack", 6000, true},
        };

        std::vector<std::string> failures;
        for (const auto& spec : maps)
        {
            auto otaRaw = vfs.readFile(std::string("maps/") + spec.otaName + ".ota");
            if (!otaRaw)
            {
                LOG_INFO << "deep smoke: " << spec.otaName << " skipped (no ota)";
                continue;
            }
            auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
            unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

            std::function<void(GameSimulation&)> hook;
            if (spec.seedSquad)
            {
                hook = [&pack](GameSimulation& s) {
                    // one complete armed squad per computer player,
                    // ringed just outside its commander
                    for (const auto& st : s.campaignAi)
                    {
                        const UnitState* com = nullptr;
                        for (const auto& [id, u] : s.units)
                        {
                            if (u.owner == st.player && !u.isDead())
                            {
                                com = &u;
                                break;
                            }
                        }
                        REQUIRE(com);
                        unsigned int placed = 0;
                        for (int ring = 6; ring <= 12 && placed < 4; ++ring)
                        {
                            for (int k = 0; k < 8 && placed < 4; ++k)
                            {
                                auto cell = s.terrain.worldToHeightmapCoordinateNearest(com->position);
                                auto pos = s.terrain.heightmapIndexToWorldCenter(
                                    cell.x + (k % 3 - 1) * ring,
                                    cell.y + (k / 3 - 1) * ring);
                                pos.y = s.terrain.getHeightAt(pos.x, pos.z);
                                auto spawned = s.trySpawnUnit("CORAK", st.player, pos, std::nullopt, true);
                                if (!spawned)
                                {
                                    continue;
                                }
                                s.getUnitState(*spawned).finishBuilding(pack.unitDefinitions.at("CORAK"));
                                ++placed;
                            }
                        }
                        REQUIRE(placed == 4);
                    }
                };
            }

            auto run = buildAndTickSkirmishSchema(pack, spec.otaName, ota, schemaIndex, 2, vfs, spec.ticks, SkirmishDifficulty::Medium, hook);
            auto& sim = run.sim;

            INFO("map " << spec.otaName << " (" << spec.note << ")");
            REQUIRE(run.result.commanderSpawnOk);
            REQUIRE(run.result.aiStatesAttached == 1);
            REQUIRE(run.result.aiPlayersExpected == 1);
            REQUIRE(run.result.aiProfileResolved);

            const auto& st = sim.campaignAi.front();
            const auto& aiPlayer = sim.players[st.player.value];
            REQUIRE(aiPlayer.type == GamePlayerType::Computer);

            LOG_INFO << "  " << spec.otaName << " ai groups: builders=" << st.groups[AiGroupBuilder].size()
                     << " factories=" << st.groups[AiGroupFactory].size()
                     << " reserveA=" << st.groups[AiGroupReserveA].size()
                     << " reserveB=" << st.groups[AiGroupReserveB].size()
                     << " attackTarget=" << (st.attackTarget ? "set" : "none");

            unsigned int aiUnits = 0;
            unsigned int aiBuilders = 0;
            unsigned int aiFactories = 0;
            unsigned int aiMobileCombat = 0;
            unsigned int aiEconomy = 0;
            bool attackOrderSeen = run.result.aiAttackOrderSeen;
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != st.player || unit.isDead())
                {
                    continue;
                }
                ++aiUnits;
                const auto& def = sim.unitDefinitions.at(toUpper(unit.unitType));
                if (def.builder && def.isMobile)
                {
                    ++aiBuilders;
                }
                if (def.builder && !def.isMobile)
                {
                    ++aiFactories;
                }
                if (def.isMobile && !def.builder && def.canAttack)
                {
                    ++aiMobileCombat;
                }
                if (!def.isMobile
                    && (def.energyMake > Energy(0) || def.windGenerator > Energy(0) || def.tidalGenerator
                        || def.metalMake > Metal(0) || def.extractsMetal > Metal(0) || def.makesMetal > Metal(0)
                        || def.energyStorage > Energy(0) || def.metalStorage > Metal(0)))
                {
                    ++aiEconomy;
                }
                for (const auto& o : unit.orders)
                {
                    if (std::holds_alternative<AttackOrder>(o))
                    {
                        attackOrderSeen = true;
                    }
                }
            }
            if (st.attackTarget)
            {
                attackOrderSeen = true;
            }

            LOG_INFO << "deep smoke " << spec.otaName << " schema" << schemaIndex
                     << " profile '" << ota.schemas[schemaIndex].aiProfile << "' -> ai/"
                     << run.result.aiProfileResolvedTo
                     << " after " << spec.ticks << "t: units=" << aiUnits
                     << " builders=" << aiBuilders << " factories=" << aiFactories
                     << " econ=" << aiEconomy << " mobileCombat=" << aiMobileCombat
                     << " attackSeen=" << attackOrderSeen
                     << " metal=" << sim.players[st.player.value].metal.value
                     << " energy=" << sim.players[st.player.value].energy.value;

            // the AI must have grown beyond its lone commander
            if (aiUnits < 2)
            {
                failures.push_back(std::string(spec.otaName) + ": AI produced nothing (units="
                    + std::to_string(aiUnits) + ")");
            }
            // economy: at least one producer or storage built
            if (aiEconomy == 0)
            {
                failures.push_back(std::string(spec.otaName) + ": AI built no economy structure");
            }
            // a seeded squad must drive the attack path end-to-end:
            // reserves muster -> attackScan picks the nearest enemy ->
            // AttackOrder issued (latched during ticking)
            if (spec.seedSquad && !attackOrderSeen)
            {
                failures.push_back(std::string(spec.otaName) + ": seeded AI squad never received an attack order");
            }
        }
        for (const auto& f : failures)
        {
            LOG_INFO << "FAIL " << f;
            WARN(f);
        }
        REQUIRE(failures.empty());
    }

    TEST_CASE("skirmish resolver matches retail Network-schema semantics")
    {
        // retail TotalA.exe 0x436860 mode 3: candidates "Network 1".."4"
        // in order; last exact-StartPos-match wins, else the first
        // maximum-count candidate wins
        OtaRecord ota;
        auto makeSchema = [](const std::string& type, unsigned int startPos) {
            OtaSchema s;
            s.type = type;
            for (unsigned int i = 1; i <= startPos; ++i)
            {
                OtaSpecial sp;
                sp.specialWhat = "StartPos" + std::to_string(i);
                s.specials.push_back(sp);
            }
            return s;
        };

        SECTION("single Network 1 schema always resolves to index 0")
        {
            ota.schemas = {makeSchema("Network 1", 10)};
            for (unsigned int n = 2; n <= 10; ++n)
            {
                REQUIRE(resolveSkirmishSchemaIndex(ota, n) == 0);
            }
        }

        SECTION("exact StartPos match wins over a larger schema")
        {
            // KotH shape: N1 has 10, N2 has 3 -> n=3 resolves to N2
            ota.schemas = {makeSchema("Network 1", 10), makeSchema("Network 2", 3)};
            REQUIRE(resolveSkirmishSchemaIndex(ota, 3) == 1);
            REQUIRE(resolveSkirmishSchemaIndex(ota, 5) == 0);
            REQUIRE(resolveSkirmishSchemaIndex(ota, 10) == 0);
        }

        SECTION("with no exact match the largest StartPos count wins")
        {
            ota.schemas = {makeSchema("Network 1", 4), makeSchema("Network 2", 8)};
            REQUIRE(resolveSkirmishSchemaIndex(ota, 6) == 1);
            // n=2 matches neither; the argmax fallback takes the 8-seat
            // schema even though the request is smaller than both
            REQUIRE(resolveSkirmishSchemaIndex(ota, 2) == 1);
        }

        SECTION("the last exact match in scan order wins")
        {
            // two candidates seat 4: Network 2 is scanned after
            // Network 1 and retail's overwrite-on-accept keeps it
            ota.schemas = {makeSchema("Network 1", 4), makeSchema("Network 2", 4)};
            REQUIRE(resolveSkirmishSchemaIndex(ota, 4) == 1);
        }

        SECTION("zero-StartPos schemas are never selected")
        {
            ota.schemas = {makeSchema("Network 1", 0), makeSchema("Network 2", 6)};
            REQUIRE(resolveSkirmishSchemaIndex(ota, 4) == 1);
        }

        SECTION("throws when no Network schema declares StartPos")
        {
            ota.schemas = {makeSchema("Easy", 5), makeSchema("Network 1", 0)};
            REQUIRE_THROWS_AS(resolveSkirmishSchemaIndex(ota, 2), OtaParseException);
        }
    }

    /**
     * Retail Fixed start (TotalA.exe 0x497908-0x497972): the spawn
     * loop iterates the ten physical lobby slots and calls
     * spawn(slot=i, posNum=i); the lookup (0x437320) resolves the
     * StartPos special whose parsed ident equals i, i.e.
     * StartPos{i+1}. An open slot is skipped and never shifts later
     * assignments -- holes consume their StartPos number.
     */
    TEST_CASE("skirmish fixed start maps physical slot index to StartPos number")
    {
        // real schema path: parse an OTA-shaped specials block
        auto otaRoot = parseTdfFromString(R"(
            [Schema]
            {
                Type=Network 1;
                [specials]
                {
                    [special0] { specialwhat=StartPos1; XPos=100; ZPos=100; }
                    [special1] { specialwhat=StartPos2; XPos=200; ZPos=200; }
                    [special2] { specialwhat=StartPos3; XPos=300; ZPos=300; }
                    [special3] { specialwhat=StartPos4; XPos=400; ZPos=400; }
                }
            }
        )");
        auto schemaBlock = otaRoot.findBlock("Schema");
        REQUIRE(schemaBlock);
        auto schema = parseOtaSchema(schemaBlock->get());

        std::minstd_rand rng(12345);
        auto occupied = [](std::initializer_list<unsigned int> slots) {
            std::array<std::optional<PlayerInfo>, 10> players;
            for (auto i : slots)
            {
                players[i] = PlayerInfo{
                    std::nullopt,
                    PlayerControllerType(i == 0
                            ? PlayerControllerType(PlayerControllerTypeHuman())
                            : PlayerControllerType(PlayerControllerTypeComputer())),
                    "ARM",
                    PlayerColorIndex(i),
                    Metal(0),
                    Energy(0)};
            }
            return players;
        };

        SECTION("dense table: slot i occupies StartPos{i+1}")
        {
            auto players = occupied({0, 1});
            auto nums = assignSkirmishStartPosNums(players, SkirmishStartLocation::Fixed, rng);
            REQUIRE(nums[0] == std::optional<unsigned int>(0));
            REQUIRE(nums[1] == std::optional<unsigned int>(1));
            REQUIRE(findStartPosSpecial(schema, *nums[0])->xPos == 100);
            REQUIRE(findStartPosSpecial(schema, *nums[1])->xPos == 200);
        }

        SECTION("leading hole: slot 1 occupies StartPos2, slot 2 StartPos3")
        {
            auto players = occupied({1, 2});
            players[1]->controller = PlayerControllerTypeHuman();
            auto nums = assignSkirmishStartPosNums(players, SkirmishStartLocation::Fixed, rng);
            REQUIRE(!nums[0]);
            REQUIRE(nums[1] == std::optional<unsigned int>(1));
            REQUIRE(nums[2] == std::optional<unsigned int>(2));
            REQUIRE(findStartPosSpecial(schema, *nums[1])->xPos == 200);
            REQUIRE(findStartPosSpecial(schema, *nums[2])->xPos == 300);
        }

        SECTION("middle hole: slot 0 occupies StartPos1, slot 2 StartPos3")
        {
            auto players = occupied({0, 2});
            auto nums = assignSkirmishStartPosNums(players, SkirmishStartLocation::Fixed, rng);
            REQUIRE(nums[0] == std::optional<unsigned int>(0));
            REQUIRE(!nums[1]);
            REQUIRE(nums[2] == std::optional<unsigned int>(2));
            REQUIRE(findStartPosSpecial(schema, *nums[0])->xPos == 100);
            REQUIRE(findStartPosSpecial(schema, *nums[2])->xPos == 300);
        }

        SECTION("a slot beyond the declared positions gets no commander")
        {
            // retail prints "Could not find start position number N"
            // and leaves the slot commander-less rather than shifting
            auto players = occupied({0, 9});
            auto nums = assignSkirmishStartPosNums(players, SkirmishStartLocation::Fixed, rng);
            REQUIRE(nums[0] == std::optional<unsigned int>(0));
            REQUIRE(nums[9] == std::optional<unsigned int>(9));
            REQUIRE(findStartPosSpecial(schema, *nums[0]) != nullptr);
            REQUIRE(findStartPosSpecial(schema, *nums[9]) == nullptr);
        }
    }

    /**
     * Retail Random start (TotalA.exe 0x497979-0x497ad8): the draw
     * pool is the set of OCCUPIED slot indices, shuffled once with
     * retail's rand()%i swap loop, then dealt to occupied slots in
     * physical order -- a permutation of the players' fixed
     * positions, not a draw from all declared StartPos entries.
     */
    TEST_CASE("skirmish random start permutes the occupied slots' positions")
    {
        auto occupied = [](std::initializer_list<unsigned int> slots) {
            std::array<std::optional<PlayerInfo>, 10> players;
            for (auto i : slots)
            {
                players[i] = PlayerInfo{
                    std::nullopt,
                    PlayerControllerType(PlayerControllerTypeComputer()),
                    "ARM",
                    PlayerColorIndex(i),
                    Metal(0),
                    Energy(0)};
            }
            return players;
        };

        SECTION("deterministic for one seed; draws without replacement")
        {
            auto players = occupied({0, 1, 2, 3, 4, 5});
            std::minstd_rand rngA(12345), rngB(12345);
            auto a = assignSkirmishStartPosNums(players, SkirmishStartLocation::Random, rngA);
            auto b = assignSkirmishStartPosNums(players, SkirmishStartLocation::Random, rngB);
            REQUIRE(a == b);
            std::set<unsigned int> drawn;
            for (auto i : {0u, 1u, 2u, 3u, 4u, 5u})
            {
                REQUIRE(a[i]);
                drawn.insert(*a[i]);
            }
            REQUIRE(drawn.size() == 6);
            // the pool is the occupied slot indices {0..5}, not all
            // ten declared positions
            for (auto v : drawn)
            {
                REQUIRE(v < 6);
            }
        }

        SECTION("sparse table: pool is the occupied indices themselves")
        {
            auto players = occupied({1, 2});
            std::minstd_rand rngA(7), rngB(7);
            auto a = assignSkirmishStartPosNums(players, SkirmishStartLocation::Random, rngA);
            auto b = assignSkirmishStartPosNums(players, SkirmishStartLocation::Random, rngB);
            REQUIRE(a == b);
            REQUIRE(!a[0]);
            REQUIRE(a[1]);
            REQUIRE(a[2]);
            // each draws a distinct member of {1, 2}
            std::set<unsigned int> drawn{*a[1], *a[2]};
            REQUIRE(drawn == std::set<unsigned int>{1, 2});
        }
    }

    TEST_CASE("skirmish difficulty scales computer economy like campaign")
    {
        REQUIRE(skirmishDifficultyComputerResourceScale(SkirmishDifficulty::Easy) == 0.5f);
        REQUIRE(skirmishDifficultyComputerResourceScale(SkirmishDifficulty::Medium) == 0.7f);
        REQUIRE(skirmishDifficultyComputerResourceScale(SkirmishDifficulty::Hard) == 1.0f);
    }

    /**
     * Retail writes the same ctx+0x37eee difficulty global from the
     * skirmish Difficulty stage that the campaign path uses for AI
     * `plan` gating (0x406c90): Easy -> plan easy, Medium -> plan
     * medium, Hard -> plan hard.
     */
    TEST_CASE("skirmish difficulty maps to the retail AI plan gate")
    {
        REQUIRE(skirmishAiDifficultyIndex(SkirmishDifficulty::Easy) == CampaignAiDifficulty::Easy);
        REQUIRE(skirmishAiDifficultyIndex(SkirmishDifficulty::Medium) == CampaignAiDifficulty::Medium);
        REQUIRE(skirmishAiDifficultyIndex(SkirmishDifficulty::Hard) == CampaignAiDifficulty::Hard);

        // and the gated plans actually differ: a skirmish-Medium
        // parse must not see the Easy section's directives
        auto medium = parseCampaignAiProfile(
            "plan easy\nlimit ARMCOM 1\nplan medium\nlimit ARMCOM 5\nplan hard\nlimit ARMCOM 9\n",
            skirmishAiDifficultyIndex(SkirmishDifficulty::Medium));
        REQUIRE(medium.limitDirectives.size() == 1);
        REQUIRE(medium.limitDirectives.front().value == 5);
    }

    /**
     * Retail skirmish Start-handler validation (TotalA.exe
     * 0x47ae60-0x47b0f7). Checks, in order: >=1 human and >=1
     * computer ("There must be at least one player and one computer
     * opponent"), occupied count <= resolved schema's StartPos count
     * ("There are too many players enabled for this map"), then
     * alliance uniformity (0x479760: an absent ally field -- sentinel
     * 5 -- is an independent alliance, never "the same" group; only
     * ALL occupied slots sharing one explicit group rejects -- "All
     * players may not be in the same allied group."). The OTA
     * GlobalHeader numplayers list is not consulted at Start.
     */
    TEST_CASE("skirmish start validation matches the retail Start handler")
    {
        auto makeOta = [](unsigned int startPosCount, const std::string& numPlayers) {
            std::string specials;
            for (unsigned int i = 1; i <= startPosCount; ++i)
            {
                specials += "[special" + std::to_string(i - 1) + "] { specialwhat=StartPos" + std::to_string(i) + "; XPos=" + std::to_string(100 * i) + "; ZPos=" + std::to_string(100 * i) + "; } ";
            }
            auto root = parseTdfFromString(
                "[GlobalHeader] { missionname=x; numplayers=" + numPlayers + "; SCHEMACOUNT=1; "
                "[Schema 0] { Type=Network 1; [specials] { " + specials + " } } }");
            return parseOta(root);
        };

        auto human = [](std::optional<unsigned int> ally = std::nullopt) {
            return std::optional<PlayerInfo>(PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(0), Energy(0), ally});
        };
        auto computer = [](std::optional<unsigned int> ally = std::nullopt) {
            return std::optional<PlayerInfo>(PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(0), Energy(0), ally});
        };
        auto table = [](std::initializer_list<std::pair<unsigned int, std::optional<PlayerInfo>>> slots) {
            std::array<std::optional<PlayerInfo>, 10> players;
            for (const auto& s : slots)
            {
                players[s.first] = s.second;
            }
            return players;
        };

        // the QA configuration: Anteer Strait declares numplayers=3
        // yet seats ten start positions; retail allows any occupied
        // count up to the position total
        SECTION("sparse Open/Human/Computer on a numplayers=3 map is allowed")
        {
            auto ota = makeOta(10, "3");
            auto players = table({{1, human()}, {2, computer()}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("two unset ally groups are two independent alliances")
        {
            auto ota = makeOta(2, "2");
            auto players = table({{0, human()}, {1, computer()}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("the same explicit ally group on every player rejects")
        {
            auto ota = makeOta(2, "2");
            auto players = table({{0, human(0)}, {1, computer(0)}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "All players may not be in the same allied group.");
        }

        SECTION("different explicit ally groups are allowed")
        {
            auto ota = makeOta(2, "2");
            auto players = table({{0, human(0)}, {1, computer(1)}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("unset plus explicit group is allowed")
        {
            auto ota = makeOta(2, "2");
            auto players = table({{0, human()}, {1, computer(0)}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("three unset players are three independent alliances")
        {
            auto ota = makeOta(3, "2-4");
            auto players = table({{0, human()}, {1, computer()}, {2, computer()}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("a shared group plus an independent third player is allowed")
        {
            auto ota = makeOta(3, "2-4");
            auto players = table({{0, human(0)}, {1, computer(0)}, {2, computer()}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("no human player rejects")
        {
            auto ota = makeOta(4, "4");
            auto players = table({{0, computer()}, {1, computer()}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "There must be at least one player and one computer opponent");
        }

        SECTION("no computer opponent rejects")
        {
            auto ota = makeOta(4, "4");
            auto players = table({{0, human()}, {1, human()}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "There must be at least one player and one computer opponent");
        }

        SECTION("a single occupied slot rejects")
        {
            auto ota = makeOta(4, "4");
            auto players = table({{0, human()}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "There must be at least one player and one computer opponent");
        }

        SECTION("more players than declared StartPos rejects")
        {
            auto ota = makeOta(3, "4");
            auto players = table({{0, human()}, {1, computer()}, {2, computer()}, {3, computer()}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "There are too many players enabled for this map");
        }

        SECTION("occupied count need not equal numplayers")
        {
            // numplayers is a schema-selection hint, not a legality
            // list: 3 players on a "2" map with 4 positions is legal
            auto ota = makeOta(4, "2");
            auto players = table({{0, human()}, {1, computer()}, {2, computer()}});
            REQUIRE(!checkSkirmishStart(players, ota));
        }

        SECTION("a map with no StartPos-bearing Network schema rejects")
        {
            auto ota = makeOta(0, "2");
            auto players = table({{0, human()}, {1, computer()}});
            auto rejection = checkSkirmishStart(players, ota);
            REQUIRE(rejection);
            REQUIRE(*rejection == "There are too many players enabled for this map");
        }
    }

    /**
     * Retail keyboard-audit companion: every stock transport class must
     * complete the interactive LoadOrder path end to end on real data --
     * navigate to the target, run the unit's own COB TransportPickup /
     * QueryTransport script, ATTACH_UNIT, and release on UnloadOrder.
     *
     * Covers all six stock canload units:
     *   ARMATLAS / CORVALK   (VTOL)
     *   ARMTHOVR / CORTHOVR  (hover, TANKHOVER4)
     *   ARMTSHIP / CORTSHIP  (ship, BOATS6 -- shoreline boarding)
     *
     * CORTSHIP "Envoy" is the C13A human-QA failure: its TransportPickup
     * calls `call-script BoomCalc(posxz, posy)` and the boom reach check
     * only succeeds when script call parameters bind in declaration
     * order.
     */
    TEST_CASE("all six stock transports board and release a unit on a real map")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_transport.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        struct TransportSpec
        {
            const char* unitName;
            const char* movementClassName; // nullptr for VTOLs
            bool vtol;
        };
        const std::vector<TransportSpec> specs = {
            {"ARMATLAS", nullptr, true},
            {"CORVALK", nullptr, true},
            {"ARMTHOVR", "TANKHOVER4", false},
            {"CORTHOVR", "TANKHOVER4", false},
            {"ARMTSHIP", "BOATS6", false},
            {"CORTSHIP", "BOATS6", false},
        };

        for (const auto& spec : specs)
        {
            REQUIRE(pack.unitDefinitions.count(spec.unitName) == 1);
            const auto& transportDef = pack.unitDefinitions.at(spec.unitName);
            CAPTURE(spec.unitName);
            REQUIRE(transportDef.canLoad);
            REQUIRE(transportDef.transportCapacity > 0);
            REQUIRE(transportDef.transportSize > 0);

            // candidate maps -- first one yielding workable geometry wins
            const std::vector<std::string> candidates = {
                "Anteer Strait", "Hundred Isles", "Seven Islands",
                "Ring Atoll", "Coast To Coast", "Over Crude Water",
                "Two Continents", "Painted Desert", "The Barrier Reef"};

            // each spec runs in both single-player shapes: a bare
            // skirmish sim, and a mission-shaped sim (missionInfo plus
            // authored missionState on the units) standing in for a
            // canonical-campaign or Battle Tactics launch. Interactive
            // Load/Unload must be mode-invariant.
            for (const bool mission : {false, true})
            {
            bool ran = false;
            for (const auto& mapName : candidates)
            {
                auto tntBytes = vfs.readFile("maps/" + mapName + ".tnt");
                if (!tntBytes)
                {
                    continue;
                }
                SpanStream tntStream(tntBytes->data(), tntBytes->size());
                TntArchive tnt(&tntStream);
                Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
                tnt.readMapAttributes(mapAttributes.getData());
                auto heightGrid = getHeightGrid(mapAttributes);

                GameSimulation sim(
                    MapTerrain(std::move(heightGrid), SimScalar(tnt.getHeader().seaLevel)),
                    255, 0, 0, 0);
                sim.unitDefinitions = pack.unitDefinitions;
                sim.weaponDefinitions = pack.weaponDefinitions;
                sim.movementClassDatabase = pack.movementClassDatabase;
                sim.unitModelDefinitions = pack.unitModelDefinitions;
                sim.unitScriptDefinitions = pack.unitScriptDefinitions;
                sim.movementClassCollisionService = createMovementClassCollisionService(sim.terrain, sim.movementClassDatabase);

                auto tanksh2 = sim.movementClassDatabase.resolveMovementClassByName("TANKSH2");
                REQUIRE(tanksh2);

                auto playerId = sim.addPlayer(GamePlayerInfo{
                    std::nullopt, GamePlayerType::Human, PlayerColorIndex(0), GamePlayerStatus::Alive,
                    "CORE", std::nullopt, Metal(10000), Energy(10000),
                    Metal(10000), Energy(10000), Metal(10000), Energy(10000)});

                if (mission)
                {
                    // the mission envelope LoadingScene installs for a
                    // --campaign launch; BT missions ride the same shape
                    MissionInfo info;
                    info.campaignName = "ARM";
                    info.campaignSide = "ARM";
                    info.missionName = "PARITY";
                    info.localPlayerId = playerId;
                    sim.missionInfo = std::move(info);
                    sim.initMissionObjectives();
                }

                std::optional<std::pair<Point, Point>> geometry;
                if (spec.vtol)
                {
                    // VTOLs fly to the target directly: any TANKSH2 cell
                    // for the cargo, with a second open cell a short
                    // distance away for the transport spawn.
                    std::optional<Point> cargoCell;
                    for (int y = 2; y < static_cast<int>(sim.occupiedGrid.getHeight()) - 2 && !cargoCell; ++y)
                    {
                        for (int x = 2; x < static_cast<int>(sim.occupiedGrid.getWidth()) - 2 && !cargoCell; ++x)
                        {
                            if (sim.movementClassCollisionService.isWalkable(*tanksh2, Point(x, y)))
                            {
                                cargoCell = Point(x, y);
                            }
                        }
                    }
                    if (cargoCell)
                    {
                        // spawn cell ~20 cells away on walkable land
                        for (int radius = 20; radius >= 4 && !geometry; --radius)
                        {
                            Point tCell(cargoCell->x + radius, cargoCell->y);
                            if (tCell.x >= 0 && tCell.x < static_cast<int>(sim.occupiedGrid.getWidth())
                                && sim.movementClassCollisionService.isWalkable(*tanksh2, tCell))
                            {
                                geometry = std::make_pair(tCell, *cargoCell);
                            }
                        }
                    }
                }
                else
                {
                    auto cls = sim.movementClassDatabase.resolveMovementClassByName(spec.movementClassName);
                    REQUIRE(cls);
                    const auto& clsGrid = sim.movementClassCollisionService.getGrid(*cls);

                    // scan for a transport berth whose footprint ring
                    // borders a TANKSH2-walkable cell for the cargo
                    auto adHoc = sim.getAdHocMovementClass(transportDef.movementCollisionInfo);
                    int fpX = static_cast<int>(adHoc.footprintX);
                    int fpZ = static_cast<int>(adHoc.footprintZ);
                    for (int y = 0; y < static_cast<int>(clsGrid.getHeight()) && !geometry; ++y)
                    {
                        for (int x = 0; x < static_cast<int>(clsGrid.getWidth()) && !geometry; ++x)
                        {
                            if (!sim.movementClassCollisionService.isWalkable(*cls, Point(x, y)))
                            {
                                continue;
                            }
                            for (int ry = y - 1; ry <= y + fpZ && !geometry; ++ry)
                            {
                                for (int rx = x - 1; rx <= x + fpX && !geometry; ++rx)
                                {
                                    if (rx >= x && rx < x + fpX && ry >= y && ry < y + fpZ)
                                    {
                                        continue; // inside the berth
                                    }
                                    if (rx >= 0 && ry >= 0
                                        && rx < static_cast<int>(sim.occupiedGrid.getWidth())
                                        && ry < static_cast<int>(sim.occupiedGrid.getHeight())
                                        && sim.movementClassCollisionService.isWalkable(*tanksh2, Point(rx, ry)))
                                    {
                                        geometry = std::make_pair(Point(x, y), Point(rx, ry));
                                    }
                                }
                            }
                        }
                    }
                }
                if (!geometry)
                {
                    continue;
                }
                ran = true;
                LOG_INFO << spec.unitName << " on " << mapName << ": transport cell=("
                         << geometry->first.x << "," << geometry->first.y << ") cargo cell=("
                         << geometry->second.x << "," << geometry->second.y << ")";

                auto transportCorner = sim.terrain.heightmapIndexToWorldCorner(geometry->first);
                auto adHocDims = sim.getAdHocMovementClass(transportDef.movementCollisionInfo);
                auto transportPos = transportCorner
                    + SimVector(
                        SimScalar(adHocDims.footprintX) * MapTerrain::HeightTileWidthInWorldUnits / 2_ss,
                        0_ss,
                        SimScalar(adHocDims.footprintZ) * MapTerrain::HeightTileHeightInWorldUnits / 2_ss);
                auto cargoCorner = sim.terrain.heightmapIndexToWorldCorner(geometry->second);
                auto cargoPos = cargoCorner + SimVector(8_ss, 0_ss, 8_ss);
                transportPos.y = sim.terrain.getHeightAt(transportPos.x, transportPos.z);
                cargoPos.y = sim.terrain.getHeightAt(cargoPos.x, cargoPos.z);

                auto transportId = sim.trySpawnUnit(spec.unitName, playerId, transportPos, std::nullopt);
                auto cargoId = sim.trySpawnUnit("CORAK", playerId, cargoPos, std::nullopt);
                INFO("spawn ok transport=" << transportId.has_value() << " cargo=" << cargoId.has_value());
                REQUIRE(transportId);
                REQUIRE(cargoId);
                sim.getUnitState(*transportId).finishBuilding(transportDef);
                sim.getUnitState(*cargoId).finishBuilding(sim.unitDefinitions.at("CORAK"));
                if (mission)
                {
                    // mission-spawned units carry authored missionState
                    sim.getUnitState(*transportId).missionState = MissionUnitState{};
                    sim.getUnitState(*cargoId).missionState = MissionUnitState{};
                }

                sim.getUnitState(*transportId).addOrder(LoadOrder{*cargoId});
                for (int i = 0; i < 4000 && !sim.getUnitState(*cargoId).carriedBy; ++i)
                {
                    sim.tick();
                }
                const auto& cargo = sim.getUnitState(*cargoId);
                const auto& transport = sim.getUnitState(*transportId);
                INFO(spec.unitName << ": cargo carried by=" << (cargo.carriedBy ? "transport" : "none")
                                   << " passengers=" << transport.passengers.size());
                REQUIRE(cargo.carriedBy == std::optional<UnitId>(*transportId));
                REQUIRE(transport.passengers.size() == 1);

                // unload back at the cargo's original position
                sim.getUnitState(*transportId).addOrder(UnloadOrder{cargoPos});
                for (int i = 0; i < 4000 && sim.getUnitState(*cargoId).carriedBy; ++i)
                {
                    sim.tick();
                }
                const auto& cargoAfter = sim.getUnitState(*cargoId);
                const auto& transportAfter = sim.getUnitState(*transportId);
                INFO(spec.unitName << ": after unload carriedBy=" << (cargoAfter.carriedBy ? "transport" : "none")
                                   << " passengers=" << transportAfter.passengers.size());
                REQUIRE(!cargoAfter.carriedBy);
                REQUIRE(transportAfter.passengers.empty());
                REQUIRE(!cargoAfter.isDead());
                break;
            }
            REQUIRE(ran);
            }
        }
    }

    /**
     * TA-Transplant balance boundary: interactive LoadOrder must admit
     * exactly the owner-approved passenger counts and no more.
     * Effective project capacities (retail 3.1 values in parens):
     *   ARMATLAS 1 (5), CORVALK 1 (1), ARMTHOVR 5 (5), CORTHOVR 5 (5),
     *   ARMTSHIP 24 (20), CORTSHIP 24 (5).
     * The override is applied at definition-load time
     * (applyTransplantBalanceOverrides), so these boundaries hold
     * identically in bare and mission-shaped sims; scripted `i`
     * boarding never consulted the count and is untouched.
     *
     * Each spec fills its transport with small eligible CORAK cargo via
     * sequential interactive LoadOrders, then proves passenger N+1 is
     * refused, that unloading frees the slot, and that the refused unit
     * boards once capacity is free again.
     */
    TEST_CASE("transport capacity honors the TA-Transplant balance override")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_transport_cap.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        // definition-level proof first: the override is visible on the
        // loaded definitions themselves
        REQUIRE(pack.unitDefinitions.at("ARMATLAS").transportCapacity == 1);
        REQUIRE(pack.unitDefinitions.at("CORVALK").transportCapacity == 1);
        REQUIRE(pack.unitDefinitions.at("ARMTHOVR").transportCapacity == 5);
        REQUIRE(pack.unitDefinitions.at("CORTHOVR").transportCapacity == 5);
        REQUIRE(pack.unitDefinitions.at("ARMTSHIP").transportCapacity == 24);
        REQUIRE(pack.unitDefinitions.at("CORTSHIP").transportCapacity == 24);

        struct CapacitySpec
        {
            const char* unitName;
            const char* movementClassName; // nullptr for VTOLs
            bool vtol;
            unsigned int capacity;
        };
        const std::vector<CapacitySpec> specs = {
            {"ARMATLAS", nullptr, true, 1},
            {"CORVALK", nullptr, true, 1},
            {"ARMTHOVR", "TANKHOVER4", false, 5},
            {"CORTHOVR", "TANKHOVER4", false, 5},
            {"ARMTSHIP", "BOATS6", false, 24},
            {"CORTSHIP", "BOATS6", false, 24},
        };

        for (const auto& spec : specs)
        {
            CAPTURE(spec.unitName);
            // run the boundary in both single-player shapes; the
            // override is definition data, it cannot fork on mode
            for (const bool mission : {false, true})
            {
            bool ran = false;
            const std::vector<std::string> candidates = {
                "Anteer Strait", "Hundred Isles", "Seven Islands",
                "Ring Atoll", "Coast To Coast", "Over Crude Water",
                "Two Continents", "Painted Desert", "The Barrier Reef"};

            for (const auto& mapName : candidates)
            {
                auto tntBytes = vfs.readFile("maps/" + mapName + ".tnt");
                if (!tntBytes)
                {
                    continue;
                }
                SpanStream tntStream(tntBytes->data(), tntBytes->size());
                TntArchive tnt(&tntStream);
                Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
                tnt.readMapAttributes(mapAttributes.getData());
                auto heightGrid = getHeightGrid(mapAttributes);

                GameSimulation sim(
                    MapTerrain(std::move(heightGrid), SimScalar(tnt.getHeader().seaLevel)),
                    255, 0, 0, 0);
                sim.unitDefinitions = pack.unitDefinitions;
                sim.weaponDefinitions = pack.weaponDefinitions;
                sim.movementClassDatabase = pack.movementClassDatabase;
                sim.unitModelDefinitions = pack.unitModelDefinitions;
                sim.unitScriptDefinitions = pack.unitScriptDefinitions;
                sim.movementClassCollisionService = createMovementClassCollisionService(sim.terrain, sim.movementClassDatabase);

                auto tanksh2 = sim.movementClassDatabase.resolveMovementClassByName("TANKSH2");
                REQUIRE(tanksh2);

                auto playerId = sim.addPlayer(GamePlayerInfo{
                    std::nullopt, GamePlayerType::Human, PlayerColorIndex(0), GamePlayerStatus::Alive,
                    "CORE", std::nullopt, Metal(10000), Energy(10000),
                    Metal(10000), Energy(10000), Metal(10000), Energy(10000)});

                if (mission)
                {
                    MissionInfo info;
                    info.campaignName = "ARM";
                    info.campaignSide = "ARM";
                    info.missionName = "PARITY";
                    info.localPlayerId = playerId;
                    sim.missionInfo = std::move(info);
                    sim.initMissionObjectives();
                }

                const unsigned int cargoCount = spec.capacity + 1;
                const bool needsShore = !spec.vtol && spec.movementClassName != nullptr
                    && std::string(spec.movementClassName) == "BOATS6";

                // Collect cargo cells clustered on walkable land.
                // VTOLs and hovers berth anywhere; ships (BOATS6) need
                // the cargo within ~2 cells of navigable water.
                std::optional<MovementClassId> shipClass;
                if (!spec.vtol)
                {
                    shipClass = sim.movementClassDatabase.resolveMovementClassByName(spec.movementClassName);
                    REQUIRE(shipClass);
                }

                // gather a candidate pool (far more than needed) so the
                // selection below can enforce footprint spacing
                std::vector<Point> candidates;
                for (int y = 1; y < static_cast<int>(sim.occupiedGrid.getHeight()) - 1 && candidates.size() < 600; ++y)
                {
                    for (int x = 1; x < static_cast<int>(sim.occupiedGrid.getWidth()) - 1 && candidates.size() < 600; ++x)
                    {
                        Point p(x, y);
                        if (!sim.movementClassCollisionService.isWalkable(*tanksh2, p))
                        {
                            continue;
                        }
                        if (needsShore)
                        {
                            // shore cell: within 2 cells of navigable water
                            bool nearWater = false;
                            for (int dy = -2; dy <= 2 && !nearWater; ++dy)
                            {
                                for (int dx = -2; dx <= 2 && !nearWater; ++dx)
                                {
                                    Point q(x + dx, y + dy);
                                    if (q.x >= 0 && q.y >= 0
                                        && q.x < static_cast<int>(sim.occupiedGrid.getWidth())
                                        && q.y < static_cast<int>(sim.occupiedGrid.getHeight())
                                        && sim.movementClassCollisionService.isWalkable(*shipClass, q))
                                    {
                                        nearWater = true;
                                    }
                                }
                            }
                            if (!nearWater)
                            {
                                continue;
                            }
                        }
                        candidates.push_back(p);
                    }
                }

                if (candidates.empty())
                {
                    continue;
                }

                // keep the cluster tight: sort by distance from the
                // first cell so pickups stay local
                const auto clusterAnchor = candidates.front();
                std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
                    auto da = std::abs(a.x - clusterAnchor.x) + std::abs(a.y - clusterAnchor.y);
                    auto db = std::abs(b.x - clusterAnchor.x) + std::abs(b.y - clusterAnchor.y);
                    return da < db;
                });

                // CORAK occupies a 2x2 footprint; keep cargo cells far
                // enough apart that a 4x4/6x6 transport can still find
                // a clear berth next to each remaining passenger
                const int cargoSpacing = spec.vtol ? 3 : 6;
                std::vector<Point> cargoCells;
                for (const auto& p : candidates)
                {
                    bool clear = true;
                    for (const auto& c : cargoCells)
                    {
                        if (std::abs(p.x - c.x) < cargoSpacing && std::abs(p.y - c.y) < cargoSpacing)
                        {
                            clear = false;
                            break;
                        }
                    }
                    if (clear)
                    {
                        cargoCells.push_back(p);
                        if (cargoCells.size() >= cargoCount)
                        {
                            break;
                        }
                    }
                }
                if (cargoCells.size() < cargoCount)
                {
                    continue;
                }

                // spawn the cargo first (soft-fail to the next
                // candidate map on footprint/feature collisions), then
                // pick a transport cell disjoint from every cargo
                // footprint
                std::vector<UnitId> cargoIds;
                std::vector<SimVector> cargoPositions;
                bool spawnFailed = false;
                for (const auto& cell : cargoCells)
                {
                    auto cCorner = sim.terrain.heightmapIndexToWorldCorner(cell);
                    auto cPos = cCorner + SimVector(8_ss, 0_ss, 8_ss);
                    cPos.y = sim.terrain.getHeightAt(cPos.x, cPos.z);
                    auto cargoId = sim.trySpawnUnit("CORAK", playerId, cPos, std::nullopt);
                    if (!cargoId)
                    {
                        spawnFailed = true;
                        break;
                    }
                    sim.getUnitState(*cargoId).finishBuilding(sim.unitDefinitions.at("CORAK"));
                    if (mission)
                    {
                        sim.getUnitState(*cargoId).missionState = MissionUnitState{};
                    }
                    cargoIds.push_back(*cargoId);
                    cargoPositions.push_back(cPos);
                }
                if (spawnFailed)
                {
                    continue;
                }

                const auto& transportDef = pack.unitDefinitions.at(spec.unitName);
                auto tDims = sim.getAdHocMovementClass(transportDef.movementCollisionInfo);
                const int tFootX = static_cast<int>(tDims.footprintX);
                const int tFootZ = static_cast<int>(tDims.footprintZ);
                auto overlapsCargo = [&](const Point& t) {
                    for (const auto& c : cargoCells)
                    {
                        const bool disjoint = c.x + 1 < t.x || c.x > t.x + tFootX - 1
                            || c.y + 1 < t.y || c.y > t.y + tFootZ - 1;
                        if (!disjoint)
                        {
                            return true;
                        }
                    }
                    return false;
                };

                // transport spawn cell: nearest navigable cell to the
                // cluster's last cell (ground/ship classes) or any
                // walkable cell offset from the cluster (VTOLs fly)
                std::optional<Point> transportCell;
                const auto& anchor = cargoCells.back();
                if (spec.vtol)
                {
                    for (int radius = 24; radius >= 6 && !transportCell; --radius)
                    {
                        Point t(anchor.x + radius, anchor.y);
                        if (t.x >= 0 && t.x < static_cast<int>(sim.occupiedGrid.getWidth())
                            && sim.movementClassCollisionService.isWalkable(*tanksh2, t)
                            && !overlapsCargo(t))
                        {
                            transportCell = t;
                        }
                    }
                }
                else
                {
                    int bestDist = 1 << 30;
                    for (int y = 0; y < static_cast<int>(sim.occupiedGrid.getHeight()); ++y)
                    {
                        for (int x = 0; x < static_cast<int>(sim.occupiedGrid.getWidth()); ++x)
                        {
                            Point p(x, y);
                            if (!sim.movementClassCollisionService.isWalkable(*shipClass, p)
                                || overlapsCargo(p))
                            {
                                continue;
                            }
                            auto d = std::abs(x - anchor.x) + std::abs(y - anchor.y);
                            if (d < bestDist)
                            {
                                bestDist = d;
                                transportCell = p;
                            }
                        }
                    }
                }
                if (!transportCell)
                {
                    continue;
                }

                auto tCorner = sim.terrain.heightmapIndexToWorldCorner(*transportCell);
                auto transportPos = tCorner
                    + SimVector(SimScalar(tDims.footprintX) * MapTerrain::HeightTileWidthInWorldUnits / 2_ss,
                        0_ss,
                        SimScalar(tDims.footprintZ) * MapTerrain::HeightTileHeightInWorldUnits / 2_ss);
                transportPos.y = sim.terrain.getHeightAt(transportPos.x, transportPos.z);
                auto transportId = sim.trySpawnUnit(spec.unitName, playerId, transportPos, std::nullopt);
                if (!transportId)
                {
                    continue;
                }
                sim.getUnitState(*transportId).finishBuilding(transportDef);
                if (mission)
                {
                    sim.getUnitState(*transportId).missionState = MissionUnitState{};
                }
                ran = true;

                // fill to capacity via sequential interactive loads
                for (unsigned int i = 0; i < spec.capacity; ++i)
                {
                    sim.getUnitState(*transportId).addOrder(LoadOrder{cargoIds[i]});
                    for (int t = 0; t < 12000 && !sim.getUnitState(cargoIds[i]).carriedBy; ++t)
                    {
                        sim.tick();
                    }
                    INFO(spec.unitName << " passenger " << (i + 1) << " failed to board on " << mapName);
                    REQUIRE(sim.getUnitState(cargoIds[i]).carriedBy == std::optional<UnitId>(*transportId));
                }
                REQUIRE(sim.getUnitState(*transportId).passengers.size() == spec.capacity);

                // passenger capacity+1 is refused: the order is
                // consumed and the cargo never boards
                auto& extraCargoId = cargoIds.back();
                sim.getUnitState(*transportId).addOrder(LoadOrder{extraCargoId});
                for (int t = 0; t < 1200 && !sim.getUnitState(extraCargoId).carriedBy; ++t)
                {
                    sim.tick();
                }
                REQUIRE(!sim.getUnitState(extraCargoId).carriedBy);
                REQUIRE(sim.getUnitState(*transportId).passengers.size() == spec.capacity);

                // unloading frees capacity, then the refused unit
                // boards; ships must drop onto land, so target the
                // first passenger's original shore cell
                sim.getUnitState(*transportId).addOrder(UnloadOrder{cargoPositions.front()});
                for (int t = 0; t < 12000 && sim.getUnitState(*transportId).passengers.size() == spec.capacity; ++t)
                {
                    sim.tick();
                }
                INFO(spec.unitName << " unload freed no slot on " << mapName);
                REQUIRE(sim.getUnitState(*transportId).passengers.size() < spec.capacity);

                sim.getUnitState(*transportId).addOrder(LoadOrder{extraCargoId});
                for (int t = 0; t < 12000 && !sim.getUnitState(extraCargoId).carriedBy; ++t)
                {
                    sim.tick();
                }
                INFO(spec.unitName << " refused cargo did not board after unload on " << mapName);
                REQUIRE(sim.getUnitState(extraCargoId).carriedBy == std::optional<UnitId>(*transportId));
                break;
            }
            INFO(spec.unitName << (mission ? " (mission-shaped sim)" : " (skirmish-shaped sim)") << ": no candidate map provided workable geometry");
            REQUIRE(ran);
            }
        }
    }

    /**
     * F2/F3 live-path regression: orders issued the way the real game
     * issues them -- GameScene::issueUnitOrder is clearOrders() +
     * addOrder(expandPatrolOrder(unit, order)) -- on a real retail map
     * with real retail unit definitions. The synthetic
     * UnitBehaviorService fixtures place candidates directly on top of
     * the builder; this test stages targets at realistic "alongside the
     * route" offsets, which is where the live QA gap lives.
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("player-issued patrol, guard and repair-divert behave on a real map")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        struct Spawned
        {
            UnitId commander;
            UnitId patroller;   // plain combat patrol probe
            UnitId builder;     // repair-divert probe
            UnitId damaged;     // damaged allied target beside the route
            UnitId nanoframe;   // unfinished allied target beside the route
            UnitId guardCv;     // guard-follow probe
            SimVector commanderStart;
        };
        std::optional<Spawned> spawnedIds;

        auto hook = [&pack, &spawnedIds](GameSimulation& s) {
            const PlayerId p0(0);
            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == p0 && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);
            Spawned sp;
            sp.commander = [&] {
                for (const auto& [id, u] : s.units)
                {
                    if (u.owner == p0 && !u.isDead())
                    {
                        return id;
                    }
                }
                return UnitId(0);
            }();
            sp.commanderStart = com->position;

            auto spawnAt = [&](const char* type, float dx, float dz, bool complete) {
                SimVector pos = com->position + SimVector(SimScalar(dx), SimScalar(0), SimScalar(dz));
                pos.y = s.terrain.getHeightAt(pos.x, pos.z);
                auto id = s.trySpawnUnit(type, p0, pos, std::nullopt, true);
                REQUIRE(id);
                if (complete)
                {
                    s.getUnitState(*id).finishBuilding(pack.unitDefinitions.at(type));
                }
                return *id;
            };

            // plain patrol probe: a Stumpy that must keep cycling two
            // waypoints indefinitely
            sp.patroller = spawnAt("ARMSTUMP", 0.0f, 192.0f, true);

            // repair-divert probe: a builder patrolling a short leg whose
            // closest approach sits within working range but whose
            // center-to-center distance exceeds its 40-unit buildDistance
            sp.builder = spawnAt("ARMCV", 0.0f, 96.0f, true);
            sp.damaged = spawnAt("ARMSTUMP", 112.0f, 48.0f, true);
            s.getUnitState(sp.damaged).hitPoints = 200;
            // unfinished frame parked alongside the route's outbound
            // half: closest approach (48,96) is 48 center distance but
            // ~32 edge distance
            sp.nanoframe = spawnAt("ARMSTUMP", 48.0f, 48.0f, false);

            // guard probe: a second CV guarding the commander
            sp.guardCv = spawnAt("ARMCV", -96.0f, 0.0f, true);

            // player-issued orders exactly as GameScene::issueUnitOrder
            // applies them: clear then add the expanded order
            {
                auto& u = s.getUnitState(sp.patroller);
                u.clearOrders();
                SimVector far = u.position + SimVector(SimScalar(256), SimScalar(0), SimScalar(0));
                far.y = s.terrain.getHeightAt(far.x, far.z);
                u.addOrder(expandPatrolOrder(u, PatrolOrder({far})));
            }
            {
                auto& u = s.getUnitState(sp.builder);
                u.clearOrders();
                // tight leg along z=96: closest approach to the damaged
                // target at (112,48) is (112,96) -> 48 center distance
                // (> buildDistance 40) but ~32 edge distance (< 40),
                // i.e. genuinely adjacent on real footprints
                SimVector leg = com->position + SimVector(SimScalar(112), SimScalar(0), SimScalar(96));
                leg.y = s.terrain.getHeightAt(leg.x, leg.z);
                u.addOrder(expandPatrolOrder(u, PatrolOrder({leg})));
            }
            {
                auto& u = s.getUnitState(sp.guardCv);
                u.clearOrders();
                u.addOrder(GuardOrder(sp.commander));
            }
            spawnedIds = sp;
        };

        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 1, SkirmishDifficulty::Medium, hook);
        auto& sim = run.sim;
        REQUIRE(spawnedIds);
        const auto sp = *spawnedIds;

        const auto damagedStartHp = sim.getUnitState(sp.damaged).hitPoints;

        // probes
        bool divertSeen = false;
        bool patrolMovedAcrossTicks = false;
        auto lastPatrolPos = sim.getUnitState(sp.patroller).position;
        bool nanoCompleted = false;

        for (int t = 0; t < 1200; ++t)
        {
            sim.tick();

            const auto& bu = sim.getUnitState(sp.builder);
            if (!bu.orders.empty())
            {
                if (auto p = std::get_if<PatrolOrder>(&bu.orders.front()); p && p->divertTarget)
                {
                    divertSeen = true;
                }
            }

            const auto& pu = sim.getUnitState(sp.patroller);
            if (pu.position.x != lastPatrolPos.x || pu.position.z != lastPatrolPos.z)
            {
                patrolMovedAcrossTicks = true;
            }
            lastPatrolPos = pu.position;

            if (!sim.getUnitState(sp.nanoframe).isBeingBuilt(pack.unitDefinitions.at("ARMSTUMP")))
            {
                nanoCompleted = true;
            }
        }

        INFO("patroller kept moving (looped route, never settled)");
        CHECK(patrolMovedAcrossTicks);
        CHECK(!sim.getUnitState(sp.patroller).orders.empty());

        INFO("builder diverted to allied work beside the patrol route");
        CHECK(divertSeen);
        INFO("damaged target repaired above its staged hit points");
        CHECK(sim.getUnitState(sp.damaged).hitPoints > damagedStartHp);
        INFO("unfinished nanoframe was assisted to completion");
        CHECK(nanoCompleted);

        // guard-follow: walk the commander away; the guarding CV must
        // trail it (retail: follow when farther than 200 units)
        auto& com = sim.getUnitState(sp.commander);
        com.clearOrders();
        SimVector retreat = sp.commanderStart + SimVector(SimScalar(0), SimScalar(0), SimScalar(-640));
        retreat.y = sim.terrain.getHeightAt(retreat.x, retreat.z);
        com.addOrder(MoveOrder(retreat));
        for (int t = 0; t < 900; ++t)
        {
            sim.tick();
        }
        const auto& cv = sim.getUnitState(sp.guardCv);
        const auto distSq = cv.position.distanceSquared(sim.getUnitState(sp.commander).position);
        INFO("guarding CV stays with the moved commander (dist^2=" << simScalarToFloat(distSq) << ")");
        CHECK(distSq < SimScalar(240 * 240));
    }

    /**
     * The stockpile player path end to end on retail data: the real
     * ARMSILO definition carries a stockpile+commandfire weapon, the
     * ModifyWeaponQueue player command's sim effect queues a round,
     * production drains the weapon's per-shot resource cost across its
     * reload period, and the resulting round is consumed when the
     * command-fire order launches it -- all on real retail definitions
     * and terrain. Skips without RWE_DATA_PATH.
     */
    TEST_CASE("player-issued stockpile round builds and fires on a real map")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        std::optional<UnitId> siloId;
        SimVector siloPos;

        auto hook = [&pack, &siloId, &siloPos](GameSimulation& s) {
            const PlayerId p0(0);
            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == p0 && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);

            // NUCLEAR_MISSILE's 180s assembly drains ~180kE/2kM; stage
            // a player that can pay it so production never stalls --
            // storage must be raised too since each settlement clamps
            // stocks to maxEnergy/maxMetal
            auto& player = s.getPlayer(p0);
            player.energy = Energy(1000000);
            player.metal = Metal(1000000);
            player.maxEnergy = Energy(1000000);
            player.maxMetal = Metal(1000000);

            SimVector pos = com->position + SimVector(SimScalar(160), SimScalar(0), SimScalar(0));
            pos.y = s.terrain.getHeightAt(pos.x, pos.z);
            auto id = s.trySpawnUnit("ARMSILO", p0, pos, std::nullopt, true);
            REQUIRE(id);
            s.getUnitState(*id).finishBuilding(pack.unitDefinitions.at("ARMSILO"));
            siloId = *id;
            siloPos = pos;
        };

        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 1, SkirmishDifficulty::Medium, hook);
        auto& sim = run.sim;
        REQUIRE(siloId);

        // the retail signals the frontend keys on: the silo opens its
        // GUI-authored build page and BLAST lights without candgun
        const auto& siloDef = pack.unitDefinitions.at("ARMSILO");
        REQUIRE(unitDefinitionHasStockpileWeapon(sim, siloDef));
        REQUIRE(unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, siloDef));
        REQUIRE(initialUnitGuiInfo(siloDef, true).section == UnitGuiInfo::Section::Build);

        auto& silo = sim.getUnitState(*siloId);
        const auto& weaponDef = sim.weaponDefinitions.at(toUpper(siloDef.weapon1));
        REQUIRE(weaponDef.stockpile);
        REQUIRE(weaponDef.commandFire);
        REQUIRE(silo.weapons[0]);

        // the ModifyWeaponQueue player command's sim effect (retail:
        // one click on the ARMMAKENUKE gadget queues one round)
        applyModifyWeaponQueue(silo, 1);
        REQUIRE(silo.backgroundOrders.size() == 1);

        const auto buildTicks = deltaSecondsToTicks(weaponDef.reloadTime).value;
        auto& player = sim.getPlayer(silo.owner);
        const auto startEnergy = player.energy;

        for (unsigned int t = 0; t < buildTicks + 600 && silo.weapons[0]->stockpileCount == 0; ++t)
        {
            // production stalls while resources are unavailable; stand
            // in for the player's real economy so the drain always lands
            player.energy = player.maxEnergy;
            player.metal = player.maxMetal;
            sim.tick();
        }
        INFO("one stockpiled round assembled within the weapon's reload period");
        CHECK(silo.weapons[0]->stockpileCount == 1);
        INFO("assembly drained the weapon's per-shot resource cost");
        CHECK(player.energy < startEnergy);

        // a second queued round keeps producing in the background
        applyModifyWeaponQueue(silo, 1);
        // the BLAST order fires the stockpiled round: retail's
        // CommandFireOrder resolved onto the commandfire weapon
        silo.clearOrders();
        SimVector target = siloPos + SimVector(SimScalar(512), SimScalar(0), SimScalar(512));
        target.y = sim.terrain.getHeightAt(target.x, target.z);
        silo.addOrder(CommandFireOrder(target));

        bool launched = false;
        for (unsigned int t = 0; t < 900; ++t)
        {
            sim.tick();
            if (silo.weapons[0]->stockpileCount == 0)
            {
                launched = true;
                break;
            }
        }
        INFO("the command-fire order consumed the stockpiled round");
        CHECK(launched);
    }

    TEST_CASE("a strategic missile reaches a target beyond the old weapontimer cap on a real map")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        std::optional<UnitId> siloId;
        SimVector siloPos;

        auto hook = [&pack, &siloId, &siloPos](GameSimulation& s) {
            const PlayerId p0(0);
            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == p0 && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);

            auto& player = s.getPlayer(p0);
            player.energy = Energy(1000000);
            player.metal = Metal(1000000);
            player.maxEnergy = Energy(1000000);
            player.maxMetal = Metal(1000000);

            SimVector pos = com->position + SimVector(SimScalar(160), SimScalar(0), SimScalar(0));
            pos.y = s.terrain.getHeightAt(pos.x, pos.z);
            auto id = s.trySpawnUnit("ARMSILO", p0, pos, std::nullopt, true);
            REQUIRE(id);
            s.getUnitState(*id).finishBuilding(pack.unitDefinitions.at("ARMSILO"));
            siloId = *id;
            siloPos = pos;
        };

        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 1, SkirmishDifficulty::Medium, hook);
        auto& sim = run.sim;
        REQUIRE(siloId);

        auto& silo = sim.getUnitState(*siloId);
        const auto& siloDef = pack.unitDefinitions.at("ARMSILO");
        const auto& weaponDef = sim.weaponDefinitions.at(toUpper(siloDef.weapon1));
        REQUIRE(weaponDef.stockpile);
        REQUIRE(weaponDef.cruise);
        // the decoded contract: cruise missiles live `flighttime`
        // frames (400) rather than `weapontimer` (150) -- the old cap
        // killed the shot ~1,750 units out against a range=32000 gate
        REQUIRE(weaponDef.flightTime == GameTime(400));
        REQUIRE(weaponDef.maxRange >= SimScalar(32000));
        REQUIRE(silo.weapons[0]);

        // stage a built round directly so the flight is the only test
        silo.weapons[0]->stockpileCount = 1;

        // aim at a point ~4,000 world units away: under the old
        // 150-tick/constant-velocity model the shot died at ~1,750;
        // flighttime=400 plus weaponacceleration=50 (px/s^2) carries it
        SimVector target = siloPos;
        auto mapWidth = sim.terrain.getWidthInWorldUnits();
        auto mapHeight = sim.terrain.getHeightInWorldUnits();
        target.x = rweMin(target.x + SimScalar(4000), mapWidth - SimScalar(64));
        target.z = rweMin(target.z + SimScalar(400), mapHeight - SimScalar(64));
        target.y = sim.terrain.getHeightAt(target.x, target.z);
        auto targetDistance = siloPos.distance(target);
        REQUIRE(targetDistance > SimScalar(2000));

        silo.clearOrders();
        silo.addOrder(CommandFireOrder(target));

        const auto nukeName = toUpper(siloDef.weapon1);
        bool launched = false;
        bool seenNuke = false;
        SimVector lastProjectilePos;
        for (unsigned int t = 0; t < weaponDef.flightTime.value + 120; ++t)
        {
            sim.tick();
            if (!launched && silo.weapons[0]->stockpileCount == 0)
            {
                launched = true;
            }
            bool live = false;
            for (const auto& [pid, p] : sim.projectiles)
            {
                if (p.weaponType == nukeName)
                {
                    live = true;
                    lastProjectilePos = p.position;
                }
            }
            seenNuke = seenNuke || live;
            if (launched && seenNuke && !live)
            {
                break;
            }
        }

        INFO("the missile launched and consumed its stockpiled round");
        CHECK(launched);
        INFO("a live nuke projectile was observed");
        CHECK(seenNuke);
        INFO("the shot died at its target, not mid-flight");
        CHECK(lastProjectilePos.distanceSquared(target) < SimScalar(400) * SimScalar(400));
    }

    /**
     * Spawns a building whose footprint covers cells
     * [anchorX, anchorX+w) x [anchorY, anchorY+h) of the heightmap.
     * Returns the spawned id plus the footprint rect actually
     * occupied, or nullopt when the placement is rejected.
     */
    struct WallPiece
    {
        UnitId id;
        DiscreteRect rect;
    };

    std::optional<WallPiece> spawnWallAtCells(
        GameSimulation& s,
        const UnitDefinition& def,
        const std::string& type,
        PlayerId owner,
        int anchorX,
        int anchorY)
    {
        const auto [fw, fh] = s.getFootprintXZ(def.movementCollisionInfo);
        const int w = static_cast<int>(fw);
        const int h = static_cast<int>(fh);
        const auto& hm = s.terrain.getHeightMap();
        if (anchorX < 0 || anchorY < 0 || anchorX + w > static_cast<int>(hm.getWidth()) || anchorY + h > static_cast<int>(hm.getHeight()))
        {
            return std::nullopt;
        }
        auto c0 = s.terrain.heightmapIndexToWorldCenter(anchorX, anchorY);
        auto c1 = s.terrain.heightmapIndexToWorldCenter(anchorX + w - 1, anchorY + h - 1);
        SimVector pos((c0.x + c1.x) / SimScalar(2), SimScalar(0), (c0.z + c1.z) / SimScalar(2));
        pos.y = s.terrain.getHeightAt(pos.x, pos.z);
        auto id = s.trySpawnUnit(type, owner, pos, std::nullopt, true);
        if (!id)
        {
            return std::nullopt;
        }
        s.getUnitState(*id).finishBuilding(def);
        return WallPiece{*id, DiscreteRect(anchorX, anchorY, w, h)};
    }

    /**
     * Tiles a wall ring one cell outside `labRect` using `w`x`h`
     * wall pieces. `gapEdge`/`gapIndex` leave exactly one slot
     * unbuilt: the produced unit can escape through it.
     */
    struct PocketLayout
    {
        std::vector<WallPiece> walls;
        /** Cell rect of the single unbuilt slot. */
        DiscreteRect gapRect;
    };

    PocketLayout spawnPocketWalls(
        GameSimulation& s,
        const UnitDefinition& wallDef,
        const std::string& wallType,
        PlayerId owner,
        const DiscreteRect& labRect,
        int gapEdge,
        bool fillGap,
        int moat)
    {
        PocketLayout out;
        const auto [fw_, fh_] = s.getFootprintXZ(wallDef.movementCollisionInfo);
        const int w = static_cast<int>(fw_);
        const int h = static_cast<int>(fh_);
        const int x0 = labRect.x;
        const int y0 = labRect.y;
        const int x1 = labRect.x + labRect.width;
        const int y1 = labRect.y + labRect.height;
        // `moat` free cells between the yard edge and the wall inner
        // faces: room for trapped units to stand and builders to work
        const int m = moat;

        struct Slot
        {
            int ax;
            int ay;
            int edge;
            int index;
        };
        std::vector<Slot> slots;
        // step `w`/`h` along each edge and force a flush end slot so
        // no 1..w-1 cell hole survives at the far corner
        auto edge = [&](int xStart, int xEnd, int ySlot, int edge) {
            int i = 0;
            for (int x = xStart; x < xEnd; x += w, ++i)
            {
                slots.push_back({x, ySlot, edge, i});
            }
            slots.push_back({xEnd, ySlot, edge, i});
        };
        auto side = [&](int xSlot, int yStart, int yEnd, int edge) {
            int i = 0;
            for (int y = yStart; y < yEnd; y += h, ++i)
            {
                slots.push_back({xSlot, y, edge, i});
            }
            slots.push_back({xSlot, yEnd, edge, i});
        };
        edge(x0 - w - m, x1 + m, y0 - h - m, 0);
        edge(x0 - w - m, x1 + m, y1 + m, 1);
        side(x0 - w - m, y0 - m, y1 + m, 2);
        side(x1 + m, y0 - m, y1 + m, 3);

        // the gap is a mid-edge slot: a corner slot is only
        // diagonally connected to the yard and a real unit cannot
        // squeeze through it either
        int edgeCount = 0;
        for (const auto& s_ : slots)
        {
            if (s_.edge == gapEdge)
            {
                ++edgeCount;
            }
        }
        const int gapIndex = edgeCount / 2;
        int edgeSeen = 0;
        for (const auto& s_ : slots)
        {
            if (s_.edge == gapEdge && edgeSeen++ == gapIndex)
            {
                out.gapRect = DiscreteRect(s_.ax, s_.ay, w, h);
                if (!fillGap)
                {
                    continue;
                }
            }
            auto piece = spawnWallAtCells(s, wallDef, wallType, owner, s_.ax, s_.ay);
            if (piece)
            {
                out.walls.push_back(*piece);
            }
        }
        return out;
    }


    /**
     * The ring edge whose mid-gap cells the produced class can most
     * reliably stand on -- a gap cut through unwalkable cells is not
     * a gap.
     */
    int pickGapEdge(GameSimulation& s, const MovementClassDefinition& mc, const DiscreteRect& labRect)
    {
        int bestWalk = -1, bestEdge = 0;
        for (int e = 0; e < 4; ++e)
        {
            int sx0 = labRect.x + labRect.width / 2 - 2;
            int sy0 = labRect.y + labRect.height / 2 - 2;
            if (e == 0)
            {
                sy0 = labRect.y - 4;
            }
            else if (e == 1)
            {
                sy0 = labRect.y + labRect.height + 1;
            }
            else if (e == 2)
            {
                sx0 = labRect.x - 4;
            }
            else
            {
                sx0 = labRect.x + labRect.width + 1;
            }
            int walk = 0;
            for (int dx = 0; dx < 5; ++dx)
            {
                for (int dy = 0; dy < 4; ++dy)
                {
                    int cx = sx0 + dx, cy = sy0 + dy;
                    if (cx < 0 || cy < 0)
                    {
                        continue;
                    }
                    if (isGridPointWalkable(s.terrain, mc, cx, cy))
                    {
                        ++walk;
                    }
                }
            }
            if (walk > bestWalk)
            {
                bestWalk = walk;
                bestEdge = e;
            }
        }
        return bestEdge;
    }

    /**
     * Finds a flat-ish site for `labType` near `com` where the
     * produced movement class's egress ring is already walkable.
     * Returns the lab's world position and footprint rect.
     */
    struct LabSite
    {
        UnitId id;
        SimVector pos;
        DiscreteRect rect;
    };

    std::optional<LabSite> findOpenFactorySite(
        GameSimulation& s,
        const SimVector& center,
        const UnitDefinition& labDef,
        const std::string& labType,
        PlayerId owner,
        const MovementClassDefinition& mc,
        const std::vector<DiscreteRect>& taken,
        int minSepCells)
    {
        for (int r = 0; r <= 1600; r += 32)
        {
            for (int ddx = -r; ddx <= r; ddx += 32)
            {
                for (int ddz = -r; ddz <= r; ddz += 32)
                {
                    if (r > 0 && std::max(std::abs(ddx), std::abs(ddz)) != r)
                    {
                        continue;
                    }
                    SimVector p = center + SimVector(SimScalar(ddx), SimScalar(0), SimScalar(ddz));
                    p.y = s.terrain.getHeightAt(p.x, p.z);
                    auto rect = s.computeFootprintRegion(p, labDef.movementCollisionInfo);
                    // the wall ring needs room around the yard
                    const auto& hm = s.terrain.getHeightMap();
                    if (rect.x < 8 || rect.y < 8
                        || rect.x + rect.width + 8 > static_cast<int>(hm.getWidth())
                        || rect.y + rect.height + 8 > static_cast<int>(hm.getHeight()))
                    {
                        continue;
                    }
                    bool clashes = false;
                    for (const auto& t : taken)
                    {
                        if (rect.x < t.x + t.width + minSepCells && rect.x + rect.width + minSepCells > t.x
                            && rect.y < t.y + t.height + minSepCells && rect.y + rect.height + minSepCells > t.y)
                        {
                            clashes = true;
                            break;
                        }
                    }
                    if (clashes)
                    {
                        continue;
                    }
                    if (!probeFactoryEgress(s, rect, mc, nullptr, nullptr).open)
                    {
                        continue;
                    }
                    // the pocket moat must be substantially walkable
                    // for the produced class: a marginal site leaves
                    // the unit sealed by terrain even after a wall is
                    // removed, which is a terrain trap, not a
                    // base-layout deadlock
                    {
                        int walk = 0, total = 0;
                        for (int cx = rect.x - 3; cx < rect.x + rect.width + 3; ++cx)
                        {
                            for (int cy = rect.y - 3; cy < rect.y + rect.height + 3; ++cy)
                            {
                                if (cx < 0 || cy < 0
                                    || cx + 2 >= static_cast<int>(hm.getWidth())
                                    || cy + 2 >= static_cast<int>(hm.getHeight()))
                                {
                                    continue;
                                }
                                ++total;
                                if (isGridPointWalkable(s.terrain, mc, cx, cy))
                                {
                                    ++walk;
                                }
                            }
                        }
                        if (total == 0 || walk * 10 < total * 7)
                        {
                            continue;
                        }
                    }
                    auto id = s.trySpawnUnit(labType, owner, p, std::nullopt, true);
                    if (!id)
                    {
                        continue;
                    }
                    s.getUnitState(*id).finishBuilding(labDef);
                    return LabSite{*id, p, rect};
                }
            }
        }
        return std::nullopt;
    }

    TEST_CASE("AI build placement preserves factory egress")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        struct Result
        {
            bool openWithGap{false};
            bool sealedByCandidate{false};
            bool candidateRejected{false};
            bool alternativeValid{false};
            bool alternativeKeepsEgress{false};
        };
        std::optional<Result> outcome;

        auto hook = [&outcome](GameSimulation& s) {
            REQUIRE(!s.campaignAi.empty());
            auto& st = s.campaignAi.front();
            const PlayerId ai = st.player;
            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == ai && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);

            const auto& labDef = s.unitDefinitions.at("CORLAB");
            const auto& wallDef = s.unitDefinitions.at("CORSOLAR");

            // kbot product (CORAK) as the produced movement class
            auto mc = s.getAdHocMovementClass(s.unitDefinitions.at("CORAK").movementCollisionInfo);

            // a site where the kbot egress ring is walkable pre-walls
            auto site0 = findOpenFactorySite(s, com->position, labDef, "CORLAB", ai, mc, {}, 0);
            REQUIRE(site0);
            auto labRect = site0->rect;
            UnitId labId = site0->id;

            // U-shaped pocket with one open slot on the most
            // walkable edge -- a gap over unwalkable cells is no
            // gap; moat=2 leaves interior anchors so the fill can
            // actually route through it
            auto gapEdge = pickGapEdge(s, mc, labRect);
            auto pocket = spawnPocketWalls(s, wallDef, "CORSOLAR", ai, labRect, gapEdge, false, 2);
            REQUIRE(!pocket.walls.empty());

            Result r;
            auto probe0 = probeFactoryEgress(s, labRect, mc, nullptr, nullptr);
            r.openWithGap = probe0.open;

            // the missing slot is exactly the candidate that seals it
            auto sealed = probeFactoryEgress(s, labRect, mc, &pocket.gapRect, nullptr);
            r.sealedByCandidate = !sealed.open;

            st.groups[AiGroupFactory].push_back(labId);
            r.candidateRejected = placementSealsFactoryEgress(s, st, pocket.gapRect);

            // a builder standing by the gap: the spiral reaches the
            // sealing slot early but must skip it and take the next
            // footprint-valid site instead
            SimVector bPos = s.terrain.heightmapIndexToWorldCenter(
                pocket.gapRect.x + static_cast<int>(pocket.gapRect.width) / 2,
                pocket.gapRect.y - 3);
            bPos.y = s.terrain.getHeightAt(bPos.x, bPos.z);
            auto cvId = s.trySpawnUnit("CORCV", ai, bPos, std::nullopt, true);
            REQUIRE(cvId);
            const auto& cv = s.getUnitState(*cvId);
            const auto& cvDef = s.unitDefinitions.at(toUpper(cv.unitType));

            auto site = findBuildSite(s, st, cv, cvDef, wallDef);
            r.alternativeValid = site.has_value();
            if (site)
            {
                auto siteRect = s.computeFootprintRegion(*site, wallDef.movementCollisionInfo);
                r.alternativeKeepsEgress = !placementSealsFactoryEgress(s, st, siteRect);
            }
            outcome = r;
        };

        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 1, SkirmishDifficulty::Medium, hook);
        REQUIRE(outcome);
        const auto r = *outcome;

        INFO("the gapped pocket has open egress");
        CHECK(r.openWithGap);
        INFO("occupying the gap cell rect seals the factory yard");
        CHECK(r.sealedByCandidate);
        INFO("the prevention check flags the sealing candidate");
        CHECK(r.candidateRejected);
        INFO("findBuildSite still returns a legal site");
        CHECK(r.alternativeValid);
        INFO("the chosen site does not seal any factory");
        CHECK(r.alternativeKeepsEgress);
    }

    TEST_CASE("AI deadlock recovery reclaims own blocker and ignores enemy/scripted/mobile walls")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_deadlock_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        struct Pocket
        {
            UnitId lab;
            DiscreteRect labRect;
            std::vector<UnitId> wallIds;
            UnitId plug{0};
            bool hasPlug{false};
            UnitId trapped{0};
            UnitId builder{0};
            bool openAtStart{false};
            bool sealedAtStart{false};
        };
        struct Captured
        {
            Pocket own;         // sealed pocket -> reclaim expected
            Pocket enemy;       // enemy-owned walls -> no reclaim
            Pocket scripted;    // own walls, all scripted -> never reclaimed
            Pocket mobile;      // gap plugged by a mobile unit -> open
            bool hasAll{false};
        };
        std::optional<Captured> captured;

        auto hook = [&captured](GameSimulation& s) {
            REQUIRE(!s.campaignAi.empty());
            const PlayerId ai = s.campaignAi.front().player;
            const PlayerId human(0);

            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == ai && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);

            const auto& labDef = s.unitDefinitions.at("CORLAB");
            const auto& wallDef = s.unitDefinitions.at("CORSOLAR");
            const auto& akDef = s.unitDefinitions.at("CORAK");
            const auto& cvDef = s.unitDefinitions.at("CORCV");
            auto mc = s.getAdHocMovementClass(akDef.movementCollisionInfo);

            // four pockets spread out so their escape regions cannot
            // overlap (egress probe reaches ~384 world units)
            Captured c;
            std::vector<DiscreteRect> taken;
            Pocket* pockets[4] = {&c.own, &c.enemy, &c.scripted, &c.mobile};
            PlayerId wallOwners[4] = {ai, ai, ai, ai};
            // pocket k=1..3 cluster around the own pocket's verified
            // flat site -- recovery requires a land approach a builder
            // can actually path to, and the own pocket's site proves it
            const SimVector k0center = com->position + SimVector(SimScalar(448), SimScalar(0), SimScalar(0));
            SimVector k0pos = k0center;
            const int kOff[3][2] = {{896, 0}, {0, 896}, {896, 896}};
            int k = 0;
            for (auto* pk : pockets)
            {
                SimVector center = k == 0 ? k0center : k0pos + SimVector(SimScalar(kOff[k - 1][0]), SimScalar(0), SimScalar(kOff[k - 1][1]));
                // retry candidate sites until the laid ring actually
                // seals the pocket AND at least one own wall's removal
                // re-opens it -- map features (rocks etc.) can hold
                // the seal shut independently of the spawned walls,
                // which is a terrain trap no wall reclaim can fix
                bool ok = false;
                for (int attempt = 0; attempt < 12 && !ok; ++attempt)
                {
                    auto siteK = findOpenFactorySite(s, center, labDef, "CORLAB", ai, mc, taken, 30);
                    if (!siteK)
                    {
                        return;
                    }
                    taken.push_back(siteK->rect);
                    pk->lab = siteK->id;
                    pk->labRect = siteK->rect;
                    if (k == 0)
                    {
                        k0pos = siteK->pos;
                    }

                    std::vector<UnitId> spawned{siteK->id};

                    // the mobile pocket's plug must hold a walkable gap:
                    // pick the edge whose mid-gap cells the produced class
                    // can actually stand on
                    int gapEdge = pk == &c.mobile ? pickGapEdge(s, mc, pk->labRect) : 0;

                    auto lay = spawnPocketWalls(s, wallDef, "CORSOLAR", wallOwners[k], pk->labRect, gapEdge, pk != &c.mobile, 3);
                    pk->wallIds.clear();
                    for (const auto& w : lay.walls)
                    {
                        pk->wallIds.push_back(w.id);
                        spawned.push_back(w.id);
                    }
                    if (pk == &c.enemy)
                    {
                        // an ENEMY structure plugs the gap: recovery must
                        // route around it by removing an own wall, never
                        // reclaim it. Huge hit points so AI attack squads
                        // cannot destroy it during the run.
                        auto piece = spawnWallAtCells(s, wallDef, "CORSOLAR", human, lay.gapRect.x, lay.gapRect.y);
                        if (piece)
                        {
                            // an enemy structure plugs the gap: recovery
                            // must route around it by removing an own
                            // wall, never reclaim it. Huge hit points +
                            // immunity keep it alive AND off the attack
                            // task's target list -- otherwise squads
                            // volley it forever and the AoE splash
                            // grinds down the pocket contents
                            auto& plugUnit = s.getUnitState(piece->id);
                            plugUnit.hitPoints = 999999;
                            plugUnit.missionState = MissionUnitState{};
                            plugUnit.missionState->immunity = true;
                            pk->plug = piece->id;
                            pk->hasPlug = true;
                            spawned.push_back(piece->id);
                        }
                    }
                    if (pk == &c.mobile)
                    {
                        // a mobile own unit plugs the last open slot:
                        // traffic, never a wall
                        auto c0 = s.terrain.heightmapIndexToWorldCenter(lay.gapRect.x, lay.gapRect.y);
                        SimVector p = c0;
                        p.y = s.terrain.getHeightAt(p.x, p.z);
                        auto mid = s.trySpawnUnit("CORAK", ai, p, std::nullopt, true);
                        if (mid)
                        {
                            s.getUnitState(*mid).finishBuilding(akDef);
                            pk->plug = *mid;
                            pk->hasPlug = true;
                            spawned.push_back(*mid);
                        }
                    }

                    auto probe = probeFactoryEgress(s, pk->labRect, mc, nullptr, nullptr);
                    pk->openAtStart = probe.open;
                    pk->sealedAtStart = !probe.open;

                    if (pk == &c.mobile)
                    {
                        // the mobile plug must leave egress open
                        ok = probe.open;
                    }
                    else
                    {
                        // sealed at start, and some single own wall
                        // removal must be enough to open the route --
                        // the property recovery relies on
                        if (probe.open)
                        {
                            ok = false;
                        }
                        else
                        {
                            for (auto w : pk->wallIds)
                            {
                                if (probeFactoryEgress(s, pk->labRect, mc, nullptr, &w).open)
                                {
                                    ok = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (ok)
                    {
                        break;
                    }
                    // unsuitable site: strip everything and try the
                    // next candidate -- reclaim-style kills leave no
                    // corpses behind to poison the next layout
                    for (auto id : spawned)
                    {
                        s.quietlyKillUnit(id);
                    }
                    s.deleteDeadUnits();
                    pk->hasPlug = false;
                }
                if (!ok)
                {
                    return;
                }

                if (pk == &c.scripted)
                {
                    for (auto id : pk->wallIds)
                    {
                        s.getUnitState(id).missionState = MissionUnitState{};
                    }
                }

                // trapped product + a builder parked in the free moat
                // corridor beside the yard (the wall inner faces are
                // two cells further out -- within reclaim range)
                {
                    SimVector p = s.terrain.heightmapIndexToWorldCenter(pk->labRect.x + 1, pk->labRect.y - 2);
                    p.y = s.terrain.getHeightAt(p.x, p.z);
                    auto id = s.trySpawnUnit("CORAK", ai, p, std::nullopt, true);
                    if (!id)
                    {
                        p = s.terrain.heightmapIndexToWorldCenter(pk->labRect.x - 2, pk->labRect.y + 1);
                        p.y = s.terrain.getHeightAt(p.x, p.z);
                        id = s.trySpawnUnit("CORAK", ai, p, std::nullopt, true);
                    }
                    REQUIRE(id);
                    s.getUnitState(*id).finishBuilding(akDef);
                    // park the trapped product: without an order the
                    // AI repartitions it into a wander group and it
                    // roams the moat, jamming the reclaim path
                    s.getUnitState(*id).addOrder(WaitOrder{200000});
                    pk->trapped = *id;
                }
                {
                    SimVector p = s.terrain.heightmapIndexToWorldCenter(pk->labRect.x + pk->labRect.width + 1, pk->labRect.y + 1);
                    p.y = s.terrain.getHeightAt(p.x, p.z);
                    auto id = s.trySpawnUnit("CORCV", ai, p, std::nullopt, true);
                    if (!id)
                    {
                        p = s.terrain.heightmapIndexToWorldCenter(pk->labRect.x + 1, pk->labRect.y + pk->labRect.height + 1);
                        p.y = s.terrain.getHeightAt(p.x, p.z);
                        id = s.trySpawnUnit("CORCV", ai, p, std::nullopt, true);
                    }
                    REQUIRE(id);
                    s.getUnitState(*id).finishBuilding(cvDef);
                    pk->builder = *id;
                }
                ++k;
            }
            c.hasAll = true;
            captured = c;
        };

        // deadlock scan 300t cadence + 1800t confirm + reclaim
        // execution -- the enemy-pocket builder is an open-terrain
        // fallback (the pocketed CORCV cannot traverse kbot-class
        // moat terrain), so the commander walks across the map:
        // 14000t covers travel + reclaim with margin
        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 14000, SkirmishDifficulty::Medium, hook);
        auto& sim = run.sim;
        REQUIRE((captured && captured->hasAll));
        const auto c = *captured;
        const auto& st = sim.campaignAi.front();
        const PlayerId ai = st.player;
        auto mc = sim.getAdHocMovementClass(sim.unitDefinitions.at("CORAK").movementCollisionInfo);

        // --- recovery pocket: egress re-opened by reclaiming a wall
        {
            INFO("the sealed pocket started blocked");
            CHECK(c.own.sealedAtStart);
            auto probe = probeFactoryEgress(sim, c.own.labRect, mc, nullptr, nullptr);
            INFO("egress re-opened after reclaim (walls left standing: "
                << [&] {
                       unsigned int n = 0;
                       for (auto id : c.own.wallIds)
                       {
                           auto u = sim.tryGetUnitState(id);
                           if (u && u->get().isAlive())
                           {
                               ++n;
                           }
                       }
                       return n;
                   }()
                << " of " << c.own.wallIds.size() << ")");
            CHECK(probe.open);
            CHECK(st.deadlockedFactories.find(c.own.lab) == st.deadlockedFactories.end());
        }

        // --- enemy-plugged pocket: the enemy structure is never a
        // reclaim target; recovery removes an own wall instead
        {
            INFO("the enemy plug must survive -- never a reclaim target");
            auto plug = sim.tryGetUnitState(c.enemy.plug);
            CHECK((plug && plug->get().isAlive()));
            unsigned int ownWallsLeft = 0;
            for (auto id : c.enemy.wallIds)
            {
                auto u = sim.tryGetUnitState(id);
                if (u && u->get().isAlive())
                {
                    ++ownWallsLeft;
                }
            }
            INFO("egress restored by removing an own wall (walls left: "
                << ownWallsLeft << " of " << c.enemy.wallIds.size() << ")");
            auto probe = probeFactoryEgress(sim, c.enemy.labRect, mc, nullptr, nullptr);
            auto it0 = st.deadlockedFactories.find(c.enemy.lab);
            if (it0 != st.deadlockedFactories.end())
            {
                std::cout << " [enemy-lab dl since=" << (it0->second.blockedSince ? it0->second.blockedSince->value : -1)
                          << " tgt=" << (it0->second.reclaimTarget ? it0->second.reclaimTarget->value : -1)
                          << " bld=" << (it0->second.reclaimBuilder ? it0->second.reclaimBuilder->value : -1)
                          << " att=" << it0->second.reclaimAttempts
                          << " stalledAt=" << (it0->second.reclaimStalledAt ? it0->second.reclaimStalledAt->value : -1)
                          << "]" << std::endl;
            }
            else
            {
                std::cout << " [enemy-lab dl entry absent]" << std::endl;
            }
            // which wall died, and are its cells free?
            for (auto id : c.enemy.wallIds)
            {
                auto u = sim.tryGetUnitState(id);
                if (!u)
                {
                    std::cout << " [wall " << id.value << " dead+swept]" << std::endl;
                }
                else if (!u->get().isAlive())
                {
                    std::cout << " [wall " << id.value << " dead unswept pos=" << (int)simScalarToFloat(u->get().position.x) << "," << (int)simScalarToFloat(u->get().position.z) << "]" << std::endl;
                }
                else
                {
                    auto wc = sim.terrain.worldToHeightmapCoordinateNearest(u->get().position);
                    std::cout << " [wall " << id.value << " cell=" << wc.x << "," << wc.y << "]" << std::endl;
                }
            }
            {
                std::cout << " [sim t=" << sim.gameTime.value
                          << " lab-grouped=" << (std::find(st.groups[AiGroupFactory].begin(), st.groups[AiGroupFactory].end(), c.enemy.lab) != st.groups[AiGroupFactory].end())
                          << " lab=" << [&] {
                                 auto lu = sim.tryGetUnitState(c.enemy.lab);
                                 if (!lu)
                                     return std::string("swept");
                                 const auto& u = lu->get();
                                 return std::string("hp=") + std::to_string(u.hitPoints)
                                     + " dead=" + std::to_string(u.isDead())
                                     + " sd=" + std::to_string(u.selfDestruct ? (int)u.selfDestruct->countdownTicks : -1);
                             }()
                          << "]" << std::endl;
                if (it0 != st.deadlockedFactories.end() && it0->second.reclaimBuilder)
                {
                    auto ru = sim.tryGetUnitState(*it0->second.reclaimBuilder);
                    if (ru)
                    {
                        const auto& u = ru->get();
                        std::cout << " [enemy-rbld " << it0->second.reclaimBuilder->value
                                  << " pos=" << (int)simScalarToFloat(u.position.x) << "," << (int)simScalarToFloat(u.position.z)
                                  << " orders=" << u.orders.size()
                                  << " idx=" << (u.orders.empty() ? -1 : (int)u.orders.front().index())
                                  << " bstate=" << (int)u.behaviourState.index()
                                  << " nstate=" << (int)u.navigationState.state.index()
                                  << " tgtpos=" << [&] {
                                         auto tu = sim.tryGetUnitState(it0->second.reclaimTarget.value_or(UnitId(0)));
                                         if (!tu)
                                             return std::string("?");
                                         return std::to_string((int)simScalarToFloat(tu->get().position.x)) + ","
                                             + std::to_string((int)simScalarToFloat(tu->get().position.z));
                                     }()
                                  << "]" << std::endl;
                    }
                }
                auto bu = sim.tryGetUnitState(c.enemy.builder);
                if (bu)
                {
                    const auto& u = bu->get();
                    std::cout << " [enemy-builder " << c.enemy.builder.value << " alive=" << u.isAlive()
                              << " orders=" << u.orders.size()
                              << " idx=" << (u.orders.empty() ? -1 : (int)u.orders.front().index())
                              << " pos=" << (int)simScalarToFloat(u.position.x) << "," << (int)simScalarToFloat(u.position.z)
                              << "]" << std::endl;
                }
                else
                {
                    std::cout << " [enemy-builder " << c.enemy.builder.value << " swept]" << std::endl;
                }
                auto tu = sim.tryGetUnitState(c.enemy.trapped);
                if (tu)
                {
                    const auto& u = tu->get();
                    std::cout << " [enemy-trapped " << c.enemy.trapped.value << " alive=" << u.isAlive()
                              << " orders=" << u.orders.size()
                              << " pos=" << (int)simScalarToFloat(u.position.x) << "," << (int)simScalarToFloat(u.position.z)
                              << "]" << std::endl;
                }
            }
            std::cout << " [enemy-lab rect=" << c.enemy.labRect.x << "," << c.enemy.labRect.y
                      << " " << c.enemy.labRect.width << "x" << c.enemy.labRect.height
                      << " plug-cell=" << [&] {
                             auto pu = sim.tryGetUnitState(c.enemy.plug);
                             if (!pu)
                                 return std::string("dead");
                             auto pc = sim.terrain.worldToHeightmapCoordinateNearest(pu->get().position);
                             return std::to_string(pc.x) + "," + std::to_string(pc.y);
                         }()
                      << "]" << std::endl;
            for (const auto& [fid, f] : sim.features)
            {
                auto cell = sim.terrain.worldToHeightmapCoordinateNearest(f.position);
                if (cell.x >= c.enemy.labRect.x - 12 && cell.x <= c.enemy.labRect.x + c.enemy.labRect.width + 12
                    && cell.y >= c.enemy.labRect.y - 12 && cell.y <= c.enemy.labRect.y + c.enemy.labRect.height + 12)
                {
                    const auto& fdef = sim.getFeatureDefinition(f.featureName);
                    std::cout << " [feature near enemy-lab: " << f.featureName.value
                              << " blocking=" << (int)fdef.blocking
                              << " at cell=" << cell.x << "," << cell.y << "]" << std::endl;
                }
            }
            std::cout << " [probe open=" << probe.open << " pocket=" << probe.pocket.size() << " boundary=" << probe.boundaryBuildings.size() << "]" << std::endl;
            CHECK(probe.open);
            auto it = st.deadlockedFactories.find(c.enemy.lab);
            CHECK((it == st.deadlockedFactories.end() || !it->second.reclaimTarget
                     || *it->second.reclaimTarget != c.enemy.plug));
        }

        // --- scripted-walled pocket: authored content never reclaimed
        {
            INFO("scripted walls are never reclaim candidates");
            bool allAlive = true;
            for (auto id : c.scripted.wallIds)
            {
                auto u = sim.tryGetUnitState(id);
                allAlive = allAlive && u && u->get().isAlive();
            }
            CHECK(allAlive);
            auto it = st.deadlockedFactories.find(c.scripted.lab);
            CHECK((it == st.deadlockedFactories.end() || !it->second.reclaimTarget));
        }

        // --- mobile-plugged pocket: never counted as sealed
        {
            INFO("a mobile unit in the gap is traffic, not a wall");
            CHECK(c.mobile.openAtStart);
            CHECK(st.deadlockedFactories.find(c.mobile.lab) == st.deadlockedFactories.end());
        }
    }

    /**
     * Production-stall recovery: a factory can report OPEN egress and
     * still be deadlocked when a persistent occupant stands on its
     * footprint -- the yard-open script retries forever (no
     * IN_BUILD_STANCE) or the nanoframe spawn cell is occupied, and
     * no completed product exists to trip the sealed-egress test.
     * Four labs cover the real-save failure shapes: an own structure
     * overlapping the footprint (reclaim), a mobile sealed on yard
     * cells (reclaim, leaves no corpse), a scripted mobile sealed on
     * yard cells (never a candidate), and a mobile half-off the
     * footprint that can still leave under its own power (evict).
     */
    TEST_CASE("AI production-stall recovery clears yard occupants with open egress")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_stall_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        struct StallCase
        {
            UnitId lab;
            DiscreteRect labRect;
            UnitId blocker{0};
            UnitId builder{0};
        };
        struct Captured
        {
            StallCase wallLab;      // own structure on footprint -> reclaim
            StallCase sealedMobLab; // mobile sealed on yard cells -> reclaim
            StallCase scriptMobLab; // scripted mobile on yard cells -> never
            StallCase openMobLab;   // mobile that can leave -> evict
            bool hasAll{false};
        };
        std::optional<Captured> captured;

        auto hook = [&captured](GameSimulation& s) {
            REQUIRE(!s.campaignAi.empty());
            const PlayerId ai = s.campaignAi.front().player;

            const UnitState* com = nullptr;
            for (const auto& [id, u] : s.units)
            {
                if (u.owner == ai && !u.isDead())
                {
                    com = &u;
                    break;
                }
            }
            REQUIRE(com);

            const auto& labDef = s.unitDefinitions.at("CORLAB");
            const auto& wallDef = s.unitDefinitions.at("CORSOLAR");
            const auto& akDef = s.unitDefinitions.at("CORAK");
            const auto& cvDef = s.unitDefinitions.at("CORCV");
            auto mc = s.getAdHocMovementClass(akDef.movementCollisionInfo);

            Captured c;
            std::vector<DiscreteRect> taken;
            StallCase* cases[4] = {&c.wallLab, &c.sealedMobLab, &c.scriptMobLab, &c.openMobLab};
            const SimVector k0center = com->position + SimVector(SimScalar(448), SimScalar(0), SimScalar(0));
            SimVector k0pos = k0center;
            const int kOff[3][2] = {{896, 0}, {0, 896}, {896, 896}};
            int k = 0;
            for (auto* sc : cases)
            {
                SimVector center = k == 0 ? k0center : k0pos + SimVector(SimScalar(kOff[k - 1][0]), SimScalar(0), SimScalar(kOff[k - 1][1]));
                auto siteK = findOpenFactorySite(s, center, labDef, "CORLAB", ai, mc, taken, 30);
                if (!siteK)
                {
                    return;
                }
                taken.push_back(siteK->rect);
                sc->lab = siteK->id;
                sc->labRect = siteK->rect;
                if (k == 0)
                {
                    k0pos = siteK->pos;
                }

                auto& lab = s.getUnitState(sc->lab);
                lab.buildQueue.emplace_back("CORAK", 1);
                // wallLab and openMobLab exercise the yard-open
                // stall branch; the mobile labs exercise the
                // nanoframe-spawn stall branch
                if (k == 1 || k == 2)
                {
                    lab.inBuildStance = true;
                }

                // a recovery builder parked in reclaim range, pinned
                // by a WaitOrder the deadlock task may preempt
                {
                    SimVector p = s.terrain.heightmapIndexToWorldCenter(sc->labRect.x + sc->labRect.width + 1, sc->labRect.y + 1);
                    p.y = s.terrain.getHeightAt(p.x, p.z);
                    auto id = s.trySpawnUnit("CORCV", ai, p, std::nullopt, true);
                    if (!id)
                    {
                        p = s.terrain.heightmapIndexToWorldCenter(sc->labRect.x + 1, sc->labRect.y + sc->labRect.height + 1);
                        p.y = s.terrain.getHeightAt(p.x, p.z);
                        id = s.trySpawnUnit("CORCV", ai, p, std::nullopt, true);
                    }
                    REQUIRE(id);
                    s.getUnitState(*id).finishBuilding(cvDef);
                    s.getUnitState(*id).addOrder(WaitOrder{200000});
                    sc->builder = *id;
                }

                auto spawnAk = [&](int cx, int cy) {
                    SimVector p = s.terrain.heightmapIndexToWorldCenter(cx, cy);
                    p.y = s.terrain.getHeightAt(p.x, p.z);
                    auto id = s.trySpawnUnit("CORAK", ai, p, std::nullopt, true);
                    REQUIRE(id);
                    s.getUnitState(*id).finishBuilding(akDef);
                    return *id;
                };

                if (sc == &c.wallLab)
                {
                    // an own structure overlapping the footprint
                    // corner: the yard-open cells it covers can never
                    // clear -- occupancy is the proof
                    auto piece = spawnWallAtCells(s, wallDef, "CORSOLAR", ai, sc->labRect.x, sc->labRect.y);
                    REQUIRE(piece);
                    sc->blocker = piece->id;
                }
                else if (sc == &c.openMobLab)
                {
                    // half-on the footprint edge: still inside the
                    // yard scan, but the unit's own flood probe
                    // reaches open ground so a shove suffices
                    sc->blocker = spawnAk(sc->labRect.x + sc->labRect.width - 1, sc->labRect.y + 1);
                }
                else
                {
                    // sealedMobLab / scriptMobLab: a mobile parked
                    // entirely on the factory's cells is sealed by
                    // the closed yard itself -- its own probe has
                    // nowhere to flood from
                    sc->blocker = spawnAk(sc->labRect.x + 1, sc->labRect.y + 1);
                    if (sc == &c.scriptMobLab)
                    {
                        s.getUnitState(sc->blocker).missionState = MissionUnitState{};
                    }
                }
                ++k;
            }
            c.hasAll = true;
            captured = c;
        };

        // deadlock scan 300t cadence + 1800t confirm + reclaim travel
        // and execution -- the parked CORCVs keep travel short
        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 14000, SkirmishDifficulty::Medium, hook);
        auto& sim = run.sim;
        REQUIRE((captured && captured->hasAll));
        const auto c = *captured;
        const auto& st = sim.campaignAi.front();

        // --- own structure on the footprint: reclaimed, lab recovered
        {
            auto blocker = sim.tryGetUnitState(c.wallLab.blocker);
            INFO("the footprint structure was reclaimed");
            CHECK((!blocker || !blocker->get().isAlive()));
            CHECK(st.deadlockedFactories.find(c.wallLab.lab) == st.deadlockedFactories.end());
        }

        // --- mobile sealed on yard cells: reclaimed (no corpse
        // possible from reclaim), lab recovered
        {
            auto blocker = sim.tryGetUnitState(c.sealedMobLab.blocker);
            INFO("the sealed yard mobile was reclaimed");
            CHECK((!blocker || !blocker->get().isAlive()));
            CHECK(st.deadlockedFactories.find(c.sealedMobLab.lab) == st.deadlockedFactories.end());
        }

        // --- scripted mobile on yard cells: authored content is
        // never a reclaim target even when it is the proven blocker
        {
            auto blocker = sim.tryGetUnitState(c.scriptMobLab.blocker);
            INFO("the scripted mobile must survive");
            CHECK((blocker && blocker->get().isAlive()));
            auto it = st.deadlockedFactories.find(c.scriptMobLab.lab);
            CHECK((it == st.deadlockedFactories.end() || !it->second.reclaimTarget
                     || *it->second.reclaimTarget != c.scriptMobLab.blocker));
        }

        // --- open mobile half-off the footprint: eviction freed it
        // (it moved off the yard cells) or recovery cleared the lab
        // some other way -- either way the lab must not stay stalled
        {
            CHECK(st.deadlockedFactories.find(c.openMobLab.lab) == st.deadlockedFactories.end());
        }
    }

    /**
     * Legacy-save AI diagnosis: restores a real save image against
     * retail definitions (the same harness shape as the SavedGame
     * apply test), re-attaches campaign AI for any computer player
     * the old format could not serialize (LoadingScene's
     * attachCampaignAiPlayers), then ticks forward while dumping the
     * AI's group roster, factory egress probes, deadlock bookkeeping
     * and issued recovery orders. RWE_TEST_SAVE_TICKS bounds the run
     * (default 7200t: 300t scan cadence + 1800t confirm + reclaim
     * travel margin). Gated on RWE_DATA_PATH + RWE_TEST_SAVE_FILE.
     */
    TEST_CASE("a legacy save restores and resumes its computer players")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        const char* savePathEnv = std::getenv("RWE_TEST_SAVE_FILE");
        const char* ticksEnv = std::getenv("RWE_TEST_SAVE_TICKS");
        if (dataPathEnv == nullptr || savePathEnv == nullptr
            || !std::filesystem::exists(std::filesystem::path(dataPathEnv))
            || !std::filesystem::exists(std::filesystem::path(savePathEnv)))
        {
            return;
        }
        int tickCount = ticksEnv ? std::atoi(ticksEnv) : 7200;
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_legacy_save_diag.log", false));

        std::ifstream saveFile(savePathEnv, std::ios::binary);
        std::vector<char> fileBytes((std::istreambuf_iterator<char>(saveFile)), std::istreambuf_iterator<char>());
        auto decoded = decodeSavedGame(fileBytes);
        std::cout << "[legacy-save] format v" << decoded.version
                  << " map=" << decoded.meta.mapName
                  << " schema=" << decoded.meta.schemaIndex
                  << " campaign=" << decoded.meta.isCampaign
                  << " gameTime=" << decoded.meta.gameTime.value
                  << " players=" << decoded.meta.playerCount << std::endl;

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        // --- static world reconstruction (same as the SavedGame
        // apply harness, but defs come from the GUI-flattened pack
        // so builder buildOptions are populated) ---
        auto tntBytes = vfs.readFile("maps/" + decoded.meta.mapName + ".tnt");
        REQUIRE(tntBytes);
        SpanStream tntStream(tntBytes->data(), tntBytes->size());
        TntArchive tnt(&tntStream);
        Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
        tnt.readMapAttributes(mapAttributes.getData());
        auto heightGrid = getHeightGrid(mapAttributes);
        auto featureNames = getFeatureNames(tnt);

        auto otaBytes = vfs.readFile("maps/" + decoded.meta.mapName + ".ota");
        REQUIRE(otaBytes);
        auto ota = parseOta(parseTdfFromString(std::string(otaBytes->data(), otaBytes->size())));
        const auto& schema = ota.schemas.at(decoded.meta.schemaIndex);

        auto surfaceMetal = schema.surfaceMetal;
        GameSimulation sim(
            MapTerrain(std::move(heightGrid), SimScalar(tnt.getHeader().seaLevel)),
            static_cast<unsigned char>(surfaceMetal),
            ota.minWindSpeed,
            ota.maxWindSpeed,
            ota.tidalStrength);
        sim.unitDefinitions = pack.unitDefinitions;
        sim.weaponDefinitions = pack.weaponDefinitions;
        sim.movementClassDatabase = pack.movementClassDatabase;
        sim.unitModelDefinitions = pack.unitModelDefinitions;
        sim.unitScriptDefinitions = pack.unitScriptDefinitions;
        sim.movementClassCollisionService = createMovementClassCollisionService(sim.terrain, sim.movementClassDatabase);

        std::unordered_set<std::string> requiredFeatures;
        mapAttributes.forEachIndexed([&](auto, const auto& e) {
            if (e.feature != TntTileAttributes::FeatureNone
                && e.feature != TntTileAttributes::FeatureUnknown
                && e.feature != TntTileAttributes::FeatureVoid)
            {
                requiredFeatures.insert(toUpper(featureNames.at(e.feature)));
            }
        });
        for (const auto& f : schema.features)
        {
            requiredFeatures.insert(toUpper(f.featureName));
        }
        for (const auto& [name, def] : sim.unitDefinitions)
        {
            if (!def.corpse.empty())
            {
                requiredFeatures.insert(toUpper(def.corpse));
            }
        }
        for (const auto& name : requiredFeatures)
        {
            if (pack.featureTdfs.count(name) != 0)
            {
                loadFeatureDefinitions(pack.featureTdfs, sim.featureDefinitions, sim.featureNameIndex, name);
            }
        }

        CampaignProgress progress;
        applySavedGame(sim, decoded, &progress);

        // LoadingScene restore path: any computer player the old
        // format could not serialize gets a fresh AI state from the
        // schema profile
        auto aiDifficulty = decoded.meta.isCampaign
            ? campaignAiDifficultyIndex(decoded.meta.difficulty)
            : skirmishAiDifficultyIndex(decoded.meta.skirmishDifficulty
                  ? static_cast<SkirmishDifficulty>(*decoded.meta.skirmishDifficulty)
                  : SkirmishDifficulty::Medium);
        if (auto profileText = loadCampaignAiProfileText(vfs, "ai", schema.aiProfile))
        {
            attachCampaignAiPlayers(sim, parseCampaignAiProfile(*profileText, aiDifficulty));
        }

        std::cout << "[legacy-save] post-attach campaignAi states=" << sim.campaignAi.size() << std::endl;
        for (const auto& st : sim.campaignAi)
        {
            std::cout << "  ai player=" << st.player.value
                      << " weights=" << st.unitWeights.size()
                      << " limits=" << st.unitLimits.size()
                      << " factoryGrp=" << st.groups[AiGroupFactory].size()
                      << " builderGrp=" << st.groups[AiGroupBuilder].size()
                      << " deadlocked=" << st.deadlockedFactories.size() << std::endl;
        }
        for (Index i = 0; i < getSize(sim.players); ++i)
        {
            const auto& p = sim.players[i];
            if (p.type != GamePlayerType::Computer)
            {
                continue;
            }
            unsigned int ownedMobile = 0, ownedStatic = 0;
            for (const auto& [id, u] : sim.units)
            {
                if (u.owner != PlayerId(i) || u.isDead())
                {
                    continue;
                }
                const auto& d = sim.unitDefinitions.at(toUpper(u.unitType));
                (d.isMobile ? ownedMobile : ownedStatic)++;
            }
            std::cout << "  computer p" << i << " status=" << static_cast<int>(p.status)
                      << " M=" << p.metal.value << " E=" << p.energy.value
                      << " mobile=" << ownedMobile << " static=" << ownedStatic << std::endl;
        }

        // --- tick forward with periodic AI/egress diagnostics ---
        std::map<unsigned int, unsigned int> reclaimOrdersSeen;
        std::map<unsigned int, unsigned int> selfDestructSeen;
        std::map<unsigned int, unsigned int> buggerOffSeen;
        unsigned int aiOrderedEver = 0;
        for (int i = 0; i < tickCount; ++i)
        {
            sim.tick();
            for (const auto& st : sim.campaignAi)
            {
                for (const auto& [id, u] : sim.units)
                {
                    if (u.owner != st.player || u.isDead() || u.orders.empty())
                    {
                        continue;
                    }
                    for (const auto& o : u.orders)
                    {
                        if (std::holds_alternative<ReclaimOrder>(o))
                        {
                            ++reclaimOrdersSeen[st.player.value];
                        }
                        if (std::holds_alternative<SelfDestructOrder>(o))
                        {
                            ++selfDestructSeen[st.player.value];
                        }
                        if (std::holds_alternative<BuggerOffOrder>(o))
                        {
                            ++buggerOffSeen[st.player.value];
                        }
                    }
                    aiOrderedEver |= 1u << st.player.value;
                }
            }
            if ((i + 1) % 300 == 0 || i + 1 == tickCount)
            {
                for (const auto& st : sim.campaignAi)
                {
                    std::cout << "  t+" << (i + 1) << " ai p" << st.player.value
                              << " factories=" << st.groups[AiGroupFactory].size()
                              << " builders=" << st.groups[AiGroupBuilder].size()
                              << " deadlocked=" << st.deadlockedFactories.size();
                    for (UnitId bid : st.groups[AiGroupBuilder])
                    {
                        if (auto bu = sim.tryGetUnitState(bid))
                        {
                            const auto& b = bu->get();
                            const char* otype = "none";
                            if (!b.orders.empty())
                            {
                                const auto& o = b.orders.front();
                                otype = std::holds_alternative<MoveOrder>(o) ? "move"
                                    : std::holds_alternative<PatrolOrder>(o) ? "patrol"
                                    : std::holds_alternative<GuardOrder>(o) ? "guard"
                                    : std::holds_alternative<ReclaimOrder>(o) ? "reclaim"
                                    : std::holds_alternative<BuildOrder>(o) ? "build"
                                    : std::holds_alternative<BuggerOffOrder>(o) ? "buggeroff"
                                    : std::holds_alternative<CompleteBuildOrder>(o) ? "completebuild"
                                    : std::holds_alternative<RepairOrder>(o) ? "repair"
                                    : "other";
                            }
                            const auto& bDef = sim.unitDefinitions.at(toUpper(b.unitType));
                            auto brect = sim.computeFootprintRegion(b.position, bDef.movementCollisionInfo);
                            auto bmc = sim.getAdHocMovementClass(bDef.movementCollisionInfo);
                            auto bprobe = probeFactoryEgress(sim, brect, bmc, nullptr, nullptr);
                            std::cout << " [b" << bid.value << " " << b.unitType << " ord=" << otype
                                      << " idx=" << (b.orders.empty() ? -1 : (int)b.orders.front().index())
                                      << " eg=" << (bprobe.open ? "open" : "SEALED")
                                      << " n=" << b.orders.size()
                                      << " (" << (int)simScalarToFloat(b.position.x) << "," << (int)simScalarToFloat(b.position.z) << ")]";
                        }
                    }
                    for (const auto& [fid, dl] : st.deadlockedFactories)
                    {
                        std::cout << " [f" << fid.value
                                  << " since=" << (dl.blockedSince ? std::to_string(dl.blockedSince->value) : "-")
                                  << " tgt=" << (dl.reclaimTarget ? std::to_string(dl.reclaimTarget->value) : "-")
                                  << " bld=" << (dl.reclaimBuilder ? std::to_string(dl.reclaimBuilder->value) : "-")
                                  << " att=" << dl.reclaimAttempts << "]";
                    }
                    std::cout << std::endl;

                    // probe every own static builder (factory)
                    for (const auto& [id, u] : sim.units)
                    {
                        if (u.owner != st.player || u.isDead())
                        {
                            continue;
                        }
                        const auto& d = sim.unitDefinitions.at(toUpper(u.unitType));
                        if (!d.builder || d.isMobile)
                        {
                            continue;
                        }
                        auto mc = std::optional<MovementClassDefinition>{};
                        for (const auto& option : d.buildOptions)
                        {
                            auto it = sim.unitDefinitions.find(option);
                            if (it != sim.unitDefinitions.end() && it->second.isMobile)
                            {
                                mc = sim.getAdHocMovementClass(it->second.movementCollisionInfo);
                                break;
                            }
                        }
                        if (!mc)
                        {
                            continue;
                        }
                        auto rect = sim.computeFootprintRegion(u.position, d.movementCollisionInfo);
                        auto probe = probeFactoryEgress(sim, rect, *mc, nullptr, nullptr);
                        const char* fstate = std::holds_alternative<FactoryBehaviorStateIdle>(u.factoryState) ? "idle"
                            : std::holds_alternative<FactoryBehaviorStateCreatingUnit>(u.factoryState) ? "creating"
                            : "building";
                        std::cout << "    factory " << u.unitType << " id=" << id.value
                                  << " egress=" << (probe.open ? "open" : "SEALED")
                                  << " boundary=" << probe.boundaryBuildings.size()
                                  << " queue=" << u.buildQueue.size()
                                  << " orders=" << u.orders.size()
                                  << " fstate=" << fstate
                                  << " stance=" << u.inBuildStance
                                  << " yard=" << u.yardOpen
                                  << " active=" << u.activated
                                  << " cobThreads=";
                        if (u.cobEnvironment)
                        {
                            bool first = true;
                            for (const auto& t : u.cobEnvironment->threads)
                            {
                                std::cout << (first ? "" : ",") << t->name;
                                first = false;
                            }
                        }
                        std::cout << std::endl;

                        // report occupied cells inside the yard
                        // rect -- the nanoframe footprint must be
                        // clear for production to even begin
                        if (auto region = sim.occupiedGrid.tryToRegion(rect))
                        {
                            std::unordered_set<unsigned int> reported;
                            sim.occupiedGrid.forEach(*region, [&](const auto& c) {
                                if (c.buildingInfo && c.buildingInfo->unit.value != id.value
                                    && reported.insert(c.buildingInfo->unit.value).second)
                                {
                                    if (auto bu = sim.tryGetUnitState(c.buildingInfo->unit))
                                    {
                                        std::cout << "      yard-block building " << bu->get().unitType
                                                  << " id=" << c.buildingInfo->unit.value
                                                  << " owner=" << bu->get().owner.value
                                                  << " scripted=" << (bu->get().missionState ? 1 : 0)
                                                  << " passable=" << c.buildingInfo->passable << std::endl;
                                    }
                                }
                                if (c.featureId && reported.insert(0x80000000u + c.featureId->value).second)
                                {
                                    const auto& f = sim.getFeature(*c.featureId);
                                    const auto& fdef = sim.getFeatureDefinition(f.featureName);
                                    std::cout << "      yard-block feature id=" << c.featureId->value
                                              << " defId=" << f.featureName.value
                                              << " blocking=" << fdef.blocking << std::endl;
                                }
                                if (c.mobileUnitId && reported.insert(0x40000000u + c.mobileUnitId->value).second)
                                {
                                    if (auto mu = sim.tryGetUnitState(*c.mobileUnitId))
                                    {
                                        const auto& m = mu->get();
                                        const char* mstate = std::holds_alternative<UnitBehaviorStateIdle>(m.behaviourState) ? "idle"
                                            : std::holds_alternative<UnitBehaviorStateBuilding>(m.behaviourState) ? "building"
                                            : std::holds_alternative<UnitBehaviorStateReclaiming>(m.behaviourState) ? "reclaiming"
                                            : "other";
                                        std::cout << "      yard-block mobile " << m.unitType
                                                  << " id=" << c.mobileUnitId->value
                                                  << " owner=" << m.owner.value
                                                  << " state=" << mstate
                                                  << " nav=" << std::holds_alternative<NavigationStateMoving>(m.navigationState.state)
                                                  << " orders=" << m.orders.size()
                                                  << " pos=(" << simScalarToFloat(m.position.x) << "," << simScalarToFloat(m.position.z) << ")";
                                        if (const auto* mv = std::get_if<NavigationStateMoving>(&m.navigationState.state))
                                        {
                                            std::cout << " pReq=" << mv->pathRequested
                                                      << " pEx=" << mv->pathExhausted
                                                      << " hasPath=" << (mv->path ? 1 : 0);
                                        }
                                        if (auto mdIt = sim.unitDefinitions.find(toUpper(m.unitType)); mdIt != sim.unitDefinitions.end())
                                        {
                                            auto mrect = sim.computeFootprintRegion(m.position, mdIt->second.movementCollisionInfo);
                                            auto mmc = sim.getAdHocMovementClass(mdIt->second.movementCollisionInfo);
                                            auto mprobe = probeFactoryEgress(sim, mrect, mmc, nullptr, nullptr);
                                            std::cout << " egress=" << (mprobe.open ? "open" : "SEALED")
                                                      << " boundary=" << mprobe.boundaryBuildings.size();
                                            for (UnitId bid : mprobe.boundaryBuildings)
                                            {
                                                if (auto bu = sim.tryGetUnitState(bid))
                                                {
                                                    std::cout << " [" << bu->get().unitType << " " << bid.value
                                                              << " owner=" << bu->get().owner.value
                                                              << (bu->get().missionState ? " scripted" : "") << "]";
                                                }
                                            }
                                        }
                                        std::cout << std::endl;
                                    }
                                }
                            });
                        }
                    }
                }
            }
        }

        // the AI must at minimum still be partitioning its forces
        for (const auto& st : sim.campaignAi)
        {
            INFO("ai player " << st.player.value << " never grouped any units");
            CHECK(st.groups[AiGroupFactory].size() + st.groups[AiGroupBuilder].size()
                    + st.groups[AiGroupReserveA].size() + st.groups[AiGroupReserveB].size()
                > 0);
        }
        for (const auto& [player, count] : reclaimOrdersSeen)
        {
            std::cout << "[legacy-save] ai p" << player << " reclaim orders observed=" << count << std::endl;
        }
        for (const auto& [player, count] : selfDestructSeen)
        {
            std::cout << "[legacy-save] ai p" << player << " self-destruct orders observed=" << count << std::endl;
        }
        for (const auto& [player, count] : buggerOffSeen)
        {
            std::cout << "[legacy-save] ai p" << player << " bugger-off orders observed=" << count << std::endl;
        }
    }

    /**
     * Save artifact generator -- not a regression check. When
     * RWE_WRITE_V7_SAVE names an output path, builds a real skirmish
     * sim on a real retail map, ticks it into a live mid-game state
     * and writes a v7-format RWESAVE image so the
     * RWE_TEST_SAVE_FILE harness can exercise the v7 reader path
     * against freshly loaded retail definitions. The writer is the
     * production encodeSavedGame with formatVersion=7: every
     * `version >= 8` field is absent, matching the bytes a v7 build
     * produced for the same state.
     */
    TEST_CASE("v7-format save image artifact writer")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        const char* outPath = std::getenv("RWE_WRITE_V7_SAVE");
        if (dataPathEnv == nullptr || outPath == nullptr
            || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);
        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 600, SkirmishDifficulty::Medium);

        GameParameters params("Coast To Coast", schemaIndex);
        for (Index i = 0; i < getSize(run.sim.players); ++i)
        {
            const auto& p = run.sim.players[i];
            params.players[i] = PlayerInfo{
                std::nullopt,
                p.type == GamePlayerType::Human
                    ? PlayerControllerType(PlayerControllerTypeHuman())
                    : PlayerControllerType(PlayerControllerTypeComputer()),
                p.side,
                p.color,
                p.metal,
                p.energy};
        }
        params.skirmish.startLocation = SkirmishStartLocation::Fixed;
        params.skirmish.mapping = SkirmishMapping::Mapped;
        params.skirmish.lineOfSight = SkirmishLineOfSight::True;
        params.skirmish.difficulty = SkirmishDifficulty::Medium;

        auto bytes = encodeSavedGame(run.sim, params, nullptr, "v7qa", 1700000000, 7);
        std::ofstream out(outPath, std::ios::binary);
        out.write(bytes.data(), bytes.size());
        REQUIRE(out.good());
    }

    /**
     * Save/restore COB animation continuity: piece names on restored
     * units must keep resolving so script piece commands still land.
     * A regression where the restored pieceNameToIndices map is keyed
     * by raw (lowercase 3DO) names while findPiece() queries uppercase
     * leaves every COB move/turn a silent no-op -- units slide without
     * leg animation and factory doors freeze in their saved pose.
     * The hash alone cannot see this: pieces and the cob environment
     * are presentation-side state excluded from computeHash, so this
     * test compares piece transforms and factory yard flags directly
     * against a never-saved control sim.
     */
    TEST_CASE("save/load preserves COB piece animation and factory state")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto pack = loadSkirmishDefinitionPack(vfs);
        REQUIRE(!pack.unitDefinitions.empty());

        auto otaRaw = vfs.readFile("maps/Coast To Coast.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        unsigned int schemaIndex = resolveSkirmishSchemaIndex(ota, 2);

        auto run = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 0, SkirmishDifficulty::Medium);
        auto& sim = run.sim;
        PlayerId human = run.playerIds.front();

        // anchor on player 1's schema start pos -- guaranteed
        // buildable ground; player 2's start makes a far move target
        // so the kbot is still mid-stride at save time
        const auto& schema = ota.schemas.at(schemaIndex);
        const auto* startPos = findStartPosSpecial(schema, 1);
        const auto* farStart = findStartPosSpecial(schema, 2);
        REQUIRE(startPos);
        REQUIRE(farStart);
        auto basePos = sim.terrain.topLeftCoordinateToWorld(
            SimVector(SimScalar(startPos->xPos), SimScalar(0), SimScalar(startPos->zPos)));
        auto cell = sim.terrain.worldToHeightmapCoordinateNearest(basePos);
        auto place = [&](int dx, int dz) {
            return sim.terrain.heightmapIndexToWorldCenter(cell.x + dx, cell.y + dz);
        };
        auto farPos = sim.terrain.topLeftCoordinateToWorld(
            SimVector(SimScalar(farStart->xPos), SimScalar(0), SimScalar(farStart->zPos)));

        auto spawnDone = [&](const std::string& type, SimVector pos) -> UnitId {
            auto id = sim.trySpawnUnit(type, human, pos, std::nullopt, true, std::nullopt, true);
            REQUIRE(id);
            sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(toUpper(type)));
            return *id;
        };

        // diagnostic set: walking kbot, an idle unit, an open
        // producing factory, and a closed idle factory
        auto kbot = spawnDone("CORCK", place(10, -4));
        auto idle = spawnDone("ARMCK", place(10, 4));
        auto lab = spawnDone("ARMLAB", place(20, -4));
        auto vp = spawnDone("ARMVP", place(20, 4));

        sim.getUnitState(kbot).orders.push_back(UnitOrder(MoveOrder(farPos)));
        sim.getUnitState(lab).buildQueue.emplace_back("ARMPW", 1);

        for (int i = 0; i < 400; ++i)
        {
            sim.tick();
        }

        const auto& labPre = sim.getUnitState(lab);
        REQUIRE(labPre.yardOpen);
        REQUIRE((std::holds_alternative<FactoryBehaviorStateBuilding>(labPre.factoryState)
            || std::holds_alternative<FactoryBehaviorStateCreatingUnit>(labPre.factoryState)));

        auto pieceSignature = [](const UnitState& u) {
            long long sig = 0;
            for (const auto& p : u.pieces)
            {
                sig = sig * 31
                    + static_cast<long long>(p.offset.x.value + p.offset.y.value + p.offset.z.value)
                    + static_cast<long long>(p.rotationX.value + p.rotationY.value + p.rotationZ.value);
            }
            return sig;
        };
        auto threadCount = [](const UnitState& u) {
            return u.cobEnvironment ? u.cobEnvironment->threads.size() : std::size_t(0);
        };

        // snapshot pre-save state for the tracked units
        const auto sigKbotPre = pieceSignature(sim.getUnitState(kbot));
        const auto sigLabPre = pieceSignature(sim.getUnitState(lab));
        const auto threadsKbotPre = threadCount(sim.getUnitState(kbot));

        GameParameters params("Coast To Coast", schemaIndex);
        for (Index i = 0; i < getSize(sim.players); ++i)
        {
            const auto& p = sim.players[i];
            params.players[i] = PlayerInfo{
                std::nullopt,
                p.type == GamePlayerType::Human
                    ? PlayerControllerType(PlayerControllerTypeHuman())
                    : PlayerControllerType(PlayerControllerTypeComputer()),
                p.side,
                p.color,
                p.metal,
                p.energy};
        }

        auto bytes = encodeSavedGame(sim, params, nullptr, "anim", 1700000000);
        auto decoded = decodeSavedGame(bytes);

        auto run2 = buildAndTickSkirmishSchema(pack, "Coast To Coast", ota, schemaIndex, 2, vfs, 0, SkirmishDifficulty::Medium);
        applySavedGame(run2.sim, decoded, nullptr);
        auto& sim2 = run2.sim;

        // immediate post-load parity: piece poses, yard flags, and the
        // serialized thread table all carry across
        REQUIRE(pieceSignature(sim2.getUnitState(kbot)) == sigKbotPre);
        REQUIRE(pieceSignature(sim2.getUnitState(lab)) == sigLabPre);
        REQUIRE(threadCount(sim2.getUnitState(kbot)) == threadsKbotPre);
        REQUIRE(sim2.getUnitState(lab).yardOpen == sim.getUnitState(lab).yardOpen);

        // a moving unit's walk cycle must keep animating after load:
        // the piece signature changes while it strides. The restored
        // pieceNameToIndices map is what lets the script's piece
        // commands land at all.
        for (int i = 0; i < 30; ++i)
        {
            sim2.tick();
            sim.tick();
        }
        const auto& kbotL = sim2.getUnitState(kbot);
        const auto& kbotC = sim.getUnitState(kbot);
        const bool kbotStillMoving = !areCloserThan(kbotL.previousPosition, kbotL.position, 0.1_ssf);
        if (kbotStillMoving)
        {
            REQUIRE(pieceSignature(kbotL) == pieceSignature(kbotC));
        }

        // post-load orders: idle unit moves, closed factory produces.
        // Tick both sims identically so end-state comparisons are fair.
        sim2.getUnitState(idle).orders.push_back(UnitOrder(MoveOrder(farPos)));
        sim2.getUnitState(vp).buildQueue.emplace_back("ARMCV", 1);
        sim.getUnitState(idle).orders.push_back(UnitOrder(MoveOrder(farPos)));
        sim.getUnitState(vp).buildQueue.emplace_back("ARMCV", 1);
        for (int i = 0; i < 870; ++i)
        {
            sim2.tick();
            sim.tick();
        }

        // CASE A: the saved-open lab finished production and ran its
        // close animation -- logical yard closed AND doors physically
        // returned to rest, identical to the never-saved control.
        const auto& labL = sim2.getUnitState(lab);
        const auto& labC = sim.getUnitState(lab);
        REQUIRE(labL.yardOpen == labC.yardOpen);
        REQUIRE(pieceSignature(labL) == pieceSignature(labC));

        // CASE B: the saved-closed VP opened both logically and
        // physically on the post-load production order.
        const auto& vpL = sim2.getUnitState(vp);
        const auto& vpC = sim.getUnitState(vp);
        REQUIRE(vpL.yardOpen == vpC.yardOpen);
        REQUIRE(pieceSignature(vpL) == pieceSignature(vpC));

        // no thread duplication / loss across the round trip
        REQUIRE(threadCount(sim2.getUnitState(kbot)) == threadCount(sim.getUnitState(kbot)));

        // deterministic continuation: identical hashed state
        REQUIRE(sim2.computeHash() == sim.computeHash());
    }
#include "RecoverySaveRegression.inc"

}
