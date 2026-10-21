#include "LoadingScene.h"
#include <algorithm>
#include <unordered_set>
#include <rwe/util/SpanStream.h>
#include <rwe/LoadingScene_util.h>
#include <rwe/atlas_util.h>
#include <rwe/collections/SimpleVectorMap.h>
#include <rwe/game/FeatureMediaInfo.h>
#include <rwe/game/GameNetworkService.h>
#include <rwe/game/MapTerrainGraphics.h>
#include <rwe/geometry/CollisionMesh.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/moveinfotdf/MovementClassTdf.h>
#include <rwe/io/moveinfotdf/io.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/tnt/TntArchive.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/sim/CampaignAi.h>
#include <rwe/sim/FeatureDefinitionId.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/util/Index.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    const Viewport MenuUiViewport(0, 0, 640, 480);

    std::seed_seq seedFromGameParameters(const GameParameters& params)
    {
        std::vector<unsigned int> initialVec;
        std::copy(params.mapName.begin(), params.mapName.end(), std::back_inserter(initialVec));

        for (const auto& e : params.players)
        {
            if (!e)
            {
                initialVec.push_back(0);
                continue;
            }

            initialVec.push_back(e->color.value);
            initialVec.push_back(e->energy.value);
            initialVec.push_back(e->metal.value);
            if (e->name)
            {
                std::copy(e->name->begin(), e->name->end(), std::back_inserter(initialVec));
            }
            else
            {
                initialVec.push_back(0);
            }
        }

        return std::seed_seq(initialVec.begin(), initialVec.end());
    }

    GameParameters::GameParameters(const std::string& mapName, unsigned int schemaIndex)
        : mapName(mapName),
          schemaIndex(schemaIndex)
    {
    }

    LoadingScene::LoadingScene(
        const SceneContext& sceneContext,
        TdfBlock* audioLookup,
        AudioService::LoopToken&& bgm,
        GameParameters gameParameters,
        std::optional<DecodedSaveGame>&& savedGame)
        : sceneContext(sceneContext),
          scaledUiRenderService(sceneContext.graphics, sceneContext.shaders, &MenuUiViewport),
          nativeUiRenderService(sceneContext.graphics, sceneContext.shaders, sceneContext.viewport),
          audioLookup(audioLookup),
          bgm(std::move(bgm)),
          gameParameters(std::move(gameParameters)),
          savedGame(std::move(savedGame)),
          uiFactory(sceneContext.textureService, sceneContext.audioService, audioLookup, sceneContext.vfs, sceneContext.pathMapping, sceneContext.viewport->width(), sceneContext.viewport->height())
    {
    }

    void LoadingScene::init()
    {
        auto backgroundSprite = sceneContext.textureService->getBitmapRegion(
            "Loadgame2bg",
            0,
            0,
            640,
            480);

        panel = std::make_unique<UiPanel>(0, 0, 640, 480, backgroundSprite);

        std::vector<std::string> categories{
            "Textures",
            "Terrain",
            "Units",
            "Animation",
            "3D Data",
            "Explosions"};

        auto font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto barSpriteSeries = sceneContext.textureService->getGuiTexture("", "LIGHTBAR");

        auto barSprite = barSpriteSeries
            ? (*barSpriteSeries)->sprites[0]
            : sceneContext.textureService->getDefaultSpriteSeries()->sprites[0];

        for (Index i = 0; i < getSize(categories); ++i)
        {
            int y = 136 + (i * 42);
            auto label = std::make_unique<UiLabel>(90, y, 100, 12, categories[i], font);
            panel->appendChild(std::move(label));

            auto bar = std::make_unique<UiLightBar>(205, y, 351, 21, barSprite);
            bar->setPercentComplete(i / 5.0f); // for demo/debugging purposes
            bars.push_back(bar.get());
            panel->appendChild(std::move(bar));
        }

        // set up network
        for (Index i = 0; i < getSize(gameParameters.players); ++i)
        {
            const auto& p = gameParameters.players[i];
            if (!p)
            {
                continue;
            }
            const auto address = std::visit(GetNetworkAddressVisitor(), p->controller);
            if (!address)
            {
                continue;
            }

            networkService.addEndpoint(i, address->first, address->second);
        }
        networkService.start(gameParameters.localNetworkPort);

        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(createGameScene(gameParameters.mapName, gameParameters.schemaIndex)));

        // wait for other players before starting
        networkService.setDoneLoading();
        networkService.waitForAllToBeReady();
    }

    void LoadingScene::render()
    {
        panel->render(scaledUiRenderService);
    }


    std::unique_ptr<GameScene> LoadingScene::createGameScene(const std::string& mapName, unsigned int schemaIndex)
    {
        auto atlasInfo = createTextureAtlases(sceneContext.vfs, sceneContext.graphics, sceneContext.palette);
        MeshService meshService(sceneContext.vfs, sceneContext.graphics, std::move(atlasInfo.textureAtlasMap), std::move(atlasInfo.teamTextureAtlasMap), std::move(atlasInfo.colorAtlasMap));

        auto otaRaw = sceneContext.vfs->readFile(std::string("maps/").append(mapName).append(".ota"));
        if (!otaRaw)
        {
            throw std::runtime_error("Failed to read OTA file");
        }
        std::string otaStr(otaRaw->begin(), otaRaw->end());
        auto ota = parseOta(parseTdfFromString(otaStr));

        // campaign missions select their schema by difficulty name;
        // skirmishes resolve the Network schema from the configured
        // player count (retail TotalA.exe 0x436860 mode 3); a saved
        // game reuses the index recorded in its metadata
        if (gameParameters.campaign)
        {
            schemaIndex = findCampaignSchemaIndex(ota, gameParameters.campaign->difficulty);
        }
        else if (!savedGame)
        {
            unsigned int playerCount = 0;
            for (const auto& p : gameParameters.players)
            {
                if (p)
                {
                    ++playerCount;
                }
            }
            schemaIndex = resolveSkirmishSchemaIndex(ota, playerCount);
        }

        // the mission's useonly whitelist, needed early so build menus
        // can be filtered before the scene takes ownership of them
        std::unordered_set<std::string> useOnlyUnits;
        if (gameParameters.campaign)
        {
            useOnlyUnits = loadUseOnlyUnits(*sceneContext.vfs, sceneContext.pathMapping->camps + "/useonly", ota.useOnlyUnits);
        }

        auto mapInfo = loadMap(mapName, ota, schemaIndex);
        std::unordered_set<std::string> requiredFeatureNames;
        for (const auto& f : mapInfo.features)
        {
            requiredFeatureNames.insert(f.second);
        }

        auto dataMaps = loadDefinitions(meshService, requiredFeatureNames);

        // campaign missions restrict build menus to the useonly whitelist
        if (gameParameters.campaign && !useOnlyUnits.empty())
        {
            for (auto& [builderName, pages] : dataMaps.builderGuisDatabase.builderGuisMap)
            {
                for (auto& page : pages)
                {
                    // index 0 is the page container, not a unit button
                    page.erase(
                        std::remove_if(
                            page.begin() + (page.empty() ? 0 : 1),
                            page.end(),
                            [&](const GuiEntry& e) {
                                auto n = toUpper(e.common.name);
                                return dataMaps.unitDefinitions.find(n) != dataMaps.unitDefinitions.end()
                                    && useOnlyUnits.find(n) == useOnlyUnits.end();
                            }),
                        page.end());
                }
            }
        }

        // flatten each builder's GUI pages into its retail build list
        // (unitdef +0x156), already mission-filtered above; the
        // campaign AI picks products from this list
        for (const auto& [builderName, pages] : dataMaps.builderGuisDatabase.builderGuisMap)
        {
            auto defIt = dataMaps.unitDefinitions.find(toUpper(builderName));
            if (defIt == dataMaps.unitDefinitions.end())
            {
                continue;
            }
            std::unordered_set<std::string> seen;
            for (const auto& page : pages)
            {
                for (std::size_t i = 1; i < page.size(); ++i)
                {
                    // only BehaviorBuildButton gadgets are products;
                    // order-panel buttons can collide with unit names
                    // (CORBUILD is the build-mode radio, not the CC
                    // hydration-plant unitdef)
                    if (!(page[i].common.attribs & GuiButtonAttrib::BehaviorBuildButton))
                    {
                        continue;
                    }
                    auto n = toUpper(page[i].common.name);
                    if (dataMaps.unitDefinitions.find(n) != dataMaps.unitDefinitions.end()
                        && seen.insert(n).second)
                    {
                        defIt->second.buildOptions.push_back(n);
                    }
                }
            }
        }

        auto movementClassCollisionService = createMovementClassCollisionService(mapInfo.terrain, dataMaps.movementClassDatabase);

        GameSimulation simulation(std::move(mapInfo.terrain), mapInfo.surfaceMetal, std::max(0, mapInfo.minWindSpeed), std::min(mapInfo.maxWindSpeed, MaxUtilizableWindSpeed), mapInfo.tidalStrength);

        // OTA fog-of-war toggles (retail globals at TotalA.exe
        // +0x39221/+0x3921d): `lineofsight` enables per-player
        // detection gating, `mapping` keeps explored terrain revealed.
        simulation.lineOfSightEnabled = ota.lineOfSight != 0;
        simulation.lineOfSightMode = ota.lineOfSight;
        simulation.mappingEnabled = ota.mapping != 0;

        // retail per-player UnitLimit: OTA `maxunits`, default 250,
        // already clamped to [20,500] at parse
        simulation.maxUnitsPerPlayer = static_cast<unsigned int>(ota.maxUnits);

        // retail water-hazard globals (missionstate +0xd48..+0xd50)
        simulation.noSeaLevelTrigger = ota.noSeaLevelTrigger;
        simulation.mapGravity = ota.gravity;
        simulation.waterDoesDamage = ota.waterDoesDamage;
        simulation.waterDamage = ota.waterDamage;

        // retail campaign difficulty economy coefficient for
        // computer-owned units (TotalA.exe ctx+0x37eee)
        if (gameParameters.campaign)
        {
            simulation.computerResourceScale = campaignDifficultyComputerResourceScale(gameParameters.campaign->difficulty);
        }
        else
        {
            // retail SKIRMISH.GUI toggles override the OTA fog/mapping
            // globals (live flags word 0x14281: bit0 mapping, bit1
            // lineofsight, bit2 lostype) plus the CommanderDeath field
            // at live +0x37ef6. All three LOS stages are fog modes;
            // the stage index is kept for the GAMEOPTIONS display.
            simulation.lineOfSightEnabled = true;
            simulation.lineOfSightMode = static_cast<int>(gameParameters.skirmish.lineOfSight);
            simulation.mappingEnabled = gameParameters.skirmish.mapping == SkirmishMapping::Mapped;
            simulation.commanderDeathContinues = gameParameters.skirmish.commanderDeath == SkirmishCommanderDeath::Continues;
            // the skirmish Difficulty stage indexes the same economy
            // coefficient table as the campaign difficulty global
            simulation.computerResourceScale = skirmishDifficultyComputerResourceScale(gameParameters.skirmish.difficulty);
        }

        simulation.unitDefinitions = std::move(dataMaps.unitDefinitions);
        simulation.weaponDefinitions = std::move(dataMaps.weaponDefinitions);
        simulation.movementClassDatabase = std::move(dataMaps.movementClassDatabase);
        simulation.movementClassCollisionService = std::move(movementClassCollisionService);
        simulation.unitModelDefinitions = dataMaps.modelDefinitions;
        simulation.unitScriptDefinitions = loadCobScripts(*sceneContext.vfs);
        simulation.featureDefinitions = std::move(dataMaps.featureDefinitions);
        simulation.featureNameIndex = std::move(dataMaps.featureNameIndex);

        for (const auto& [pos, featureName] : mapInfo.features)
        {
            // TNT/OTA can name features that have no definition;
            // retail drops them
            if (auto featureId = simulation.tryGetFeatureDefinitionId(featureName))
            {
                simulation.addFeature(*featureId, pos.x, pos.y);
            }
        }

        auto seedSeq = seedFromGameParameters(gameParameters);
        simulation.rng.seed(seedSeq);

        auto minimap = sceneContext.textureService->getMinimap(mapName);

        GameCameraState worldCameraState;

        std::optional<PlayerId> localPlayerId;

        auto playerCommandService = std::make_unique<PlayerCommandService>();

        std::array<std::optional<PlayerId>, 10> gamePlayers;
        std::vector<PlayerId> missionPlayers;
        std::vector<GameNetworkService::EndpointInfo> endpointInfos;

        if (gameParameters.campaign)
        {
            // campaign players come from the mission schema:
            // mission player 1 is human, others are computers.
            const auto& schema = ota.schemas.at(schemaIndex);
            auto campaignPlayers = buildCampaignPlayers(schema, gameParameters.campaign->campaignSide, oppositeSide(gameParameters.campaign->campaignSide));

            for (Index i = 0; i < getSize(campaignPlayers); ++i)
            {
                auto playerId = addCampaignPlayer(simulation, campaignPlayers[i]);
                missionPlayers.push_back(playerId);
                playerCommandService->registerPlayer(playerId, campaignPlayers[i].type == GamePlayerType::Human);

                if (campaignPlayers[i].type == GamePlayerType::Human)
                {
                    if (localPlayerId)
                    {
                        throw std::runtime_error("Multiple local human players found");
                    }
                    localPlayerId = playerId;
                }
            }
        }
        else
        {
            for (Index i = 0; i < getSize(gameParameters.players); ++i)
            {
                const auto& params = gameParameters.players[i];
                if (params)
                {
                    auto playerType = std::visit(IsComputerVisitor(), params->controller) ? GamePlayerType::Computer : GamePlayerType::Human;
                    GamePlayerInfo gpi{params->name, playerType, params->color, GamePlayerStatus::Alive, params->side, params->allyGroup, params->metal, params->energy, params->metal, params->energy, params->metal, params->energy};
                    auto playerId = addSkirmishPlayer(simulation, gpi);
                    gamePlayers[i] = playerId;
                    playerCommandService->registerPlayer(playerId, playerType == GamePlayerType::Human);

                    if (std::visit(IsHumanVisitor(), params->controller))
                    {
                        if (localPlayerId)
                        {
                            throw std::runtime_error("Multiple local human players found");
                        }

                        localPlayerId = gamePlayers[i];
                    }

                    if (auto networkInfo = std::get_if<PlayerControllerTypeNetwork>(&params->controller); networkInfo != nullptr)
                    {
                        endpointInfos.emplace_back(playerId, networkService.getEndpoint(i));
                    }
                }
            }
        }
        if (!localPlayerId)
        {
            throw std::runtime_error("No local player!");
        }

        // a saved game overrides the params-derived local player with
        // the id recorded in the save; player ids are table indexes so
        // the saved index is valid against the rebuilt table
        if (savedGame)
        {
            localPlayerId = savedGame->meta.localPlayerId;
        }

        auto gameNetworkService = std::make_unique<GameNetworkService>(*localPlayerId, std::stoi(gameParameters.localNetworkPort), endpointInfos, playerCommandService.get());

        auto minimapDots = sceneContext.textureService->getGafEntry("anims/FX.GAF", "radlogo");
        if (minimapDots->sprites.size() != 10)
        {
            throw std::runtime_error("Incorrect number of frames in anims/FX.GAF radlogo");
        }
        auto minimapDotHighlight = sceneContext.textureService->getGafEntry("anims/FX.GAF", "radlogohigh")->sprites.at(0);

        InGameSoundsInfo sounds;
        sounds.immediateOrders = lookUpSound("IMMEDIATEORDERS");
        sounds.specialOrders = lookUpSound("SPECIALORDERS");
        sounds.setFireOrders = lookUpSound("SETFIREORDERS");
        sounds.nextBuildMenu = lookUpSound("NEXTBUILDMENU");
        sounds.buildButton = lookUpSound("BUILDBUTTON");
        sounds.ordersButton = lookUpSound("ORDERSBUTTON");
        sounds.addBuild = lookUpSound("ADDBUILD");
        sounds.okToBuild = lookUpSound("OKTOBUILD");
        sounds.notOkToBuild = lookUpSound("NOTOKTOBUILD");
        sounds.selectMultipleUnits = lookUpSound("SelectMultipleUnits");

        auto consoleFont = sceneContext.textureService->getFont("fonts/CONSOLE.FNT");

        std::optional<std::ofstream> stateLogStream;
        if (gameParameters.stateLogFile)
        {
            stateLogStream = std::ofstream(*gameParameters.stateLogFile, std::ios::binary);
        }

        const auto& schema = ota.schemas.at(schemaIndex);

        // OTA meteor-shower hazard (retail schema read 0x4219d1 +
        // apply 0x418ed0 + init 0x418e50). Runs for every map --
        // skirmish schemas carry the fields too -- before any saved
        // game overlay restores the mutable scheduler state.
        configureMeteorShower(simulation, schema, loadMeteorShowerDefaults(*sceneContext.vfs));

        std::optional<SimVector> campaignCameraPos;

        // saved games restore their full dynamic state over the
        // freshly-built static world: mission objectives, orders and
        // script state all come from the save, so the normal
        // spawn/objective setup below is skipped
        std::optional<SimVector> savedCameraPos;
        if (savedGame)
        {
            applySavedGame(simulation, *savedGame, sceneContext.campaignProgress);
            for (const auto& [id, unit] : simulation.units)
            {
                if (unit.owner == *localPlayerId)
                {
                    savedCameraPos = unit.position;
                    break;
                }
            }
        }

        // campaign mission state is established on the simulation
        // before it is moved into the scene
        if (!savedGame && gameParameters.campaign)
        {
            const auto& campaign = *gameParameters.campaign;

            MissionInfo missionInfo;
            missionInfo.campaignName = campaign.campaignName;
            missionInfo.campaignSide = campaign.campaignSide;
            missionInfo.missionName = campaign.missionName;
            missionInfo.missionIndex = campaign.missionIndex;
            missionInfo.missionFile = campaign.missionFile;
            missionInfo.schemaType = schema.type;
            missionInfo.aiProfile = schema.aiProfile;
            missionInfo.localPlayerId = *localPlayerId;
            for (const auto& o : ota.objectives)
            {
                missionInfo.objectives.push_back(MissionObjective{o.type, o.params});
            }
            missionInfo.useOnlyUnits = useOnlyUnits;
            missionInfo.brief = ota.brief;
            missionInfo.killMul = ota.killMul;
            missionInfo.timeMul = ota.timeMul;

            simulation.missionInfo = std::move(missionInfo);
            simulation.initMissionObjectives();

            spawnMissionUnits(simulation, schema.units, missionPlayers);

            // execute retained InitialMission strings now that every
            // mission unit exists and idents can resolve
            auto bootstrapStats = installInitialMissionOrders(simulation);
            LOG_INFO << "InitialMission bootstrap: " << bootstrapStats.scriptedUnits << " scripted units, "
                     << bootstrapStats.parsedCommands << " commands parsed, "
                     << bootstrapStats.installedOrders << " orders installed, "
                     << bootstrapStats.stanceCommands << " stance commands, "
                     << bootstrapStats.deferredCommands << " deferred, "
                     << bootstrapStats.unsupportedCommands.size() << " unsupported, "
                     << bootstrapStats.errors.size() << " errors";
            for (const auto& u : bootstrapStats.unsupportedCommands)
            {
                LOG_WARN << "unsupported InitialMission command: " << u;
            }
            for (const auto& e : bootstrapStats.errors)
            {
                LOG_WARN << "InitialMission error: " << e;
            }

            // camera: prefer the mission's StartPos1 special, else the
            // first human-owned unit, else map center
            auto startPosIt = std::find_if(schema.specials.begin(), schema.specials.end(), [](const OtaSpecial& s) { return s.specialWhat == "StartPos1"; });
            if (startPosIt != schema.specials.end())
            {
                campaignCameraPos = simulation.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(startPosIt->xPos), 0_ss, SimScalar(startPosIt->zPos)));
            }
            else
            {
                for (const auto& [id, unit] : simulation.units)
                {
                    if (unit.owner == *localPlayerId)
                    {
                        campaignCameraPos = unit.position;
                        break;
                    }
                }
            }
        }

        // Retail AI bootstrap (TotalA.exe 0x4648e0): the selected
        // schema's aiprofile resolves once per game against
        // ai\<name>[.txt] with ai\default.txt fallback, and the parsed
        // profile is applied to every computer player (0x464990 slot
        // loop testing type+0x73==2). Campaign and single-player
        // skirmish share this machinery; the mode difference is only
        // which staged difficulty feeds plan gating. Human and network
        // players never receive a state. For saves that restored AI
        // state this is a no-op per player.
        auto aiDifficulty = gameParameters.campaign
            ? campaignAiDifficultyIndex(gameParameters.campaign->difficulty)
            : skirmishAiDifficultyIndex(gameParameters.skirmish.difficulty);
        if (auto profileText = loadCampaignAiProfileText(*sceneContext.vfs, sceneContext.pathMapping->ai, schema.aiProfile))
        {
            attachCampaignAiPlayers(simulation, parseCampaignAiProfile(*profileText, aiDifficulty));
        }

        // retail TotalA.exe 0x497908-0x497ad8: Fixed spawns each
        // occupied physical slot i at the special whose StartPos
        // ident equals i (StartPos{i+1}); open slots never shift
        // later assignments. Random shuffles the pool of occupied
        // slot indices and deals one to each occupied slot in slot
        // order. Drawn before the simulation moves into the scene so
        // the seeded sim RNG is the draw source.
        std::array<std::optional<unsigned int>, 10> startPosNums;
        if (!savedGame && !gameParameters.campaign)
        {
            startPosNums = assignSkirmishStartPosNums(
                gameParameters.players, gameParameters.skirmish.startLocation, simulation.rng);
        }

        auto gameScene = std::make_unique<GameScene>(
            sceneContext,
            std::move(playerCommandService),
            std::move(dataMaps.gameMediaDatabase),
            worldCameraState,
            atlasInfo.textureAtlas,
            std::move(atlasInfo.teamTextureAtlases),
            std::move(simulation),
            std::move(mapInfo.terrainGraphics),
            std::move(dataMaps.builderGuisDatabase),
            std::move(gameNetworkService),
            minimap,
            minimapDots,
            minimapDotHighlight,
            std::move(sounds),
            consoleFont,
            *localPlayerId,
            audioLookup,
            gameParameters,
            std::move(stateLogStream));

        if (savedGame)
        {
            auto cameraPos = savedCameraPos.value_or(SimVector(0_ss, 0_ss, 0_ss));
            gameScene->setCameraPosition(Vector3f(simScalarToFloat(cameraPos.x), 0.0f, simScalarToFloat(cameraPos.z)));

            return gameScene;
        }

        if (gameParameters.campaign)
        {
            auto cameraPos = campaignCameraPos.value_or(SimVector(0_ss, 0_ss, 0_ss));
            gameScene->setCameraPosition(Vector3f(simScalarToFloat(cameraPos.x), 0.0f, simScalarToFloat(cameraPos.z)));

            return gameScene;
        }

        std::optional<SimVector> humanStartPos;

        for (Index i = 0; i < getSize(gameParameters.players); ++i)
        {
            const auto& player = gameParameters.players[i];
            if (!player || !startPosNums[i])
            {
                continue;
            }

            // retail 0x437320: the slot's assigned number resolves to
            // the StartPos special with that ident. A missing ident
            // is retail's "Could not find start position number N on
            // the map!" path -- the player stays in the game with no
            // commander rather than shifting onto another slot's spot
            const OtaSpecial* startPos = findStartPosSpecial(schema, *startPosNums[i]);
            if (!startPos)
            {
                continue;
            }

            auto worldStartPos = gameScene->getTerrain().topLeftCoordinateToWorld(SimVector(SimScalar(startPos->xPos), 0_ss, SimScalar(startPos->zPos)));
            worldStartPos.y = gameScene->getTerrain().getHeightAt(worldStartPos.x, worldStartPos.z);

            if (*gamePlayers[i] == *localPlayerId)
            {
                humanStartPos = worldStartPos;
            }

            const auto& sideData = getSideData(player->side);
            // retail places skirmish start units unconditionally --
            // authored start positions may overlap decorative blocking
            // TNT features and are still occupied (same allowOverlap
            // path campaign uses for authored mission units)
            gameScene->spawnCompletedUnit(sideData.commander, *gamePlayers[i], worldStartPos, true);
        }

        if (!humanStartPos)
        {
            throw std::runtime_error("No human player!");
        }

        gameScene->setCameraPosition(Vector3f(simScalarToFloat(humanStartPos->x), 0.0f, simScalarToFloat(humanStartPos->z)));

        return gameScene;
    }


    LoadingScene::LoadMapResult LoadingScene::loadMap(const std::string& mapName, const OtaRecord& ota, unsigned int schemaIndex)
    {
        auto tntBytes = sceneContext.vfs->readFile("maps/" + mapName + ".tnt");
        if (!tntBytes)
        {
            throw std::runtime_error("Failed to load map bytes");
        }

        rwe::SpanStream tntStream(tntBytes->data(), tntBytes->size());
        TntArchive tnt(&tntStream);

        auto tileTextures = getTileTextures(tnt);

        auto dataGrid = getMapData(tnt);

        Grid<TntTileAttributes> mapAttributes(tnt.getHeader().width, tnt.getHeader().height);
        tnt.readMapAttributes(mapAttributes.getData());

        auto heightGrid = getHeightGrid(mapAttributes);

        MapTerrain terrain(
            std::move(heightGrid),
            SimScalar(tnt.getHeader().seaLevel));

        MapTerrainGraphics terrainGraphics(
            std::move(tileTextures),
            std::move(dataGrid));

        const auto& schema = ota.schemas.at(schemaIndex);

        auto featureNames = getFeatureNames(tnt);
        std::vector<std::pair<Point, std::string>> features;

        mapAttributes.forEachIndexed([&](auto c, const auto& e) {
            switch (e.feature)
            {
                case TntTileAttributes::FeatureNone:
                case TntTileAttributes::FeatureUnknown:
                case TntTileAttributes::FeatureVoid:
                    break;
                default:
                    features.emplace_back(Point(c.x, c.y), featureNames.at(e.feature));
            }
        });

        // add features from the OTA schema
        for (const auto& f : schema.features)
        {
            features.emplace_back(Point(f.xPos, f.zPos), f.featureName);
        }

        return LoadMapResult{std::move(terrain), static_cast<unsigned char>(schema.surfaceMetal), ota.minWindSpeed, ota.maxWindSpeed, ota.tidalStrength, std::move(features), std::move(terrainGraphics)};
    }

    std::vector<TextureArrayRegion> LoadingScene::getTileTextures(TntArchive& tnt)
    {
        static const unsigned int tileWidth = 32;
        static const unsigned int tileHeight = 32;
        static const unsigned int mipMapLevels = 5;
        static const auto tilesPerTextureArray = 256;

        std::vector<TextureArrayRegion> tileTextures;

        std::vector<Color> textureArrayBuffer;
        textureArrayBuffer.reserve(tileWidth * tileHeight * tilesPerTextureArray);

        std::vector<SharedTextureArrayHandle> textureArrayHandles;

        // read the tile graphics into textures
        {
            tnt.readTiles([&](const char* tile) {
                if (textureArrayBuffer.size() == tileWidth * tileHeight * tilesPerTextureArray)
                {
                    SharedTextureArrayHandle handle(sceneContext.graphics->createTextureArray(tileWidth, tileHeight, mipMapLevels, textureArrayBuffer));
                    textureArrayHandles.push_back(std::move(handle));
                    textureArrayBuffer.clear();
                }

                for (unsigned int i = 0; i < (tileWidth * tileHeight); ++i)
                {
                    auto index = static_cast<unsigned char>(tile[i]);
                    textureArrayBuffer.push_back((*sceneContext.palette)[index]);
                }
            });
        }
        textureArrayHandles.emplace_back(sceneContext.graphics->createTextureArray(tileWidth, tileHeight, mipMapLevels, textureArrayBuffer));

        // populate the list of texture regions referencing the textures
        for (unsigned int i = 0; i < tnt.getHeader().numberOfTiles; ++i)
        {
            assert(textureArrayHandles.size() > i / tilesPerTextureArray);
            auto textureIndex = i / tilesPerTextureArray;
            auto tileIndex = i % tilesPerTextureArray;
            tileTextures.emplace_back(textureArrayHandles[textureIndex], tileIndex);
        }

        return tileTextures;
    }


    const SideData& LoadingScene::getSideData(const std::string& side) const
    {
        auto it = sceneContext.sideData->find(side);
        if (it == sceneContext.sideData->end())
        {
            throw std::runtime_error("Missing side data for " + side);
        }

        return it->second;
    }

    void LoadingScene::loadFeatureMedia(MeshService& meshService, std::unordered_map<std::string, UnitModelDefinition>& modelDefinitions, GameMediaDatabase& gameMediaDatabase, const FeatureTdf& tdf)
    {
        FeatureMediaInfo f;

        if (tdf.flamable)
        {
            if (auto sound = lookUpSound("treeburn")) { gameMediaDatabase.addSound("treeburn", *sound); }
        }
        f.world = tdf.world;
        f.description = tdf.description;
        f.category = tdf.category;

        if (!tdf.object.empty())
        {
            auto normalizedObjectName = toUpper(tdf.object);

            if (modelDefinitions.find(normalizedObjectName) == modelDefinitions.end())
            {
                auto meshInfo = meshService.loadProjectileMesh(normalizedObjectName);
                modelDefinitions.insert({normalizedObjectName, std::move(meshInfo.modelDefinition)});
                for (const auto& m : meshInfo.pieceMeshes)
                {
                    gameMediaDatabase.addUnitPieceMesh(normalizedObjectName, m.first, m.second);
                }
            }
            f.renderInfo = FeatureObjectInfo{normalizedObjectName};
        }
        else
        {
            FeatureSpriteInfo spriteInfo;
            spriteInfo.transparentAnimation = tdf.animTrans;
            spriteInfo.transparentShadow = tdf.shadTrans;
            if (!tdf.fileName.empty() && !tdf.seqName.empty())
            {
                spriteInfo.animation = sceneContext.textureService->getGafEntry("anims/" + tdf.fileName + ".GAF", tdf.seqName);
            }
            if (!spriteInfo.animation)
            {
                spriteInfo.animation = sceneContext.textureService->getDefaultSpriteSeries();
            }

            if (!tdf.fileName.empty() && !tdf.seqNameShad.empty())
            {
                // Some third-party features have broken shadow anim names (e.g. "empty"),
                // ignore them if they don't exist.
                spriteInfo.shadowAnimation = sceneContext.textureService->tryGetGafEntry("anims/" + tdf.fileName + ".GAF", tdf.seqNameShad);
            }
            const std::array<std::pair<std::string, std::string>, 3> sequences{{
                {tdf.seqNameDie, tdf.seqNameDieShad}, {tdf.seqNameReclamate, tdf.seqNameReclamateShad},
                {tdf.seqNameBurn, tdf.seqNameBurnShad}}};
            for (unsigned int i = 0; i < sequences.size(); ++i)
            {
                auto load = [&](const std::string& seq) -> std::shared_ptr<SpriteSeries> {
                    if (tdf.fileName.empty() || seq.empty()) { return nullptr; }
                    return sceneContext.textureService->tryGetGafEntry("anims/" + tdf.fileName + ".GAF", seq).value_or(nullptr);
                };
                spriteInfo.events[i] = load(sequences[i].first);
                spriteInfo.eventShadows[i] = load(sequences[i].second);
                for (const auto& series : {spriteInfo.events[i], spriteInfo.eventShadows[i]})
                {
                    if (!series) { continue; }
                    for (const auto& sprite : series->sprites)
                    {
                        const auto& b = sprite->bounds;
                        auto x = std::max(std::abs(b.left()), std::abs(b.right()));
                        auto y = std::max(std::abs(b.top()), std::abs(b.bottom())) * (tdf.height >= 10 ? 2.0f : 1.0f);
                        spriteInfo.eventCullRadii[i] = std::max(spriteInfo.eventCullRadii[i], std::sqrt(x * x + y * y) + 2.0f);
                    }
                }
            }
            f.renderInfo = std::move(spriteInfo);
        }

        f.seqNameReclamate = tdf.seqNameReclamate;

        f.seqNameBurn = tdf.seqNameBurn;
        f.seqNameBurnShad = tdf.seqNameBurnShad;

        f.seqNameDie = tdf.seqNameDie;

        gameMediaDatabase.addFeature(std::move(f));
    }

    void LoadingScene::loadFeature(MeshService& meshService, GameMediaDatabase& gameMediaDatabase, const std::unordered_map<std::string, FeatureTdf>& tdfs, DataMaps& dataMaps, const std::string& initialFeatureName)
    {
        // pure-data registration is shared with the save-restore
        // definition path; media loading stays here
        auto loadedTdfs = loadFeatureDefinitions(tdfs, dataMaps.featureDefinitions, dataMaps.featureNameIndex, initialFeatureName);
        for (const auto* tdf : loadedTdfs)
        {
            loadFeatureMedia(meshService, dataMaps.modelDefinitions, gameMediaDatabase, *tdf);
        }
    }

    LoadingScene::DataMaps LoadingScene::loadDefinitions(MeshService& meshService, const std::unordered_set<std::string>& requiredFeatures)
    {
        DataMaps dataMaps;

        // read sound categories
        {
            auto path = sceneContext.pathMapping->gamedata + "/SOUND.TDF";
            auto bytes = sceneContext.vfs->readFile(path);
            if (!bytes)
            {
                throw std::runtime_error("Failed to read " + path);
            }

            std::string soundString(bytes->data(), bytes->size());
            auto sounds = parseSoundTdf(parseTdfFromString(soundString));
            for (auto& s : sounds)
            {
                const auto& c = s.second;
                preloadSound(dataMaps.gameMediaDatabase, c.select1);
                preloadSound(dataMaps.gameMediaDatabase, c.unitComplete);
                preloadSound(dataMaps.gameMediaDatabase, c.activate);
                preloadSound(dataMaps.gameMediaDatabase, c.deactivate);
                preloadSound(dataMaps.gameMediaDatabase, c.ok1);
                preloadSound(dataMaps.gameMediaDatabase, c.arrived1);
                preloadSound(dataMaps.gameMediaDatabase, c.cant1);
                preloadSound(dataMaps.gameMediaDatabase, c.underAttack);
                preloadSound(dataMaps.gameMediaDatabase, c.build);
                preloadSound(dataMaps.gameMediaDatabase, c.repair);
                preloadSound(dataMaps.gameMediaDatabase, c.working);
                preloadSound(dataMaps.gameMediaDatabase, c.cloak);
                preloadSound(dataMaps.gameMediaDatabase, c.uncloak);
                preloadSound(dataMaps.gameMediaDatabase, c.capture);
                preloadSound(dataMaps.gameMediaDatabase, c.count5);
                preloadSound(dataMaps.gameMediaDatabase, c.count4);
                preloadSound(dataMaps.gameMediaDatabase, c.count3);
                preloadSound(dataMaps.gameMediaDatabase, c.count2);
                preloadSound(dataMaps.gameMediaDatabase, c.count1);
                preloadSound(dataMaps.gameMediaDatabase, c.count0);
                preloadSound(dataMaps.gameMediaDatabase, c.cancelDestruct);
                dataMaps.gameMediaDatabase.addSoundClass(s.first, std::move(s.second));
            }
        }

        // read movement classes
        {
            auto path = sceneContext.pathMapping->gamedata + "/MOVEINFO.TDF";
            auto bytes = sceneContext.vfs->readFile(path);
            if (!bytes)
            {
                throw std::runtime_error("Failed to read " + path);
            }

            std::string movementString(bytes->data(), bytes->size());
            auto classes = parseMoveInfoTdf(parseTdfFromString(movementString));
            for (auto& c : classes)
            {
                auto movementClassDefinition = parseMovementClassDefinition(c.second);
                dataMaps.movementClassDatabase.registerMovementClass(movementClassDefinition);
            }
        }

        // read weapons
        {
            auto weaponFiles = sceneContext.vfs->getFileNames(sceneContext.pathMapping->weapons, ".tdf");

            for (const auto& fileName : weaponFiles)
            {
                auto bytes = sceneContext.vfs->readFile(sceneContext.pathMapping->weapons + "/" + fileName);
                if (!bytes)
                {
                    throw std::runtime_error("File in listing could not be read: " + fileName);
                }

                std::string tdfString(bytes->data(), bytes->size());
                auto entries = parseWeaponTdf(parseTdfFromString(tdfString));

                for (auto& pair : entries)
                {
                    auto weaponDefinition = parseWeaponDefinition(pair.second);
                    auto weaponMediaInfo = parseWeaponMediaInfo(*sceneContext.palette, *sceneContext.guiPalette, pair.second);

                    preloadSound(dataMaps.gameMediaDatabase, weaponMediaInfo.soundStart);
                    preloadSound(dataMaps.gameMediaDatabase, weaponMediaInfo.soundHit);
                    preloadSound(dataMaps.gameMediaDatabase, weaponMediaInfo.soundWater);

                    if (auto modelRenderType = std::get_if<ProjectileRenderTypeModel>(&weaponMediaInfo.renderType); modelRenderType != nullptr)
                    {
                        auto meshInfo = meshService.loadProjectileMesh(modelRenderType->objectName);
                        dataMaps.modelDefinitions.insert({modelRenderType->objectName, std::move(meshInfo.modelDefinition)});
                        for (const auto& m : meshInfo.pieceMeshes)
                        {
                            dataMaps.gameMediaDatabase.addUnitPieceMesh(modelRenderType->objectName, m.first, m.second);
                        }
                    }

                    if (weaponMediaInfo.explosionAnim)
                    {
                        auto anim = sceneContext.textureService->getGafEntry("anims/" + weaponMediaInfo.explosionAnim->gafName + ".gaf", weaponMediaInfo.explosionAnim->animName);
                        dataMaps.gameMediaDatabase.addSpriteSeries(weaponMediaInfo.explosionAnim->gafName, weaponMediaInfo.explosionAnim->animName, anim);
                    }
                    if (weaponMediaInfo.waterExplosionAnim)
                    {
                        auto anim = sceneContext.textureService->getGafEntry("anims/" + weaponMediaInfo.waterExplosionAnim->gafName + ".gaf", weaponMediaInfo.waterExplosionAnim->animName);
                        dataMaps.gameMediaDatabase.addSpriteSeries(weaponMediaInfo.waterExplosionAnim->gafName, weaponMediaInfo.waterExplosionAnim->animName, anim);
                    }

                    dataMaps.gameMediaDatabase.addWeapon(toUpper(pair.first), std::move(weaponMediaInfo));

                    dataMaps.weaponDefinitions.insert({toUpper(pair.first), std::move(weaponDefinition)});
                }
            }
        }

        std::unordered_set<std::string> requiredFeaturesSet;
        for (const auto& f : requiredFeatures)
        {
            requiredFeaturesSet.insert(toUpper(f));
        }

        // read unit FBIs
        {
            auto fbis = sceneContext.vfs->getFileNames(sceneContext.pathMapping->units, ".fbi");

            for (const auto& fbiName : fbis)
            {
                auto bytes = sceneContext.vfs->readFile(sceneContext.pathMapping->units + "/" + fbiName);
                if (!bytes)
                {
                    throw std::runtime_error("File in listing could not be read: " + fbiName);
                }

                std::string fbiString(bytes->data(), bytes->size());
                auto fbi = parseUnitFbi(parseTdfFromString(fbiString));

                auto unitDefinition = parseUnitDefinition(fbi, dataMaps.movementClassDatabase);
                dataMaps.unitDefinitions.insert({toUpper(fbi.unitName), std::move(unitDefinition)});

                // also attempt to read the unit's gui pages: builder
                // panels hold the unit product grid, and stockpile
                // units (ARMSILO/CORSILO/ARMAMD/CORFMD/ARMEMP/CORTRON)
                // carry a GUI-authored weapon build button
                // (commonattribs=8 -- e.g. ARMMAKENUKE/ARMMAKEANTI) that
                // is the only player-facing stockpile control
                auto guiPages = loadBuilderGui(fbi.unitName);
                if (guiPages)
                {
                    dataMaps.builderGuisDatabase.addBuilderGui(fbi.unitName, std::move(*guiPages));
                }

                // TODO: if no gui defined, attempt to build it dynamically?
                // Need a database of download.tdf mappings first...

                auto meshInfo = meshService.loadUnitMesh(fbi.objectName);
                dataMaps.modelDefinitions.insert({toUpper(fbi.objectName), std::move(meshInfo.modelDefinition)});
                for (const auto& m : meshInfo.pieceMeshes)
                {
                    dataMaps.gameMediaDatabase.addUnitPieceMesh(fbi.objectName, m.first, m.second);
                }

                dataMaps.gameMediaDatabase.addSelectionCollisionMesh(fbi.objectName, std::make_shared<CollisionMesh>(std::move(meshInfo.selectionMesh.collisionMesh)));
                dataMaps.gameMediaDatabase.addSelectionMesh(fbi.objectName, std::make_shared<GlMesh>(std::move(meshInfo.selectionMesh.visualMesh)));

                if (!fbi.corpse.empty())
                {
                    requiredFeaturesSet.insert(toUpper(fbi.corpse));
                }
            }
        }

        // TA-Transplant balance: patch the interactive transport
        // capacities on the loaded definitions (owner-approved
        // divergence from retail 3.1 -- see DECISIONS.md). Applied
        // here so every launch mode inherits it identically.
        applyTransplantBalanceOverrides(dataMaps.unitDefinitions);

        // read feature TDFs
        {
            auto files = sceneContext.vfs->getFileNamesRecursive("features", ".tdf");

            std::unordered_map<std::string, FeatureTdf> featureTdfs;

            for (const auto& name : files)
            {
                auto bytes = sceneContext.vfs->readFile("features/" + name);
                if (!bytes)
                {
                    throw std::runtime_error("Failed to read feature " + name);
                }

                std::string tdfString(bytes->data(), bytes->size());

                auto tdfRoot = parseTdfFromString(tdfString);
                for (const auto& e : tdfRoot.blocks)
                {
                    auto featureTdf = parseFeatureTdf(*e.second);
                    featureTdfs.insert({toUpper(e.first), featureTdf});
                }
            }

            loadFeatureAnimations(*sceneContext.vfs, featureTdfs);

            // actually parse and load assets for features that we require;
            // stock maps reference feature names with no TDF anywhere
            // (e.g. EXP1AC04 LushFoliage2-*), which retail drops
            for (const auto& featureName : requiredFeaturesSet)
            {
                if (featureTdfs.find(featureName) != featureTdfs.end())
                {
                    loadFeature(meshService, dataMaps.gameMediaDatabase, featureTdfs, dataMaps, featureName);
                }
            }
        }

        // preload smoke
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "smoke 1");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "smoke 1", anim);
            auto anim2 = sceneContext.textureService->getGafEntry("anims/FX.GAF", "smoke 2");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "smoke 2", anim2);
        }

        // preload weapon sprites
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "cannonshell");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "cannonshell", anim);
        }
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "plasmasm");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "plasmasm", anim);
        }
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "plasmamd");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "plasmamd", anim);
        }
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "ultrashell");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "ultrashell", anim);
        }
        {
            auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "flamestream");
            dataMaps.gameMediaDatabase.addSpriteSeries("FX", "flamestream", anim);
        }

        return dataMaps;
    }

    void LoadingScene::preloadSound(GameMediaDatabase& meshDb, const std::optional<std::string>& soundName)
    {
        if (!soundName)
        {
            return;
        }

        preloadSound(meshDb, *soundName);
    }

    void LoadingScene::preloadSound(GameMediaDatabase& meshDb, const std::string& soundName)
    {
        auto sound = sceneContext.audioService->loadSound(soundName);
        if (!sound)
        {
            return; // sometimes sound categories name invalid sounds
        }

        meshDb.addSound(soundName, *sound);
    }

    std::optional<AudioService::SoundHandle> LoadingScene::lookUpSound(const std::string& key)
    {
        auto soundBlock = audioLookup->findBlock(key);
        if (!soundBlock)
        {
            return std::nullopt;
        }

        auto soundName = soundBlock->get().findValue("sound");
        if (!soundName)
        {
            return std::nullopt;
        }

        return sceneContext.audioService->loadSound(*soundName);
    }

    std::optional<std::vector<std::vector<GuiEntry>>> LoadingScene::loadBuilderGui(const std::string& unitName)
    {
        std::vector<std::vector<GuiEntry>> entries;
        for (int i = 1; auto rawGui = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/" + unitName + std::to_string(i) + ".GUI"); ++i)
        {
            auto parsedGui = parseGuiFromBytes(*rawGui);
            if (!parsedGui)
            {
                throw std::runtime_error("Failed to parse unit builder GUI: " + unitName + std::to_string(i));
            }
            entries.push_back(std::move(*parsedGui));
        }

        if (entries.empty())
        {
            return std::nullopt;
        }

        return entries;
    }
}
