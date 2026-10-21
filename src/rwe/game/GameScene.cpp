#include "GameScene.h"
#include <rwe/sim/Veterancy.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <rwe/CroppedViewport.h>
#include <rwe/EndMissionScene.h>
#include <rwe/LoadingScene.h>
#include <rwe/MainMenuScene.h>
#include <rwe/util/ConfigFile.h>
#include <rwe/util/rwe_string.h>
#include <rwe/Mesh.h>
#include <rwe/camera_util.h>
#include <rwe/game/GameScene_util.h>
#include <rwe/game/dump_util.h>
#include <rwe/game/matrix_util.h>
#include <rwe/game/selection_util.h>
#include <rwe/resource_io.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitState_util.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/save/SaveGameService.h>
#include <rwe/save/SaveListDialog.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiTextBox.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/UiSurface.h>
#include <rwe/util/Index.h>
#include <rwe/util/match.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    namespace
    {
        /**
         * Wall-clock scope timer feeding a nanosecond accumulator.
         * Null destination = profiling disabled (branch only).
         */
        struct ScopedFrameTimer
        {
            std::chrono::steady_clock::time_point start;
            std::uint64_t* out;

            explicit ScopedFrameTimer(std::uint64_t* out)
                : start(out != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}), out(out)
            {
            }

            ~ScopedFrameTimer()
            {
                if (out != nullptr)
                {
                    *out += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
                }
            }
        };

        /**
         * Conservative render-only cull bound for a unit centred on its
         * origin. Covers piece extents, hierarchical animation offsets,
         * tall structures and partially constructed frames. Generous on
         * purpose: the cost of a false positive is one emitted unit.
         */
        constexpr float unitRenderCullRadius = 320.0f;
        constexpr float featureRenderCullRadius = 320.0f;

        /**
         * Shadow-caster visibility test. The shadow shader projects every
         * vertex onto the terrain at groundHeight, displaced diagonally by
         * 0.25 * (vertexY - groundHeight) in +x/-z, so the visible shadow
         * sits near the landing point even when the caster is high above.
         */
        bool unitOrShadowInFrustum(const ViewFrustum& frustum, const SimVector& position, float groundHeight, float radius)
        {
            const auto p = simVectorToFloat(position);
            const float heightAbove = p.y - groundHeight;
            if (heightAbove <= 0.0f)
            {
                return sphereInFrustum(frustum, p, radius);
            }
            const Vector3f shadowCenter(p.x + 0.25f * heightAbove, groundHeight, p.z - 0.25f * heightAbove);
            return sphereInFrustum(frustum, shadowCenter, radius + 0.25f * (heightAbove + radius));
        }
    }

    bool isValidUnitType(const GameSimulation& simulation, const std::string& unitType)
    {
        return simulation.unitDefinitions.find(unitType) != simulation.unitDefinitions.end();
    }

    std::optional<std::reference_wrapper<const std::vector<GuiEntry>>> getBuilderGui(const BuilderGuisDatabase& db, const std::string& unitType, unsigned int page)
    {
        const auto& pages = db.tryGetBuilderGui(unitType);
        if (!pages)
        {
            return std::nullopt;
        }

        const auto& unwrappedPages = pages->get();

        if (page >= unwrappedPages.size())
        {
            return std::nullopt;
        }

        return unwrappedPages[page];
    }

    /** If the unit has no build gui, this will be zero. */
    unsigned int getBuildPageCount(const BuilderGuisDatabase& db, const std::string& unitType)
    {
        const auto& pages = db.tryGetBuilderGui(unitType);
        if (!pages)
        {
            return 0;
        }

        return pages->get().size();
    }

    bool unitCanAttack(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.canAttack;
    }

    /**
     * Whether the unit can receive the command-fire (BLAST) order.
     * Retail admits FBI `candgun` units and units carrying a
     * `commandfire=1` weapon (stockpile silos); the per-shot resource
     * cost is checked separately for the cursor.
     */
    bool unitCanCommandFire(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.canDGun || unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, unitDefinition);
    }

    /**
     * Retail BLAST cursor eligibility result: the command-fire weapon's
     * per-shot cost is compared against the owner's stocks when the
     * unit is `candgun`. The selection-wide cursor takes the minimum
     * result across selected units.
     */
    enum class CommandFireEligibility
    {
        Ready,
        InsufficientResources,
        Ineligible
    };

    CommandFireEligibility unitCommandFireEligibility(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        if (!unitDefinition.canDGun && !unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, unitDefinition))
        {
            return CommandFireEligibility::Ineligible;
        }

        for (const auto& weapon : unit.weapons)
        {
            if (!weapon)
            {
                continue;
            }

            const auto& weaponDefinition = sim.weaponDefinitions.at(weapon->weaponType);
            if (!weaponDefinition.commandFire)
            {
                continue;
            }

            // a stockpile weapon's cost was paid at build time; firing
            // only requires a built round
            if (weaponDefinition.stockpile)
            {
                return weapon->stockpileCount > 0
                    ? CommandFireEligibility::Ready
                    : CommandFireEligibility::InsufficientResources;
            }

            const auto& player = sim.getPlayer(unit.owner);
            if (player.energy >= weaponDefinition.energyPerShot && player.metal >= weaponDefinition.metalPerShot)
            {
                return CommandFireEligibility::Ready;
            }
            return CommandFireEligibility::InsufficientResources;
        }

        return CommandFireEligibility::Ineligible;
    }

    bool unitCanMove(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.canMove;
    }

    bool unitCanGuard(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.canGuard;
    }

    bool unitIsBuilder(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.builder;
    }

    bool unitIsBuilder(const GameSimulation& sim, std::optional<UnitId> singleSelectedUnit)
    {
        if (!singleSelectedUnit)
        {
            return false;
        }
        const auto& unit = sim.getUnitState(*singleSelectedUnit);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.builder;
    }

    bool unitIsBeingBuilt(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unit.isBeingBuilt(unitDefinition);
    }

    bool unitNeedsRepair(const GameSimulation& sim, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return !unit.isBeingBuilt(unitDefinition) && unit.hitPoints < unitDefinition.maxHitPoints;
    }

    bool unitIsSelectableBy(const GameSimulation& sim, UnitId unitId, PlayerId playerId)
    {
        const auto& unit = sim.getUnitState(unitId);
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unit.isSelectableBy(unitDefinition, playerId);
    }

    /**
     * True when the feature exists, its definition allows reclaiming, and
     * some of its resource pool remains to be taken.
     */
    bool featureIsReclaimableTarget(const GameSimulation& sim, FeatureId featureId)
    {
        auto feature = sim.tryGetFeature(featureId);
        if (!feature)
        {
            return false;
        }
        const auto& featureDefinition = sim.getFeatureDefinition(feature->get().featureName);
        return featureDefinition.reclaimable && !sim.reclaimComplete(feature->get());
    }

    /**
     * True when the unit exists, is alive, and is not a commander.
     * Original TA rejects commander targets regardless of ownership
     * (reclaim targeting is how players identify enemy commanders).
     * Units still under construction are valid targets but yield
     * no resources.
     */
    bool unitIsReclaimableTarget(const GameSimulation& sim, UnitId unitId)
    {
        auto unit = sim.tryGetUnitState(unitId);
        if (!unit || unit->get().isDead())
        {
            return false;
        }
        const auto& unitDefinition = sim.unitDefinitions.at(unit->get().unitType);
        return !unitDefinition.commander;
    }

    /**
     * True when the unit may be issued Capture orders — retail gates
     * the order on the FBI `CanCapture` flag (unitdef bit 0x1000,
     * TotalA.exe 0x4042cf); in stock data only commanders set it.
     */
    bool unitCanCapture(const GameSimulation& sim, UnitId unitId)
    {
        auto unit = sim.tryGetUnitState(unitId);
        return unit && !unit->get().isDead() && sim.unitDefinitions.at(unit->get().unitType).canCapture;
    }

    /**
     * True when `targetId` is a valid capture target for `player`'s
     * unit: alive, construction-complete, not carried, not owned or
     * allied, and not capture-immune. Retail rejects capture-capable
     * targets (commanders), unfinished nanoframes and carried units
     * (TotalA.exe 0x4042e3-0x404342); mission `Immunity=1` units are
     * untargetable by the same convention weapons use.
     */
    bool unitIsCapturableTarget(const GameSimulation& sim, UnitId targetId, PlayerId capturingPlayer)
    {
        auto unit = sim.tryGetUnitState(targetId);
        if (!unit || unit->get().isDead() || unit->get().carriedBy || sim.areAllied(capturingPlayer, unit->get().owner))
        {
            return false;
        }
        const auto& u = unit->get();
        const auto& def = sim.unitDefinitions.at(u.unitType);
        return !def.canCapture && !u.isBeingBuilt(def) && !(u.missionState && u.missionState->immunity);
    }

    /**
     * True when the unit may be issued interactive Load orders —
     * retail gates the order on the FBI `CanLoad` flag (unitdef bit
     * 0x100, TotalA.exe 0x40679f) and rejects a carrier that is
     * itself carried (0x411226).
     */
    bool unitCanLoad(const GameSimulation& sim, UnitId unitId)
    {
        auto unit = sim.tryGetUnitState(unitId);
        return unit
            && !unit->get().isDead()
            && !unit->get().carriedBy
            && sim.unitDefinitions.at(unit->get().unitType).canLoad;
    }

    /**
     * True when `targetId` is a valid interactive Load passenger for
     * the transport `transportId`: alive, same side, mobile, finished,
     * transportable (FBI `CantBeTransported` clear), not carried,
     * not carrying units itself, and within the transport's
     * `TransportSize` footprint cap and `TransportCapacity` count cap
     * (TotalA.exe 0x4067cf-0x40681f, 0x4111f0-0x4112a0).
     */
    bool unitIsLoadableTarget(const GameSimulation& sim, UnitId targetId, UnitId transportId)
    {
        auto unit = sim.tryGetUnitState(targetId);
        auto transport = sim.tryGetUnitState(transportId);
        if (!unit || !transport || targetId == transportId)
        {
            return false;
        }
        const auto& u = unit->get();
        const auto& t = transport->get();
        if (u.isDead() || !u.isOwnedBy(t.owner))
        {
            return false;
        }
        const auto& unitDef = sim.unitDefinitions.at(u.unitType);
        const auto& transportDef = sim.unitDefinitions.at(t.unitType);
        if (!unitDef.isMobile
            || unitDef.cantBeTransported
            || u.carriedBy
            || !u.passengers.empty()
            || u.isBeingBuilt(unitDef))
        {
            return false;
        }
        if (t.passengers.size() >= transportDef.transportCapacity)
        {
            return false;
        }
        return sim.getFootprintXZ(unitDef.movementCollisionInfo).first <= transportDef.transportSize;
    }

    bool unitIsOwnedByPlayerAndIsBuilder(const GameSimulation& sim, PlayerId playerId, std::optional<UnitId> singleSelectedUnit)
    {
        if (!singleSelectedUnit)
        {
            return false;
        }
        const auto& unit = sim.getUnitState(*singleSelectedUnit);
        if (!unit.isOwnedBy(playerId))
        {
            return false;
        }
        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
        return unitDefinition.builder;
    }

    bool shouldShowAllBuildBoxes(const GameSimulation& sim, PlayerId localPlayerId, std::optional<UnitId> singleSelectedUnit, std::optional<UnitId> hoveredUnit)
    {
        return unitIsBuilder(sim, singleSelectedUnit) || unitIsOwnedByPlayerAndIsBuilder(sim, localPlayerId, hoveredUnit);
    }

    Line3x<SimScalar> floatToSimLine(const Line3f& line)
    {
        return Line3x<SimScalar>(floatToSimVector(line.start), floatToSimVector(line.end));
    }

    Matrix4f computeView(const Vector3f& cameraPosition)
    {
        auto translation = Matrix4f::translation(-cameraPosition);
        auto rotation = Matrix4f::rotationToAxes(Vector3f(1.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, -1.0f), Vector3f(0.0f, 1.0f, 0.0f));
        return rotation * translation;
    }

    Matrix4f computeInverseView(const Vector3f& cameraPosition)
    {
        auto translation = Matrix4f::translation(cameraPosition);
        auto rotation = Matrix4f::rotationToAxes(Vector3f(1.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, -1.0f), Vector3f(0.0f, 1.0f, 0.0f)).transposed();
        return translation * rotation;
    }

    Matrix4f computeProjection(float width, float height)
    {
        float halfWidth = width / 2.0f;
        float halfHeight = height / 2.0f;

        auto cabinet = Matrix4f::cabinetProjection(0.0f, 0.5f);

        auto ortho = Matrix4f::orthographicProjection(
            -halfWidth,
            halfWidth,
            -halfHeight,
            halfHeight,
            -1000.0f,
            1000.0f);

        return ortho * cabinet;
    }

    Matrix4f computeInverseProjection(float width, float height)
    {
        float halfWidth = width / 2.0f;
        float halfHeight = height / 2.0f;

        auto inverseCabinet = Matrix4f::cabinetProjection(0.0f, -0.5f);

        auto inverseOrtho = Matrix4f::inverseOrthographicProjection(
            -halfWidth,
            halfWidth,
            -halfHeight,
            halfHeight,
            -1000.0f,
            1000.0f);

        return inverseCabinet * inverseOrtho;
    }

    Matrix4f computeViewProjectionMatrix(const GameCameraState& cameraState, int screenWidth, int screenHeight)
    {
        auto view = computeView(cameraState.getRoundedPosition());
        auto projection = computeProjection(cameraState.scaleDimension(screenWidth), cameraState.scaleDimension(screenHeight));
        return projection * view;
    }

    Matrix4f computeInverseViewProjectionMatrix(const GameCameraState& cameraState, int screenWidth, int screenHeight)
    {
        auto inverseView = computeInverseView(cameraState.getRoundedPosition());
        auto inverseProjection = computeInverseProjection(cameraState.scaleDimension(screenWidth), cameraState.scaleDimension(screenHeight));
        return inverseView * inverseProjection;
    }

    const Rectangle2f GameScene::minimapViewport = Rectangle2f::fromTopLeft(0.0f, 0.0f, GuiSizeLeft, GuiSizeLeft);

    /**
     * The in-game menu dialogs are authored in the stock 640x480 GUI
     * coordinate space; they render through this viewport scaled to the
     * window, matching the frontend menu rendering.
     */
    const Viewport InGameMenuViewport(0, 0, 640, 480);

    GameScene::GameScene(
        const SceneContext& sceneContext,
        std::unique_ptr<PlayerCommandService>&& playerCommandService,
        GameMediaDatabase&& meshDatabase,
        const GameCameraState& cameraState,
        SharedTextureHandle unitTextureAtlas,
        std::vector<SharedTextureHandle>&& unitTeamTextureAtlases,
        GameSimulation&& simulation,
        MapTerrainGraphics&& terrainGraphics,
        BuilderGuisDatabase&& builderGuisDatabase,
        std::unique_ptr<GameNetworkService>&& gameNetworkService,
        const std::shared_ptr<Sprite>& minimap,
        const std::shared_ptr<SpriteSeries>& minimapDots,
        const std::shared_ptr<Sprite>& minimapDotHighlight,
        InGameSoundsInfo sounds,
        const std::shared_ptr<SpriteSeries>& guiFont,
        PlayerId localPlayerId,
        TdfBlock* audioLookup,
        const GameParameters& gameParameters,
        std::optional<std::ofstream>&& stateLogStream)
        : sceneContext(sceneContext),
          worldViewport(CroppedViewport(this->sceneContext.viewport, GuiSizeLeft, GuiSizeTop, GuiSizeRight, GuiSizeBottom)),
          playerCommandService(std::move(playerCommandService)),
          worldCameraState(cameraState),
          gameMediaDatabase(std::move(meshDatabase)),
          unitTextureAtlas(unitTextureAtlas),
          unitTeamTextureAtlases(std::move(unitTeamTextureAtlases)),
          worldUiRenderService(this->sceneContext.graphics, this->sceneContext.shaders, &this->worldViewport),
          chromeUiRenderService(this->sceneContext.graphics, this->sceneContext.shaders, this->sceneContext.viewport),
          menuUiRenderService(this->sceneContext.graphics, this->sceneContext.shaders, &InGameMenuViewport),
          simulation(std::move(simulation)),
          terrainGraphics(std::move(terrainGraphics)),
          builderGuisDatabase(std::move(builderGuisDatabase)),
          gameNetworkService(std::move(gameNetworkService)),
          minimap(minimap),
          minimapDots(minimapDots),
          minimapDotHighlight(minimapDotHighlight),
          minimapRect(minimapViewport.scaleToFit(this->minimap->bounds)),
          sounds(std::move(sounds)),
          guiFont(guiFont),
          localPlayerId(localPlayerId),
          uiFactory(sceneContext.textureService, sceneContext.audioService, audioLookup, sceneContext.vfs, sceneContext.pathMapping, sceneContext.viewport->width(), sceneContext.viewport->height()),
          stateLogStream(std::move(stateLogStream)),
          audioLookup(audioLookup),
          gameParameters(gameParameters)
    {
        perfEnabled = std::getenv("RWE_PERF") != nullptr;
        perfWindowStart = std::chrono::steady_clock::now();

        // RWE_SOAK=<seconds>: run this long then request exit -- for the
        // progressive-slowdown soak harness. Implies perf reporting.
        if (const char* soak = std::getenv("RWE_SOAK"))
        {
            soakSeconds = static_cast<unsigned int>(std::strtoul(soak, nullptr, 10));
            perfEnabled = true;
        }
        if (perfEnabled)
        {
            // note: `simulation` in this ctor body is the moved-from
            // rvalue-ref parameter; the member needs explicit this->
            this->simulation.collectTickPhaseTimings = true;
            perfPfReqBase = this->simulation.pathFindingService.stats.requestsProcessed;
            perfPfExpBase = this->simulation.pathFindingService.stats.totalExpansions;
        }

        // RWE_SCENE_BENCH=<count>:<unitType> -- scripted scenario for
        // live render profiling; implies RWE_PERF reporting
        if (const char* spec = std::getenv("RWE_SCENE_BENCH"))
        {
            std::string s(spec);
            auto colon = s.find(':');
            sceneBench.count = static_cast<unsigned int>(std::strtoul(colon == std::string::npos ? s.c_str() : s.substr(0, colon).c_str(), nullptr, 10));
            sceneBench.unitType = colon == std::string::npos ? std::string("ARMPW") : toUpper(s.substr(colon + 1));
            // comma-separated type list for the visual matrix; a '~'
            // suffix leaves the unit under construction so the build
            // shader path is exercised
            {
                std::string types = sceneBench.unitType;
                std::size_t start = 0;
                while (start <= types.size())
                {
                    auto comma = types.find(',', start);
                    auto token = types.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!token.empty())
                    {
                        bool unfinished = token.back() == '~';
                        if (unfinished)
                        {
                            token.pop_back();
                        }
                        sceneBench.unitTypes.emplace_back(token, unfinished);
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
            }
            // unit type validity is checked at spawn time -- the ctor
            // runs before unitDefinitions is fully populated
            sceneBench.active = sceneBench.count > 0;
            perfEnabled = true;
        }

        // RWE_DISABLE_CULL=1 bypasses render-side frustum culling so a
        // single binary can produce culled/unculled A/B screenshots and
        // benchmarks
        cullingEnabled = std::getenv("RWE_DISABLE_CULL") == nullptr;

        // RWE_SHOT_DIR=<dir> + RWE_SHOT_TICKS=t1,t2,... capture the world
        // framebuffer as binary PPM on the first rendered frame after
        // each listed sim tick. Interpolation is frozen at tick
        // boundaries so captures are deterministic across runs.
        if (const char* dir = std::getenv("RWE_SHOT_DIR"))
        {
            shotDir = dir;
            if (const char* ticks = std::getenv("RWE_SHOT_TICKS"))
            {
                std::string t(ticks);
                std::size_t start = 0;
                while (start <= t.size())
                {
                    auto comma = t.find(',', start);
                    auto token = t.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!token.empty())
                    {
                        shotTicks.push_back(static_cast<unsigned int>(std::strtoul(token.c_str(), nullptr, 10)));
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    start = comma + 1;
                }
                std::sort(shotTicks.begin(), shotTicks.end());
            }
        }
    }

    void GameScene::init()
    {
        const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
        currentPanel = uiFactory.panelFromGuiFile(sidePrefix + "MAIN2");

        // Restored saves insert units without emitting UnitSpawnedEvent,
        // so the per-unit GUI state the event normally registers must be
        // rebuilt from live simulation state before any selection or
        // panel code queries it. For fresh games this registers the same
        // values the already-queued spawn events will re-assign.
        rebuildUnitGuiInfos(simulation, unitGuiInfos);

        // flush track-finish notifications queued while menu scenes
        // ran; draining here keeps stale channel ids from being
        // delivered against channels this scene assigns afterwards
        sceneContext.audioService->dispatchFinishedChannels();

        sceneContext.audioService->reserveChannels(reservedChannelsCount);
        gameNetworkService->start();

        recreateWorldRenderTextures();
    }

    float computeSoundCeiling(int soundCount)
    {
        assert(soundCount > 0);
        if (soundCount <= 4)
        {
            return soundCount;
        }

        if (soundCount <= 8)
        {
            return 4 + ((soundCount - 4) * 0.5f);
        }

        if (soundCount <= 16)
        {
            return (6 + ((soundCount - 8) * 0.25f));
        }

        return 8;
    }

    int computeSoundVolume(int soundCount)
    {
        soundCount = std::max(soundCount, 1);
        auto headRoom = computeSoundCeiling(soundCount);
        return std::clamp(static_cast<int>(headRoom * 128) / soundCount, 1, 128);
    }

    void GameScene::render()
    {
        ScopedFrameTimer perfTimer(perfEnabled ? &perfRenderNs : nullptr);

        // after the debrief handoff the scene manager still runs one
        // more render against this scene before swapping; the
        // simulation is moved-from by then, so skip drawing entirely
        if (simulationTransferredToDebrief)
        {
            return;
        }

        if (guiVisible)
        {
            ScopedFrameTimer uiTimer(perfEnabled ? &perfUiNs : nullptr);
            renderUi();
        }

        sceneContext.graphics->enableDepthBuffer();

        renderWorld();
        sceneContext.graphics->disableDepthBuffer();

        // deterministic screenshot capture: on each listed tick grab the
        // world framebuffer as-built, then re-render the identical frame
        // with culling disabled so culled vs unculled pixels can be
        // diffed against the same sim state
        if (shotIndex < shotTicks.size() && simulation.gameTime.value >= shotTicks[shotIndex])
        {
            const auto tag = std::to_string(shotTicks[shotIndex]);
            captureWorldShot(tag);
            if (cullingEnabled)
            {
                cullingEnabled = false;
                renderWorld();
                captureWorldShot(tag + "_nocull");
                cullingEnabled = true;
            }
            // third variant: culling on, instancing off -- isolates the
            // instanced draw path against identical sim state
            shotInstancingDisabled = true;
            renderWorld();
            captureWorldShot(tag + "_noinst");
            shotInstancingDisabled = false;
            ++shotIndex;
        }

        // oh yeah also regulate sound
        std::vector<int> channels;
        int volume;
        {
            std::scoped_lock<std::mutex> lock(playingUnitChannelsLock);
            channels.assign(playingUnitChannels.begin(), playingUnitChannels.end());
            volume = computeSoundVolume(playingUnitChannels.size());
        }
        // mixer calls must not run while holding playingUnitChannelsLock:
        // the track-stopped callback takes that lock on the audio thread
        // while holding a mixer-internal lock, so acquiring a mixer lock
        // under our lock can deadlock.
        for (auto channel : channels)
        {
            sceneContext.audioService->setVolume(channel, volume);
        }

        if (inGameMenuOpen())
        {
            renderInGameMenu();
        }

        // igvictory/igdefeat banner: drawn over the world while the
        // ENDMSN transition delay runs down (retail draws the overlay
        // every frame the committed flag is set).
        if (missionResultSprite)
        {
            const auto& sprite = *missionResultSprite;
            float x = worldViewport.x() + (worldViewport.width() - sprite.bounds.width()) / 2.0f;
            float y = worldViewport.y() + (worldViewport.height() - sprite.bounds.height()) / 2.0f;
            chromeUiRenderService.drawSpriteAbs(x, y, sprite);
        }

        if (perfEnabled)
        {
            ++perfFrameCount;
            ++sceneBenchFrame;
            perfSamples.push_back(PerfSample{perfUpdateNs, perfTickNs, perfRenderNs});
            perfUpdateNs = perfTickNs = perfRenderNs = 0;
            auto now = std::chrono::steady_clock::now();
            auto windowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - perfWindowStart).count();
            if (windowNs >= 5000000000LL && !perfSamples.empty())
            {
                reportPerfWindow(static_cast<double>(windowNs) / 1e9);
            }
        }
    }

    void GameScene::reportPerfWindow(double windowS)
    {
        double n = static_cast<double>(perfSamples.size());
        auto pct = [&](std::uint64_t PerfSample::* field, double p) {
            std::vector<std::uint64_t> v;
            v.reserve(perfSamples.size());
            for (const auto& s : perfSamples)
            {
                v.push_back(s.*field);
            }
            std::sort(v.begin(), v.end());
            return static_cast<double>(v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]) / 1e6;
        };
        // workload accumulation snapshot -- the progressive-slowdown
        // probe watches these for growth while unit count stays flat
        std::size_t cobThreads = 0;
        for (const auto& u : simulation.units)
        {
            if (u.second.cobEnvironment)
            {
                cobThreads += u.second.cobEnvironment->threads.size();
            }
        }
        std::size_t aiDeadlocks = 0;
        for (const auto& st : simulation.campaignAi)
        {
            aiDeadlocks += st.deadlockedFactories.size();
        }
        const auto& pf = simulation.pathFindingService.stats;

        LOG_INFO << "RWE_PERF_SCENE" << (perfWindowLabel.empty() ? "" : " phase=" + perfWindowLabel)
                 << " frames=" << perfSamples.size()
                 << " fps=" << (n / windowS)
                 << " tps=" << (static_cast<double>(perfTicksExec) / windowS)
                 << " update=[p50 " << pct(&PerfSample::updateNs, 0.50) << " p95 " << pct(&PerfSample::updateNs, 0.95) << " p99 " << pct(&PerfSample::updateNs, 0.99) << " max " << pct(&PerfSample::updateNs, 1.0) << "]"
                 << " sim=[p50 " << pct(&PerfSample::tickNs, 0.50) << " p99 " << pct(&PerfSample::tickNs, 0.99) << " max " << pct(&PerfSample::tickNs, 1.0) << "]"
                 << " render=[p50 " << pct(&PerfSample::renderNs, 0.50) << " p95 " << pct(&PerfSample::renderNs, 0.95) << " p99 " << pct(&PerfSample::renderNs, 0.99) << " max " << pct(&PerfSample::renderNs, 1.0) << "]"
                 << " ticks/frame=" << (static_cast<double>(perfTickCount) / n)
                 << " accMax=" << perfAccMax
                 << " tExec=" << perfTicksExec
                 << " tBlocked=" << perfTicksBlocked
                 << " rw=[terrain " << (static_cast<double>(perfRwTerrainNs) / 1e6 / n)
                 << " shadows " << (static_cast<double>(perfRwShadowsNs) / 1e6 / n)
                 << " unitEmit " << (static_cast<double>(perfRwUnitEmitNs) / 1e6 / n)
                 << " unitSubmit " << (static_cast<double>(perfRwUnitSubmitNs) / 1e6 / n)
                 << " fog " << (static_cast<double>(perfRwFogNs) / 1e6 / n)
                 << " ui " << (static_cast<double>(perfUiNs) / 1e6 / n) << "]"
                 << " units=" << std::distance(simulation.units.begin(), simulation.units.end())
                 << " emit=" << (static_cast<double>(perfUnitsEmitted) / n)
                 << "/" << (static_cast<double>(perfUnitsCulled) / n)
                 << " shEmit=" << (static_cast<double>(perfShadowsEmitted) / n)
                 << "/" << (static_cast<double>(perfShadowsCulled) / n)
                 << " projectiles=" << std::distance(simulation.projectiles.begin(), simulation.projectiles.end())
                 << " pathQ=" << simulation.pathRequests.size()
                 << " pfReq+=" << (pf.requestsProcessed - perfPfReqBase)
                 << " pfExp+=" << (pf.totalExpansions - perfPfExpBase)
                 << " pfMaxQ=" << pf.maxQueueDepth
                 << " cobT=" << cobThreads
                 << " aiDL=" << aiDeadlocks
                 << " feats=" << std::distance(simulation.features.begin(), simulation.features.end());
        const auto& pt = simulation.tickPhaseTimings;
        if (pt.tickCount > 0)
        {
            double tn = static_cast<double>(pt.tickCount);
            LOG_INFO << "RWE_PERF_TICKPHASE ticks=" << pt.tickCount
                     << " ms/tick=[vis " << (static_cast<double>(pt.visibility) / 1e6 / tn)
                     << " ai " << (static_cast<double>(pt.campaignAi) / 1e6 / tn)
                     << " path " << (static_cast<double>(pt.pathfinding) / 1e6 / tn)
                     << " behave " << (static_cast<double>(pt.unitBehavior) / 1e6 / tn)
                     << " pieces " << (static_cast<double>(pt.pieces) / 1e6 / tn)
                     << " cob " << (static_cast<double>(pt.cob) / 1e6 / tn)
                     << " proj " << (static_cast<double>(pt.projectiles) / 1e6 / tn)
                     << " dead " << (static_cast<double>(pt.deadUnitCleanup) / 1e6 / tn)
                     << " res " << (static_cast<double>(pt.resources) / 1e6 / tn) << "]";
            simulation.tickPhaseTimings.reset();
        }
        perfTickCount = perfFrameCount = 0;
        perfSamples.clear();
        perfRwTerrainNs = perfRwShadowsNs = perfRwUnitEmitNs = perfRwUnitSubmitNs = perfRwFogNs = perfUiNs = 0;
        perfUnitsEmitted = perfUnitsCulled = perfShadowsEmitted = perfShadowsCulled = 0;
        perfAccMax = 0.0f;
        perfTicksExec = perfTicksBlocked = 0;
        perfPfReqBase = pf.requestsProcessed;
        perfPfExpBase = pf.totalExpansions;
        perfWindowStart = std::chrono::steady_clock::now();
    }

    void GameScene::enterMissionDebrief()
    {
        if (!gameParameters.campaign || !simulation.missionInfo)
        {
            // unreachable: missionInfo is only installed for campaign
            // launches, which always carry campaign parameters
            sceneContext.sceneManager->requestExit();
            return;
        }

        EndMissionInfo info;
        info.won = missionResultWon;
        info.campaign = *gameParameters.campaign;
        info.gameTime = simulation.gameTime;
        info.killMul = simulation.missionInfo->killMul;
        info.timeMul = simulation.missionInfo->timeMul;

        auto gameSeconds = static_cast<unsigned int>(simulation.gameTime.value / 30u);
        for (unsigned int i = 0; i < simulation.players.size(); ++i)
        {
            const auto& p = simulation.players[i];
            DebriefPlayerRow row;
            row.name = p.name ? *p.name : ("Player " + std::to_string(i + 1));
            row.kills = static_cast<int>(p.unitsKilled);
            row.losses = static_cast<int>(p.unitsLost);
            row.energyProduced = static_cast<int>(p.energyProduced);
            row.metalProduced = static_cast<int>(p.metalProduced);
            row.energyExcess = static_cast<int>(p.energyExcess);
            row.metalExcess = static_cast<int>(p.metalExcess);
            row.score = debriefScore(row.kills, gameSeconds, info.killMul, info.timeMul);
            info.stats.push_back(std::move(row));
        }

        // retail retains the completed game context beneath ENDMSN
        // (its SaveGame button invokes the ordinary save writer at
        // 0x4326b0 against that context); hand the finished
        // simulation and its launch parameters to the debrief scene
        // rather than tearing them down here
        auto scene = std::make_unique<EndMissionScene>(
            sceneContext,
            audioLookup,
            info,
            std::make_unique<GameSimulation>(std::move(simulation)),
            std::move(gameParameters));
        simulationTransferredToDebrief = true;
        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }

    void GameScene::renderUi()
    {
        renderMinimap();

        const auto& localSideData = sceneContext.sideData->at(getPlayer(localPlayerId).side);

        // render top bar
        const auto& intGafName = localSideData.intGaf;
        auto topPanelBackground = sceneContext.textureService->tryGetGafEntry("anims/" + intGafName + ".GAF", "PANELTOP");
        auto bottomPanelBackground = sceneContext.textureService->tryGetGafEntry("anims/" + intGafName + ".GAF", "PANELBOT");
        float topXBuffer = GuiSizeLeft;
        if (topPanelBackground)
        {
            const auto& sprite = *(*topPanelBackground)->sprites.at(0);
            chromeUiRenderService.drawSpriteAbs(topXBuffer, 0, sprite);
            topXBuffer += sprite.bounds.width();
        }
        if (bottomPanelBackground)
        {
            while (topXBuffer < sceneContext.viewport->width())
            {
                const auto& sprite = *(*bottomPanelBackground)->sprites.at(0);
                chromeUiRenderService.drawSpriteAbs(topXBuffer, 0.0f, sprite);
                topXBuffer += sprite.bounds.width();
            }
        }

        auto logos = sceneContext.textureService->tryGetGafEntry("textures/LOGOS.GAF", "32xlogos");
        if (logos)
        {
            auto playerColorIndex = getPlayer(localPlayerId).color;
            const auto& rect = localSideData.logo.toDiscreteRect();
            chromeUiRenderService.drawSpriteAbs(rect.x, rect.y, rect.width, rect.height, *(*logos)->sprites.at(playerColorIndex.value));
        }

        // draw energy bar
        {
            const auto& rect = localSideData.energyBar.toDiscreteRect();
            const auto& localPlayer = getPlayer(localPlayerId);
            auto rectWidth = localPlayer.maxEnergy == Energy(0) ? 0 : (rect.width * std::max(Energy(0), localPlayer.energy).value) / localPlayer.maxEnergy.value;
            const auto& colorIndex = localSideData.energyColor;
            const auto& color = sceneContext.palette->at(colorIndex);
            chromeUiRenderService.fillColor(rect.x, rect.y, rectWidth, rect.height, color);
        }
        {
            const auto& rect = localSideData.energy0;
            chromeUiRenderService.drawText(rect.x1, rect.y1, formatResource(Energy(0)), *guiFont);
        }
        {
            const auto& rect = localSideData.energyMax;
            auto text = formatResource(getPlayer(localPlayerId).maxEnergy);
            chromeUiRenderService.drawTextAlignRight(rect.x1, rect.y1, text, *guiFont);
        }
        {
            const auto& rect = localSideData.energyNum;
            auto text = formatResource(std::max(Energy(0), getPlayer(localPlayerId).energy));
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont);
        }
        {
            const auto& rect = localSideData.energyProduced;
            auto text = formatResourceDelta(getPlayer(localPlayerId).energyProductionBuffer);
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont, Color(83, 223, 79));
        }
        {
            const auto& rect = localSideData.energyConsumed;
            auto text = formatResourceDelta(getPlayer(localPlayerId).previousDesiredEnergyConsumptionBuffer);
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont, Color(255, 71, 0));
        }

        // draw metal bar
        {
            const auto& rect = localSideData.metalBar.toDiscreteRect();
            const auto& localPlayer = getPlayer(localPlayerId);
            auto rectWidth = localPlayer.maxMetal == Metal(0) ? 0 : (rect.width * std::max(Metal(0), localPlayer.metal).value) / localPlayer.maxMetal.value;
            const auto& colorIndex = localSideData.metalColor;
            const auto& color = sceneContext.palette->at(colorIndex);
            chromeUiRenderService.fillColor(rect.x, rect.y, rectWidth, rect.height, color);
        }
        {
            const auto& rect = localSideData.metal0;
            chromeUiRenderService.drawText(rect.x1, rect.y1, "0", *guiFont);
        }
        {
            const auto& rect = localSideData.metalMax;
            auto text = formatResource(getPlayer(localPlayerId).maxMetal);
            chromeUiRenderService.drawTextAlignRight(rect.x1, rect.y1, text, *guiFont);
        }
        {
            const auto& rect = localSideData.metalNum;
            auto text = formatResource(std::max(Metal(0), getPlayer(localPlayerId).metal));
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont);
        }
        {
            const auto& rect = localSideData.metalProduced;
            auto text = formatResourceDelta(getPlayer(localPlayerId).metalProductionBuffer);
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont, Color(83, 223, 79));
        }
        {
            const auto& rect = localSideData.metalConsumed;
            auto text = formatResourceDelta(getPlayer(localPlayerId).previousDesiredMetalConsumptionBuffer);
            chromeUiRenderService.drawText(rect.x1, rect.y1, text, *guiFont, Color(255, 71, 0));
        }

        // render bottom bar
        float bottomXBuffer = GuiSizeLeft;
        if (bottomPanelBackground)
        {
            while (bottomXBuffer < sceneContext.viewport->width())
            {
                const auto& sprite = *(*bottomPanelBackground)->sprites.at(0);
                chromeUiRenderService.drawSpriteAbs(bottomXBuffer, worldViewport.bottom(), sprite);
                bottomXBuffer += sprite.bounds.width();
            }
        }

        auto extraBottom = sceneContext.viewport->height() - 480;
        if (hoveredUnit)
        {
            const auto& unit = getUnit(*hoveredUnit);
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            if (logos)
            {
                const auto& rect = localSideData.logo2.toDiscreteRect();
                const auto& color = *(*logos)->sprites.at(getPlayer(unit.owner).color.value);
                chromeUiRenderService.drawSpriteAbs(rect.x, extraBottom + rect.y, rect.width, rect.height, color);
            }

            {
                const auto& rect = localSideData.unitName;
                const auto& playerName = getPlayer(unit.owner).name;
                const auto& text = unitDefinition.showPlayerName && playerName ? *playerName : unitDefinition.unitName;
                chromeUiRenderService.drawTextCenteredX(rect.x1, extraBottom + rect.y1, text, *guiFont);
            }

            if (unit.isOwnedBy(localPlayerId) || !unitDefinition.hideDamage)
            {
                const auto& rect = localSideData.damageBar.toDiscreteRect();
                chromeUiRenderService.drawHealthBar2(rect.x, extraBottom + rect.y, rect.width, rect.height, static_cast<float>(unit.hitPoints) / static_cast<float>(unitDefinition.maxHitPoints));
            }

            if (unit.isOwnedBy(localPlayerId))
            {
                if (unit.kills != 0)
                {
                    chromeUiRenderService.drawTextCenteredX(localSideData.unitName.x1,
                        extraBottom + localSideData.damageBar.y2 + 2, veteranLabel(unit.kills), *guiFont);
                }
                {
                    const auto& rect = localSideData.unitMetalMake;
                    auto text = "+" + formatResourceDelta(unit.getMetalMake());
                    chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont, Color(83, 223, 79));
                }
                {
                    const auto& rect = localSideData.unitMetalUse;
                    auto text = "-" + formatResourceDelta(unit.getMetalUse());
                    chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont, Color(255, 71, 0));
                }
                {
                    const auto& rect = localSideData.unitEnergyMake;
                    auto text = "+" + formatResourceDelta(unit.getEnergyMake());
                    chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont, Color(83, 223, 79));
                }
                {
                    const auto& rect = localSideData.unitEnergyUse;
                    auto text = "-" + formatResourceDelta(unit.getEnergyUse());
                    chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont, Color(255, 71, 0));
                }
                {
                    const auto& rect = localSideData.missionText;
                    auto text = "Standby";
                    chromeUiRenderService.drawTextCenteredX(rect.x1, extraBottom + rect.y1, text, *guiFont);
                }
            }
        }
        else if (hoveredFeature && simulation.tryGetFeature(*hoveredFeature))
        {
            const auto& feature = simulation.getFeature(*hoveredFeature);
            const auto& featureDefinition = simulation.getFeatureDefinition(feature.featureName);
            const auto& featureMediaInfo = gameMediaDatabase.getFeature(feature.featureName);

            {
                const auto& rect = localSideData._name;
                auto text = featureMediaInfo.description;
                if (featureDefinition.reclaimable)
                {
                    text += " ";
                    auto remainingMetal = featureDefinition.metal - std::min(featureDefinition.metal, feature.reclaimMetalGiven);
                    auto remainingEnergy = featureDefinition.energy - std::min(featureDefinition.energy, feature.reclaimEnergyGiven);
                    if (remainingMetal > 0)
                    {
                        text += " M:" + formatResource(Metal(remainingMetal));
                    }

                    if (remainingEnergy > 0)
                    {
                        text += " E:" + formatResource(Energy(remainingEnergy));
                    }
                }
                chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont);
            }
        }
        else if (auto hoveredBuildButtonUnitType = getUnitBuildButtonUnderCursor(); hoveredBuildButtonUnitType)
        {
            const auto& unitDefinition = simulation.unitDefinitions.at(*hoveredBuildButtonUnitType);

            {
                const auto& rect = localSideData._name;
                auto text = unitDefinition.unitName + "  M:" + formatResource(unitDefinition.buildCostMetal) + " E:" + formatResource(unitDefinition.buildCostEnergy);
                chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont);
            }

            {
                const auto& rect = localSideData.description;
                chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, unitDefinition.unitDescription, *guiFont);
            }
        }
        else if (auto weapon = getStockpileWeaponUnderCursor(); weapon)
        {
            // stockpile weapon-build button (GUI commonattribs=8):
            // retail shows the WEAPON's name and per-shot resource
            // cost, not a unit build entry
            const auto& weaponDefinition = simulation.weaponDefinitions.at(*weapon);
            const auto& rect = localSideData._name;
            auto text = weaponDefinition.name + "  M:" + formatResource(weaponDefinition.metalPerShot) + " E:" + formatResource(weaponDefinition.energyPerShot);
            chromeUiRenderService.drawText(rect.x1, extraBottom + rect.y1, text, *guiFont);
        }

        currentPanel->render(chromeUiRenderService);
    }

    void GameScene::renderMinimap()
    {
        // draw minimap
        chromeUiRenderService.drawSpriteAbs(minimapRect, *minimap);

        auto cameraInverse = computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height());
        auto worldToMinimap = worldToMinimapMatrix(simulation.terrain, minimapRect);

        // draw minimap fog (darken cells outside LOS)
        drawMinimapFog();

        // draw selected units' sensor range circles (retail 0x466f70):
        // radar + sonar in the sensor colour, radar-jam + sonar-jam in
        // the jam colour, for every selected unit whose active systems
        // are on (powered-off onoffable units draw nothing)
        {
            auto mmOrigin = worldToMinimap * Vector3f(0.0f, 0.0f, 0.0f);
            auto mmUnitX = worldToMinimap * Vector3f(1.0f, 0.0f, 0.0f);
            auto pxPerWu = Vector2f(mmUnitX.x - mmOrigin.x, mmUnitX.y - mmOrigin.y).length();

            const Vector3f sensorColor(0.35f, 0.50f, 0.85f);
            const Vector3f jamColor(0.90f, 0.45f, 0.25f);
            std::vector<GlColoredVertex> rangeLines;
            for (const auto& [unitId, unit] : simulation.units)
            {
                if (!selectedUnits.contains(unitId) || unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                const auto& def = simulation.unitDefinitions.at(unit.unitType);
                if (!GameSimulation::isUnitSensorOn(unit, def))
                {
                    continue;
                }
                auto center = (worldToMinimap * simVectorToFloat(unit.position)).xy();
                if (def.radarDistance > 0_ss)
                {
                    appendMinimapSensorCircleVertices(rangeLines, center, simScalarToFloat(def.radarDistance) * pxPerWu, sensorColor);
                }
                if (def.sonarDistance > 0_ss)
                {
                    appendMinimapSensorCircleVertices(rangeLines, center, simScalarToFloat(def.sonarDistance) * pxPerWu, sensorColor);
                }
                if (def.radarDistanceJam > 0_ss)
                {
                    appendMinimapSensorCircleVertices(rangeLines, center, simScalarToFloat(def.radarDistanceJam) * pxPerWu, jamColor);
                }
                if (def.sonarDistanceJam > 0_ss)
                {
                    appendMinimapSensorCircleVertices(rangeLines, center, simScalarToFloat(def.sonarDistanceJam) * pxPerWu, jamColor);
                }
            }
            chromeUiRenderService.drawLinesBatch(rangeLines);
        }

        // anti-missile coverage ring (retail 0x467082): units carrying
        // the `antiweapons` flag draw each interceptor weapon's
        // coverage minus 0x200 in the weapon colour while selected
        {
            auto mmOrigin = worldToMinimap * Vector3f(0.0f, 0.0f, 0.0f);
            auto mmUnitX = worldToMinimap * Vector3f(1.0f, 0.0f, 0.0f);
            auto pxPerWu = Vector2f(mmUnitX.x - mmOrigin.x, mmUnitX.y - mmOrigin.y).length();

            const Vector3f weaponColor(0.45f, 0.85f, 0.55f); // ctx+0xf -- palette index unknown, approximation
            const auto ringInset = SimScalar(0x200);
            std::vector<GlColoredVertex> rangeLines;
            for (const auto& [unitId, unit] : simulation.units)
            {
                if (!selectedUnits.contains(unitId) || unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                const auto& def = simulation.unitDefinitions.at(unit.unitType);
                if (!def.antiWeapons || !GameSimulation::isUnitSensorOn(unit, def))
                {
                    continue;
                }
                auto center = (worldToMinimap * simVectorToFloat(unit.position)).xy();
                for (const auto& weapon : unit.weapons)
                {
                    if (!weapon)
                    {
                        continue;
                    }
                    const auto& weaponDefinition = simulation.weaponDefinitions.at(weapon->weaponType);
                    if (!weaponDefinition.interceptor || weaponDefinition.coverage <= ringInset)
                    {
                        continue;
                    }
                    appendMinimapSensorCircleVertices(rangeLines, center, simScalarToFloat(weaponDefinition.coverage - ringInset) * pxPerWu, weaponColor);
                }
            }
            chromeUiRenderService.drawLinesBatch(rangeLines);
        }

        // draw minimap dots
        for (const auto& [unitId, unit] : simulation.units)
        {
            if (unit.carriedBy)
            {
                continue;
            }
            if (!simulation.isDetectedByPlayer(unitId, localPlayerId))
            {
                // undetected enemies get no minimap dot; radar contacts
                // (isDetectedByPlayer but not isLosVisibleToPlayer)
                // still render as dots
                continue;
            }
            auto minimapPos = worldToMinimap * simVectorToFloat(unit.position);
            minimapPos.x = std::floor(minimapPos.x);
            minimapPos.y = std::floor(minimapPos.y);
            auto ownerId = unit.owner;
            auto colorIndex = getPlayer(ownerId).color;
            chromeUiRenderService.drawSprite(minimapPos.x, minimapPos.y, *minimapDots->sprites[colorIndex.value]);
        }
        // highlight the minimap dot for the hovered unit
        if (hoveredUnit)
        {
            const auto& unit = getUnit(*hoveredUnit);
            auto minimapPos = worldToMinimap * simVectorToFloat(unit.position);
            minimapPos.x = std::floor(minimapPos.x);
            minimapPos.y = std::floor(minimapPos.y);
            chromeUiRenderService.drawSprite(minimapPos.x, minimapPos.y, *minimapDotHighlight);
        }

        // draw minimap viewport rectangle
        {
            auto transform = worldToMinimap * cameraInverse;
            auto bottomLeft = transform * Vector3f(-1.0f, -1.0f, 0.0f);
            auto topRight = transform * Vector3f(1.0f, 1.0f, 0.0f);

            chromeUiRenderService.drawBoxOutline(
                std::round(bottomLeft.x),
                std::round(topRight.y),
                std::round(topRight.x - bottomLeft.x),
                std::round(bottomLeft.y - topRight.y),
                Color(247, 227, 103));
        }
    }

    void GameScene::renderBuildBoxes(const UnitState& unit, const Color& color, const Matrix4f& worldToUi, std::vector<GlColoredVertex>& trianglesOut)
    {
        for (const auto& order : unit.orders)
        {
            if (const auto buildOrder = std::get_if<BuildOrder>(&order))
            {
                const auto& unitType = buildOrder->unitType;
                const auto& unitDefinition = simulation.unitDefinitions.at(unitType);
                auto mc = simulation.getAdHocMovementClass(unitDefinition.movementCollisionInfo);
                auto footprintRect = simulation.computeFootprintRegion(buildOrder->position, unitDefinition.movementCollisionInfo);

                auto topLeftWorld = simulation.terrain.heightmapIndexToWorldCorner(footprintRect.x, footprintRect.y);
                topLeftWorld.y = simulation.terrain.getHeightAt(
                    topLeftWorld.x + ((SimScalar(footprintRect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss),
                    topLeftWorld.z + ((SimScalar(footprintRect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss));

                auto topLeftUi = worldToUi * simVectorToFloat(topLeftWorld);
                appendUiBoxOutlineVertices(
                    trianglesOut,
                    topLeftUi.x,
                    topLeftUi.y,
                    footprintRect.width * simScalarToFloat(MapTerrain::HeightTileWidthInWorldUnits),
                    footprintRect.height * simScalarToFloat(MapTerrain::HeightTileHeightInWorldUnits),
                    color,
                    2.0f);
            }
        }
    }

    void GameScene::renderUnitOrders(UnitId unitId, bool drawLines, const Matrix4f& worldToUi, std::vector<GlColoredVertex>& linesOut)
    {
        const auto& unit = getUnit(unitId);
        auto pos = unit.position;
        for (const auto& order : unit.orders)
        {
            auto nextPos = orderMarkerPosition(simulation, order, pos);

            auto waypointIcon = orderMarkerCursorType(order);

            // draw waypoint icons
            if (waypointIcon)
            {
                auto timeInMillis = sceneContext.timeService->getTicks();
                unsigned int frameRateInSeconds = 2;
                unsigned int millisPerFrame = 1000 / frameRateInSeconds;

                const auto& frames = sceneContext.cursor->getCursor(*waypointIcon)->sprites;
                auto frameIndex = (timeInMillis / millisPerFrame) % frames.size();

                auto uiDest = worldToUi * simVectorToFloat(nextPos);
                worldUiRenderService.drawSprite(uiDest.x, uiDest.y, *(frames[frameIndex]), Color(255, 255, 255, 100));
            }

            if (drawLines)
            {
                auto drawLine = match(
                    order,
                    [&](const BuildOrder&) { return true; },
                    [&](const MoveOrder&) { return true; },
                    [&](const AttackOrder&) { return false; },
                    [&](const BuggerOffOrder&) { return false; },
                    [&](const CompleteBuildOrder&) { return true; },
                    [&](const GuardOrder&) { return true; },
                    [&](const RepairOrder&) { return true; },
                    [&](const ReclaimOrder&) { return true; },
                    [&](const PatrolOrder&) { return true; },
                    [&](const WaitOrder&) { return false; },
                    [&](const ParalyzeOrder&) { return false; },
                    [&](const LandOrder&) { return true; },
                    [&](const WaitForAttackOrder&) { return false; },
                    [&](const MakeSelectableOrder&) { return false; },
                    [&](const AttackUnitTypeOrder&) { return false; },
                    [&](const MissionBuildOrder&) { return true; },
                    [&](const MissionFactoryBuildOrder&) { return false; },
                    [&](const SelfDestructOrder&) { return false; },
                    [&](const UnloadOrder&) { return true; },
                    [&](const LoadOrder&) { return true; },
                    [&](const BuildWeaponOrder&) { return false; },
                    [&](const CommandFireOrder&) { return false; },
                    [&](const CaptureOrder&) { return true; });

                if (drawLine)
                {
                    auto uiPos = worldToUi * simVectorToFloat(pos);
                    auto uiDest = worldToUi * simVectorToFloat(nextPos);
                    appendUiLineVertices(linesOut, uiPos.xy(), uiDest.xy());
                }
            }

            pos = nextPos;
        }
    }

    void GameScene::renderWorld()
    {
        sceneContext.graphics->bindFrameBuffer(worldFrameBuffer.frameBuffer.get());
        sceneContext.graphics->setViewport(
            0,
            0,
            worldViewport.width(),
            worldViewport.height());

        sceneContext.graphics->clear();

        const auto& viewProjectionMatrix = computeViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height());
        const auto viewFrustum = extractFrustum(viewProjectionMatrix);
        RenderService worldRenderService(sceneContext.graphics, sceneContext.shaders, &viewProjectionMatrix);
        if (shotInstancingDisabled)
        {
            worldRenderService.setUnitInstancingEnabled(false);
        }

        sceneContext.graphics->disableDepthBuffer();

        {
            ScopedFrameTimer t(perfEnabled ? &perfRwTerrainNs : nullptr);
            worldRenderService.drawMapTerrain(terrainGraphics, worldCameraState.getRoundedPosition(), worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()));
        }

        SpriteBatch flatFeatureBatch;
        SpriteBatch flatFeatureShadowBatch;
        for (const auto& f : simulation.features)
        {
            const auto& featureDefinition = simulation.getFeatureDefinition(f.second.featureName);
            if (!featureDefinition.isStanding())
            {
                if (cullingEnabled && !sphereInFrustum(viewFrustum, simVectorToFloat(f.second.position), featureEventCullRadius(gameMediaDatabase, f.second)))
                {
                    continue;
                }
                drawFeature(gameMediaDatabase, f.second, featureDefinition, viewProjectionMatrix, flatFeatureBatch);
                drawFeatureShadow(gameMediaDatabase, f.second, featureDefinition, viewProjectionMatrix, flatFeatureShadowBatch);
            }
        }
        worldRenderService.drawSpriteBatch(flatFeatureShadowBatch);
        worldRenderService.drawSpriteBatch(flatFeatureBatch);

        ColoredMeshBatch squareParticlesBatch;
        for (const auto& particle : particles)
        {
            drawWakeParticle(gameMediaDatabase, simulation.gameTime, viewProjectionMatrix, particle, squareParticlesBatch);
        }
        worldRenderService.drawBatch(squareParticlesBatch, viewProjectionMatrix);

        ColoredMeshBatch terrainOverlayBatch;

        if (occupiedGridVisible)
        {
            drawOccupiedGrid(worldCameraState.getRoundedPosition(), worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()), simulation.terrain, simulation.occupiedGrid, terrainOverlayBatch);
        }
        if (pathfindingVisualisationVisible)
        {
            drawPathfindingVisualisation(simulation.terrain, simulation.pathFindingService.lastPathDebugInfo, terrainOverlayBatch);
        }

        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit && movementClassGridVisible)
        {
            const auto& unit = simulation.getUnitState(*selectedUnit);
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            match(
                unitDefinition.movementCollisionInfo,
                [&](const UnitDefinition::NamedMovementClass& c) {
                    const auto& grid = simulation.movementClassCollisionService.getGrid(c.movementClassId);
                    drawMovementClassCollisionGrid(simulation.terrain, grid, worldCameraState.getRoundedPosition(), worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()), terrainOverlayBatch);
                },
                [&](const auto&) {

                });
        }

        worldRenderService.drawBatch(terrainOverlayBatch, viewProjectionMatrix);

        // screenshot mode pins interpolation to the tick boundary so a
        // given gameTime renders identically regardless of frame pacing
        auto interpolationFraction = shotTicks.empty()
            ? tickAccumulator.interpolationFraction()
            : 0.0f;
        ColoredMeshesBatch selectionRectBatch;
        ColoredMeshBatch decloakRingBatch;
        for (const auto& selectedUnitId : selectedUnits)
        {
            const auto& unit = getUnit(selectedUnitId);
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            if (cullingEnabled && !sphereInFrustum(viewFrustum, simVectorToFloat(unit.position), unitRenderCullRadius))
            {
                continue;
            }
            drawSelectionRect(gameMediaDatabase, viewProjectionMatrix, unit, unitDefinition, interpolationFraction, selectionRectBatch);

            // retail draws the mincloakdistance decloak ring around a
            // selected unit while it is effectively cloaked (0x4390e2)
            if (unit.cloaked && unitDefinition.minCloakDistance > 0_ss)
            {
                appendDecloakRingVertices(decloakRingBatch.lines, unit, simScalarToFloat(unitDefinition.minCloakDistance), interpolationFraction);
            }
        }
        worldRenderService.drawLineLoopsBatch(selectionRectBatch);
        worldRenderService.drawBatch(decloakRingBatch, viewProjectionMatrix);

        auto seaLevel = simulation.terrain.getSeaLevel();

        UnitShadowMeshBatch unitShadowMeshBatch;
        {
            ScopedFrameTimer t(perfEnabled ? &perfRwShadowsNs : nullptr);
            for (const auto& [unitId, unit] : simulation.units)
            {
                if (unit.carriedBy)
                {
                    // cargo inside a transport is not drawn
                    continue;
                }
                if (!simulation.isLosVisibleToPlayer(unitId, localPlayerId))
                {
                    // undetected enemies are hidden by fog of war
                    continue;
                }
                const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
                const auto& modelDefinition = simulation.unitModelDefinitions.at(unitDefinition.objectName);

                auto groundHeight = simulation.terrain.getHeightAt(unit.position.x, unit.position.z);
                if (unitDefinition.floater || unitDefinition.canHover)
                {
                    groundHeight = rweMax(groundHeight, seaLevel);
                }
                if (cullingEnabled && !unitOrShadowInFrustum(viewFrustum, unit.position, simScalarToFloat(groundHeight), unitRenderCullRadius))
                {
                    ++perfShadowsCulled;
                    continue;
                }
                ++perfShadowsEmitted;
                drawUnitShadow(gameMediaDatabase, viewProjectionMatrix, unit, unitDefinition, modelDefinition, interpolationFraction, simScalarToFloat(groundHeight), unitTextureAtlas.get(), unitTeamTextureAtlases, unitShadowMeshBatch);
            }
            for (const auto& [_, feature] : simulation.features)
            {
                const auto& position = feature.position;
                auto groundHeight = simulation.terrain.getHeightAt(position.x, position.z);
                if (position.y >= seaLevel && groundHeight < seaLevel)
                {
                    groundHeight = seaLevel;
                }
                if (cullingEnabled && !unitOrShadowInFrustum(viewFrustum, position, simScalarToFloat(groundHeight), featureRenderCullRadius))
                {
                    continue;
                }

                drawFeatureMeshShadow(simulation.unitModelDefinitions, gameMediaDatabase, viewProjectionMatrix, feature, simScalarToFloat(groundHeight), unitTextureAtlas.get(), unitTeamTextureAtlases, unitShadowMeshBatch);
            }
            worldRenderService.drawUnitShadowMeshBatch(unitShadowMeshBatch);
        }

        sceneContext.graphics->enableDepthBuffer();

        UnitMeshBatch unitMeshBatch;
        {
            ScopedFrameTimer unitEmitTimer(perfEnabled ? &perfRwUnitEmitNs : nullptr);
            for (const auto& [unitId, unit] : simulation.units)
            {
                if (unit.carriedBy)
                {
                    // retail draws attached cargo under the carrier's slot
                    // piece transform (hover deck cargo, VTOL sling load)
                    const auto carrier = simulation.tryGetUnitState(*unit.carriedBy);
                    if (!carrier)
                    {
                        continue;
                    }
                    if (!simulation.isLosVisibleToPlayer(*unit.carriedBy, localPlayerId))
                    {
                        continue;
                    }
                    const auto& carriedUnitDefinition = simulation.unitDefinitions.at(unit.unitType);
                    const auto& carriedUnitModelDefinition = simulation.unitModelDefinitions.at(carriedUnitDefinition.objectName);
                    const auto& carrierUnitDefinition = simulation.unitDefinitions.at(carrier->get().unitType);
                    const auto& carrierUnitModelDefinition = simulation.unitModelDefinitions.at(carrierUnitDefinition.objectName);
                    drawAttachedUnit(gameMediaDatabase, viewProjectionMatrix, unit, carriedUnitDefinition, carriedUnitModelDefinition, carrier->get(), carrierUnitModelDefinition, unit.attachSlot, getPlayer(unit.owner).color, interpolationFraction, unitTextureAtlas.get(), unitTeamTextureAtlases, unitMeshBatch, unit.cloaked ? 0.5f : 1.0f);
                    continue;
                }
                if (!simulation.isLosVisibleToPlayer(unitId, localPlayerId))
                {
                    // undetected enemies are hidden by fog of war
                    continue;
                }
                const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
                const auto& unitModelDefinition = simulation.unitModelDefinitions.at(unitDefinition.objectName);
                if (cullingEnabled && !sphereInFrustum(viewFrustum, simVectorToFloat(unit.position), unitRenderCullRadius))
                {
                    ++perfUnitsCulled;
                    continue;
                }
                ++perfUnitsEmitted;
                // a cloaked unit reaching this point is ours (enemy-cloaked
                // units are filtered by isLosVisibleToPlayer) -- retail
                // draws it through its ghost/dimmed variant so the owner
                // still sees it
                drawUnit(gameMediaDatabase, viewProjectionMatrix, unit, unitDefinition, unitModelDefinition, getPlayer(unit.owner).color, interpolationFraction, unitTextureAtlas.get(), unitTeamTextureAtlases, unitMeshBatch, unit.cloaked ? 0.5f : 1.0f);
            }
            for (const auto& [_, feature] : simulation.features)
            {
                if (cullingEnabled && !sphereInFrustum(viewFrustum, simVectorToFloat(feature.position), featureRenderCullRadius))
                {
                    continue;
                }
                drawMeshFeature(simulation.unitModelDefinitions, gameMediaDatabase, viewProjectionMatrix, feature, unitTextureAtlas.get(), unitTeamTextureAtlases, unitMeshBatch);
            }
        }
        {
            ScopedFrameTimer t(perfEnabled ? &perfRwUnitSubmitNs : nullptr);
            worldRenderService.drawUnitMeshBatch(unitMeshBatch, simScalarToFloat(seaLevel), simulation.gameTime.value);

            ColoredMeshBatch lineProjectilesBatch;
            SpriteBatch spriteProjectilesBatch;
            UnitMeshBatch meshProjectilesBatch;
            drawProjectiles(simulation, gameMediaDatabase, viewProjectionMatrix, simulation.projectiles, simulation.gameTime, interpolationFraction, unitTextureAtlas.get(), unitTeamTextureAtlases, lineProjectilesBatch, spriteProjectilesBatch, meshProjectilesBatch);
            worldRenderService.drawBatch(lineProjectilesBatch, viewProjectionMatrix);
            worldRenderService.drawUnitMeshBatch(meshProjectilesBatch, simScalarToFloat(seaLevel), simulation.gameTime.value);
            worldRenderService.drawSpriteBatch(spriteProjectilesBatch);
        }

        sceneContext.graphics->disableDepthWrites();

        SpriteBatch featureBatch;
        SpriteBatch featureShadowBatch;
        for (const auto& f : simulation.features)
        {
            const auto& featureDefinition = simulation.getFeatureDefinition(f.second.featureName);
            if (featureDefinition.isStanding())
            {
                if (cullingEnabled && !sphereInFrustum(viewFrustum, simVectorToFloat(f.second.position), featureEventCullRadius(gameMediaDatabase, f.second)))
                {
                    continue;
                }
                drawFeature(gameMediaDatabase, f.second, featureDefinition, viewProjectionMatrix, featureBatch);
                drawFeatureShadow(gameMediaDatabase, f.second, featureDefinition, viewProjectionMatrix, featureShadowBatch);
            }
        }
        worldRenderService.drawSpriteBatch(featureShadowBatch);
        worldRenderService.drawSpriteBatch(featureBatch);

        // fog of war: darken terrain outside the local player's LOS.
        // Unexplored cells go fully black; explored-but-not-visible
        // cells dim (OTA `mapping` semantics).
        if (simulation.lineOfSightEnabled && localPlayerId.value < simulation.playerVisibility.size())
        {
            ScopedFrameTimer t(perfEnabled ? &perfRwFogNs : nullptr);
            const auto& pv = simulation.playerVisibility[localPlayerId.value];
            ColoredMeshBatch fogUnexploredBatch;
            ColoredMeshBatch fogExploredBatch;
            fogExploredBatch.alpha = 0.55f;
            fillFogOfWarBatches(
                worldCameraState.getRoundedPosition(),
                worldCameraState.scaleDimension(worldViewport.width()),
                worldCameraState.scaleDimension(worldViewport.height()),
                simulation.terrain,
                pv,
                fogUnexploredBatch,
                fogExploredBatch);
            worldRenderService.drawBatch(fogUnexploredBatch, viewProjectionMatrix);
            worldRenderService.drawBatch(fogExploredBatch, viewProjectionMatrix);
        }

        sceneContext.graphics->disableDepthTest();
        // retail's nanolathe spray is a front-most overlay: solid
        // palette squares, no depth test (TotalA.exe 0x473a00)
        ColoredMeshBatch nanolatheBatch;
        for (const auto& particle : particles)
        {
            drawNanolatheParticle(simulation, localPlayerId, worldCameraState.scaleDimension(1.0f), particle, nanolatheBatch);
        }
        worldRenderService.drawBatch(nanolatheBatch, viewProjectionMatrix);

        sceneContext.graphics->bindFrameBufferColorBuffer(dodgeMask.get());
        sceneContext.graphics->clearColor();
        worldRenderService.drawFlashes(simulation.gameTime, flashes);
        sceneContext.graphics->bindFrameBufferColorBuffer(worldFrameBuffer.texture.get());

        sceneContext.graphics->unbindFrameBuffer();
        auto viewportPos = worldViewport.toOtherViewport(*sceneContext.viewport, 0, worldViewport.height());
        sceneContext.graphics->setViewport(
            viewportPos.x,
            sceneContext.viewport->height() - viewportPos.y,
            worldViewport.width(),
            worldViewport.height());

        sceneContext.graphics->disableDepthBuffer();
        auto quadMesh = sceneContext.graphics->createUnitTexturedQuadFlipped(Rectangle2f::fromTLBR(1.0f, 0.0f, 0.0f, 1.0f));
        sceneContext.graphics->bindShader(sceneContext.shaders->worldPost.handle.get());
        sceneContext.graphics->setUniformInt(sceneContext.shaders->worldPost.dodgeMask, 1);
        sceneContext.graphics->bindTexture(worldFrameBuffer.texture.get());
        sceneContext.graphics->setActiveTextureSlot1();
        sceneContext.graphics->bindTexture(dodgeMask.get());
        sceneContext.graphics->setActiveTextureSlot0();
        sceneContext.graphics->drawTriangles(quadMesh);

        SpriteBatch spriteParticlesBatch;
        for (const auto& particle : particles)
        {
            drawSpriteParticle(gameMediaDatabase, simulation.gameTime, viewProjectionMatrix, particle, spriteParticlesBatch);
        }
        worldRenderService.drawSpriteBatch(spriteParticlesBatch);
        sceneContext.graphics->enableDepthTest();

        sceneContext.graphics->enableDepthWrites();

        // In-world UI overlay geometry (route lines and build-box
        // outlines) is accumulated into these vectors and streamed to
        // GL once per frame -- per-primitive meshes were a perf problem.
        std::vector<GlColoredVertex> overlayLines;
        std::vector<GlColoredVertex> overlayTriangles;
        auto worldToUi = worldUiRenderService.getInverseViewProjectionMatrix()
            * viewProjectionMatrix;

        // in-world UI/overlay rendering
        if (isShiftDown())
        {
            auto singleSelectedUnit = getSingleSelectedUnit();

            // if unit is a builder, show all other buildings being built
            if (shouldShowAllBuildBoxes(simulation, localPlayerId, singleSelectedUnit, hoveredUnit))
            {
                for (const auto& [_, unit] : simulation.units)
                {
                    if (unit.isOwnedBy(localPlayerId) && !unit.carriedBy)
                    {
                        renderBuildBoxes(unit, Color(0, 0, 255), worldToUi, overlayTriangles);
                    }
                }
            }

            // draw orders + lines for hovered unit
            if (hoveredUnit && getUnit(*hoveredUnit).isOwnedBy(localPlayerId))
            {
                renderUnitOrders(*hoveredUnit, true, worldToUi, overlayLines);
            }

            // draw orders for all selected units
            for (const auto& selectedUnitId : selectedUnits)
            {
                renderBuildBoxes(getUnit(selectedUnitId), Color(0, 255, 0), worldToUi, overlayTriangles);

                if (selectedUnitId != hoveredUnit)
                {
                    // draw lines if only one unit is selected--hovered unit is drawn aleady
                    renderUnitOrders(selectedUnitId, singleSelectedUnit == selectedUnitId, worldToUi, overlayLines);
                }
            }
        }

        if (healthBarsVisible)
        {
            for (const auto& [_, unit] : simulation.units)
            {
                if (unit.carriedBy)
                {
                    continue;
                }
                if (!unit.isOwnedBy(localPlayerId))
                {
                    // only draw healthbars on units we own
                    continue;
                }

                if (unit.hitPoints == 0)
                {
                    // Do not show health bar when the unit has zero health.
                    // This can happen when the unit is still a freshly created nanoframe.
                    continue;
                }

                const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);

                auto uiPos = worldUiRenderService.getInverseViewProjectionMatrix()
                    * viewProjectionMatrix
                    * simVectorToFloat(unit.position);
                worldUiRenderService.drawHealthBar(uiPos.x, uiPos.y, static_cast<float>(unit.hitPoints) / static_cast<float>(unitDefinition.maxHitPoints));
            }
        }

        // Draw build box outline(s) when a unit is selected to be built.
        // During an engaged linear-build drag, preview every candidate.
        auto drawBuildGhost = [&](const DiscreteRect& rect, bool isValid, SimScalar baseY) {
            Color color = isValid ? Color(0, 255, 0) : Color(255, 0, 0);

            auto topLeftWorld = simulation.terrain.heightmapIndexToWorldCorner(rect.x, rect.y);
            topLeftWorld.y = baseY;

            auto topLeftUi = worldToUi * simVectorToFloat(topLeftWorld);
            appendUiBoxOutlineVertices(
                overlayTriangles,
                topLeftUi.x,
                topLeftUi.y,
                rect.width * simScalarToFloat(MapTerrain::HeightTileWidthInWorldUnits),
                rect.height * simScalarToFloat(MapTerrain::HeightTileHeightInWorldUnits),
                color,
                2.0f);
        };

        if (buildDragState && buildDragState->engaged)
        {
            for (const auto& candidate : buildDragCandidates)
            {
                drawBuildGhost(candidate.rect, candidate.valid, candidate.baseY);
            }
        }
        else if (hoverBuildInfo)
        {
            drawBuildGhost(hoverBuildInfo->rect, hoverBuildInfo->isValid, hoverBuildInfo->baseY);
        }

        // stream all accumulated overlay geometry in two draw calls
        worldUiRenderService.drawLinesBatch(overlayLines);
        worldUiRenderService.fillColorBatch(overlayTriangles, 255);

        // Draw bandbox selection rectangle
        if (auto normalCursorMode = std::get_if<NormalCursorMode>(&cursorMode.getValue()))
        {
            if (auto selectingState = std::get_if<NormalCursorMode::SelectingState>(&normalCursorMode->state))
            {
                const auto& start = selectingState->startPosition;
                const auto cameraPosition = worldCameraState.getRoundedPosition();
                Point cameraRelativeStart(start.x - cameraPosition.x, start.y - cameraPosition.z);

                auto worldViewportPos = sceneContext.viewport->toOtherViewport(worldViewport, getMousePosition());
                auto rect = DiscreteRect::fromPoints(cameraRelativeStart, worldViewportPos);

                worldUiRenderService.drawBoxOutline(rect.x, rect.y, rect.width, rect.height, Color(255, 255, 255));
                if (rect.width > 2 && rect.height > 2)
                {
                    worldUiRenderService.drawBoxOutline(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, Color(0, 0, 0));
                }
            }
        }

        if (cursorTerrainDotVisible)
        {
            // draw a dot where we think the cursor intersects terrain
            auto ray = screenToWorldRayUtil(computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height()), screenToWorldClipSpace(getMousePosition()));
            auto intersect = simulation.intersectLineWithTerrain(floatToSimLine(ray.toLine()));
            if (intersect)
            {
                auto cursorTerrainPos = worldUiRenderService.getInverseViewProjectionMatrix()
                    * viewProjectionMatrix
                    * simVectorToFloat(*intersect);
                worldUiRenderService.fillColor(cursorTerrainPos.x - 2, cursorTerrainPos.y - 2, 4, 4, Color(0, 0, 255));

                intersect->y = simulation.terrain.getHeightAt(intersect->x, intersect->z);

                auto heightTestedTerrainPos = worldUiRenderService.getInverseViewProjectionMatrix()
                    * viewProjectionMatrix
                    * simVectorToFloat(*intersect);
                worldUiRenderService.fillColor(heightTestedTerrainPos.x - 2, heightTestedTerrainPos.y - 2, 4, 4, Color(255, 0, 0));
            }
        }

        sceneContext.graphics->enableDepthBuffer();
        sceneContext.graphics->setViewport(0, 0, sceneContext.viewport->width(), sceneContext.viewport->height());
    }

    /**
     * Dumps the world framebuffer to <shotDir>/shot_<name>.ppm.
     * Called with the world framebuffer still bound.
     */
    void GameScene::captureWorldShot(const std::string& name)
    {
        auto w = worldViewport.width();
        auto h = worldViewport.height();
        std::vector<unsigned char> pixels;
        sceneContext.graphics->readPixelsRGBA(0, 0, w, h, pixels);

        std::filesystem::create_directories(shotDir);
        auto path = std::filesystem::path(shotDir) / ("shot_" + name + ".ppm");
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << w << " " << h << "\n255\n";
        for (int y = h - 1; y >= 0; --y)
        {
            for (int x = 0; x < w; ++x)
            {
                auto* px = pixels.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                out.write(reinterpret_cast<const char*>(px), 3);
            }
        }
        LOG_INFO << "RWE_SHOT wrote " << path << " at tick " << simulation.gameTime.value;
    }

    const char* stateToString(const UnitBehaviorState& state)
    {
        return match(
            state,
            [&](const UnitBehaviorStateIdle&) {
                return "idle";
            },
            [&](const UnitBehaviorStateBuilding&) {
                return "building";
            },
            [&](const UnitBehaviorStateRepairing&) {
                return "repairing";
            },
            [&](const UnitBehaviorStateReclaiming&) {
                return "reclaiming";
            },
            [&](const UnitBehaviorStateCapturing&) {
                return "capturing";
            },
            [&](const UnitBehaviorStateCreatingUnit&) {
                return "creating unit";
            });
    }

    const char* cobAxisToString(const CobAxis& axis)
    {
        switch (axis)
        {
            case CobAxis::X:
                return "x-axis";
            case CobAxis::Y:
                return "Y-axis";
            case CobAxis::Z:
                return "z-axis";
            default:
                throw std::logic_error("invalid axis");
        }
    }

    std::string blockedStatusToString(const CobEnvironment::BlockedStatus& status)
    {
        return match(
            status.condition,
            [&](const CobEnvironment::BlockedStatus::Move& m) {
                return "wait-for-move piece " + std::to_string(m.object) + " along " + cobAxisToString(m.axis);
            },
            [&](const CobEnvironment::BlockedStatus::Turn& t) {
                return "wait-for-turn piece " + std::to_string(t.object) + " around " + cobAxisToString(t.axis);
            });
    }

    void renderUnitInfoSection(const UnitState& unit)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("Unit Info"))
        {
            ImGui::LabelText("State", "%s", stateToString(unit.behaviourState));

            ImGui::LabelText("x", "%f", unit.position.x.value);
            ImGui::LabelText("y", "%f", unit.position.y.value);
            ImGui::LabelText("z", "%f", unit.position.z.value);
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("COB Scripts"))
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("Static Variables"))
            {
                for (Index i = 0; i < getSize(unit.cobEnvironment->_statics); ++i)
                {
                    ImGui::Text("%lld: %d", i, unit.cobEnvironment->_statics[i]);
                }
                ImGui::TreePop();
            }

            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("Threads"))
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode("All"))
                {

                    for (Index i = 0; i < getSize(unit.cobEnvironment->threads); ++i)
                    {
                        const auto& thread = *unit.cobEnvironment->threads[i];
                        ImGui::Text("%lld: %s (%u)", i, thread.name.c_str(), thread.signalMask);
                    }
                    ImGui::TreePop();
                }

                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode("Ready"))
                {
                    for (Index i = 0; i < getSize(unit.cobEnvironment->readyQueue); ++i)
                    {
                        ImGui::Text("%s", unit.cobEnvironment->readyQueue[i]->name.c_str());
                    }
                    ImGui::TreePop();
                }

                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode("Blocked"))
                {
                    for (Index i = 0; i < getSize(unit.cobEnvironment->blockedQueue); ++i)
                    {
                        const auto& pair = unit.cobEnvironment->blockedQueue[i];
                        ImGui::Text("%s, %s", pair.second->name.c_str(), blockedStatusToString(pair.first).c_str());
                    }
                    ImGui::TreePop();
                }

                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode("Sleeping"))
                {
                    for (Index i = 0; i < getSize(unit.cobEnvironment->sleepingQueue); ++i)
                    {
                        const auto& pair = unit.cobEnvironment->sleepingQueue[i];
                        ImGui::Text("%s, wake time: %d", pair.second->name.c_str(), pair.first.value);
                    }
                    ImGui::TreePop();
                }

                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNode("Finished"))
                {
                    for (Index i = 0; i < getSize(unit.cobEnvironment->finishedQueue); ++i)
                    {
                        ImGui::Text("%s", unit.cobEnvironment->finishedQueue[i]->name.c_str());
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
        }
    }

    void GameScene::renderDebugWindow()
    {
        if (!showDebugWindow || simulationTransferredToDebrief)
        {
            return;
        }

        ImGui::Begin("Game Debug", &showDebugWindow);
        ImGui::Checkbox("Health bars", &healthBarsVisible);
        if (ImGui::Checkbox("GUI", &guiVisible))
        {
            if (guiVisible)
            {
                worldViewport.setInset(GuiSizeLeft, GuiSizeTop, GuiSizeRight, GuiSizeBottom);
            }
            else
            {
                worldViewport.setInset(0, 0, 0, 0);
            }
            recreateWorldRenderTextures();
        }
        ImGui::Separator();
        ImGui::Checkbox("Cursor terrain dot", &cursorTerrainDotVisible);
        ImGui::Checkbox("Occupied grid", &occupiedGridVisible);
        ImGui::Checkbox("Pathfinding visualisation", &pathfindingVisualisationVisible);
        ImGui::Checkbox("Movement class grid", &movementClassGridVisible);
        ImGui::InputInt("Side", &unitSpawnPlayer);
        if (ImGui::InputText("Spawn Unit", unitSpawnText, IM_ARRAYSIZE(unitSpawnText), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            std::string text(unitSpawnText);
            text = toUpper(text);
            if (!text.empty() && isValidUnitType(simulation, text) && unitSpawnPlayer >= 0 && unitSpawnPlayer < getSize(simulation.players))
            {
                if (auto terrainPos = getMouseTerrainCoordinate())
                {
                    spawnCompletedUnit(text, PlayerId(unitSpawnPlayer), *terrainPos);
                }
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::Separator();
        {
            std::scoped_lock<std::mutex> lock(playingUnitChannelsLock);
            ImGui::LabelText("Unit sounds", "%lld", getSize(playingUnitChannels));
            ImGui::LabelText("Sound volume", "%d", computeSoundVolume(getSize(playingUnitChannels)));
        }

        if (ImGui::CollapsingHeader("Selected Unit"))
        {
            ImGui::Indent();
            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                const auto& unit = getUnit(*selectedUnit);
                renderUnitInfoSection(unit);
            }
            else
            {
                ImGui::Text("None");
            }
            ImGui::Unindent();
        }

        auto mouseTerrainCoordinate = getMouseTerrainCoordinate();

        if (mouseTerrainCoordinate)
        {
            ImGui::LabelText("mouse terrain x", "%f", mouseTerrainCoordinate->x.value);
            ImGui::LabelText("mouse terrain y", "%f", mouseTerrainCoordinate->y.value);
            ImGui::LabelText("mouse terrain z", "%f", mouseTerrainCoordinate->z.value);
        }

        ImGui::End();
    }

    void GameScene::onKeyDown(const SDL_KeyboardEvent& keysym)
    {
        if (inGameMenuOpen())
        {
            // a child's activation can pop or close the very dialog
            // being dispatched to; retire it until this scope unwinds
            DialogDispatchScope dispatchScope(*this);

            if (keysym.key == SDLK_F2)
            {
                closeInGameMenu();
                return;
            }

            auto& topDialog = *inGameMenuDialogs.back();
            // ESC/Enter on a companion option pane resolve against the
            // owning screen's defaults (SPEEDSRT has none; PREFS maps
            // both to its committing PREV button)
            auto* keyTarget = inGameMenuKeyTarget();
            if (keyTarget == nullptr)
            {
                return;
            }
            if (keysym.key == SDLK_ESCAPE)
            {
                // stock GUI escdefault names the control Escape
                // activates; retail resolves it even while a TextBox
                // owns keyboard focus
                if (!keyTarget->getEscDefault().empty())
                {
                    if (keyTarget->find<UiStagedButton>(keyTarget->getEscDefault()))
                    {
                        // activating a control focuses it in retail;
                        // this also lifts focus off a text entry field
                        keyTarget->setFocusByName(keyTarget->getEscDefault());
                        keyTarget->keyDown(KeyEvent(SDLK_SPACE));
                        return;
                    }
                }
                // no escdefault authored: the key still reaches the
                // dialog -- a focused TextBox consumes it to drop
                // focus, an Escape quickkey (e.g. LOADGAME's CANCEL=27)
                // may fire; if nothing consumed the key it bubbles up
                // and the dialog pops
                if (!keyTarget->keyDown(KeyEvent(keysym.key)))
                {
                    popInGameMenuDialog();
                }
                return;
            }
            if (keysym.key == SDLK_RETURN || keysym.key == SDLK_KP_ENTER)
            {
                // retail suppresses crdefault while a TextBox owns
                // keyboard focus (TotalA.exe 0x4a9c80): the focused
                // gadget consumes Enter to drop focus, and an Enter
                // quickkey (e.g. LOADGAME's LOAD=13) may fire instead
                if (keyTarget->keyDown(KeyEvent(keysym.key)))
                {
                    return;
                }
                // crdefault names the control Enter activates
                if (!keyTarget->getCrDefault().empty())
                {
                    if (keyTarget->find<UiStagedButton>(keyTarget->getCrDefault()))
                    {
                        keyTarget->setFocusByName(keyTarget->getCrDefault());
                        keyTarget->keyDown(KeyEvent(SDLK_SPACE));
                    }
                }
                return;
            }

            // modified letter/digit chords translate to command codes
            // in retail and never match a plain-char quickkey
            if (isModifiedLetterOrDigit(keysym.key))
            {
                return;
            }
            topDialog.keyDown(KeyEvent(keysym.key));
            if (keyTarget != &topDialog)
            {
                // companion panes share the composite screen's
                // quickkeys with their owning dialog
                keyTarget->keyDown(KeyEvent(keysym.key));
            }
            return;
        }

        if (keysym.key == SDLK_F2)
        {
            openInGameMenu();
            return;
        }

        if (keysym.key == SDLK_F1)
        {
            openInGameMenuHelp();
            return;
        }

        if (keysym.key == SDLK_PAUSE)
        {
            togglePause();
            return;
        }

        // retail's key translator (TotalA.exe 0x4c1c70) turns modified
        // letters/digits into dedicated command codes (Ctrl/Alt+letter
        // -> 0xaa-0xc3, Ctrl+digit -> 0xc4-0xcd) that never reach GUI
        // quickkeys; suppress the panel broadcast so e.g. Ctrl+D arms
        // self-destruct without also firing the `d` order quickkey
        if (!isModifiedLetterOrDigit(keysym.key))
        {
            currentPanel->keyDown(KeyEvent(keysym.key));
        }

        // SDL3 folds Shift into the keycode for letters ('a'->'A');
        // retail's letter commands key off the letter regardless of
        // Shift, so dispatch on the case-folded value
        const auto key = (keysym.key >= 'A' && keysym.key <= 'Z')
            ? keysym.key + ('a' - 'A')
            : keysym.key;

        if (key == SDLK_UP)
        {
            up = true;
        }
        else if (key == SDLK_DOWN)
        {
            down = true;
        }
        else if (key == SDLK_LEFT)
        {
            left = true;
        }
        else if (key == SDLK_RIGHT)
        {
            right = true;
        }
        else if (key == SDLK_ESCAPE)
        {
            handleEscapeDown();
        }
        else if (key == SDLK_F10)
        {
            showDebugWindow = true;
        }
        else if (key == SDLK_MINUS || keysym.key == SDLK_KP_MINUS)
        {
            adjustGameSpeed(-1);
        }
        else if (key == SDLK_EQUALS || key == SDLK_PLUS || keysym.key == SDLK_KP_PLUS || keysym.key == SDLK_KP_EQUALS)
        {
            adjustGameSpeed(+1);
        }
        else if (keysym.scancode == SDL_SCANCODE_GRAVE)
        {
            healthBarsVisible = !healthBarsVisible;
        }
        else if (key == SDLK_T)
        {
            startTrack();
        }
        else if (key == SDLK_C)
        {
            if (isCtrlDown())
            {
                const auto& localSideData = sceneContext.sideData->at(getPlayer(localPlayerId).side);
                if (!isShiftDown())
                {
                    clearUnitSelection();
                }

                // Select commanders (edge case: debug mode allows spawning multiple commanders)
                std::optional<UnitId> commanderUnitId;
                for (const auto& [unitId, unit] : simulation.units)
                {
                    const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
                    if (unitDefinition.commander && unit.isOwnedBy(localPlayerId))
                    {
                        selectAdditionalUnit(unitId);
                        // For multiple commanders, OTA selects all but always tracks only the last spawned (it won't cycle with repeated ctrl-c)
                        commanderUnitId = unitId;
                    }
                }

                if (commanderUnitId)
                {
                    startTrackInternal({*commanderUnitId});
                }
            }
        }
        else if (key == SDLK_D)
        {
            // retail translates Ctrl/Alt+letter to the same command code,
            // so Ctrl+D and Alt+D both issue SELFDESTRUCT (TotalA.exe
            // 0x4c1c70, dispatcher 0x495e90 case 0xad -> 0x49641d)
            if (isCtrlDown() || isAltDown())
            {
                // the retail handler snapshots the selection before
                // toggling each unit's self-destruct command node
                const std::unordered_set<UnitId> snapshot(selectedUnits);
                for (const auto& unitId : snapshot)
                {
                    localPlayerToggleSelfDestruct(unitId);
                }
            }
        }
        else if (key == SDLK_A)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectAllEligibleUnits();
            }
        }
        else if (key == SDLK_S)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectUnitsOnScreen();
            }
        }
        else if (key == SDLK_Z)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectUnitsOfSelectedTypes();
            }
        }
        else if (key == SDLK_P)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectArmedAircraft();
            }
        }
        else if (key == SDLK_R)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectRadarSonarUnits();
            }
        }
        else if (key == SDLK_W)
        {
            if (isCtrlDown() || isAltDown())
            {
                selectArmedMobileUnits();
            }
        }
    }

    void GameScene::onKeyUp(const SDL_KeyboardEvent& keysym)
    {
        if (inGameMenuOpen())
        {
            DialogDispatchScope dispatchScope(*this);
            inGameMenuDialogs.back()->keyUp(KeyEvent(keysym.key));
        }
        else
        {
            currentPanel->keyUp(KeyEvent(keysym.key));
        }

        if (keysym.key == SDLK_UP)
        {
            up = false;
        }
        else if (keysym.key == SDLK_DOWN)
        {
            down = false;
        }
        else if (keysym.key == SDLK_LEFT)
        {
            left = false;
        }
        else if (keysym.key == SDLK_RIGHT)
        {
            right = false;
        }
    }

    void GameScene::onMouseDown(MouseButtonEvent event)
    {
        if (inGameMenuOpen())
        {
            auto p = toInGameMenuCoordinates(event.x, event.y);
            event.x = p.x;
            event.y = p.y;
            inGameMenuMouseTarget = inGameMenuPointerTarget(event.x, event.y);
            if (inGameMenuMouseTarget != nullptr)
            {
                DialogDispatchScope dispatchScope(*this);
                inGameMenuMouseTarget->mouseDown(event);
            }
            return;
        }

        currentPanel->mouseDown(event);

        if (event.button == MouseButtonEvent::MouseButton::Left)
        {
            // retail: with an armed cursor, shift+click on one of the
            // selected units' queued order markers (or build ghosts)
            // removes that order instead of appending a new one
            if (isShiftDown() && isCursorOverWorld() && tryRemoveQueuedOrderAtCursor())
            {
                // a consumed removal click must not leave a latent
                // build-drag anchor for the later mouse-up
                buildDragState = std::nullopt;
                buildDragCandidates.clear();
                return;
            }

            match(
                cursorMode.getValue(),
                [&](const AttackCursorMode&) {
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (hoveredUnit)
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                        else
                        {
                            auto coord = getMouseTerrainCoordinate();
                            if (coord)
                            {
                                if (isShiftDown())
                                {
                                    localPlayerEnqueueUnitOrder(selectedUnit, AttackOrder(*coord));
                                }
                                else
                                {
                                    localPlayerIssueUnitOrder(selectedUnit, AttackOrder(*coord));
                                    cursorMode.next(NormalCursorMode());
                                }
                            }
                        }
                    }
                },
                [&](const MoveCursorMode&) {
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        auto coord = getMouseTerrainCoordinate();
                        if (coord)
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, MoveOrder(*coord));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, MoveOrder(*coord));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const GuardCursorMode&) {
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (hoveredUnit && isFriendly(*hoveredUnit))
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, GuardOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, GuardOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const RepairCursorMode&) {
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (hoveredUnit && isFriendly(*hoveredUnit))
                        {
                            if (const auto& u = getUnit(*hoveredUnit); u.isBeingBuilt(simulation.unitDefinitions.at(u.unitType)))
                            {
                                // a unit still under construction gets construction assistance
                                if (isShiftDown())
                                {
                                    localPlayerEnqueueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                }
                                else
                                {
                                    localPlayerIssueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                    cursorMode.next(NormalCursorMode());
                                }
                            }
                            else
                            {
                                if (isShiftDown())
                                {
                                    localPlayerEnqueueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                }
                                else
                                {
                                    localPlayerIssueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                    cursorMode.next(NormalCursorMode());
                                }
                            }
                        }
                    }
                },
                [&](const ReclaimCursorMode&) {
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (hoveredUnit && *hoveredUnit != selectedUnit && unitIsReclaimableTarget(simulation, *hoveredUnit))
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                        else if (hoveredFeature && featureIsReclaimableTarget(simulation, *hoveredFeature))
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const PatrolCursorMode&) {
                    if (auto coord = getMouseTerrainCoordinate(); coord)
                    {
                        for (const auto& selectedUnit : selectedUnits)
                        {
                            auto unit = tryGetUnit(selectedUnit);
                            if (!unit || !simulation.unitDefinitions.at(unit->get().unitType).canPatrol)
                            {
                                continue;
                            }

                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, PatrolOrder({*coord}));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, PatrolOrder({*coord}));
                            }
                        }

                        if (!isShiftDown())
                        {
                            cursorMode.next(NormalCursorMode());
                        }
                    }
                },
                [&](const CommandFireCursorMode&) {
                    // retail issues ATTACKSPECIAL per selected `candgun`
                    // unit; the per-shot resource check is cursor-level
                    // only and does not gate the click
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (!unitCanCommandFire(simulation, selectedUnit))
                        {
                            continue;
                        }

                        if (hoveredUnit)
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, CommandFireOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, CommandFireOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                        else
                        {
                            auto coord = getMouseTerrainCoordinate();
                            if (coord)
                            {
                                if (isShiftDown())
                                {
                                    localPlayerEnqueueUnitOrder(selectedUnit, CommandFireOrder(*coord));
                                }
                                else
                                {
                                    localPlayerIssueUnitOrder(selectedUnit, CommandFireOrder(*coord));
                                    cursorMode.next(NormalCursorMode());
                                }
                            }
                        }
                    }
                },
                [&](const CaptureCursorMode&) {
                    // retail issues the capture order per eligible
                    // (CanCapture) unit; ineligible units ignore the
                    // click (TotalA.exe 0x4042cf)
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        auto unit = tryGetUnit(selectedUnit);
                        if (unit && hoveredUnit && *hoveredUnit != selectedUnit
                            && unitCanCapture(simulation, selectedUnit)
                            && unitIsCapturableTarget(simulation, *hoveredUnit, unit->get().owner))
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, CaptureOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, CaptureOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const LoadCursorMode&) {
                    // retail mode 6: each selected CanLoad transport is
                    // ordered to pick up the clicked friendly unit;
                    // ineligible targets silently ignore the click
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        if (hoveredUnit && *hoveredUnit != selectedUnit
                            && unitCanLoad(simulation, selectedUnit)
                            && unitIsLoadableTarget(simulation, *hoveredUnit, selectedUnit))
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, LoadOrder(*hoveredUnit));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, LoadOrder(*hoveredUnit));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const UnloadCursorMode&) {
                    // retail mode 5: each selected transport carrying
                    // cargo gets the shared Ground_Unload/VTOL_Unload
                    // order for the clicked destination
                    auto coord = getMouseTerrainCoordinate();
                    if (!coord)
                    {
                        return;
                    }
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        auto unit = tryGetUnit(selectedUnit);
                        if (unit
                            && unitCanLoad(simulation, selectedUnit)
                            && !unit->get().passengers.empty())
                        {
                            if (isShiftDown())
                            {
                                localPlayerEnqueueUnitOrder(selectedUnit, UnloadOrder(*coord));
                            }
                            else
                            {
                                localPlayerIssueUnitOrder(selectedUnit, UnloadOrder(*coord));
                                cursorMode.next(NormalCursorMode());
                            }
                        }
                    }
                },
                [&](const BuildCursorMode& buildCursor) {
                    // linear-build QoL: placement is deferred to
                    // mouse-up so a drag can become a line of sites;
                    // a same-cell click still places exactly one
                    if (getSingleSelectedUnit() && hoverBuildInfo)
                    {
                        buildDragState = BuildDragState{buildCursor.unitType, hoverBuildInfo->rect};
                        buildDragCandidates.clear();
                    }
                },
                [&](const NormalCursorMode&) {
                    if (isCursorOverMinimap())
                    {
                        if (leftClickMode())
                        {
                            for (const auto& selectedUnit : selectedUnits)
                            {
                                if (hoveredUnit)
                                {
                                    if (isEnemy(*hoveredUnit))
                                    {
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                        }
                                    }
                                    else
                                    {
                                        if (const auto& u = getUnit(*hoveredUnit); u.isBeingBuilt(simulation.unitDefinitions.at(u.unitType)))
                                        {
                                            if (isShiftDown())
                                            {
                                                localPlayerEnqueueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                            }
                                            else
                                            {
                                                localPlayerIssueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    auto coord = getMouseTerrainCoordinate();
                                    if (coord)
                                    {
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, MoveOrder(*coord));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, MoveOrder(*coord));
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            cursorMode.next(NormalCursorMode{NormalCursorMode::DraggingMinimapState()});
                        }
                    }
                    else if (isCursorOverWorld())
                    {
                        Point p(event.x, event.y);
                        auto worldViewportPos = sceneContext.viewport->toOtherViewport(worldViewport, p);
                        const auto cameraPosition = worldCameraState.getRoundedPosition();
                        Point originRelativePos(cameraPosition.x + worldViewportPos.x, cameraPosition.z + worldViewportPos.y);
                        cursorMode.next(NormalCursorMode{NormalCursorMode::SelectingState(sceneTime, originRelativePos)});
                    }
                });
        }
        else if (event.button == MouseButtonEvent::MouseButton::Right)
        {
            match(
                cursorMode.getValue(),
                [&](const AttackCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const MoveCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const GuardCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const RepairCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const ReclaimCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const PatrolCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const CommandFireCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const CaptureCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const LoadCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const UnloadCursorMode&) {
                    cursorMode.next(NormalCursorMode());
                },
                [&](const BuildCursorMode&) {
                    buildDragState = std::nullopt;
                    buildDragCandidates.clear();
                    cursorMode.next(NormalCursorMode());
                },
                [&](const NormalCursorMode&) {
                    if (leftClickMode())
                    {
                        if (isCursorOverMinimap())
                        {
                            cursorMode.next(NormalCursorMode{NormalCursorMode::DraggingMinimapState()});
                        }
                        else if (isCursorOverWorld())
                        {
                            clearUnitSelection();
                        }
                    }
                    else
                    {
                        for (const auto& selectedUnit : selectedUnits)
                        {
                            if (hoveredUnit)
                            {
                                if (isEnemy(*hoveredUnit))
                                {
                                    // a builder without weapons reclaims enemy units contextually;
                                    // armed units (including commanders) keep attack precedence
                                    if (unitIsBuilder(simulation, selectedUnit)
                                        && !unitCanAttack(simulation, selectedUnit)
                                        && unitIsReclaimableTarget(simulation, *hoveredUnit))
                                    {
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                                        }
                                    }
                                    else if (isShiftDown())
                                    {
                                        localPlayerEnqueueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                    }
                                    else
                                    {
                                        localPlayerIssueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                    }
                                }
                                else
                                {
                                    const auto& u = getUnit(*hoveredUnit);
                                    const auto& ud = simulation.unitDefinitions.at(u.unitType);
                                    if (u.isBeingBuilt(ud))
                                    {
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                        }
                                    }
                                    else if (ud.isAirBase && simulation.unitDefinitions.at(getUnit(selectedUnit).unitType).canFly)
                                    {
                                        if (isShiftDown()) { localPlayerEnqueueUnitOrder(selectedUnit, LandOrder(*hoveredUnit)); }
                                        else { localPlayerIssueUnitOrder(selectedUnit, LandOrder(*hoveredUnit)); }
                                    }
                                    else if (unitCanLoad(simulation, selectedUnit)
                                        && unitIsLoadableTarget(simulation, *hoveredUnit, selectedUnit))
                                    {
                                        // retail preferred action: a
                                        // transport right-clicking a
                                        // loadable friendly unit loads it
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, LoadOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, LoadOrder(*hoveredUnit));
                                        }
                                    }
                                    else if (u.hitPoints < ud.maxHitPoints && unitIsBuilder(simulation, selectedUnit))
                                    {
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                        }
                                    }
                                    else if (unitCanGuard(simulation, selectedUnit))
                                    {
                                        // retail right-click on a
                                        // healthy friendly unit issues
                                        // Guard (the Green cursor
                                        // context); it previously fell
                                        // through to Move
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, GuardOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, GuardOrder(*hoveredUnit));
                                        }
                                    }
                                }
                            }
                            else if (hoveredFeature && unitIsBuilder(simulation, selectedUnit) && featureIsReclaimableTarget(simulation, *hoveredFeature))
                            {
                                if (isShiftDown())
                                {
                                    localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                                }
                                else
                                {
                                    localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                                }
                            }
                            else
                            {
                                auto coord = getMouseTerrainCoordinate();
                                if (coord)
                                {
                                    if (isShiftDown())
                                    {
                                        localPlayerEnqueueUnitOrder(selectedUnit, MoveOrder(*coord));
                                    }
                                    else
                                    {
                                        localPlayerIssueUnitOrder(selectedUnit, MoveOrder(*coord));
                                    }
                                }
                            }
                        }
                    }
                });
        }
        else if (event.button == MouseButtonEvent::MouseButton::Middle)
        {
            cameraControlState = CameraControlStateMiddleMousePan{getMousePosition()};
        }
    }

    void GameScene::onMouseUp(MouseButtonEvent event)
    {
        if (inGameMenuOpen())
        {
            auto p = toInGameMenuCoordinates(event.x, event.y);
            event.x = p.x;
            event.y = p.y;
            // release goes to the dialog that took the press so a
            // button armed in a companion pane resolves correctly
            if (inGameMenuMouseTarget != nullptr)
            {
                DialogDispatchScope dispatchScope(*this);
                inGameMenuMouseTarget->mouseUp(event);
                inGameMenuMouseTarget = nullptr;
            }
            return;
        }

        currentPanel->mouseUp(event);

        if (event.button == MouseButtonEvent::MouseButton::Left)
        {
            match(
                cursorMode.getValue(),
                [&](const NormalCursorMode& normalCursor) {
                    match(
                        normalCursor.state,
                        [&](const NormalCursorMode::SelectingState& state) {
                            Point p(event.x, event.y);
                            auto worldViewportPos = sceneContext.viewport->toOtherViewport(worldViewport, p);
                            const auto cameraPosition = worldCameraState.getRoundedPosition();
                            Point originRelativePos(cameraPosition.x + worldViewportPos.x, cameraPosition.z + worldViewportPos.y);

                            if (sceneTime - state.startTime < SceneTime(30) && state.startPosition.maxSingleDimensionDistance(originRelativePos) < 32)
                            {
                                // In left-click mode, a damaged friendly unit under a builder
                                // selection presents the Repair context -- the click repairs
                                // rather than selects.
                                bool repairContext = leftClickMode()
                                    && hoveredUnit
                                    && std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                                    && isFriendly(*hoveredUnit)
                                    && unitNeedsRepair(simulation, *hoveredUnit);

                                // In left-click mode, a reclaimable enemy unit under a
                                // weaponless-builder selection presents the Reclaim context.
                                // Armed selections keep the attack context, and friendly
                                // units keep selection/repair so reclaim never eats clicks.
                                bool reclaimUnitContext = !repairContext
                                    && leftClickMode()
                                    && hoveredUnit
                                    && isEnemy(*hoveredUnit)
                                    && unitIsReclaimableTarget(simulation, *hoveredUnit)
                                    && std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id) && !unitCanAttack(simulation, id); });

                                // In left-click mode, a reclaimable feature under a builder
                                // selection presents the Reclaim context -- the click reclaims
                                // the feature rather than issuing a move.
                                bool reclaimContext = !repairContext
                                    && !reclaimUnitContext
                                    && leftClickMode()
                                    && hoveredFeature
                                    && featureIsReclaimableTarget(simulation, *hoveredFeature)
                                    && std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); });

                                if (repairContext)
                                {
                                    for (const auto& selectedUnit : selectedUnits)
                                    {
                                        if (!unitIsBuilder(simulation, selectedUnit))
                                        {
                                            continue;
                                        }
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, RepairOrder(*hoveredUnit));
                                        }
                                    }
                                }
                                else if (reclaimUnitContext)
                                {
                                    for (const auto& selectedUnit : selectedUnits)
                                    {
                                        if (!unitIsBuilder(simulation, selectedUnit) || unitCanAttack(simulation, selectedUnit))
                                        {
                                            continue;
                                        }
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredUnit));
                                        }
                                    }
                                }
                                else if (reclaimContext)
                                {
                                    for (const auto& selectedUnit : selectedUnits)
                                    {
                                        if (!unitIsBuilder(simulation, selectedUnit))
                                        {
                                            continue;
                                        }
                                        if (isShiftDown())
                                        {
                                            localPlayerEnqueueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                                        }
                                        else
                                        {
                                            localPlayerIssueUnitOrder(selectedUnit, ReclaimOrder(*hoveredFeature));
                                        }
                                    }
                                }
                                else if (hoveredUnit && getUnit(*hoveredUnit).isSelectableBy(simulation.unitDefinitions.at(getUnit(*hoveredUnit).unitType), localPlayerId))
                                {
                                    if (isShiftDown())
                                    {
                                        toggleUnitSelection(*hoveredUnit);
                                    }
                                    else
                                    {
                                        replaceUnitSelection(*hoveredUnit);
                                    }
                                }
                                else if (leftClickMode() && hoveredUnit)
                                {
                                    if (isEnemy(*hoveredUnit))
                                    {
                                        for (const auto& selectedUnit : selectedUnits)
                                        {
                                            if (isShiftDown())
                                            {
                                                localPlayerEnqueueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                            }
                                            else
                                            {
                                                localPlayerIssueUnitOrder(selectedUnit, AttackOrder(*hoveredUnit));
                                            }
                                        }
                                    }
                                    else
                                    {
                                        if (const auto& u = getUnit(*hoveredUnit); u.isBeingBuilt(simulation.unitDefinitions.at(u.unitType)))
                                        {
                                            for (const auto& selectedUnit : selectedUnits)
                                            {
                                                if (isShiftDown())
                                                {
                                                    localPlayerEnqueueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                                }
                                                else
                                                {
                                                    localPlayerIssueUnitOrder(selectedUnit, CompleteBuildOrder(*hoveredUnit));
                                                }
                                            }
                                        }
                                    }
                                }
                                else if (leftClickMode())
                                {
                                    auto coord = getMouseTerrainCoordinate();
                                    if (coord)
                                    {
                                        for (const auto& selectedUnit : selectedUnits)
                                        {
                                            if (isShiftDown())
                                            {
                                                localPlayerEnqueueUnitOrder(selectedUnit, MoveOrder(*coord));
                                            }
                                            else
                                            {
                                                localPlayerIssueUnitOrder(selectedUnit, MoveOrder(*coord));
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    if (!isShiftDown())
                                    {
                                        clearUnitSelection();
                                    }
                                }
                            }
                            else
                            {
                                selectUnitsInBandbox(DiscreteRect::fromPoints(state.startPosition, originRelativePos));
                            }

                            cursorMode.next(NormalCursorMode{NormalCursorMode::UpState()});
                        },
                        [&](const NormalCursorMode::DraggingMinimapState&) {
                            cursorMode.next(NormalCursorMode{NormalCursorMode::UpState()});
                        },
                        [&](const NormalCursorMode::UpState&) {
                        });
                },
                [&](const BuildCursorMode& buildCursor) {
                    // linear-build QoL: placement deferred from mouse-down
                    completeBuildPlacement(buildCursor.unitType);
                },
                [&](const auto&) {
                    // do nothing
                });
        }
        else if (event.button == MouseButtonEvent::MouseButton::Right)
        {
            match(
                cursorMode.getValue(),
                [&](const NormalCursorMode& m) {
                    match(
                        m.state,
                        [&](const NormalCursorMode::DraggingMinimapState&) {
                            cursorMode.next(NormalCursorMode{NormalCursorMode::UpState()});
                        },
                        [](const auto&) {});
                },
                [](const auto&) {});
        }
        else if (event.button == MouseButtonEvent::MouseButton::Middle)
        {
            if (std::holds_alternative<CameraControlStateMiddleMousePan>(cameraControlState))
            {
                cameraControlState = CameraControlStateFree();
            }
        }
    }

    Rectangle2f computeCameraConstraint(const MapTerrain& terrain, float viewportWidth, float viewportHeight)
    {
        auto cameraHalfWidth = viewportWidth / 2.0f;
        auto cameraHalfHeight = viewportHeight / 2.0f;

        auto top = simScalarToFloat(terrain.topInWorldUnits()) + cameraHalfHeight;
        auto left = simScalarToFloat(terrain.leftInWorldUnits()) + cameraHalfWidth;
        auto bottom = simScalarToFloat(terrain.bottomCutoffInWorldUnits()) - cameraHalfHeight;
        auto right = simScalarToFloat(terrain.rightCutoffInWorldUnits()) - cameraHalfWidth;

        if (left > right)
        {
            auto middle = (left + right) / 2.0f;
            left = middle;
            right = middle;
        }

        if (top > bottom)
        {
            auto middle = (top + bottom) / 2.0f;
            top = middle;
            bottom = middle;
        }

        return Rectangle2f::fromTLBR(top, left, bottom, right);
    }

    void GameScene::onMouseMove(MouseMoveEvent event)
    {
        if (inGameMenuOpen())
        {
            sceneContext.cursor->useCursor(CursorType::Normal);
            auto p = toInGameMenuCoordinates(event.x, event.y);
            event.x = p.x;
            event.y = p.y;
            if (auto target = inGameMenuPointerTarget(event.x, event.y))
            {
                DialogDispatchScope dispatchScope(*this);
                target->mouseMove(event);
            }
            return;
        }

        if (auto middleMousePanningState = std::get_if<CameraControlStateMiddleMousePan>(&cameraControlState); middleMousePanningState)
        {
            auto cameraConstraint = computeCameraConstraint(simulation.terrain, worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()));

            const auto& cameraPos = worldCameraState.position;

            auto currentCursorPosition = Point(event.x, event.y);
            auto delta = currentCursorPosition - middleMousePanningState->previousCursorPosition;

            auto newCameraPos = cameraConstraint.clamp(Vector2f(cameraPos.x - delta.x, cameraPos.z - delta.y));
            worldCameraState.position = Vector3f(newCameraPos.x, cameraPos.y, newCameraPos.y);

            middleMousePanningState->previousCursorPosition = currentCursorPosition;
        }
        currentPanel->mouseMove(event);
    }

    void GameScene::onMouseWheel(MouseWheelEvent event)
    {
        if (inGameMenuOpen())
        {
            DialogDispatchScope dispatchScope(*this);
            inGameMenuDialogs.back()->mouseWheel(event);
            return;
        }

        currentPanel->mouseWheel(event);
    }

    GameScene::~GameScene()
    {
        // channelFinished is app-lifetime while this callback captures
        // `this`; dropping the unique_ptr alone leaks the subscription
        // and the audio dispatch would invoke a dead GameScene.
        audioSub->unsubscribe();
    }

    void GameScene::update(int millisecondsElapsed)
    {
        ScopedFrameTimer perfTimer(perfEnabled ? &perfUpdateNs : nullptr);

        const auto networkStatus = gameNetworkService->getStatus();
        if (networkStatus == GameNetworkService::Status::Failed || networkStatus == GameNetworkService::Status::Stopped)
        {
            throw std::runtime_error("Game network service stopped: " + gameNetworkService->getFailureReason());
        }

        updateSceneBench();

        sceneContext.audioService->dispatchFinishedChannels();

        tickAccumulator.addElapsed(millisecondsElapsed, gameSpeedLevel);

        // soak auto-exit (RWE_SOAK); requestExit is async-safe to call
        // from update -- SceneManager checks it at the top of the loop
        if (soakSeconds > 0)
        {
            if (!soakStarted)
            {
                soakStarted = true;
                soakStart = std::chrono::steady_clock::now();
            }
            else if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - soakStart).count() >= static_cast<long long>(soakSeconds))
            {
                LOG_INFO << "RWE_SOAK " << soakSeconds << "s elapsed, exiting";
                sceneContext.sceneManager->requestExit();
                soakSeconds = 0;
            }
        }

        if (inGameMenuOpen())
        {
            DialogDispatchScope dispatchScope(*this);
            inGameMenuDialogs.back()->update(static_cast<float>(millisecondsElapsed) / 1000.0f);
        }

        auto cameraConstraint = computeCameraConstraint(simulation.terrain, worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()));

        // update camera position from keyboard arrows
        {
            int directionX = (right ? 1 : 0) - (left ? 1 : 0);
            int directionZ = (down ? 1 : 0) - (up ? 1 : 0);

            if (directionX || directionZ)
            {
                nudgeCamera(millisecondsElapsed, cameraConstraint, directionX, directionZ);
            }
        }

        // update camera position from edge scroll
        // (suppressed while the in-game menu is open; the cursor may be
        // parked at a screen edge over a menu dialog)
        if (!inGameMenuOpen())
        {
            auto mousePosition = getMousePosition();
            auto directionX = mousePosition.x == sceneContext.viewport->left()
                ? -1
                : mousePosition.x == sceneContext.viewport->right() - 1
                ? 1
                : 0;
            auto directionZ = mousePosition.y == sceneContext.viewport->top()
                ? -1
                : mousePosition.y == sceneContext.viewport->bottom() - 1
                ? 1
                : 0;

            if (directionX || directionZ)
            {
                nudgeCamera(millisecondsElapsed, cameraConstraint, directionX, directionZ);
            }
        }

        // handle minimap dragging
        if (auto cursor = std::get_if<NormalCursorMode>(&cursorMode.getValue()); cursor != nullptr)
        {
            if (std::holds_alternative<NormalCursorMode::DraggingMinimapState>(cursor->state))
            {
                // ok, the cursor is dragging the minimap.
                // work out where the cursor is on the minimap,
                // convert that to the world, then set the camera's position to there
                // (clamped to map bounds)

                auto minimapToWorld = minimapToWorldMatrix(simulation.terrain, minimapRect);
                auto mousePos = getMousePosition();
                auto worldPos = minimapToWorld * Vector3f(static_cast<float>(mousePos.x) + 0.5f, static_cast<float>(mousePos.y) + 0.5, 0.0f);

                relocateCamera(cameraConstraint, worldPos.x, worldPos.z);
            }
        }

        // handle tracking
        {
            // TODO (kwh) - tracking of projectiles not yet implemented. E.g. while tracking Bertha or Nuke Silo,
            // screen should follow a projectile until it hits, then return to the tracking group

            if (std::holds_alternative<CameraControlStateTrackingUnit>(cameraControlState) && trackedUnitId)
            {
                // get tracked unit position, or stop tracking if it's gone
                auto unit = tryGetUnit(*trackedUnitId);
                if (unit && !unit->get().isDead())
                {
                    // Move camera... OTA behavior:
                    // For each x and z component (not euclidean distance), halve the distance from camera to unit each frame,
                    //  but limit to a max of 320 pixels per frame, at 30fps for normal speed (Scroll speed followed game speed, eg +10 scrolls faster).
                    // Presumably 320 to make scrolling look continuous on the lowest res setting of 640x480

                    // We will use millisecondsElapsed to interpolate for smoother scrolling at high fps in rwe,
                    // while maintaining similar scroll speed on the map; Speed is linear and clamped at 320 pixels/(1/30)s = 9600 pix/s,
                    const float maxScroll = 9.6f * millisecondsElapsed;
                    // To interpolate halving distance every 1/30s, we'll use the definition of geometric progression: a_n = a*r^(n); where:
                    //  a_n = distance (pixels) from the unit we should be after n 1/30s frames, a = current distance from the unit
                    //  r = common ratio i.e. 1/2, the ratio the distance should decrease every 1/30 seconds
                    //  n = # of OTA frames = seconds elapsed / (1/30 s per OTA frame) = (time_ms / 1000) * 30 = 3 * time_ms / 100

                    const auto& cameraPos = worldCameraState.position;
                    const auto unitPos = simVectorToFloat(unit->get().position);
                    const auto cameraPosDelta = unitPos - cameraPos;

                    float decayFactor = 1.0f - std::pow(.5f, .03f * millisecondsElapsed);

                    float newDelta_x = cameraPosDelta.x * decayFactor;
                    if (std::abs(newDelta_x) > maxScroll)
                    {
                        newDelta_x = newDelta_x < 0 ? -maxScroll : maxScroll;
                    }

                    float newDelta_z = cameraPosDelta.z * decayFactor;
                    if (std::abs(newDelta_z) > maxScroll)
                    {
                        newDelta_z = newDelta_z < 0 ? -maxScroll : maxScroll;
                    }

                    auto newPos = cameraConstraint.clamp(Vector2f(newDelta_x + cameraPos.x, newDelta_z + cameraPos.z));
                    worldCameraState.position = Vector3f(newPos.x, worldCameraState.position.y, newPos.y);
                }
            }
        }

        // reset cursor mode if shift is released and at least 1 order was queued since shift was held
        if (commandWasQueued && !isShiftDown())
        {
            cursorMode.next(NormalCursorMode());
            commandWasQueued = false;
        }

        hoveredUnit = getUnitUnderCursor();
        hoveredFeature = getFeatureUnderCursor();

        if (auto buildCursor = std::get_if<BuildCursorMode>(&cursorMode.getValue()); buildCursor != nullptr)
        {
            if (isCursorOverWorld())
            {
                auto ray = screenToWorldRayUtil(computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height()), screenToWorldClipSpace(getMousePosition()));
                auto intersect = simulation.intersectLineWithTerrain(floatToSimLine(ray.toLine()));

                if (intersect)
                {
                    const auto& unitType = buildCursor->unitType;
                    const auto& pos = *intersect;
                    const auto& unitDefinition = simulation.unitDefinitions.at(unitType);
                    auto mc = simulation.getAdHocMovementClass(unitDefinition.movementCollisionInfo);
                    auto footprintRect = simulation.computeFootprintRegion(pos, unitDefinition.movementCollisionInfo);
                    auto placement = simulation.computeBuildingPlacement(mc, unitDefinition.yardMap, unitDefinition.yardMapContainsGeo, unitDefinition.waterline, footprintRect.x, footprintRect.y);
                    hoverBuildInfo = HoverBuildInfo{footprintRect, placement && placement->valid, placement ? placement->baseY : pos.y};

                    // linear-build QoL: while a drag anchor is live,
                    // evaluate every line cell for preview and release
                    if (buildDragState && buildDragState->unitType == unitType)
                    {
                        Point cursorCell(footprintRect.x, footprintRect.y);
                        if (cursorCell != Point(buildDragState->anchorRect.x, buildDragState->anchorRect.y))
                        {
                            buildDragState->engaged = true;
                        }
                        buildDragCandidates.clear();
                        for (const auto& cell : linearBuildCells(Point(buildDragState->anchorRect.x, buildDragState->anchorRect.y), cursorCell, footprintRect.width, footprintRect.height))
                        {
                            auto candidatePlacement = simulation.computeBuildingPlacement(mc, unitDefinition.yardMap, unitDefinition.yardMapContainsGeo, unitDefinition.waterline, cell.x, cell.y);
                            auto candidateCorner = simulation.terrain.heightmapIndexToWorldCorner(cell.x, cell.y);
                            auto candidateCenterX = candidateCorner.x + ((SimScalar(footprintRect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss);
                            auto candidateCenterZ = candidateCorner.z + ((SimScalar(footprintRect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss);
                            auto candidateBaseY = (candidatePlacement && candidatePlacement->valid)
                                ? candidatePlacement->baseY
                                : simulation.terrain.getHeightAt(candidateCenterX, candidateCenterZ);
                            buildDragCandidates.push_back(BuildDragCandidate{
                                DiscreteRect(cell.x, cell.y, footprintRect.width, footprintRect.height),
                                candidatePlacement && candidatePlacement->valid,
                                candidateBaseY});
                        }
                    }
                    else
                    {
                        // armed unit type changed mid-drag -> drop the anchor
                        buildDragState = std::nullopt;
                        buildDragCandidates.clear();
                    }
                }
                else
                {
                    hoverBuildInfo = std::nullopt;
                    buildDragCandidates.clear();
                }
            }
            else
            {
                hoverBuildInfo = std::nullopt;
                buildDragCandidates.clear();
            }
        }
        else
        {
            hoverBuildInfo = std::nullopt;
            buildDragState = std::nullopt;
            buildDragCandidates.clear();
        }

        if (!isCursorOverMinimap() && !isCursorOverWorld())
        {
            // The cursor is outside the world, so over UI elements.
            sceneContext.cursor->useCursor(CursorType::Normal);
        }
        else
        {
            match(
                cursorMode.getValue(),
                [&](const AttackCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Attack);
                },
                [&](const MoveCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Move);
                },
                [&](const GuardCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Guard);
                },
                [&](const RepairCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Repair);
                },
                [&](const ReclaimCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Reclaim);
                },
                [&](const PatrolCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Patrol);
                },
                [&](const CommandFireCursorMode&) {
                    // retail picks the cursor from the minimum
                    // eligibility result across the selection:
                    // airstrike when a unit can fire, capture when the
                    // eligible units lack the per-shot resources,
                    // hourglass when nothing can fire
                    auto eligibility = CommandFireEligibility::Ineligible;
                    for (const auto& selectedUnit : selectedUnits)
                    {
                        auto e = unitCommandFireEligibility(simulation, selectedUnit);
                        if (e == CommandFireEligibility::Ready)
                        {
                            eligibility = e;
                            break;
                        }
                        if (e == CommandFireEligibility::InsufficientResources)
                        {
                            eligibility = e;
                        }
                    }

                    switch (eligibility)
                    {
                        case CommandFireEligibility::Ready:
                            sceneContext.cursor->useCursor(CursorType::CommandFire);
                            break;
                        case CommandFireEligibility::InsufficientResources:
                            sceneContext.cursor->useCursor(CursorType::Capture);
                            break;
                        default:
                            sceneContext.cursor->useCursor(CursorType::Hourglass);
                            break;
                    }
                },
                [&](const CaptureCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Capture);
                },
                [&](const LoadCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Load);
                },
                [&](const UnloadCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Unload);
                },
                [&](const BuildCursorMode&) {
                    sceneContext.cursor->useCursor(CursorType::Normal);
                },
                [&](const NormalCursorMode&) {
                    if (leftClickMode())
                    {
                        if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredUnit && isFriendly(*hoveredUnit) && unitNeedsRepair(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Repair);
                        }
                        else if (hoveredUnit && unitIsSelectableBy(simulation, *hoveredUnit, localPlayerId))
                        {
                            sceneContext.cursor->useCursor(CursorType::Select);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanAttack(simulation, id); })
                            && hoveredUnit && isEnemy(*hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Attack);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id) && !unitCanAttack(simulation, id); })
                            && hoveredUnit && isEnemy(*hoveredUnit) && unitIsReclaimableTarget(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Reclaim);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredUnit && unitIsBeingBuilt(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Repair);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredFeature && featureIsReclaimableTarget(simulation, *hoveredFeature))
                        {
                            sceneContext.cursor->useCursor(CursorType::Reclaim);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanLoad(simulation, id); })
                            && hoveredUnit && std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanLoad(simulation, id) && unitIsLoadableTarget(simulation, *hoveredUnit, id); }))
                        {
                            sceneContext.cursor->useCursor(CursorType::Load);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanMove(simulation, id); }))
                        {
                            sceneContext.cursor->useCursor(CursorType::Move);
                        }
                        else
                        {
                            sceneContext.cursor->useCursor(CursorType::Normal);
                        }
                    }
                    else
                    {
                        if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredUnit && isFriendly(*hoveredUnit) && unitNeedsRepair(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Repair);
                        }
                        else if (hoveredUnit && unitIsSelectableBy(simulation, *hoveredUnit, localPlayerId))
                        {
                            sceneContext.cursor->useCursor(CursorType::Select);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanAttack(simulation, id); })
                            && hoveredUnit && isEnemy(*hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Red);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id) && !unitCanAttack(simulation, id); })
                            && hoveredUnit && isEnemy(*hoveredUnit) && unitIsReclaimableTarget(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Reclaim);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredUnit && isFriendly(*hoveredUnit) && unitIsBeingBuilt(simulation, *hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Green);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanGuard(simulation, id); })
                            && hoveredUnit && isFriendly(*hoveredUnit))
                        {
                            sceneContext.cursor->useCursor(CursorType::Green);
                        }
                        else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitIsBuilder(simulation, id); })
                            && hoveredFeature && featureIsReclaimableTarget(simulation, *hoveredFeature))
                        {
                            sceneContext.cursor->useCursor(CursorType::Reclaim);
                        }
                        else
                        {
                            sceneContext.cursor->useCursor(CursorType::Normal);
                        }
                    }
                });
        }

        auto targetCommandBufferSize = commandBufferTarget(gameNetworkService->getMaxAverageRttMillis(), gameSpeedLevel);

        auto bufferedCommandCount = playerCommandService->bufferedCommandCount(localPlayerId);

        // per-frame "Buffer levels" diagnostic logging removed: it
        // generated a formatted write + disk flush on the main thread
        // every rendered frame (~200+/sec)

        // If we have too many commands buffered,
        // defer submitting commands this frame
        // so that we drop back down to the threshold.
        if (bufferedCommandCount <= targetCommandBufferSize)
        {
            // Queue up commands collected from the local player
            playerCommandService->pushCommands(localPlayerId, localPlayerCommandBuffer);
            gameNetworkService->submitCommands(sceneTime, localPlayerCommandBuffer);
            localPlayerCommandBuffer.clear();
            ++bufferedCommandCount;
        }

        // fill up to the required threshold
        for (; bufferedCommandCount < targetCommandBufferSize; ++bufferedCommandCount)
        {
            playerCommandService->pushCommands(localPlayerId, std::vector<PlayerCommand>());
            gameNetworkService->submitCommands(sceneTime, std::vector<PlayerCommand>());
        }

        // If we are waiting to swap in a new unit GUI panel, do that now
        if (nextPanel)
        {
            currentPanel = std::move(*nextPanel);
            nextPanel = std::nullopt;
            attachOrdersMenuEventHandlers();
        }

        // With one human there is no remote clock to follow. Applying drift
        // correction to our own pre-update time would cap a long catch-up
        // frame at the next five-tick check boundary.
        const auto synchroniseSceneTime = !isSingleHumanPlayer();
        auto averageSceneTime = gameNetworkService->estimateAvergeSceneTime(sceneTime);

        // allow skipping sim frames every so often to get back down to average.
        // We tolerate X frames of drift in either direction to cope with noisiness in the estimation.
        const SceneTime frameTolerance(3);
        const SceneTime frameCheckInterval(5);
        auto highSceneTime = averageSceneTime + frameTolerance;
        auto lowSceneTime = averageSceneTime <= frameTolerance ? SceneTime{0} : averageSceneTime - frameTolerance;
        if (perfEnabled)
        {
            perfAccMax = std::max(perfAccMax, tickAccumulator.pendingMilliseconds());
        }
        // Preserve the lockstep pacing policy: an unavailable remote frame
        // consumes this wall-clock slot, rather than banking an unbounded
        // catch-up backlog during a network stall. No simulation tick or
        // command frame is consumed until every player has supplied one.
        while (tickAccumulator.consumeTick())
        {
            if (simulationTransferredToDebrief)
            {
                break;
            }

            if (sceneTickMayAdvance(sceneTime, highSceneTime, synchroniseSceneTime))
            {
                auto before = sceneTime;
                tryTickGame();
                if (perfEnabled)
                {
                    if (sceneTime > before)
                    {
                        ++perfTicksExec;
                    }
                    else
                    {
                        ++perfTicksBlocked;
                    }
                }

                // simulate an extra frame to catch up every so often
                if (synchroniseSceneTime && sceneTime % frameCheckInterval == SceneTime(0) && sceneTime < lowSceneTime)
                {
                    auto before2 = sceneTime;
                    tryTickGame();
                    if (perfEnabled)
                    {
                        if (sceneTime > before2)
                        {
                            ++perfTicksExec;
                        }
                        else
                        {
                            ++perfTicksBlocked;
                        }
                    }
                }
            }
        }

        // stockpile counts change through sim ticks (round built or
        // fired), not UI events; poll the label so it stays current
        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
        {
            refreshWeaponGuiTotal(*selectedUnit);
        }

        renderDebugWindow();
    }

    std::optional<UnitId> GameScene::spawnUnit(const std::string& unitType, PlayerId owner, const SimVector& position, std::optional<const std::reference_wrapper<SimAngle>> rotation, bool allowOverlap)
    {
        // the initial-placement path (skirmish commander drop, debug
        // spawn command) bypasses the maxunits cap like retail's
        // direct slot-range population at game start
        return simulation.trySpawnUnit(unitType, owner, position, rotation, allowOverlap, std::nullopt, true);
    }

    std::optional<std::reference_wrapper<UnitState>> GameScene::spawnCompletedUnit(const std::string& unitType, PlayerId owner, const SimVector& position, bool allowOverlap)
    {
        auto unitId = spawnUnit(unitType, owner, position, std::nullopt, allowOverlap);
        if (unitId)
        {
            auto& unit = getUnit(*unitId);
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            // units start as unbuilt nanoframes,
            // we we need to convert it immediately into a completed unit.
            unit.finishBuilding(unitDefinition);

            return unit;
        }

        return std::nullopt;
    }

    void GameScene::setCameraPosition(const Vector3f& newPosition)
    {
        auto cameraConstraint = computeCameraConstraint(simulation.terrain, worldCameraState.scaleDimension(worldViewport.width()), worldCameraState.scaleDimension(worldViewport.height()));
        auto constrainedPosition = cameraConstraint.clamp(Vector2f(newPosition.x, newPosition.z));
        worldCameraState.position = Vector3f(constrainedPosition.x, newPosition.y, constrainedPosition.y);
    }

    /**
     * Scripted render-bench timeline (RWE_SCENE_BENCH). Runs on
     * rendered-frame counts so phases hold a fixed number of frames
     * regardless of frame rate:
     *   f60   spawn `count` units around the camera anchor
     *   f120  phase A label: idle
     *   f420  select all spawned -> phase B: selected idle
     *   f720  group MOVE to far corner -> phase C: moving on-screen
     *   f1020 pan camera away -> phase D: moving off-screen
     *   f1380 summary + exit
     */
    void GameScene::updateSceneBench()
    {
        if (!sceneBench.active || simulationTransferredToDebrief)
        {
            return;
        }

        auto anchor = worldCameraState.position;
        // world coords are centred on the origin: x in [left,right],
        // z in [top,bottom] -- half the range is negative
        const auto minX = simScalarToFloat(simulation.terrain.leftInWorldUnits()) + 250.0f;
        const auto maxX = simScalarToFloat(simulation.terrain.rightCutoffInWorldUnits()) - 250.0f;
        const auto minZ = simScalarToFloat(simulation.terrain.topInWorldUnits()) + 250.0f;
        const auto maxZ = simScalarToFloat(simulation.terrain.bottomCutoffInWorldUnits()) - 250.0f;
        auto clampX = [&](float v) { return SimScalar(std::clamp(v, minX, maxX)); };
        auto clampZ = [&](float v) { return SimScalar(std::clamp(v, minZ, maxZ)); };

        // phase transitions flush the perf window so each RWE_PERF_SCENE
        // line covers exactly one labelled phase
        auto setBenchPhase = [&](const char* label) {
            if (perfEnabled && !perfSamples.empty())
            {
                auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - perfWindowStart)
                                     .count();
                reportPerfWindow(static_cast<double>(elapsedNs) / 1e9);
            }
            perfWindowLabel = label;
            perfWindowStart = std::chrono::steady_clock::now();
        };

        auto tick = simulation.gameTime.value;

        // hold the camera on the spawn centroid during on-screen phases;
        // edge-scroll would otherwise drift it away from the cluster
        if (sceneBench.cameraFocus && sceneBench.step >= 1 && sceneBench.step <= 3)
        {
            setCameraPosition(*sceneBench.cameraFocus);
        }

        switch (sceneBench.step)
        {
            case 0:
                if (tick >= 30)
                {
                    int cols = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(sceneBench.count))));
                    const float spacing = 72.0f;
                    for (unsigned int i = 0; i < sceneBench.count; ++i)
                    {
                        const auto& spec = sceneBench.unitTypes.empty()
                            ? std::pair<std::string, bool>{sceneBench.unitType, false}
                            : sceneBench.unitTypes[i % sceneBench.unitTypes.size()];
                        SimScalar x = clampX(anchor.x + (static_cast<float>(i % cols) - cols / 2.0f) * spacing);
                        SimScalar z = clampZ(anchor.z + (static_cast<float>(i / cols) - cols / 2.0f) * spacing);
                        SimVector pos(x, simulation.terrain.getHeightAt(x, z), z);
                        if (auto id = spawnUnit(spec.first, localPlayerId, pos, std::nullopt, true))
                        {
                            auto& unit = getUnit(*id);
                            if (!spec.second)
                            {
                                unit.finishBuilding(simulation.unitDefinitions.at(unit.unitType));
                            }
                            sceneBench.spawned.push_back(*id);
                        }
                    }
                    for (const auto& spec : sceneBench.unitTypes)
                    {
                        if (!isValidUnitType(simulation, spec.first))
                        {
                            LOG_WARN << "RWE_SCENE_BENCH: unknown unit type '" << spec.first << "'";
                        }
                    }
                    // spawnUnit snaps to valid placement, which can leave the
                    // cluster away from the initial camera anchor -- recenter
                    // on the spawn centroid so phases A-C render the units
                    if (!sceneBench.spawned.empty())
                    {
                        SimScalar cx(0), cz(0);
                        for (auto id : sceneBench.spawned)
                        {
                            const auto& u = getUnit(id);
                            cx = cx + u.position.x;
                            cz = cz + u.position.z;
                        }
                        auto n = SimScalar(static_cast<float>(sceneBench.spawned.size()));
                        auto centerX = cx / n;
                        auto centerZ = cz / n;
                        const auto& u0 = getUnit(sceneBench.spawned.front());
                        sceneBench.cameraFocus = Vector3f(simScalarToFloat(centerX), worldCameraState.position.y, simScalarToFloat(centerZ));
                        setCameraPosition(*sceneBench.cameraFocus);
                        LOG_INFO << "RWE_SCENE_BENCH centroid=(" << simScalarToFloat(centerX) << "," << simScalarToFloat(centerZ)
                                 << ") u0=(" << simScalarToFloat(u0.position.x) << "," << simScalarToFloat(u0.position.z)
                                 << ") cam=(" << worldCameraState.position.x << "," << worldCameraState.position.z << ")";
                    }
                    LOG_INFO << "RWE_SCENE_BENCH spawned=" << sceneBench.spawned.size() << "/" << sceneBench.count << " type=" << sceneBench.unitType;
                    setBenchPhase("A-idle");
                    sceneBench.step = 1;
                }
                break;
            case 1:
                if (tick >= 330)
                {
                    for (auto id : sceneBench.spawned)
                    {
                        if (auto u = tryGetUnit(id); u && !u->get().isDead())
                        {
                            selectedUnits.insert(id);
                        }
                    }
                    LOG_INFO << "RWE_SCENE_BENCH selected=" << selectedUnits.size();
                    setBenchPhase("B-selected");
                    sceneBench.step = 2;
                }
                break;
            case 2:
                if (tick >= 630)
                {
                    SimVector dest(clampX(minX + 0.80f * (maxX - minX)), SimScalar(0), clampZ(minZ + 0.80f * (maxZ - minZ)));
                    dest.y = simulation.terrain.getHeightAt(dest.x, dest.z);
                    for (auto id : sceneBench.spawned)
                    {
                        if (auto u = tryGetUnit(id); u && !u->get().isDead())
                        {
                            localPlayerIssueUnitOrder(id, MoveOrder(dest));
                        }
                    }
                    LOG_INFO << "RWE_SCENE_BENCH move-issued count=" << sceneBench.spawned.size();
                    setBenchPhase("C-move-onscreen");
                    sceneBench.step = 3;
                }
                break;
            case 3:
                if (tick >= 930)
                {
                    setCameraPosition(Vector3f(minX + 0.15f * (maxX - minX), worldCameraState.position.y, minZ + 0.15f * (maxZ - minZ)));
                    LOG_INFO << "RWE_SCENE_BENCH camera-away";
                    setBenchPhase("D-move-offscreen");
                    sceneBench.step = 4;
                }
                break;
            case 4:
                if (tick >= 1230)
                {
                    LOG_INFO << "RWE_SCENE_BENCH done frames=" << sceneBenchFrame << " tick=" << tick;
                    sceneBench.step = 5;
                    sceneContext.sceneManager->requestExit();
                }
                break;
            default:
                break;
        }
    }

    const MapTerrain& GameScene::getTerrain() const
    {
        return simulation.terrain;
    }

    GameTime
    GameScene::getGameTime() const
    {
        return simulation.gameTime;
    }

    void GameScene::playUiSound(const AudioService::SoundHandle& handle)
    {
        sceneContext.audioService->playSoundIfFree(handle, UnitSelectChannel);
    }

    void GameScene::playNotificationSound(const PlayerId& playerId, const AudioService::SoundHandle& sound)
    {
        if (playerId == localPlayerId)
        {
            sceneContext.audioService->playSoundIfFree(sound, UnitSelectChannel);
        }
    }


    std::optional<std::string> getSoundName(const SoundClass& c, UnitSoundType sound)
    {
        switch (sound)
        {
            case UnitSoundType::Select1:
                return c.select1;
            case UnitSoundType::UnitComplete:
                return c.unitComplete;
            case UnitSoundType::Activate:
                return c.activate;
            case UnitSoundType::Deactivate:
                return c.deactivate;
            case UnitSoundType::Ok1:
                return c.ok1;
            case UnitSoundType::Arrived1:
                return c.arrived1;
            case UnitSoundType::Cant1:
                return c.cant1;
            case UnitSoundType::UnderAttack:
                return c.underAttack;
            case UnitSoundType::Build:
                return c.build;
            case UnitSoundType::Repair:
                return c.repair;
            case UnitSoundType::Working:
                return c.working;
            case UnitSoundType::Cloak:
                return c.cloak;
            case UnitSoundType::Uncloak:
                return c.uncloak;
            case UnitSoundType::Capture:
                return c.capture;
            case UnitSoundType::Count5:
                return c.count5;
            case UnitSoundType::Count4:
                return c.count4;
            case UnitSoundType::Count3:
                return c.count3;
            case UnitSoundType::Count2:
                return c.count2;
            case UnitSoundType::Count1:
                return c.count1;
            case UnitSoundType::Count0:
                return c.count0;
            case UnitSoundType::CancelDestruct:
                return c.cancelDestruct;
            default:
                throw std::logic_error("Invalid sound type");
        }
    }

    std::optional<AudioService::SoundHandle> getSound(const GameSimulation& sim, const GameMediaDatabase& meshDb, const std::string& unitType, UnitSoundType soundType)
    {
        const auto& unitDefinition = sim.unitDefinitions.at(unitType);
        const auto& soundClass = meshDb.getSoundClassOrDefault(unitDefinition.soundCategory);
        const auto& soundId = getSoundName(soundClass, soundType);
        if (soundId)
        {
            return meshDb.tryGetSoundHandle(*soundId);
        }
        return std::nullopt;
    }

    void GameScene::playUnitNotificationSound(const PlayerId& playerId, const std::string& unitType, UnitSoundType soundType)
    {
        auto sound = getSound(simulation, gameMediaDatabase, unitType, soundType);
        if (sound)
        {
            playNotificationSound(playerId, *sound);
        }
    }

    void GameScene::playSoundAt(const Vector3f& /*position*/, const AudioService::SoundHandle& sound)
    {
        // FIXME: should play on a position-aware channel
        auto channel = sceneContext.audioService->playSound(sound);
        int volume;
        {
            std::scoped_lock<std::mutex> lock(playingUnitChannelsLock);
            playingUnitChannels.insert(channel);
            volume = computeSoundVolume(playingUnitChannels.size());
        }
        sceneContext.audioService->setVolume(channel, volume);
    }

    void GameScene::playWeaponStartSound(const Vector3f& position, const std::string& weaponType)
    {

        const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(weaponType);
        if (weaponMediaInfo.soundStart)
        {
            auto sound = gameMediaDatabase.tryGetSoundHandle(*weaponMediaInfo.soundStart);
            if (sound)
            {
                playSoundAt(position, *sound);
            }
        }
    }

    void GameScene::playWeaponImpactSound(const Vector3f& position, const std::string& weaponType, ImpactType impactType)
    {
        const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(weaponType);
        switch (impactType)
        {
            case ImpactType::Normal:
            {
                if (weaponMediaInfo.soundHit)
                {
                    auto sound = gameMediaDatabase.tryGetSoundHandle(*weaponMediaInfo.soundHit);
                    if (sound)
                    {
                        playSoundAt(position, *sound);
                    }
                }
                break;
            }
            case ImpactType::Water:
            {
                if (weaponMediaInfo.soundWater)
                {
                    auto sound = gameMediaDatabase.tryGetSoundHandle(*weaponMediaInfo.soundWater);
                    if (sound)
                    {
                        playSoundAt(position, *sound);
                    }
                }
                break;
            }
        }
    }

    void GameScene::spawnWeaponImpactExplosion(const Vector3f& position, const std::string& weaponType, ImpactType impactType)
    {
        const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(weaponType);

        switch (impactType)
        {
            case ImpactType::Normal:
            {
                if (weaponMediaInfo.explosionAnim)
                {
                    spawnExplosion(position, *weaponMediaInfo.explosionAnim);
                }
                if (weaponMediaInfo.endSmoke)
                {
                    createLightSmoke(position);
                }
                break;
            }
            case ImpactType::Water:
            {
                if (weaponMediaInfo.waterExplosionAnim)
                {
                    spawnExplosion(position, *weaponMediaInfo.waterExplosionAnim);
                }
                break;
            }
        }

        spawnFlash(position);
    }

    void GameScene::onChannelFinished(int channel)
    {
        std::scoped_lock<std::mutex> lock(playingUnitChannelsLock);
        playingUnitChannels.erase(channel);
    }

    Matrix4f GameScene::worldToMinimapMatrix(const MapTerrain& terrain, const Rectangle2f& minimapRect)
    {
        auto view = Matrix4f::rotationToAxes(
            Vector3f(1.0f, 0.0f, 0.0f),
            Vector3f(0.0f, 0.0f, 1.0f),
            Vector3f(0.0f, -1.0f, 0.0f));
        auto cabinet = Matrix4f::cabinetProjection(0.0f, 0.5f);
        auto orthographic = Matrix4f::orthographicProjection(
            simScalarToFloat(terrain.leftInWorldUnits()),
            simScalarToFloat(terrain.rightCutoffInWorldUnits()),
            simScalarToFloat(terrain.bottomCutoffInWorldUnits()),
            simScalarToFloat(terrain.topInWorldUnits()),
            -1000.0f,
            1000.0f);
        auto worldProjection = orthographic * cabinet;
        auto minimapInverseProjection = Matrix4f::inverseOrthographicProjection(
            minimapRect.left(),
            minimapRect.right(),
            minimapRect.bottom(),
            minimapRect.top(),
            -1.0f,
            1.0f);
        return minimapInverseProjection * worldProjection * view;
    }

    Matrix4f GameScene::minimapToWorldMatrix(const MapTerrain& terrain, const Rectangle2f& minimapRect)
    {
        auto view = Matrix4f::rotationToAxes(
            Vector3f(1.0f, 0.0f, 0.0f),
            Vector3f(0.0f, 0.0f, 1.0f),
            Vector3f(0.0f, -1.0f, 0.0f));
        auto inverseView = view.transposed();
        auto inverseCabinet = Matrix4f::cabinetProjection(0.0f, -0.5f);
        auto inverseOrthographic = Matrix4f::inverseOrthographicProjection(
            simScalarToFloat(terrain.leftInWorldUnits()),
            simScalarToFloat(terrain.rightCutoffInWorldUnits()),
            simScalarToFloat(terrain.bottomCutoffInWorldUnits()),
            simScalarToFloat(terrain.topInWorldUnits()),
            -1000.0f,
            1000.0f);
        auto worldInverseProjection = inverseCabinet * inverseOrthographic;
        auto minimapProjection = Matrix4f::orthographicProjection(
            minimapRect.left(),
            minimapRect.right(),
            minimapRect.bottom(),
            minimapRect.top(),
            -1.0f,
            1.0f);
        return inverseView * worldInverseProjection * minimapProjection;
    }

    void GameScene::tryTickGame()
    {
        ScopedFrameTimer perfTimer(perfEnabled ? &perfTickNs : nullptr);

        if (simulationTransferredToDebrief)
        {
            return;
        }

        if (!playerCommandService->checkHashes())
        {
            std::ofstream dumpFile;
            dumpFile.open("rwe-dump-" + std::to_string(std::rand()) + ".json");
            dumpFile << dumpJson(simulation);
            dumpFile.close();
            throw std::runtime_error("Desync detected");
        }

        // Refill per attempted tick, rather than once per rendered update.
        // A long frame must not stop after a single computer frame (or after
        // the local human exhausts its prefilled latency horizon).
        if (ensureLocalCommandFrame(*playerCommandService, localPlayerId))
        {
            gameNetworkService->submitCommands(sceneTime, {});
        }
        for (Index i = 0; i < getSize(simulation.players); ++i)
        {
            if (simulation.players[i].type == GamePlayerType::Computer)
            {
                ensureLocalCommandFrame(*playerCommandService, PlayerId(i));
            }
        }

        auto playerCommands = playerCommandService->tryPopCommands();
        if (!playerCommands)
        {
            // Missing remote frames can still block lockstep. Rate-limit
            // diagnostics so a network stall cannot create a burst of disk
            // flushes inside the drain loop itself.
            static unsigned int suppressedBlockedLogs = 0;
            if (++suppressedBlockedLogs >= 500)
            {
                LOG_ERROR << "Blocked waiting for player commands (x" << suppressedBlockedLogs << ")";
                suppressedBlockedLogs = 0;
            }
            return;
        }

        sceneTime += SceneTime(1);

        if (!gamePaused)
        {
            processActions();

            // a queued delayed action may have handed the simulation
            // to the debrief scene (enterMissionDebrief); the moved-from
            // simulation must not tick again
            if (simulationTransferredToDebrief)
            {
                return;
            }
        }

        // commands keep flowing while paused so that an unpause command
        // (and, in network games, every peer's command stream) is still
        // consumed on schedule
        processPlayerCommands(*playerCommands);

        if (gamePaused)
        {
            auto gameHash = simulation.computeHash();
            playerCommandService->pushHash(localPlayerId, gameHash);
            gameNetworkService->submitGameHash(gameHash);
            return;
        }

        simulation.tick();
        if (perfEnabled)
        {
            ++perfTickCount;
        }

        auto gameHash = simulation.computeHash();
        playerCommandService->pushHash(localPlayerId, gameHash);
        gameNetworkService->submitGameHash(gameHash);

        if (stateLogStream)
        {
            *stateLogStream << dumpJson(simulation) << std::endl;
        }

        processSimEvents();

        updateProjectiles();

        updateFlashes();

        updateParticles(gameMediaDatabase, simulation.gameTime, particles);

        // retail emits the nanolathe spray once per work tick from
        // each actively-working unit (TotalA.exe effect channel 6)
        emitNanolatheParticles(simulation, simulation.gameTime, particles, nanolatheRng);

        if (simulation.missionInfo)
        {
            // campaign missions report via the objective evaluator
            // rather than skirmish last-player-standing
            auto action = missionEndAction(simulation.getMissionResult(), missionResultDeclared);
            if (action != MissionEndAction::None)
            {
                missionResultDeclared = true;
                missionResultWon = action == MissionEndAction::DeclareVictory;
                LOG_INFO << (missionResultWon
                        ? "Mission accomplished (victory objectives met)"
                        : "Mission failed (defeat condition met)");

                // In-game result banner (retail +0x3923b
                // bit5/bit6 -> igvictory/igdefeat overlay while
                // the simulation keeps ticking). The banner
                // then gives way to the ENDMSN debrief screen;
                // retail's interactive score interstitial and
                // the exact transition timing are approximated
                // by this fixed delay.
                if (auto series = sceneContext.textureService->tryGetGafEntry(
                        "anims/igtitles.gaf",
                        missionResultWon ? "igvictory" : "igdefeat"))
                {
                    if (!(*series)->sprites.empty())
                    {
                        missionResultSprite = (*series)->sprites.front();
                    }
                }
                delay(SceneTime(3 * 30), [this]() { enterMissionDebrief(); });
            }
        }
        else
        {
            auto winStatus = simulation.computeWinStatus();
            match(
                winStatus,
                [&](const WinStatusWon&) {
                    delay(SceneTime(5 * 30), [sm = sceneContext.sceneManager]() { sm->requestExit(); });
                },
                [&](const WinStatusDraw&) {
                    delay(SceneTime(5 * 30), [sm = sceneContext.sceneManager]() { sm->requestExit(); });
                },
                [&](const WinStatusUndecided&) {
                    // do nothing, game still in progress
                });
        }
    }

    void GameScene::drawMinimapFog()
    {
        if (!simulation.lineOfSightEnabled
            || localPlayerId.value >= simulation.playerVisibility.size())
        {
            return;
        }
        const auto& pv = simulation.playerVisibility[localPlayerId.value];
        const auto& losGrid = pv.losGrid;
        const auto& exploredGrid = pv.exploredGrid;
        auto width = losGrid.getWidth();
        auto height = losGrid.getHeight();
        if (width == 0 || height == 0)
        {
            return;
        }

        // coalesce horizontal runs into batched quads so the whole overlay
        // costs two uploads/draws instead of one per run
        std::vector<GlColoredVertex> unexploredVerts;
        std::vector<GlColoredVertex> exploredVerts;
        fillMinimapFogVertices(losGrid, exploredGrid, minimapRect, unexploredVerts, exploredVerts);
        chromeUiRenderService.fillColorBatch(unexploredVerts, 230);
        chromeUiRenderService.fillColorBatch(exploredVerts, 110);
    }

    std::optional<UnitId> GameScene::getUnitUnderCursor() const
    {
        if (isCursorOverMinimap())
        {
            auto mousePos = getMousePosition();

            auto worldToMinimap = worldToMinimapMatrix(simulation.terrain, minimapRect);

            for (const auto& [unitId, unit] : simulation.units)
            {
                if (unit.carriedBy)
                {
                    continue;
                }
                if (!simulation.isDetectedByPlayer(unitId, localPlayerId))
                {
                    continue;
                }
                // convert to minimap rect
                auto minimapPos = worldToMinimap * simVectorToFloat(unit.position);
                minimapPos.x = std::floor(minimapPos.x);
                minimapPos.y = std::floor(minimapPos.y);
                auto ownerId = unit.owner;
                auto colorIndex = getPlayer(ownerId).color;
                const auto& sprite = *minimapDots->sprites[colorIndex.value];
                auto bounds = sprite.bounds;

                // test cursor against the rect
                Vector2f mousePosFloat(static_cast<float>(mousePos.x) + 0.5f, static_cast<float>(mousePos.y) + 0.5f);
                if (bounds.contains(mousePosFloat - minimapPos.xy()))
                {
                    return unitId;
                }
            }

            return std::nullopt;
        }

        if (isCursorOverWorld())
        {
            auto ray = screenToWorldRayUtil(computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height()), screenToWorldClipSpace(getMousePosition()));
            return getFirstCollidingUnit(ray);
        }

        return std::nullopt;
    }

    std::optional<FeatureId> GameScene::getFeatureUnderCursor() const
    {
        if (!isCursorOverWorld())
        {
            return std::nullopt;
        }

        auto ray = screenToWorldRayUtil(computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height()), screenToWorldClipSpace(getMousePosition()));
        return getFirstCollidingFeature(ray);
    }

    Vector2f GameScene::screenToWorldClipSpace(Point p) const
    {
        return worldViewport.toClipSpace(sceneContext.viewport->toOtherViewport(worldViewport, p));
    }

    bool GameScene::isCursorOverMinimap() const
    {
        auto mousePos = getMousePosition();
        return minimapRect.contains(mousePos.x, mousePos.y);
    }

    bool GameScene::isCursorOverWorld() const
    {
        return worldViewport.contains(getMousePosition());
    }

    Point GameScene::getMousePosition() const
    {
        float fx;
        float fy;
        sceneContext.sdl->getMouseState(&fx, &fy);
        return Point(static_cast<int>(fx), static_cast<int>(fy));
    }

    std::optional<UnitId> GameScene::getFirstCollidingUnit(const Ray3f& ray) const
    {
        auto winnerIsMobile = false;
        auto bestDistance = std::numeric_limits<float>::infinity();
        std::optional<UnitId> it;

        for (const auto& entry : simulation.units)
        {
            if (entry.second.carriedBy)
            {
                // cargo inside a transport is not pickable
                continue;
            }
            if (!simulation.isLosVisibleToPlayer(entry.first, localPlayerId))
            {
                // undetected enemies are not pickable
                continue;
            }
            const auto& unitDefinition = simulation.unitDefinitions.at(entry.second.unitType);
            auto selectionMesh = gameMediaDatabase.getSelectionCollisionMesh(unitDefinition.objectName);
            auto distance = selectionIntersect(entry.second, *selectionMesh.value(), ray);
            auto isMobile = unitDefinition.isMobile;
            if (distance && ((!winnerIsMobile && isMobile) || distance < bestDistance))
            {
                winnerIsMobile = isMobile;
                bestDistance = *distance;
                it = entry.first;
            }
        }

        return it;
    }

    std::optional<FeatureId> GameScene::getFirstCollidingFeature(const Ray3f& ray) const
    {
        auto intersect = simulation.intersectLineWithTerrain(floatToSimLine(ray.toLine()));
        if (!intersect)
        {
            return std::nullopt;
        }

        auto heightmapPosition = simulation.terrain.worldToHeightmapCoordinate(*intersect);

        auto cellContents = simulation.occupiedGrid.tryGet(heightmapPosition);
        if (!cellContents)
        {
            return std::nullopt;
        }

        return cellContents->get().featureId;
    }

    std::optional<float> GameScene::selectionIntersect(const UnitState& unit, const CollisionMesh& mesh, const Ray3f& ray) const
    {
        auto inverseTransform = toFloatMatrix(unit.getInverseTransform());
        auto line = ray.toLine();
        Line3f modelSpaceLine(inverseTransform * line.start, inverseTransform * line.end);
        auto v = mesh.intersectLine(modelSpaceLine);
        if (!v)
        {
            return std::nullopt;
        }

        return ray.origin.distance(*v);
    }

    std::optional<SimVector> GameScene::getMouseTerrainCoordinate() const
    {
        if (isCursorOverMinimap())
        {
            auto transform = minimapToWorldMatrix(simulation.terrain, minimapRect);
            auto mousePos = getMousePosition();
            auto mouseX = static_cast<float>(mousePos.x) + 0.5f;
            auto mouseY = static_cast<float>(mousePos.y) + 0.5f;

            auto startPoint = transform * Vector3f(mouseX, mouseY, -1.0f);
            auto endPoint = transform * Vector3f(mouseX, mouseY, 1.0f);
            auto direction = endPoint - startPoint;
            Ray3f ray(startPoint, direction);
            return simulation.intersectLineWithTerrain(floatToSimLine(ray.toLine()));
        }

        if (isCursorOverWorld())
        {
            auto ray = screenToWorldRayUtil(computeInverseViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height()), screenToWorldClipSpace(getMousePosition()));
            return simulation.intersectLineWithTerrain(floatToSimLine(ray.toLine()));
        }

        return std::nullopt;
    }

    bool GameScene::tryRemoveQueuedOrderAtCursor()
    {
        return match(
            cursorMode.getValue(),
            [&](const AttackCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Attack); },
            [&](const MoveCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Move); },
            [&](const GuardCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Guard); },
            [&](const RepairCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Repair); },
            [&](const ReclaimCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Reclaim); },
            [&](const PatrolCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Patrol); },
            [&](const CommandFireCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::CommandFire); },
            [&](const CaptureCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Capture); },
            [&](const LoadCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Load); },
            [&](const UnloadCursorMode&) { return tryRemoveQueuedOrderAtMarker(CursorType::Unload); },
            [&](const BuildCursorMode&) { return tryRemoveQueuedBuildOrderAtCursor(); },
            [&](const auto&) { return false; });
    }

    bool GameScene::tryRemoveQueuedOrderAtMarker(CursorType markerType)
    {
        auto worldToUi = worldUiRenderService.getInverseViewProjectionMatrix()
            * computeViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height());

        // marker positions are drawn in worldViewport-local UI space,
        // so the mouse must be converted from scene (window) space the
        // same way rather than compared raw
        auto uiMousePos = sceneContext.viewport->toOtherViewport(worldViewport, getMousePosition());
        Vector2f uiMouse(static_cast<float>(uiMousePos.x), static_cast<float>(uiMousePos.y));

        std::vector<std::pair<UnitId, int>> candidates;
        std::vector<Vector2f> candidateCenters;

        for (const auto& unitId : selectedUnits)
        {
            const auto& unit = getUnit(unitId);
            auto pos = unit.position;
            int index = 0;
            for (const auto& order : unit.orders)
            {
                auto nextPos = orderMarkerPosition(simulation, order, pos);
                pos = nextPos;

                auto icon = orderMarkerCursorType(order);
                if (icon && *icon == markerType)
                {
                    const auto& sprites = sceneContext.cursor->getCursor(*icon)->sprites;
                    if (!sprites.empty())
                    {
                        auto uiDest = worldToUi * simVectorToFloat(nextPos);
                        const auto& bounds = sprites.front()->bounds;
                        if (orderMarkerContainsPoint(bounds, uiDest.xy(), uiMouse, OrderMarkerHitSlop))
                        {
                            candidates.emplace_back(unitId, index);
                            candidateCenters.push_back(uiDest.xy() + bounds.position);
                        }
                    }
                }

                ++index;
            }
        }

        if (auto best = nearestUiPoint(candidateCenters, uiMouse))
        {
            const auto& [unitId, index] = candidates[*best];
            localPlayerRemoveUnitOrder(unitId, index);
            return true;
        }

        return false;
    }

    bool GameScene::tryRemoveQueuedBuildOrderAtCursor()
    {
        auto terrainPos = getMouseTerrainCoordinate();
        if (!terrainPos)
        {
            return false;
        }
        auto cell = simulation.terrain.worldToHeightmapCoordinate(*terrainPos);

        for (const auto& unitId : selectedUnits)
        {
            const auto& unit = getUnit(unitId);
            int index = 0;
            for (const auto& order : unit.orders)
            {
                if (const auto buildOrder = std::get_if<BuildOrder>(&order))
                {
                    const auto& unitDefinition = simulation.unitDefinitions.at(buildOrder->unitType);
                    auto footprintRect = simulation.computeFootprintRegion(buildOrder->position, unitDefinition.movementCollisionInfo);
                    if (footprintRect.contains(cell))
                    {
                        localPlayerRemoveUnitOrder(unitId, index);
                        return true;
                    }
                }

                ++index;
            }
        }

        return false;
    }

    void GameScene::completeBuildPlacement(const std::string& unitType)
    {
        auto drag = buildDragState;
        buildDragState = std::nullopt;
        auto candidates = std::move(buildDragCandidates);
        buildDragCandidates.clear();

        if (!drag || drag->unitType != unitType)
        {
            return;
        }

        auto selectedUnit = getSingleSelectedUnit();
        if (!selectedUnit)
        {
            return;
        }

        auto candidateCenter = [&](const BuildDragCandidate& c) {
            auto corner = simulation.terrain.heightmapIndexToWorldCorner(c.rect.x, c.rect.y);
            auto x = corner.x + ((SimScalar(c.rect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss);
            auto z = corner.z + ((SimScalar(c.rect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss);
            return SimVector(x, c.baseY, z);
        };

        auto playNotOkToBuild = [&]() {
            if (sounds.notOkToBuild)
            {
                playUiSound(*sounds.notOkToBuild);
            }
        };

        if (!drag->engaged)
        {
            // same-cell click: stock single placement. Placement is
            // recomputed at the anchor rather than read back from the
            // candidate list so a click that completes between update
            // ticks (before any preview ran) still resolves.
            const auto& unitDefinition = simulation.unitDefinitions.at(unitType);
            auto mc = simulation.getAdHocMovementClass(unitDefinition.movementCollisionInfo);
            auto placement = simulation.computeBuildingPlacement(mc, unitDefinition.yardMap, unitDefinition.yardMapContainsGeo, unitDefinition.waterline, drag->anchorRect.x, drag->anchorRect.y);
            if (!placement || !placement->valid)
            {
                playNotOkToBuild();
                return;
            }
            auto corner = simulation.terrain.heightmapIndexToWorldCorner(drag->anchorRect.x, drag->anchorRect.y);
            SimVector buildPos(
                corner.x + ((SimScalar(drag->anchorRect.width) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss),
                placement->baseY,
                corner.z + ((SimScalar(drag->anchorRect.height) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss));
            if (isShiftDown())
            {
                localPlayerEnqueueUnitOrder(*selectedUnit, BuildOrder(unitType, buildPos));
            }
            else
            {
                localPlayerIssueUnitOrder(*selectedUnit, BuildOrder(unitType, buildPos));
                cursorMode.next(NormalCursorMode());
            }
            return;
        }

        // engaged drag: emit one ordinary BuildOrder per valid site in
        // line order; first site replaces the queue when Shift is up
        bool emittedAny = false;
        const auto& selectedOrders = getUnit(*selectedUnit).orders;
        for (const auto& candidate : candidates)
        {
            if (!candidate.valid)
            {
                continue;
            }
            auto buildPos = candidateCenter(candidate);

            // suppress sites already queued on this builder
            if (queuedBuildSiteExists(selectedOrders, unitType, buildPos))
            {
                continue;
            }

            if (!emittedAny && !isShiftDown())
            {
                localPlayerIssueUnitOrder(*selectedUnit, BuildOrder(unitType, buildPos));
            }
            else
            {
                localPlayerEnqueueUnitOrder(*selectedUnit, BuildOrder(unitType, buildPos));
            }
            emittedAny = true;
        }

        if (!emittedAny && !candidates.empty())
        {
            // a drag released off the world has no candidates and
            // cancels silently; a line of all-invalid sites gets feedback
            playNotOkToBuild();
        }
        // stay armed so another line can be drawn
    }

    void GameScene::localPlayerIssueUnitOrder(UnitId unitId, const UnitOrder& order)
    {
        auto kind = PlayerUnitCommand::IssueOrder::IssueKind::Immediate;
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::IssueOrder(order, kind)));

        if (std::holds_alternative<BuildOrder>(order))
        {
            if (sounds.okToBuild)
            {
                playUiSound(*sounds.okToBuild);
            }
        }
        else
        {
            const auto& unit = getUnit(unitId);
            auto handle = getSound(simulation, gameMediaDatabase, unit.unitType, UnitSoundType::Ok1);
            if (handle)
            {
                playUiSound(*handle);
            }
        }
    }

    void GameScene::localPlayerEnqueueUnitOrder(UnitId unitId, const UnitOrder& order)
    {
        auto kind = PlayerUnitCommand::IssueOrder::IssueKind::Queued;
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::IssueOrder(order, kind)));

        if (std::holds_alternative<BuildOrder>(order))
        {
            if (sounds.okToBuild)
            {
                playUiSound(*sounds.okToBuild);
            }
        }

        commandWasQueued = true;
    }

    void GameScene::localPlayerStopUnit(UnitId unitId)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::Stop()));

        const auto& unit = getUnit(unitId);
        auto handle = getSound(simulation, gameMediaDatabase, unit.unitType, UnitSoundType::Ok1);
        if (handle)
        {
            playUiSound(*handle);
        }
    }

    void GameScene::localPlayerSetFireOrders(UnitId unitId, UnitFireOrders orders)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::SetFireOrders{orders}));
    }

    void GameScene::localPlayerSetOnOff(UnitId unitId, bool on)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::SetOnOff{on}));
    }

    void GameScene::localPlayerSetCloak(UnitId unitId, bool on)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::SetCloak{on}));
    }

    void GameScene::localPlayerModifyBuildQueue(UnitId unitId, const std::string& unitType, int count)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::ModifyBuildQueue{count, unitType}));

        updateUnconfirmedBuildQueueDelta(unitId, unitType, count);
        refreshBuildGuiTotal(unitId, unitType);
    }

    void GameScene::localPlayerModifyWeaponQueue(UnitId unitId, int count)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::ModifyWeaponQueue{count}));

        unconfirmedWeaponQueueDelta[unitId] += count;
        refreshWeaponGuiTotal(unitId);
    }

    void GameScene::localPlayerRemoveUnitOrder(UnitId unitId, int index)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::RemoveOrder{index}));
    }

    void GameScene::localPlayerToggleSelfDestruct(UnitId unitId)
    {
        localPlayerCommandBuffer.push_back(PlayerUnitCommand(unitId, PlayerUnitCommand::ToggleSelfDestruct()));
    }

    void GameScene::issueUnitOrder(UnitId unitId, const UnitOrder& order)
    {
        auto unit = tryGetUnit(unitId);
        if (unit)
        {
            applyBasicPlayerUnitCommand(unit->get(), PlayerUnitCommand::IssueOrder(order, PlayerUnitCommand::IssueOrder::Immediate));
        }
    }

    void GameScene::enqueueUnitOrder(UnitId unitId, const UnitOrder& order)
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return;
        }

        applyBasicPlayerUnitCommand(unit->get(), PlayerUnitCommand::IssueOrder(order, PlayerUnitCommand::IssueOrder::Queued));
    }

    void GameScene::stopUnit(UnitId unitId)
    {
        auto unit = tryGetUnit(unitId);
        if (unit)
        {
            applyBasicPlayerUnitCommand(unit->get(), PlayerUnitCommand::Stop{});
        }
    }

    void GameScene::removeUnitOrderAt(UnitId unitId, int index)
    {
        if (index < 0)
        {
            return;
        }

        auto unit = tryGetUnit(unitId);
        if (unit)
        {
            unit->get().removeOrderAt(static_cast<std::size_t>(index));
        }
    }

    void GameScene::setFireOrders(UnitId unitId, UnitFireOrders orders)
    {
        auto unit = tryGetUnit(unitId);
        if (unit)
        {
            applyBasicPlayerUnitCommand(unit->get(), PlayerUnitCommand::SetFireOrders{orders});

            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit && *selectedUnit == unitId)
            {
                fireOrders.next(orders);
            }
        }
    }

    void GameScene::setCloak(UnitId unitId, bool on)
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return;
        }
        // the CLOAK_ON/CLOAK_OFF order handlers gate on the unitdef
        // cloak bit (+0x245 bit13 = CloakCost > 0); a non-capable
        // unit silently ignores the order (retail 0x403070/0x4030a0)
        if (!(simulation.unitDefinitions.at(unit->get().unitType).cloakCost > 0.0f))
        {
            return;
        }
        unit->get().cloakRequested = on;

        // retail writes the optimistic mask bit on click; the strip
        // shows the requested state, not the effective cloak
        cloakStage.next(cloakPanelStage(simulation, selectedUnits));
    }

    void GameScene::toggleSelfDestruct(UnitId unitId)
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return;
        }
        auto& state = unit->get();
        if (state.isDead())
        {
            return;
        }
        if (state.selfDestruct)
        {
            // the command node toggles: a second SELFDESTRUCT while
            // armed runs canceldestruct ("Self destruct terminated")
            state.selfDestruct = std::nullopt;
            simulation.events.push_back(UnitSelfDestructCancelledEvent{unitId});
            return;
        }

        const auto& unitDefinition = simulation.unitDefinitions.at(state.unitType);
        state.selfDestruct = SelfDestructOrder{static_cast<unsigned int>(unitDefinition.selfDestructCountdown) * SimTicksPerSecond};

        // retail announces the top of the countdown immediately
        simulation.events.push_back(UnitSelfDestructCountdownEvent{unitId, static_cast<int>(unitDefinition.selfDestructCountdown)});
    }

    void GameScene::startTrack()
    {
        // sort selection by unit id so repeated 'T' keydown cycles through all units in a group consistently
        std::vector<UnitId> unitIds;
        for (const auto& u : selectedUnits)
        {
            unitIds.push_back(u);
        }
        std::sort(unitIds.begin(), unitIds.end());

        startTrackInternal(unitIds);
    }

    void GameScene::startTrackInternal(const std::vector<UnitId>& unitIds)
    {
        // Only allow tracking in free camera mode or if we are already tracking.
        auto canStartTracking = match(
            cameraControlState,
            [&](const CameraControlStateFree&) {
                return true;
            },
            [&](const CameraControlStateTrackingUnit&) {
                return true;
            },
            [&](const CameraControlStateMiddleMousePan&) {
                return false;
            });

        if (!canStartTracking)
        {
            return;
        }

        // If 'T' is pressed and no units are selected, stop tracking.
        if (unitIds.empty())
        {
            cameraControlState = CameraControlStateFree();
            return;
        }

        // If already tracking, check if currently tracked unit is in this selection. If it is, select the next id in the group.
        if (trackedUnitId)
        {
            auto it = std::find(unitIds.begin(), unitIds.end(), trackedUnitId);
            if (it != unitIds.end() && ++it != unitIds.end())
            {
                trackedUnitId = *it;
            }
            else
            {
                trackedUnitId = unitIds[0];
            }
        }
        else
        {
            trackedUnitId = unitIds[0];
        }

        cameraControlState = CameraControlStateTrackingUnit();
    }

    bool GameScene::isCtrlDown() const
    {
        return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    }

    bool GameScene::isAltDown() const
    {
        return (SDL_GetModState() & SDL_KMOD_ALT) != 0;
    }

    bool GameScene::isModifiedLetterOrDigit(SDL_Keycode key) const
    {
        if (!isCtrlDown() && !isAltDown())
        {
            return false;
        }
        return (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')
            || (key >= '0' && key <= '9');
    }

    bool GameScene::isShiftDown() const
    {
        return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    }

    void GameScene::handleEscapeDown()
    {
        match(
            cursorMode.getValue(),
            [this](const NormalCursorMode&) {
                clearUnitSelection();
            },
            [this](const auto&) {
                buildDragState = std::nullopt;
                buildDragCandidates.clear();
                cursorMode.next(NormalCursorMode());
            });
    }

    bool GameScene::inGameMenuOpen() const
    {
        return !inGameMenuDialogs.empty();
    }

    bool GameScene::isSingleHumanPlayer() const
    {
        unsigned int humans = 0;
        for (const auto& player : simulation.players)
        {
            if (player.type == GamePlayerType::Human)
            {
                ++humans;
            }
        }
        return humans == 1;
    }

    void GameScene::adjustGameSpeed(int delta)
    {
        // the retail keys are single-player only; the word clamps at
        // 1..20 and 10 is real-time
        if (!isSingleHumanPlayer())
        {
            return;
        }
        gameSpeedLevel = std::clamp(gameSpeedLevel + delta, 1, 20);
    }

    void GameScene::openInGameMenu()
    {
        // an unfinished linear-build drag must place nothing once the
        // menu opens
        buildDragState = std::nullopt;
        buildDragCandidates.clear();

        // a bandbox or minimap drag in progress is abandoned; its
        // mouse-up is consumed by the menu
        if (auto normalCursor = std::get_if<NormalCursorMode>(&cursorMode.getValue()))
        {
            if (!std::holds_alternative<NormalCursorMode::UpState>(normalCursor->state))
            {
                cursorMode.next(NormalCursorMode());
            }
        }

        // drop any held camera-scroll keys; their key-up events are
        // routed to the menu while it is open
        up = false;
        down = false;
        left = false;
        right = false;
        cameraControlState = CameraControlStateFree();

        sceneContext.cursor->useCursor(CursorType::Normal);

        // single-player games pause while the menu is open; networked
        // games do not (retail pauses only offline). The pause travels
        // through the normal command stream so it stays in lockstep.
        if (isSingleHumanPlayer() && !gamePaused)
        {
            localPlayerCommandBuffer.push_back(PlayerPauseGameCommand());
            inGameMenuPauseOwned = true;
        }

        const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
        auto optionsPanel = createInGameMenuDialog(sidePrefix + "OPT");

        // save/load round-trip through the local filesystem; a
        // networked game cannot offer them, so its buttons stay
        // disabled rather than silently doing nothing
        if (!isSingleHumanPlayer())
        {
            for (const auto& buttonName : {"LOADGAME", "SAVEGAME"})
            {
                if (auto button = optionsPanel->find<UiStagedButton>(buttonName))
                {
                    button->get().setEnabled(false);
                }
            }
        }

        pushInGameMenuDialog(std::move(optionsPanel));
    }

    void GameScene::openInGameMenuHelp()
    {
        // retail F1 opens the help screen directly (TotalA.exe
        // 0x4960ea); in single-player it pauses like the options menu
        if (isSingleHumanPlayer() && !gamePaused)
        {
            localPlayerCommandBuffer.push_back(PlayerPauseGameCommand());
            inGameMenuPauseOwned = true;
        }

        auto dialog = createInGameMenuDialog("HELP");
        populateHelpDialog(*dialog);
        pushInGameMenuDialog(std::move(dialog));
    }

    void GameScene::closeInGameMenu()
    {
        // dialogs removed mid-dispatch retire instead of dying here;
        // a child's key/mouse broadcast is what emitted the activation
        for (auto& dialog : inGameMenuDialogs)
        {
            inGameMenuRetiredDialogs.push_back(std::move(dialog));
        }
        inGameMenuDialogs.clear();
        inGameMenuDialogCompanions.clear();
        inGameMenuMouseTarget = nullptr;
        inGameMenuConfirmAction = nullptr;

        reapInGameMenuDialogs();

        if (inGameMenuPauseOwned)
        {
            localPlayerCommandBuffer.push_back(PlayerUnpauseGameCommand());
            inGameMenuPauseOwned = false;
        }
    }

    void GameScene::pushInGameMenuDialog(std::unique_ptr<UiPanel>&& panel, bool companionPane)
    {
        auto& p = inGameMenuDialogs.emplace_back(std::move(panel));
        inGameMenuDialogCompanions.push_back(companionPane);
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                onInGameMenuMessage(msg.topic, msg.controlName, activate->type);
            }
        });
    }

    void GameScene::popInGameMenuDialog()
    {
        if (inGameMenuDialogs.empty())
        {
            return;
        }

        inGameMenuRetiredDialogs.push_back(std::move(inGameMenuDialogs.back()));
        inGameMenuDialogs.pop_back();
        inGameMenuDialogCompanions.pop_back();
        inGameMenuMouseTarget = nullptr;

        // a pending yes/no answer belongs to the dialog that was open
        inGameMenuConfirmAction = nullptr;

        if (inGameMenuDialogs.empty())
        {
            closeInGameMenu();
        }

        reapInGameMenuDialogs();
    }

    void GameScene::popInGameMenuDialogThrough(const std::string& panelName)
    {
        while (!inGameMenuDialogs.empty())
        {
            bool reached = inGameMenuDialogs.back()->getName() == panelName;
            inGameMenuRetiredDialogs.push_back(std::move(inGameMenuDialogs.back()));
            inGameMenuDialogs.pop_back();
            inGameMenuDialogCompanions.pop_back();
            if (reached)
            {
                break;
            }
        }
        inGameMenuMouseTarget = nullptr;
        inGameMenuConfirmAction = nullptr;

        if (inGameMenuDialogs.empty())
        {
            closeInGameMenu();
        }

        reapInGameMenuDialogs();
    }

    void GameScene::reapInGameMenuDialogs()
    {
        if (inGameMenuDispatchDepth == 0)
        {
            inGameMenuRetiredDialogs.clear();
        }
    }

    UiPanel* GameScene::inGameMenuPointerTarget(int x, int y)
    {
        // topmost-first: companion option panes receive points inside
        // their rect and otherwise yield to the screen beneath; a real
        // modal dialog owns the point or blocks it entirely
        for (auto i = inGameMenuDialogs.size(); i-- > 0;)
        {
            if (inGameMenuDialogs[i]->contains(x, y))
            {
                return inGameMenuDialogs[i].get();
            }
            if (!inGameMenuDialogCompanions[i])
            {
                return nullptr;
            }
        }
        return nullptr;
    }

    UiPanel* GameScene::inGameMenuKeyTarget()
    {
        // ESC/Enter on a companion pane resolve against the owning
        // screen's defaults (SPEEDSRT has none; PREFS maps both to
        // the committing PREV button)
        for (auto i = inGameMenuDialogs.size(); i-- > 0;)
        {
            if (!inGameMenuDialogCompanions[i])
            {
                return inGameMenuDialogs[i].get();
            }
        }
        return nullptr;
    }

    void GameScene::commitInterfaceModePreference()
    {
        if (sceneContext.globalConfig->configFilePath.empty())
        {
            return;
        }
        writeConfigValue(
            sceneContext.globalConfig->configFilePath,
            "interface-mode",
            sceneContext.globalConfig->leftClickInterfaceMode ? "left-click" : "right-click");
    }

    void GameScene::togglePause()
    {
        if (gamePaused)
        {
            localPlayerCommandBuffer.push_back(PlayerUnpauseGameCommand());
        }
        else
        {
            localPlayerCommandBuffer.push_back(PlayerPauseGameCommand());
        }
    }

    std::unique_ptr<UiPanel> GameScene::createInGameMenuDialog(const std::string& guiName)
    {
        // stock data ships only the ARM variant of the in-game options
        // panel; other sides' OPT names fall back to it
        std::string resolvedName = guiName;
        if (!sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/" + resolvedName + ".GUI") && endsWith(resolvedName, "OPT"))
        {
            resolvedName = "ARMOPT";
        }

        auto panel = uiFactory.panelFromGuiFile(resolvedName);

        // PictureBox/Surface gadgets (OPTBG, IGOPT, SPEEDSRT, ...)
        // carry their background texture in a same-named GAF entry,
        // resolved per-gui with a commongui fallback
        for (const auto& child : panel->getChildren())
        {
            auto* surface = dynamic_cast<UiSurface*>(child.get());
            if (surface == nullptr)
            {
                continue;
            }
            if (auto texture = sceneContext.textureService->getGuiTexture(resolvedName, surface->getName()))
            {
                surface->setBackground((*texture)->sprites.at(0));
            }
        }

        return panel;
    }

    std::unique_ptr<UiPanel> GameScene::createYesNoDialog(const std::string& titleText)
    {
        auto panel = createInGameMenuDialog("YESORNO");
        if (auto title = panel->find<UiLabel>("TITLE"))
        {
            title->get().setText(titleText);
        }
        return panel;
    }

    std::unique_ptr<UiPanel> GameScene::createSaveListDialog(bool saveMode)
    {
        inGameMenuSaveListIsSave = saveMode;
        auto panel = createInGameSaveListPanel(
            uiFactory,
            sceneContext.textureService,
            sceneContext.vfs,
            saveMode,
            &inGameMenuSaveListEntries);

        // save mode starts with an empty entry field and no selection;
        // load mode has nothing editable to begin with
        if (auto box = panel->find<UiTextBox>("GAMENAME"))
        {
            box->get().setText("");
        }
        if (saveMode)
        {
            panel->setFocusByName("GAMENAME");
        }

        refreshSaveListDialog(*panel);
        return panel;
    }

    void GameScene::refreshSaveListDialog(UiPanel& panel)
    {
        inGameMenuSaveListEntries.clear();
        if (auto dir = saveGameDirectory())
        {
            inGameMenuSaveListEntries = listSavedGames(*dir);
        }
        populateSaveList(panel, inGameMenuSaveListEntries);
    }

    void GameScene::performSaveGame(UiPanel& panel)
    {
        std::string typed;
        if (auto box = panel.find<UiTextBox>("GAMENAME"))
        {
            typed = box->get().getText();
        }

        auto name = sanitizeSaveName(typed);
        if (name.empty())
        {
            return;
        }

        auto dir = saveGameDirectory();
        if (!dir)
        {
            return;
        }

        // single-player menus pause the sim, so the F2 dialog is a
        // natural save boundary: the snapshot below is taken while the
        // command stream is stopped and no container is mid-mutation
        auto doSave = [this, name, dir]() {
            auto bytes = encodeSavedGame(
                simulation,
                gameParameters,
                sceneContext.campaignProgress,
                name,
                static_cast<std::uint64_t>(std::time(nullptr)));
            writeSavedGameFile(*dir, name, bytes);
            closeInGameMenu();
        };

        auto path = saveGamePath(*dir, name);
        if (path && std::filesystem::exists(*path))
        {
            inGameMenuConfirmAction = std::move(doSave);
            pushInGameMenuDialog(createYesNoDialog("Overwrite existing saved game?"));
            return;
        }

        doSave();
    }

    void GameScene::performLoadGame(UiPanel& panel)
    {
        auto listBox = panel.find<UiListBox>("GAMES");
        if (!listBox)
        {
            return;
        }
        auto selected = listBox->get().getSelectedIndex();
        if (!selected || *selected >= inGameMenuSaveListEntries.size())
        {
            return;
        }

        auto dir = saveGameDirectory();
        if (!dir)
        {
            return;
        }

        DecodedSaveGame decoded;
        try
        {
            auto bytes = readSavedGameFile(*dir, inGameMenuSaveListEntries[*selected].saveName);
            decoded = decodeSavedGame(bytes);
        }
        catch (const std::exception& e)
        {
            LOG_WARN << "Failed to read save file: " << e.what();
            return;
        }

        auto params = gameParametersFromSave(decoded);
        sceneContext.sceneManager->setNextScene(std::make_shared<LoadingScene>(
            sceneContext,
            audioLookup,
            AudioService::LoopToken(),
            params,
            std::move(decoded)));
    }

    Point GameScene::toInGameMenuCoordinates(int x, int y) const
    {
        auto clip = screenToWorldRayUtil(menuUiRenderService.getInverseViewProjectionMatrix(), sceneContext.viewport->toClipSpace(x, y));
        return Point(static_cast<int>(clip.origin.x), static_cast<int>(clip.origin.y));
    }

    void GameScene::renderInGameMenu()
    {
        for (const auto& dialog : inGameMenuDialogs)
        {
            menuUiRenderService.fillScreen(Color(0, 0, 0, 63));
            dialog->render(menuUiRenderService);
        }
    }

    void GameScene::populateHelpDialog(UiPanel& panel)
    {
        auto helpRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->gamedata + "/HELP.TDF");
        if (!helpRaw)
        {
            return;
        }

        if (auto pageButton = panel.find<UiStagedButton>("Page"))
        {
            inGameMenuHelpPage = pageButton->get().getStage();
        }

        auto text = inGameHelpText(std::string(helpRaw->begin(), helpRaw->end()), inGameMenuHelpPage, 3);

        // the stock HELP.GUI has no text gadget; the retail executable
        // draws the page contents into the dialog body between the
        // "Keyboard Commands" title (y 16-32) and the buttons (y 377)
        if (auto textArea = panel.find<UiLabel>("HelpText"))
        {
            textArea->get().setText(text);
        }
        else
        {
            auto font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");
            auto label = std::make_unique<UiLabel>(6, 28, 480, 336, text, font);
            label->setName("HelpText");
            label->setAlignment(UiLabel::Alignment::LeftWrapped);
            panel.appendChild(std::move(label));
        }
    }

    void GameScene::populateInterfaceDialog(UiPanel& panel)
    {
        // stock SPEEDSRT controls: only LEFTCLICK (Button Interface)
        // has a runtime backend -- GlobalConfig.leftClickInterfaceMode,
        // read live by leftClickMode(). Game/screen scroll speed,
        // unit chat and text sliders have no backing and are disabled
        // (horizontal sliders render as disabled display gadgets).
        if (auto leftClick = panel.find<UiStagedButton>("LEFTCLICK"))
        {
            leftClick->get().setStage(sceneContext.globalConfig->leftClickInterfaceMode ? 0 : 1);
        }
        if (auto unitChat = panel.find<UiStagedButton>("UNITCHAT"))
        {
            unitChat->get().setEnabled(false);
        }
    }

    void GameScene::populateBriefingDialog(UiPanel& panel)
    {
        // retail draws the campaign brief file's text into TextRegion
        // and pages it via TextRegion/MOREBAR clicks; `&x` color codes
        // switch colors and `*emphasis*` markers render literally
        std::string text;
        if (simulation.missionInfo && !simulation.missionInfo->brief.empty())
        {
            auto briefRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->camps + "/briefs/" + briefFileName(simulation.missionInfo->brief));
            if (briefRaw)
            {
                text = sanitizeBriefText(std::string(briefRaw->begin(), briefRaw->end()), true);
            }
        }

        inGameMenuBriefingPages.clear();
        inGameMenuBriefingPage = 0;

        if (auto region = panel.find<UiLabel>("TextRegion"))
        {
            auto font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");
            auto lines = wrapTextLines(text, region->get().getWidth(), [&](const std::string& s) {
                return menuUiRenderService.getTextWidth(s, *font);
            });

            // drawTextWrapped draws at a fixed 15px line pitch; the
            // page capacity follows the TextRegion rect
            auto linesPerPage = static_cast<unsigned int>(region->get().getHeight() / 15.0f);
            inGameMenuBriefingPages = paginateLines(lines, std::max(linesPerPage, 1u));
            region->get().setClickable(true);
            region->get().setAlignment(UiLabel::Alignment::LeftWrapped);
        }

        if (auto moreBar = panel.find<UiLabel>("MOREBAR"))
        {
            moreBar->get().setClickable(true);
            moreBar->get().setAlignment(UiLabel::Alignment::CenterRect);
        }

        applyBriefingPage(panel);
    }

    void GameScene::applyBriefingPage(UiPanel& panel)
    {
        if (auto region = panel.find<UiLabel>("TextRegion"))
        {
            region->get().setText(inGameMenuBriefingPages.at(inGameMenuBriefingPage));
        }

        // retail MOREBAR is a pager caption, not a scrollbar: "MORE..."
        // while pages remain, "BACK TO START" on the last page, empty
        // when everything fits on one page
        if (auto moreBar = panel.find<UiLabel>("MOREBAR"))
        {
            std::string caption;
            if (inGameMenuBriefingPages.size() > 1)
            {
                caption = inGameMenuBriefingPage + 1 == inGameMenuBriefingPages.size() ? "BACK TO START" : "MORE...";
            }
            moreBar->get().setText(caption);
        }
    }

    void GameScene::populateGameOptionsDialog(UiPanel& panel)
    {
        // retail draws a read-only "GameSettings" summary (labels at
        // x=18 width 110, values at x=140 width 120, rows at y=90
        // stepping 18) in the screen's default font
        auto font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        const auto& localPlayer = simulation.getPlayer(localPlayerId);
        // campaign missions report their staged difficulty name;
        // skirmishes report the frontend-selected Difficulty stage
        auto difficulty = gameParameters.skirmish.difficulty;
        if (gameParameters.campaign)
        {
            auto name = toUpper(gameParameters.campaign->difficulty);
            difficulty = name == "EASY" ? SkirmishDifficulty::Easy
                : name == "HARD"         ? SkirmishDifficulty::Hard
                                         : SkirmishDifficulty::Medium;
        }
        auto rows = makeGameOptionsRows(
            gameParameters.mapName,
            gameParameters.skirmish.commanderDeath,
            gameParameters.skirmish.startLocation,
            simulation.mappingEnabled,
            simulation.lineOfSightMode,
            difficulty,
            static_cast<unsigned int>(localPlayer.startingMetal.value),
            static_cast<unsigned int>(localPlayer.startingEnergy.value),
            simulation.maxUnitsPerPlayer);

        int y = 90;
        unsigned int rowIndex = 0;
        for (const auto& [label, value] : rows)
        {
            auto labelComponent = std::make_unique<UiLabel>(18, y, 110, 15, label, font);
            labelComponent->setName("RowLabel" + std::to_string(rowIndex));
            panel.appendChild(std::move(labelComponent));

            auto valueComponent = std::make_unique<UiLabel>(140, y, 120, 15, value, font);
            valueComponent->setName("RowValue" + std::to_string(rowIndex));
            panel.appendChild(std::move(valueComponent));

            y += 18;
            ++rowIndex;
        }
    }

    void GameScene::exitToMainMenu()
    {
        sceneContext.sceneManager->setNextScene(std::make_shared<MainMenuScene>(
            sceneContext,
            audioLookup,
            static_cast<float>(sceneContext.viewport->width()),
            static_cast<float>(sceneContext.viewport->height())));
    }

    void GameScene::restartGame()
    {
        sceneContext.sceneManager->setNextScene(std::make_shared<LoadingScene>(
            sceneContext,
            audioLookup,
            AudioService::LoopToken(),
            gameParameters));
    }

    void GameScene::onInGameMenuMessage(const std::string& topic, const std::string& controlName, ActivateMessage::Type /*type*/)
    {
        if (inGameMenuDialogs.empty())
        {
            return;
        }

        // the topic names the panel that emitted the message -- with a
        // companion pane open that differs from the stack top
        const auto& panelName = topic;

        // ARMOPT/COROPT: the retail F2 options panel
        if (matchesWithSidePrefix("OPT", panelName) || panelName == "ARMOPT")
        {
            if (controlName == "OK")
            {
                closeInGameMenu();
            }
            else if (controlName == "EXIT")
            {
                auto dialog = createInGameMenuDialog("EXITMENU");
                // the stock EXITMENU leaves the Restart gadget's
                // caption empty; the exe supplies it at runtime
                if (auto button = dialog->find<UiStagedButton>("RESTART"))
                {
                    button->get().setLabel("Restart");
                    // restart rebuilds the game locally; it cannot
                    // recreate a network session
                    if (!isSingleHumanPlayer())
                    {
                        button->get().setEnabled(false);
                    }
                }
                pushInGameMenuDialog(std::move(dialog));
            }
            else if (controlName == "HELP")
            {
                auto dialog = createInGameMenuDialog("HELP");
                populateHelpDialog(*dialog);
                pushInGameMenuDialog(std::move(dialog));
            }
            else if (controlName == "MISSION")
            {
                // retail gates on mission type: campaign missions open
                // BRIEFING.GUI, skirmish opens the GAMEOPTIONS
                // "GameSettings" summary instead (TotalA.exe 0x460b7d)
                if (simulation.missionInfo)
                {
                    auto dialog = createInGameMenuDialog("BRIEFING");
                    // retail binds bitmaps/igmbrief.pcx as the dialog
                    // background (TotalA.exe 0x460c08-0x460c29); the GUI
                    // file's panel field is empty
                    if (sceneContext.vfs->readFile("bitmaps/igmbrief.pcx"))
                    {
                        dialog->setBackground(sceneContext.textureService->getBitmapRegion(
                            "igmbrief", 0, 0, dialog->getWidth(), dialog->getHeight()));
                    }
                    populateBriefingDialog(*dialog);
                    pushInGameMenuDialog(std::move(dialog));
                }
                else if (sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/GAMEOPTIONS.GUI"))
                {
                    auto dialog = createInGameMenuDialog("GAMEOPTIONS");
                    populateGameOptionsDialog(*dialog);
                    pushInGameMenuDialog(std::move(dialog));
                }
                else
                {
                    // pre-3.1 data ships no GAMEOPTIONS.GUI; the
                    // briefing dialog is the closest available screen
                    auto dialog = createInGameMenuDialog("BRIEFING");
                    populateBriefingDialog(*dialog);
                    pushInGameMenuDialog(std::move(dialog));
                }
            }
            else if (controlName == "PREFS")
            {
                // retail snapshots the settings block when the options
                // screen opens; Undo/Cancel restore it, OK/PREV commits
                inGameMenuInterfaceSnapshot = sceneContext.globalConfig->leftClickInterfaceMode;
                auto prefsPanel = createInGameMenuDialog("PREFS");
                // SOUND/VISUALS/MUSIC have no controls backed by the
                // runtime (no master volume, no music playback, no
                // renderer options); INTERFACE/SPEEDS has the stock
                // button-interface staged button, backed by
                // GlobalConfig.leftClickInterfaceMode
                for (const auto& buttonName : {"SOUND", "VISUALS", "MUSIC"})
                {
                    if (auto button = prefsPanel->find<UiStagedButton>(buttonName))
                    {
                        button->get().setEnabled(false);
                    }
                }
                pushInGameMenuDialog(std::move(prefsPanel));
            }
            else if (controlName == "SAVEGAME")
            {
                pushInGameMenuDialog(createSaveListDialog(true));
            }
            else if (controlName == "LOADGAME")
            {
                pushInGameMenuDialog(createSaveListDialog(false));
            }
            return;
        }

        // SAVELIST (save entry) / LOADLIST (load pick): both stock
        // GUIs share the LoadList.GUI layout and control names
        if (panelName == "SAVELIST" || panelName == "LOADLIST")
        {
            if (controlName == "LOAD")
            {
                if (inGameMenuSaveListIsSave)
                {
                    performSaveGame(*inGameMenuDialogs.back());
                }
                else
                {
                    performLoadGame(*inGameMenuDialogs.back());
                }
            }
            else if (controlName == "CANCEL")
            {
                popInGameMenuDialog();
            }
            else if (controlName == "DELETE")
            {
                if (auto listBox = inGameMenuDialogs.back()->find<UiListBox>("GAMES"))
                {
                    auto selected = listBox->get().getSelectedIndex();
                    if (selected && *selected < inGameMenuSaveListEntries.size())
                    {
                        auto saveName = inGameMenuSaveListEntries[*selected].saveName;
                        inGameMenuConfirmAction = [this, saveName]() {
                            if (auto dir = saveGameDirectory())
                            {
                                deleteSavedGameFile(*dir, saveName);
                            }
                            if (!inGameMenuDialogs.empty())
                            {
                                refreshSaveListDialog(*inGameMenuDialogs.back());
                            }
                        };
                        pushInGameMenuDialog(createYesNoDialog("Delete saved game?"));
                    }
                }
            }
            return;
        }

        if (panelName == "EXITMENU")
        {
            if (controlName == "MAINMENU")
            {
                inGameMenuConfirmAction = [this]() { exitToMainMenu(); };
                pushInGameMenuDialog(createYesNoDialog("Surrender this battle and return to main menu?"));
            }
            else if (controlName == "EXITGAME")
            {
                inGameMenuConfirmAction = [this]() { sceneContext.sceneManager->requestExit(); };
                pushInGameMenuDialog(createYesNoDialog("Surrender this battle and exit to Windows?"));
            }
            else if (controlName == "RESTART")
            {
                auto dialog = createInGameMenuDialog("RESTART");
                if (auto label = dialog->find<UiLabel>("MISSIONNAME"))
                {
                    label->get().setText(gameParameters.mapName);
                }
                if (auto label1 = dialog->find<UiLabel>("MISSIONNAME1"))
                {
                    label1->get().setText(gameParameters.campaign ? gameParameters.campaign->missionName : gameParameters.mapName);
                }
                if (gameParameters.campaign)
                {
                    if (auto difficulty = dialog->find<UiStagedButton>("Difficulty"))
                    {
                        static const std::array<std::string, 3> difficulties{"Easy", "Medium", "Hard"};
                        auto it = std::find(difficulties.begin(), difficulties.end(), gameParameters.campaign->difficulty);
                        difficulty->get().setStage(it == difficulties.end() ? 1 : static_cast<unsigned int>(it - difficulties.begin()));
                    }
                }
                pushInGameMenuDialog(std::move(dialog));
            }
            else if (controlName == "CANCEL")
            {
                popInGameMenuDialog();
            }
            return;
        }

        if (panelName == "YESORNO")
        {
            if (controlName == "CHOICE1")
            {
                auto action = inGameMenuConfirmAction;
                inGameMenuConfirmAction = nullptr;
                popInGameMenuDialog();
                if (action)
                {
                    action();
                }
            }
            else if (controlName == "CHOICE2")
            {
                inGameMenuConfirmAction = nullptr;
                popInGameMenuDialog();
            }
            return;
        }

        if (panelName == "RESTART")
        {
            if (controlName == "Difficulty")
            {
                if (auto difficulty = inGameMenuDialogs.back()->find<UiStagedButton>("Difficulty"))
                {
                    difficulty->get().setStage((difficulty->get().getStage() + 1) % 3);
                }
            }
            else if (controlName == "RESTART")
            {
                if (gameParameters.campaign)
                {
                    if (auto difficulty = inGameMenuDialogs.back()->find<UiStagedButton>("Difficulty"))
                    {
                        static const std::array<std::string, 3> difficulties{"Easy", "Medium", "Hard"};
                        gameParameters.campaign->difficulty = difficulties[difficulty->get().getStage() % difficulties.size()];
                    }
                }
                restartGame();
            }
            else if (controlName == "CANCEL")
            {
                popInGameMenuDialog();
            }
            return;
        }

        if (panelName == "HELP")
        {
            if (controlName == "OK")
            {
                popInGameMenuDialog();
            }
            else if (controlName == "Page")
            {
                // staged buttons are data-owned: the click message
                // cycles the stage here, as in the main menu
                if (auto page = inGameMenuDialogs.back()->find<UiStagedButton>("Page"))
                {
                    page->get().setStage((page->get().getStage() + 1) % 3);
                }
                populateHelpDialog(*inGameMenuDialogs.back());
            }
            return;
        }

        if (panelName == "PREFS")
        {
            if (controlName == "PREV")
            {
                // retail PREV (labelled "OK") commits live settings and
                // writes them persistently ("Interface Type" registry
                // value in retail; rwe.cfg here)
                commitInterfaceModePreference();
                popInGameMenuDialogThrough("PREFS");
            }
            else if (controlName == "CANCEL")
            {
                // restores the options-entry snapshot, no persistence
                sceneContext.globalConfig->leftClickInterfaceMode = inGameMenuInterfaceSnapshot;
                popInGameMenuDialogThrough("PREFS");
            }
            else if (controlName == "SPEEDS")
            {
                // PREFS's INTERFACE-labelled gadget is named SPEEDS and
                // opens the stock SPEEDSRT screen as a non-modal
                // companion pane: the PREFS strip stays interactive
                // beside it (retail 0x45ed50 composes the same way)
                while (!inGameMenuDialogs.empty() && inGameMenuDialogCompanions.back())
                {
                    popInGameMenuDialog();
                }
                auto dialog = createInGameMenuDialog("SPEEDSRT");
                populateInterfaceDialog(*dialog);
                pushInGameMenuDialog(std::move(dialog), true);
            }
            return;
        }

        if (panelName == "SPEEDSRT")
        {
            auto setInterfaceStage = [this]() {
                if (auto leftClick = inGameMenuDialogs.back()->find<UiStagedButton>("LEFTCLICK"))
                {
                    leftClick->get().setStage(sceneContext.globalConfig->leftClickInterfaceMode ? 0 : 1);
                }
            };

            if (controlName == "LEFTCLICK")
            {
                sceneContext.globalConfig->leftClickInterfaceMode = !sceneContext.globalConfig->leftClickInterfaceMode;
                setInterfaceStage();
            }
            else if (controlName == "RESTORE")
            {
                sceneContext.globalConfig->leftClickInterfaceMode = true;
                setInterfaceStage();
            }
            else if (controlName == "UNDO")
            {
                sceneContext.globalConfig->leftClickInterfaceMode = inGameMenuInterfaceSnapshot;
                setInterfaceStage();
            }
            return;
        }

        if (panelName == "BRIEFING")
        {
            if (controlName == "OK")
            {
                popInGameMenuDialogThrough(panelName);
            }
            else if (controlName == "TextRegion" || controlName == "MOREBAR")
            {
                // retail's briefing pager plays Options and advances on
                // any TextRegion/MOREBAR click, wrapping at the end
                uiFactory.playButtonSound("OPTIONS");
                if (inGameMenuBriefingPages.size() > 1)
                {
                    inGameMenuBriefingPage = (inGameMenuBriefingPage + 1) % inGameMenuBriefingPages.size();
                    for (auto& dialog : inGameMenuDialogs)
                    {
                        if (dialog->getName() == "BRIEFING")
                        {
                            applyBriefingPage(*dialog);
                        }
                    }
                }
            }
            return;
        }

        if (panelName == "GAMEOPTIONS")
        {
            if (controlName == "OK")
            {
                popInGameMenuDialogThrough(panelName);
            }
            return;
        }
    }

    UnitState& GameScene::getUnit(UnitId id)
    {
        return simulation.getUnitState(id);
    }

    const UnitState& GameScene::getUnit(UnitId id) const
    {
        return simulation.getUnitState(id);
    }

    std::optional<std::reference_wrapper<UnitState>> GameScene::tryGetUnit(UnitId id)
    {
        return simulation.tryGetUnitState(id);
    }

    std::optional<std::reference_wrapper<const UnitState>> GameScene::tryGetUnit(UnitId id) const
    {
        return simulation.tryGetUnitState(id);
    }

    GamePlayerInfo& GameScene::getPlayer(PlayerId player)
    {
        return simulation.getPlayer(player);
    }

    const GamePlayerInfo& GameScene::getPlayer(PlayerId player) const
    {
        return simulation.getPlayer(player);
    }

    bool GameScene::isEnemy(UnitId id) const
    {
        // retail's alliedWith test: units owned by a different player
        // in the same ally group are friendly, not enemies -- this
        // gates the attack/reclaim/capture contexts against allies and
        // admits them to the guard/repair contexts
        return !simulation.areAllied(localPlayerId, getUnit(id).owner);
    }

    bool GameScene::isFriendly(UnitId id) const
    {
        return !isEnemy(id);
    }

    void GameScene::updateProjectiles()
    {
        for (auto& [projectileId, projectile] : simulation.projectiles)
        {
            const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(projectile.weaponType);
            auto& renderInfo = projectileRenderInfos[projectileId];

            // emit smoke trail
            if (weaponMediaInfo.smokeTrail)
            {
                auto gameTime = getGameTime();
                if (gameTime > projectile.lastSmoke + *weaponMediaInfo.smokeTrail)
                {
                    createLightSmoke(simVectorToFloat(projectile.position));
                    projectile.lastSmoke = gameTime;
                }
            }
        }
    }

    void GameScene::processSimEvents()
    {
        for (const auto& event : simulation.events)
        {
            match(
                event,
                [&](const FireWeaponEvent& e) {
                    const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(e.weaponType);

                    if (e.shotNumber == 0 || weaponMediaInfo.soundTrigger)
                    {
                        playWeaponStartSound(simVectorToFloat(e.firePoint), e.weaponType);
                    }

                    if (e.shotNumber == 0 && weaponMediaInfo.startSmoke)
                    {
                        createWeaponSmoke(simVectorToFloat(e.firePoint));
                    }
                },
                [&](const UnitArrivedEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Arrived1);
                    }
                },
                [&](const UnitActivatedEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Activate);

                        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit && *selectedUnit == e.unitId)
                        {
                            onOff.next(true);
                        }
                    }
                },
                [&](const UnitDeactivatedEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Deactivate);

                        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit && *selectedUnit == e.unitId)
                        {
                            onOff.next(false);
                        }
                    }
                },
                [&](const UnitCompleteEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::UnitComplete);
                    }
                },
                [&](const EmitParticleFromPieceEvent& e) {
                    if (!simulation.unitExists(e.unitId))
                    {
                        return;
                    }

                    switch (e.sfxType)
                    {
                        case EmitParticleFromPieceEvent::SfxType::LightSmoke:
                            emitLightSmokeFromPiece(e.unitId, e.pieceName);
                            break;
                        case EmitParticleFromPieceEvent::SfxType::BlackSmoke:
                            emitBlackSmokeFromPiece(e.unitId, e.pieceName);
                            break;
                        case EmitParticleFromPieceEvent::SfxType::Wake1:
                            emitWake1FromPiece(e.unitId, e.pieceName);
                            break;
                        default:
                            throw std::logic_error("unknown particle type");
                    }
                },
                [&](const UnitSpawnedEvent& e) {
                    // initialise local-player-specific UI data
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        const auto& unitDefinition = simulation.unitDefinitions.at(unit->get().unitType);
                        unitGuiInfos.insert_or_assign(e.unitId, initialUnitGuiInfo(unitDefinition, unitDefinitionHasStockpileWeapon(simulation, unitDefinition)));
                    }
                },

                [&](const UnitDiedEvent& e) {
                    if (!e.explosionWeapon.empty())
                    {
                        switch (e.deathType)
                        {
                            case UnitDiedEvent::DeathType::NormalExploded:
                                doProjectileImpact(e.position, e.explosionWeapon, ImpactType::Normal);
                                break;
                            case UnitDiedEvent::DeathType::WaterExploded:
                                doProjectileImpact(e.position, e.explosionWeapon, ImpactType::Water);
                                break;
                            case UnitDiedEvent::DeathType::Deleted:
                                // do nothing
                                break;
                        }
                    }

                    deselectUnit(e.unitId);

                    if (hoveredUnit && *hoveredUnit == e.unitId)
                    {
                        hoveredUnit = std::nullopt;
                    }

                    unitGuiInfos.erase(e.unitId);
                },
                [&](const UnitStartedBuildingEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Build);
                    }
                },
                [&](const UnitStartedRepairingEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Repair);
                    }
                },
                [&](const UnitStartedReclaimingEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        // original TA maps the reclaim work loop to the
                        // unit's "working" sound (working=reclaim1 in SOUND.TDF)
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Working);
                    }
                },
                [&](const UnitStartedCapturingEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        // retail's unit-response table gives capture
                        // its own sound key (TotalA.exe state table
                        // 0x5086e8 index 15 -> SOUND.TDF
                        // "capture"=capture1/2), dispatched by the
                        // unit-event handler at 0x48b1aa
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::Capture);
                    }
                },
                [&](const UnitSelfDestructCountdownEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        static const UnitSoundType countdownSounds[] = {
                            UnitSoundType::Count0,
                            UnitSoundType::Count1,
                            UnitSoundType::Count2,
                            UnitSoundType::Count3,
                            UnitSoundType::Count4,
                            UnitSoundType::Count5,
                        };
                        const auto index = std::clamp(e.secondsRemaining, 0, 5);
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, countdownSounds[index]);
                    }
                },
                [&](const UnitSelfDestructCancelledEvent& e) {
                    auto unit = tryGetUnit(e.unitId);
                    if (unit)
                    {
                        playUnitNotificationSound(unit->get().owner, unit->get().unitType, UnitSoundType::CancelDestruct);
                    }
                },
                [&](const ProjectileSpawnedEvent& e) {
                    projectileRenderInfos.insert({e.projectileId, ProjectileRenderInfo{getGameTime()}});
                },
                [&](const FeatureEffectEvent& e) {
                    if (e.ignition)
                    {
                        if (auto sound = gameMediaDatabase.tryGetSoundHandle("treeburn"))
                        {
                            playSoundAt(simVectorToFloat(e.position), *sound);
                        }
                    }
                    else { createLightSmoke(simVectorToFloat(e.position)); }
                },
                [&](const ProjectileDiedEvent& e) {
                    const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(e.weaponType);
                    if (weaponMediaInfo.endSmoke)
                    {
                        createLightSmoke(simVectorToFloat(e.position));
                    }

                    projectileRenderInfos.erase(e.projectileId);

                    switch (e.deathType)
                    {
                        case ProjectileDiedEvent::DeathType::NormalImpact:
                            doProjectileImpact(e.position, e.weaponType, ImpactType::Normal);
                            break;
                        case ProjectileDiedEvent::DeathType::WaterImpact:
                            doProjectileImpact(e.position, e.weaponType, ImpactType::Water);
                            break;
                        case ProjectileDiedEvent::DeathType::OutOfBounds:
                        case ProjectileDiedEvent::DeathType::EndOfLife:
                            // do nothing
                            break;
                    }
                });
        }

        simulation.events.clear();
    }

    void GameScene::updateFlashes()
    {
        flashes.erase(
            std::remove_if(
                flashes.begin(),
                flashes.end(),
                [&](const auto& flash) { return flash.isFinished(simulation.gameTime); }),
            flashes.end());
    }

    void GameScene::doProjectileImpact(const SimVector& position, const std::string& weaponType, ImpactType impactType)
    {
        playWeaponImpactSound(simVectorToFloat(position), weaponType, impactType);
        spawnWeaponImpactExplosion(simVectorToFloat(position), weaponType, impactType);
    }

    void GameScene::createLightSmoke(const Vector3f& position)
    {
        spawnSmoke(position, "FX", "smoke 1", ParticleFinishTimeEndOfFrames(), GameTime(2));
    }

    void GameScene::createWeaponSmoke(const Vector3f& position)
    {
        auto anim = sceneContext.textureService->getGafEntry("anims/FX.GAF", "smoke 1");
        spawnSmoke(position, "FX", "smoke 1", ParticleFinishTimeFixedTime{simulation.gameTime + GameTime(30)}, GameTime(15));
    }

    void GameScene::emitLightSmokeFromPiece(UnitId unitId, const std::string& pieceName)
    {
        auto position = simulation.getUnitPiecePosition(unitId, pieceName);
        spawnSmoke(simVectorToFloat(position), "FX", "smoke 1", ParticleFinishTimeEndOfFrames(), GameTime(2));
    }

    void GameScene::emitBlackSmokeFromPiece(UnitId unitId, const std::string& pieceName)
    {
        auto position = simulation.getUnitPiecePosition(unitId, pieceName);
        spawnSmoke(simVectorToFloat(position), "FX", "smoke 2", ParticleFinishTimeEndOfFrames(), GameTime(2));
    }

    float randomFloat(float low, float high)
    {
        return low + ((static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * (high - low));
    }

    void GameScene::emitWake1FromPiece(UnitId unitId, const std::string& pieceName)
    {
        const auto& unit = getUnit(unitId);
        const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
        auto pieceTransform = toFloatMatrix(simulation.getUnitPieceTransform(unitId, pieceName));
        const auto& pieceMesh = gameMediaDatabase.getUnitPieceMesh(unitDefinition.objectName, pieceName).value().get();
        auto spawnPosition = pieceTransform * pieceMesh.firstVertexPosition;
        auto otherVertexPosition = pieceTransform * pieceMesh.secondVertexPosition;

        // Travel at around 10px per second -- so 1/3rd of a pixel per tick.
        // Last for about 4 seconds, travelling 40 pixels in total.
        auto velocity = (otherVertexPosition - spawnPosition).normalized() / 3.0f;
        auto duration = GameTime(120);

        const auto variation = 4.0f;

        auto spawnPosition1 = Vector3f(
            spawnPosition.x + randomFloat(-variation, variation),
            spawnPosition.y,
            spawnPosition.z + randomFloat(-variation, variation));

        auto spawnPosition2 = Vector3f(
            spawnPosition.x + randomFloat(-variation, variation),
            spawnPosition.y,
            spawnPosition.z + randomFloat(-variation, variation));

        spawnWake(spawnPosition1, velocity, duration);
        spawnWake(spawnPosition2, velocity, duration);
    }

    void GameScene::modifyBuildQueue(UnitId unitId, const std::string& unitType, int count)
    {
        auto unit = tryGetUnit(unitId);
        if (unit)
        {
            applyBasicPlayerUnitCommand(unit->get(), PlayerUnitCommand::ModifyBuildQueue{count, unitType});

            updateUnconfirmedBuildQueueDelta(unitId, unitType, -count);
            refreshBuildGuiTotal(unitId, unitType);
        }
    }

    void GameScene::modifyWeaponQueue(UnitId unitId, int count)
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return;
        }

        applyModifyWeaponQueue(unit->get(), count);

        if (auto it = unconfirmedWeaponQueueDelta.find(unitId); it != unconfirmedWeaponQueueDelta.end())
        {
            it->second -= count;
            if (it->second == 0)
            {
                unconfirmedWeaponQueueDelta.erase(it);
            }
        }
        refreshWeaponGuiTotal(unitId);
    }

    struct CorpseSpawnInfo
    {
        std::string featureName;
        SimVector position;
        SimAngle rotation;
    };

    void GameScene::processActions()
    {
        for (auto& a : actions)
        {
            if (!a)
            {
                continue;
            }

            if (sceneTime < a->triggerTime)
            {
                continue;
            }

            a->callback();
            a = std::nullopt;
        }
    }

    void GameScene::processPlayerCommands(const std::vector<std::pair<PlayerId, std::vector<PlayerCommand>>>& commands)
    {
        for (const auto& [_, playerCommands] : commands)
        {
            for (const auto& command : playerCommands)
            {
                processPlayerCommand(command);
            }
        }
    }

    void GameScene::attachOrdersMenuEventHandlers()
    {
        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "ATTACK"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<AttackCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "MOVE"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<MoveCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "DEFEND"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<GuardCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "REPAIR"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<RepairCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "RECLAIM"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<ReclaimCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "PATROL"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<PatrolCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "BLAST"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<CommandFireCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "CAPTURE"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<CaptureCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "LOAD"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<LoadCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "UNLOAD"))
        {
            p->get().addSubscription(cursorMode.subscribe([&p = p->get()](const auto& v) { p.setToggledOn(std::holds_alternative<UnloadCursorMode>(v)); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "FIREORD"))
        {
            p->get().addSubscription(fireOrders.subscribe([&p = p->get()](const auto& v) {
                switch (v)
                {
                    case UnitFireOrders::HoldFire:
                        p.setStage(0);
                        break;
                    case UnitFireOrders::ReturnFire:
                        p.setStage(1);
                        break;
                    case UnitFireOrders::FireAtWill:
                        p.setStage(2);
                        break;
                    default:
                        throw std::logic_error("Invalid FireOrders value");
                } }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "ONOFF"))
        {
            p->get().addSubscription(onOff.subscribe([&p = p->get()](const auto& v) { p.setStage(v ? 1 : 0); }));
        }

        if (auto p = findWithSidePrefix<UiStagedButton>(*currentPanel, "CLOAK"))
        {
            p->get().addSubscription(cloakStage.subscribe([&p = p->get()](const auto& v) {
                // mask value 3 (no cloakable selected) greys the strip
                // via setEnabled, it is never pushed as a stage; clamp
                // the mixed stage for GUI files with fewer stages
                auto stageCount = p.getStageCount();
                if (stageCount > 0)
                {
                    p.setStage(std::min(v, stageCount - 1));
                } }));
        }

        // retail's orders-panel pass greys or deactivates gadgets the
        // selection cannot use (TotalA.exe 0x41a130-0x41a48d, fed by
        // the OR-aggregated capability mask built at 0x41b2e0)
        {
            std::vector<const UnitDefinition*> selectedDefinitions;
            for (const auto& unitId : selectedUnits)
            {
                if (auto unit = simulation.tryGetUnitState(unitId))
                {
                    selectedDefinitions.push_back(&simulation.unitDefinitions.at(unit->get().unitType));
                }
            }
            const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
            applyOrdersPanelCapabilities(*currentPanel, sidePrefix, aggregateOrdersPanelCapabilities(selectedDefinitions, simulation.weaponDefinitions));
        }

        currentPanel->groupMessages().subscribe([this](const auto& msg) {
            if (auto activateMessage = std::get_if<ActivateMessage>(&msg.message); activateMessage != nullptr)
            {
                onMessage(msg.controlName, activateMessage->type);
            } });
    }

    UnitFireOrders nextFireOrders(UnitFireOrders orders)
    {
        switch (orders)
        {
            case UnitFireOrders::HoldFire:
                return UnitFireOrders::ReturnFire;
            case UnitFireOrders::ReturnFire:
                return UnitFireOrders::FireAtWill;
            case UnitFireOrders::FireAtWill:
                return UnitFireOrders::HoldFire;
            default:
                throw std::logic_error("Invalid UnitFireOrders value");
        }
    }

    void GameScene::onMessage(const std::string& message, ActivateMessage::Type type)
    {
        if (matchesWithSidePrefix("ATTACK", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<AttackCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(AttackCursorMode());
            }
        }
        else if (matchesWithSidePrefix("MOVE", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<MoveCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(MoveCursorMode());
            }
        }
        else if (matchesWithSidePrefix("DEFEND", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<GuardCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(GuardCursorMode());
            }
        }
        else if (matchesWithSidePrefix("REPAIR", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<RepairCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(RepairCursorMode());
            }
        }
        else if (matchesWithSidePrefix("RECLAIM", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<ReclaimCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(ReclaimCursorMode());
            }
        }
        else if (matchesWithSidePrefix("PATROL", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<PatrolCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(PatrolCursorMode());
            }
        }
        else if (matchesWithSidePrefix("CAPTURE", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<CaptureCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(CaptureCursorMode());
            }
        }
        else if (matchesWithSidePrefix("LOAD", message))
        {
            // retail plays `immediateorders` for LOAD (TotalA.exe
            // 0x41a0ea), unlike the other order-panel modes
            if (sounds.immediateOrders)
            {
                sceneContext.audioService->playSound(*sounds.immediateOrders);
            }

            if (std::holds_alternative<LoadCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanLoad(simulation, id); }))
            {
                // the retail gadget handler gates mode 6 on a
                // selected unit carrying the CanLoad bit (0x41a4xx)
                cursorMode.next(LoadCursorMode());
            }
        }
        else if (matchesWithSidePrefix("UNLOAD", message))
        {
            // retail plays `specialorders` for UNLOAD (0x41a072)
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<UnloadCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else if (std::any_of(selectedUnits.begin(), selectedUnits.end(), [&](const auto& id) { return unitCanLoad(simulation, id); }))
            {
                // retail's UNLOAD gadget stays enabled whenever a
                // transport is selected -- cargo presence is not
                // checked at arm time (TotalA.exe 0x41a403 block);
                // an empty transport simply drops nothing
                cursorMode.next(UnloadCursorMode());
            }
        }
        else if (matchesWithSidePrefix("BLAST", message))
        {
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            if (std::holds_alternative<CommandFireCursorMode>(cursorMode.getValue()))
            {
                cursorMode.next(NormalCursorMode());
            }
            else
            {
                cursorMode.next(CommandFireCursorMode());
            }
        }
        else if (matchesWithSidePrefix("STOP", message))
        {
            if (sounds.immediateOrders)
            {
                sceneContext.audioService->playSound(*sounds.immediateOrders);
            }

            for (const auto& selectedUnit : selectedUnits)
            {
                cursorMode.next(NormalCursorMode());
                localPlayerStopUnit(selectedUnit);
            }
        }
        else if (matchesWithSidePrefix("FIREORD", message))
        {
            if (sounds.setFireOrders)
            {
                sceneContext.audioService->playSound(*sounds.setFireOrders);
            }

            for (const auto& selectedUnit : selectedUnits)
            {
                // FIXME: should set all to a consistent single fire order rather than advancing all
                auto& u = getUnit(selectedUnit);
                auto newFireOrders = nextFireOrders(u.fireOrders);
                localPlayerSetFireOrders(selectedUnit, newFireOrders);
            }
        }
        else if (matchesWithSidePrefix("ONOFF", message))
        {
            if (sounds.immediateOrders)
            {
                sceneContext.audioService->playSound(*sounds.immediateOrders);
            }

            for (const auto& selectedUnit : selectedUnits)
            {
                auto& u = getUnit(selectedUnit);
                auto newOnOff = !u.activated;
                localPlayerSetOnOff(selectedUnit, newOnOff);
            }
        }
        else if (matchesWithSidePrefix("CLOAK", message))
        {
            // retail 0x41a72e: `specialorders` sound, then mask bits
            // 3:4 == 0 issues CLOAK_ON and any other value CLOAK_OFF --
            // the mixed stage (2) can only decloak, so multi-selected
            // cloakables cannot be cloaked through the strip
            if (sounds.specialOrders)
            {
                sceneContext.audioService->playSound(*sounds.specialOrders);
            }

            const auto stage = cloakPanelStage(simulation, selectedUnits);
            const auto on = stage == 0;
            for (const auto& selectedUnit : selectedUnits)
            {
                localPlayerSetCloak(selectedUnit, on);
            }
        }
        else if (matchesWithSidePrefix("NEXT", message))
        {
            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                if (sounds.nextBuildMenu)
                {
                    sceneContext.audioService->playSound(*sounds.nextBuildMenu);
                }

                const auto& unit = getUnit(*selectedUnit);
                auto& guiInfo = unitGuiInfos.at(*selectedUnit);
                auto pages = getBuildPageCount(builderGuisDatabase, unit.unitType);
                guiInfo.currentBuildPage = (guiInfo.currentBuildPage + 1) % pages;

                auto buildPanelDefinition = getBuilderGui(builderGuisDatabase, unit.unitType, guiInfo.currentBuildPage);
                if (buildPanelDefinition)
                {
                    setNextPanel(createBuildPanel(unit.unitType + std::to_string(guiInfo.currentBuildPage + 1), *buildPanelDefinition, unit.getBuildQueueTotals()));
                }
            }
        }
        else if (matchesWithSidePrefix("PREV", message))
        {
            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                if (sounds.nextBuildMenu)
                {
                    sceneContext.audioService->playSound(*sounds.nextBuildMenu);
                }

                const auto& unit = getUnit(*selectedUnit);
                auto& guiInfo = unitGuiInfos.at(*selectedUnit);
                auto pages = getBuildPageCount(builderGuisDatabase, unit.unitType);
                assert(pages != 0);
                guiInfo.currentBuildPage = guiInfo.currentBuildPage == 0 ? pages - 1 : guiInfo.currentBuildPage - 1;

                auto buildPanelDefinition = getBuilderGui(builderGuisDatabase, unit.unitType, guiInfo.currentBuildPage);
                if (buildPanelDefinition)
                {
                    setNextPanel(createBuildPanel(unit.unitType + std::to_string(guiInfo.currentBuildPage + 1), *buildPanelDefinition, unit.getBuildQueueTotals()));
                }
            }
        }
        else if (matchesWithSidePrefix("BUILD", message))
        {
            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                if (sounds.buildButton)
                {
                    sceneContext.audioService->playSound(*sounds.buildButton);
                }

                const auto& unit = getUnit(*selectedUnit);
                auto& guiInfo = unitGuiInfos.at(*selectedUnit);
                guiInfo.section = UnitGuiInfo::Section::Build;

                auto buildPanelDefinition = getBuilderGui(builderGuisDatabase, unit.unitType, guiInfo.currentBuildPage);
                if (buildPanelDefinition)
                {
                    setNextPanel(createBuildPanel(unit.unitType + std::to_string(guiInfo.currentBuildPage + 1), *buildPanelDefinition, unit.getBuildQueueTotals()));
                }
            }
        }
        else if (matchesWithSidePrefix("ORDERS", message))
        {
            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                if (sounds.ordersButton)
                {
                    sceneContext.audioService->playSound(*sounds.ordersButton);
                }

                auto& guiInfo = unitGuiInfos.at(*selectedUnit);
                guiInfo.section = UnitGuiInfo::Section::Orders;

                const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
                setNextPanel(uiFactory.panelFromGuiFile(sidePrefix + "GEN"));
            }
        }
        else if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit && getWeaponBuildGadgetName(*selectedUnit) == message)
        {
            // stockpile weapon button (GUI commonattribs=8, e.g.
            // ARMMAKENUKE/ARMMAKEANTI): primary click queues one round,
            // shift queues five, secondary cancels -- the same
            // click-count convention as factory build buttons
            if (sounds.addBuild)
            {
                sceneContext.audioService->playSound(*sounds.addBuild);
            }

            int count = (isShiftDown() ? 5 : 1) * (type == ActivateMessage::Type::Primary ? 1 : -1);
            localPlayerModifyWeaponQueue(*selectedUnit, count);
        }
        else if (isValidUnitType(simulation, message))
        {
            if (sounds.addBuild)
            {
                sceneContext.audioService->playSound(*sounds.addBuild);
            }

            if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
            {
                const auto& unit = getUnit(*selectedUnit);
                const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
                if (unitDefinition.isMobile)
                {
                    cursorMode.next(BuildCursorMode{message});
                }
                else
                {
                    int count = (isShiftDown() ? 5 : 1) * (type == ActivateMessage::Type::Primary ? 1 : -1);
                    localPlayerModifyBuildQueue(*selectedUnit, message, count);
                }
            }
        }
    }

    bool GameScene::matchesWithSidePrefix(const std::string& suffix, const std::string& value) const
    {
        for (const auto& [_, side] : *sceneContext.sideData)
        {
            if (side.namePrefix + suffix == value)
            {
                return true;
            }
        }

        return false;
    }

    std::optional<UnitId> GameScene::getSingleSelectedUnit() const
    {
        return selectedUnits.size() == 1
            ? std::make_optional(*selectedUnits.begin())
            : std::nullopt;
    }

    void GameScene::selectUnitsInBandbox(const DiscreteRect& box)
    {
        const auto cameraPos = worldCameraState.getRoundedPosition();
        auto cameraBox = box.translate(-cameraPos.x, -cameraPos.z);
        const auto& matrix = computeViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height());
        std::unordered_set<UnitId> units;

        for (const auto& e : simulation.units)
        {
            const auto& unitDefinition = simulation.unitDefinitions.at(e.second.unitType);
            if (!e.second.isSelectableBy(unitDefinition, localPlayerId))
            {
                continue;
            }

            const auto& worldPos = e.second.position;
            auto clipPos = matrix * simVectorToFloat(worldPos);
            Point viewportPos = worldViewport.toViewportSpace(clipPos.x, clipPos.y);
            if (!cameraBox.contains(viewportPos))
            {
                continue;
            }

            units.insert(e.first);
        }

        if (isShiftDown())
        {
            toggleUnitSelection(units);
        }
        else
        {
            replaceUnitSelection(units);
        }
    }

    void GameScene::addSelectableUnitsToSelection(const std::function<bool(const UnitState&, const UnitDefinition&)>& predicate)
    {
        // the retail Ctrl+A/Z/P/R/W handlers iterate the player unit
        // list and set each matching unit's select bit without
        // touching the rest (TotalA.exe 0x48bd50, 0x48be00, 0x48bf30)
        std::unordered_set<UnitId> units(selectedUnits);
        for (const auto& [unitId, unit] : simulation.units)
        {
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            if (unit.isSelectableBy(unitDefinition, localPlayerId) && predicate(unit, unitDefinition))
            {
                units.insert(unitId);
            }
        }
        replaceUnitSelection(units);
    }

    void GameScene::selectAllEligibleUnits()
    {
        addSelectableUnitsToSelection([](const UnitState&, const UnitDefinition&) {
            return true;
        });
    }

    void GameScene::selectUnitsOnScreen()
    {
        // retail Ctrl+S clears every unit's select bit, then sets it on
        // eligible owned units whose screen position is inside the world
        // viewport (TotalA.exe 0x48c030)
        const auto& matrix = computeViewProjectionMatrix(worldCameraState, worldViewport.width(), worldViewport.height());
        std::unordered_set<UnitId> units;
        for (const auto& [unitId, unit] : simulation.units)
        {
            const auto& unitDefinition = simulation.unitDefinitions.at(unit.unitType);
            if (!unit.isSelectableBy(unitDefinition, localPlayerId))
            {
                continue;
            }

            const auto clipPos = matrix * simVectorToFloat(unit.position);
            const auto viewportPos = worldViewport.toViewportSpace(clipPos.x, clipPos.y);
            if (viewportPos.x >= 0 && viewportPos.x < static_cast<int>(worldViewport.width())
                && viewportPos.y >= 0 && viewportPos.y < static_cast<int>(worldViewport.height()))
            {
                units.insert(unitId);
            }
        }
        replaceUnitSelection(units);
    }

    void GameScene::selectUnitsOfSelectedTypes()
    {
        // retail Ctrl+Z (0x48be00) adds every eligible owned unit whose
        // type matches a currently-selected unit
        std::unordered_set<std::string> types;
        for (const auto& unitId : selectedUnits)
        {
            if (auto unit = tryGetUnit(unitId); unit)
            {
                types.insert(unit->get().unitType);
            }
        }
        if (types.empty())
        {
            return;
        }
        addSelectableUnitsToSelection([&types](const UnitState& unit, const UnitDefinition&) {
            return types.count(unit.unitType) != 0;
        });
    }

    void GameScene::selectArmedAircraft()
    {
        // readme.txt: Ctrl+P selects all aircraft that have weapons
        addSelectableUnitsToSelection([](const UnitState&, const UnitDefinition& def) {
            return def.canFly && def.canAttack;
        });
    }

    void GameScene::selectRadarSonarUnits()
    {
        // readme.txt: Ctrl+R selects all radar, radar jammers, sonar,
        // and sonar jammers
        addSelectableUnitsToSelection([](const UnitState&, const UnitDefinition& def) {
            return def.radarDistance > 0_ss || def.sonarDistance > 0_ss
                || def.radarDistanceJam > 0_ss || def.sonarDistanceJam > 0_ss;
        });
    }

    void GameScene::selectArmedMobileUnits()
    {
        // readme.txt: Ctrl+W selects all mobile units that have weapons
        // except the Commander
        addSelectableUnitsToSelection([](const UnitState&, const UnitDefinition& def) {
            return def.isMobile && def.canAttack && !def.commander;
        });
    }

    void GameScene::toggleUnitSelection(const rwe::UnitId& unitId)
    {
        auto it = selectedUnits.find(unitId);
        if (it != selectedUnits.end())
        {
            deselectUnit(unitId);
            return;
        }

        selectAdditionalUnit(unitId);
    }

    void GameScene::toggleUnitSelection(const std::unordered_set<UnitId>& units)
    {
        std::unordered_set<UnitId> newSelection(selectedUnits);
        for (const auto& unitId : units)
        {
            auto [it, inserted] = newSelection.insert(unitId);
            if (!inserted)
            {
                newSelection.erase(it);
            }
        }

        replaceUnitSelection(newSelection);
    }

    void GameScene::selectAdditionalUnit(const rwe::UnitId& unitId)
    {
        selectedUnits.insert(unitId);

        const auto& unit = getUnit(unitId);
        auto selectionSound = getSound(simulation, gameMediaDatabase, unit.unitType, UnitSoundType::Select1);
        if (selectionSound)
        {
            playUiSound(*selectionSound);
        }

        onSelectedUnitsChanged();
    }

    void GameScene::replaceUnitSelection(const UnitId& unitId)
    {
        selectedUnits.clear();
        selectAdditionalUnit(unitId);
    }

    void GameScene::deselectUnit(const UnitId& unitId)
    {
        selectedUnits.erase(unitId);
        onSelectedUnitsChanged();
    }

    void GameScene::clearUnitSelection()
    {
        selectedUnits.clear();
        onSelectedUnitsChanged();
    }

    void GameScene::replaceUnitSelection(const std::unordered_set<UnitId>& units)
    {
        selectedUnits = units;

        if (selectedUnits.size() == 1)
        {
            const auto& unit = getUnit(*units.begin());
            auto selectionSound = getSound(simulation, gameMediaDatabase, unit.unitType, UnitSoundType::Select1);
            if (selectionSound)
            {
                playUiSound(*selectionSound);
            }
        }
        else if (selectedUnits.size() > 0)
        {
            if (sounds.selectMultipleUnits)
            {
                playUiSound(*sounds.selectMultipleUnits);
            }
        }

        onSelectedUnitsChanged();
    }

    void GameScene::onSelectedUnitsChanged()
    {
        // dead units are removed from the sim before their death
        // events are processed, so the selection can hold stale ids
        purgeStaleSelectedUnits(selectedUnits, simulation);

        if (selectedUnits.empty())
        {
            const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
            setNextPanel(uiFactory.panelFromGuiFile(sidePrefix + "MAIN2"));
        }
        else if (auto unitId = getSingleSelectedUnit(); unitId)
        {
            const auto& unit = getUnit(*unitId);
            fireOrders.next(unit.fireOrders);
            onOff.next(unit.activated);
            cloakStage.next(cloakPanelStage(simulation, selectedUnits));

            const auto& guiInfo = getGuiInfo(*unitId);
            auto buildPanelDefinition = getBuilderGui(builderGuisDatabase, unit.unitType, guiInfo.currentBuildPage);
            if (guiInfo.section == UnitGuiInfo::Section::Build && buildPanelDefinition)
            {
                setNextPanel(createBuildPanel(unit.unitType + std::to_string(guiInfo.currentBuildPage + 1), *buildPanelDefinition, unit.getBuildQueueTotals()));
            }
            else
            {
                const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
                setNextPanel(uiFactory.panelFromGuiFile(sidePrefix + "GEN"));
            }
        }
        else
        {
            const auto& sidePrefix = sceneContext.sideData->at(getPlayer(localPlayerId).side).namePrefix;
            setNextPanel(uiFactory.panelFromGuiFile(sidePrefix + "GEN"));
        }
    }

    const UnitGuiInfo& GameScene::getGuiInfo(const UnitId& unitId) const
    {
        auto it = unitGuiInfos.find(unitId);
        if (it == unitGuiInfos.end())
        {
            throw std::logic_error("Gui info not found for unit " + std::to_string(unitId.value));
        }
        return it->second;
    }

    void GameScene::setNextPanel(std::unique_ptr<UiPanel>&& panel)
    {
        nextPanel = std::move(panel);
    }

    void GameScene::refreshBuildGuiTotal(UnitId unitId, const std::string& unitType)
    {
        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit == unitId)
        {
            const auto& unit = getUnit(*selectedUnit);
            auto total = unit.getBuildQueueTotal(unitType) + getUnconfirmedBuildQueueCount(unitId, unitType);
            auto button = currentPanel->find<UiStagedButton>(unitType);
            if (button)
            {
                button->get().setLabel(total > 0 ? "+" + std::to_string(total) : "");
            }
        }
    }

    std::optional<std::string> GameScene::getWeaponBuildGadgetName(UnitId unitId) const
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return std::nullopt;
        }
        auto guiIt = unitGuiInfos.find(unitId);
        if (guiIt == unitGuiInfos.end())
        {
            return std::nullopt;
        }
        auto page = getBuilderGui(builderGuisDatabase, unit->get().unitType, guiIt->second.currentBuildPage);
        if (!page)
        {
            return std::nullopt;
        }
        for (const auto& e : page->get())
        {
            if ((e.common.commonAttribs & GuiCommonAttrib::WeaponBuildButton)
                && (e.common.attribs & GuiButtonAttrib::BehaviorBuildButton))
            {
                return e.common.name;
            }
        }
        return std::nullopt;
    }

    unsigned int GameScene::getWeaponStockpileTotal(UnitId unitId) const
    {
        auto unit = tryGetUnit(unitId);
        if (!unit)
        {
            return 0;
        }
        const auto& u = unit->get();
        unsigned int total = 0;
        for (const auto& weapon : u.weapons)
        {
            if (weapon && simulation.weaponDefinitions.at(weapon->weaponType).stockpile)
            {
                total += weapon->stockpileCount;
            }
        }
        for (const auto& order : u.backgroundOrders)
        {
            if (auto* buildWeapon = std::get_if<BuildWeaponOrder>(&order))
            {
                total += buildWeapon->remaining;
            }
        }
        return total;
    }

    void GameScene::refreshWeaponGuiTotal(UnitId unitId)
    {
        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit != unitId)
        {
            return;
        }
        auto gadget = getWeaponBuildGadgetName(unitId);
        if (!gadget)
        {
            return;
        }
        auto button = currentPanel->find<UiStagedButton>(*gadget);
        if (!button)
        {
            return;
        }
        int unconfirmed = 0;
        if (auto it = unconfirmedWeaponQueueDelta.find(unitId); it != unconfirmedWeaponQueueDelta.end())
        {
            unconfirmed = it->second;
        }
        auto total = static_cast<int>(getWeaponStockpileTotal(unitId)) + unconfirmed;
        button->get().setLabel(total > 0 ? "+" + std::to_string(total) : "");
    }

    void GameScene::updateUnconfirmedBuildQueueDelta(UnitId unitId, const std::string& unitType, int count)
    {
        auto it = unconfirmedBuildQueueDelta.find(unitId);
        if (it == unconfirmedBuildQueueDelta.end())
        {
            unconfirmedBuildQueueDelta.emplace(unitId, std::unordered_map<std::string, int>{{unitType, count}});
        }
        else
        {
            auto it2 = it->second.find(unitType);
            if (it2 == it->second.end())
            {
                it->second.emplace(unitType, count);
            }
            else
            {
                int newTotal = it2->second + count;
                if (newTotal != 0)
                {
                    it2->second = newTotal;
                }
                else
                {
                    it->second.erase(it2);
                }
            }
        }
    }

    int GameScene::getUnconfirmedBuildQueueCount(UnitId unitId, const std::string& unitType) const
    {
        auto it = unconfirmedBuildQueueDelta.find(unitId);
        if (it == unconfirmedBuildQueueDelta.end())
        {
            return 0;
        }

        auto it2 = it->second.find(unitType);
        if (it2 == it->second.end())
        {
            return 0;
        }

        return it2->second;
    }

    std::unique_ptr<UiPanel> GameScene::createBuildPanel(const std::string& guiName, const std::vector<GuiEntry>& buildPanelDefinition, const std::unordered_map<std::string, int>& totals)
    {
        auto panel = uiFactory.panelFromGuiFile(guiName, buildPanelDefinition);
        for (const auto& e : totals)
        {
            auto button = panel->find<UiStagedButton>(e.first);
            if (button)
            {
                button->get().setLabel("+" + std::to_string(e.second));
            }
        }

        // weapon build gadgets carry the stockpile counter (built
        // rounds + queued rounds), not a factory buildQueue total
        if (auto selectedUnit = getSingleSelectedUnit(); selectedUnit)
        {
            if (auto gadget = getWeaponBuildGadgetName(*selectedUnit); gadget)
            {
                if (auto button = panel->find<UiStagedButton>(*gadget); button)
                {
                    auto total = getWeaponStockpileTotal(*selectedUnit);
                    button->get().setLabel(total > 0 ? "+" + std::to_string(total) : "");
                }
            }
        }

        return panel;
    }

    void GameScene::processPlayerCommand(const PlayerCommand& playerCommand)
    {
        match(
            playerCommand,
            [&](const PlayerUnitCommand& c) {
                processUnitCommand(c);
            },
            [&](const PlayerPauseGameCommand&) {
                gamePaused = true;
            },
            [&](const PlayerUnpauseGameCommand&) {
                gamePaused = false;
            });
    }

    void GameScene::processUnitCommand(const PlayerUnitCommand& unitCommand)
    {
        match(
            unitCommand.command,
            [&](const PlayerUnitCommand::IssueOrder& c) {
                switch (c.issueKind)
                {
                    case PlayerUnitCommand::IssueOrder::IssueKind::Immediate:
                        issueUnitOrder(unitCommand.unit, c.order);
                        break;
                    case PlayerUnitCommand::IssueOrder::IssueKind::Queued:
                        enqueueUnitOrder(unitCommand.unit, c.order);
                        break;
                }
            },
            [&](const PlayerUnitCommand::ModifyBuildQueue& c) {
                modifyBuildQueue(unitCommand.unit, c.unitType, c.count);
            },
            [&](const PlayerUnitCommand::Stop&) {
                stopUnit(unitCommand.unit);
            },
            [&](const PlayerUnitCommand::RemoveOrder& c) {
                removeUnitOrderAt(unitCommand.unit, c.index);
            },
            [&](const PlayerUnitCommand::SetFireOrders& c) {
                setFireOrders(unitCommand.unit, c.orders);
            },
            [&](const PlayerUnitCommand::SetOnOff& c) {
                if (c.on)
                {
                    simulation.activateUnit(unitCommand.unit);
                }
                else
                {
                    simulation.deactivateUnit(unitCommand.unit);
                }
            },
            [&](const PlayerUnitCommand::SetCloak& c) {
                setCloak(unitCommand.unit, c.on);
            },
            [&](const PlayerUnitCommand::ToggleSelfDestruct&) {
                toggleSelfDestruct(unitCommand.unit);
            },
            [&](const PlayerUnitCommand::ModifyWeaponQueue& c) {
                modifyWeaponQueue(unitCommand.unit, c.count);
            });
    }

    bool GameScene::leftClickMode() const
    {
        return sceneContext.globalConfig->leftClickInterfaceMode;
    }

    void GameScene::spawnExplosion(const Vector3f& position, const AnimLocation& anim)
    {
        Particle particle;
        particle.position = position;
        particle.velocity = Vector3f(0.0f, 0.0f, 0.0f);
        particle.renderType = ParticleRenderTypeSprite{
            anim.gafName,
            anim.animName,
            ParticleFinishTimeEndOfFrames(),
            GameTime(2),
            false,
        };
        particle.startTime = simulation.gameTime;

        particles.push_back(particle);
    }

    void GameScene::spawnFlash(const Vector3f& position)
    {
        FlashEffect flash;
        flash.position = position;
        flash.startTime = simulation.gameTime;
        flash.duration = GameTime(15);
        flash.maxRadius = 30.0f;
        flash.color = Vector3f(1.0f, 1.0f, 1.0f);
        flash.maxIntensity = 1.0f;
        flashes.push_back(flash);
    }

    void GameScene::spawnSmoke(const Vector3f& position, const std::string& gaf, const std::string& anim, ParticleFinishTime duration, GameTime frameDuration)
    {
        Particle particle;
        particle.position = position;
        particle.velocity = Vector3f(0.0f, 0.5f, 0.0f);
        particle.renderType = ParticleRenderTypeSprite{
            gaf,
            anim,
            duration,
            frameDuration,
            true,
        };
        particle.startTime = simulation.gameTime;

        particles.push_back(particle);
    }

    void GameScene::spawnWake(const Vector3f& position, const Vector3f& velocity, GameTime duration)
    {
        Particle particle;
        particle.position = position;
        particle.velocity = velocity;
        particle.renderType = ParticleRenderTypeWake{
            simulation.gameTime + duration};
        particle.startTime = simulation.gameTime;

        particles.push_back(particle);
    }

    void GameScene::recreateWorldRenderTextures()
    {
        worldFrameBuffer = sceneContext.graphics->createFrameBuffer(worldViewport.width(), worldViewport.height());
        dodgeMask = sceneContext.graphics->createEmptyTexture(worldViewport.width(), worldViewport.height());
    }

    void GameScene::nudgeCamera(int millisecondsElapsed, const Rectangle2f& cameraConstraint, int directionX, int directionZ)
    {
        assert(directionX == 1 || directionX == 0 || directionX == -1);
        assert(directionZ == 1 || directionZ == 0 || directionZ == -1);

        // The player can only nudge the camera in free mode.
        // If the camera is in a different mode, try and transition out of it.
        cameraControlState = match(
            cameraControlState,
            [&](const CameraControlStateTrackingUnit&) -> CameraControlState {
                return CameraControlStateFree();
            },
            [&](const CameraControlStateFree& s) -> CameraControlState {
                return s;
            },
            [&](const CameraControlStateMiddleMousePan& s) -> CameraControlState {
                // Middle mouse pan takes precedence over nudging the camera.
                return s;
            });

        // Only nudge the camera if it is now in free mode.
        match(
            cameraControlState,
            [&](const CameraControlStateFree&) {
                const float speed = CameraPanSpeed * millisecondsElapsed / 1000.0f;

                auto dx = directionX * speed;
                auto dz = directionZ * speed;
                const auto& cameraPos = worldCameraState.position;
                auto newPos = cameraConstraint.clamp(Vector2f(cameraPos.x + dx, cameraPos.z + dz));

                worldCameraState.position = Vector3f(newPos.x, cameraPos.y, newPos.y);
            },
            [&](const CameraControlStateTrackingUnit&) {
                // do nothing
            },
            [&](const CameraControlStateMiddleMousePan&) {
                // do nothing
            });
    }

    void GameScene::relocateCamera(const Rectangle2f& cameraConstraint, float x, float z)
    {
        // The player can only relocate the camera in free mode.
        // If the camera is in a different mode, try and transition out of it.
        cameraControlState = match(
            cameraControlState,
            [&](const CameraControlStateTrackingUnit&) -> CameraControlState {
                return CameraControlStateFree();
            },
            [&](const CameraControlStateFree& s) -> CameraControlState {
                return s;
            },
            [&](const CameraControlStateMiddleMousePan& s) -> CameraControlState {
                // Middle mouse pan takes precedence over relocating the camera.
                return s;
            });

        auto newCameraPos = cameraConstraint.clamp(Vector2f(x, z));
        worldCameraState.position = Vector3f(newCameraPos.x, worldCameraState.position.y, newCameraPos.y);
    }

    std::optional<std::string> GameScene::getStockpileWeaponUnderCursor() const
    {
        auto selectedUnit = getSingleSelectedUnit();
        if (!selectedUnit)
        {
            return std::nullopt;
        }
        auto gadget = getWeaponBuildGadgetName(*selectedUnit);
        if (!gadget)
        {
            return std::nullopt;
        }

        auto cursorPosition = getMousePosition();
        auto control = currentPanel->findAtPosition<UiStagedButton>(cursorPosition.x, cursorPosition.y);
        if (!control || control->get().getName() != *gadget)
        {
            return std::nullopt;
        }

        // the gadget stockpiles the unit's stockpile weapon
        const auto& unit = getUnit(*selectedUnit);
        for (const auto& weapon : unit.weapons)
        {
            if (weapon && simulation.weaponDefinitions.at(weapon->weaponType).stockpile)
            {
                return weapon->weaponType;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> GameScene::getUnitBuildButtonUnderCursor() const
    {
        auto cursorPosition = getMousePosition();
        auto control = currentPanel->findAtPosition<UiStagedButton>(cursorPosition.x, cursorPosition.y);
        if (!control)
        {
            return std::nullopt;
        }

        const auto& name = control->get().getName();
        if (!isValidUnitType(simulation, name))
        {
            return std::nullopt;
        }

        return name;
    }
}
