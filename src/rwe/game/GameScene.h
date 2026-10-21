#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <rwe/AudioService.h>
#include <rwe/CroppedViewport.h>
#include <rwe/CursorService.h>
#include <rwe/GameParameters.h>
#include <rwe/RenderService.h>
#include <rwe/SceneContext.h>
#include <rwe/TextureService.h>
#include <rwe/UiRenderService.h>
#include <rwe/Viewport.h>
#include <rwe/game/BuilderGuisDatabase.h>
#include <rwe/game/GameCameraState.h>
#include <rwe/game/GameMediaDatabase.h>
#include <rwe/game/GameNetworkService.h>
#include <rwe/game/InGameSoundsInfo.h>
#include <rwe/game/Particle.h>
#include <rwe/game/PlayerCommand.h>
#include <rwe/game/PlayerCommandService.h>
#include <rwe/game/SceneTime.h>
#include <rwe/game/SceneTickScheduler.h>
#include <rwe/game/UnitSoundType.h>
#include <rwe/game/WeaponMediaInfo.h>
#include <rwe/grid/DiscreteRect.h>
#include <rwe/io/featuretdf/FeatureTdf.h>
#include <rwe/observable/BehaviorSubject.h>
#include <rwe/save/SavedGame.h>
#include <rwe/scene/Scene.h>
#include <rwe/scene/util.h>
#include <rwe/sim/FeatureId.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/OccupiedGrid.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/UnitId.h>
#include <rwe/sim/UnitState.h>
#include <rwe/ui/UiFactory.h>
#include <rwe/ui/UiPanel.h>
#include <unordered_set>
#include <variant>

namespace rwe
{
    struct GameSceneTimeAction
    {
        using Time = SceneTime;
        Time triggerTime;
        std::function<void()> callback;

        GameSceneTimeAction(Time triggerTime, const std::function<void()>& callback)
            : triggerTime(triggerTime), callback(callback)
        {
        }

        GameSceneTimeAction(Time triggerTime, std::function<void()>&& callback)
            : triggerTime(triggerTime), callback(std::move(callback))
        {
        }
    };

    struct AttackCursorMode
    {
        bool operator==(const AttackCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const AttackCursorMode& /*rhs*/) const { return false; }
    };

    struct MoveCursorMode
    {
        bool operator==(const MoveCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const MoveCursorMode& /*rhs*/) const { return false; }
    };

    struct GuardCursorMode
    {
        bool operator==(const GuardCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const GuardCursorMode& /*rhs*/) const { return false; }
    };

    struct RepairCursorMode
    {
        bool operator==(const RepairCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const RepairCursorMode& /*rhs*/) const { return false; }
    };

    struct ReclaimCursorMode
    {
        bool operator==(const ReclaimCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const ReclaimCursorMode& /*rhs*/) const { return false; }
    };

    struct PatrolCursorMode
    {
        bool operator==(const PatrolCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const PatrolCursorMode& /*rhs*/) const { return false; }
    };

    /**
     * Pending command-fire (D-Gun/BLAST) click mode -- retail cursor
     * mode 4, entered via the side-prefixed BLAST button.
     */
    struct CommandFireCursorMode
    {
        bool operator==(const CommandFireCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const CommandFireCursorMode& /*rhs*/) const { return false; }
    };

    /**
     * Pending capture click mode -- retail cursor mode 0xd, entered
     * via the side-prefixed CAPTURE button (ARMCAPTURE/CORCAPTURE,
     * quickkey `c`). Only units whose FBI sets `CanCapture=1` act on
     * the click (stock: commanders only).
     */
    struct CaptureCursorMode
    {
        bool operator==(const CaptureCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const CaptureCursorMode& /*rhs*/) const { return false; }
    };

    /**
     * Pending transport-load click mode -- retail cursor mode 6,
     * entered via the side-prefixed LOAD button (ARMLOAD/CORLOAD,
     * quickkey `l`). Only units whose FBI sets `CanLoad=1` act on
     * the click (stock: the six transport units).
     */
    struct LoadCursorMode
    {
        bool operator==(const LoadCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const LoadCursorMode& /*rhs*/) const { return false; }
    };

    /**
     * Pending transport-unload click mode -- retail cursor mode 5,
     * entered via the side-prefixed UNLOAD button
     * (ARMUNLOAD/CORUNLOAD, quickkey `u`).
     */
    struct UnloadCursorMode
    {
        bool operator==(const UnloadCursorMode& /*rhs*/) const { return true; }
        bool operator!=(const UnloadCursorMode& /*rhs*/) const { return false; }
    };

    struct NormalCursorMode
    {
        struct SelectingState
        {
            SceneTime startTime;
            Point startPosition;
            explicit SelectingState(SceneTime startTime, const Point& startPosition) : startTime(startTime), startPosition(startPosition) {}
            SelectingState(int x, int y) : startPosition(x, y) {}
            bool operator==(const SelectingState& rhs) const
            {
                return startTime == rhs.startTime && startPosition == rhs.startPosition;
            }
            bool operator!=(const SelectingState& rhs) const
            {
                return !(rhs == *this);
            }
        };
        struct DraggingMinimapState
        {
            bool operator==(const DraggingMinimapState& /*rhs*/) const { return true; }
            bool operator!=(const DraggingMinimapState& /*rhs*/) const { return false; }
        };
        struct UpState
        {
            bool operator==(const UpState& /*rhs*/) const { return true; }
            bool operator!=(const UpState& /*rhs*/) const { return false; }
        };
        using State = std::variant<SelectingState, DraggingMinimapState, UpState>;

        State state{UpState()};

        bool operator==(const NormalCursorMode& rhs) const
        {
            return state == rhs.state;
        }

        bool operator!=(const NormalCursorMode& rhs) const
        {
            return !(rhs == *this);
        }
    };

    struct BuildCursorMode
    {
        std::string unitType;

        bool operator==(const BuildCursorMode& rhs) const
        {
            return unitType == rhs.unitType;
        }

        bool operator!=(const BuildCursorMode& rhs) const
        {
            return !(rhs == *this);
        }
    };

    using CursorMode = std::variant<AttackCursorMode, MoveCursorMode, GuardCursorMode, RepairCursorMode, ReclaimCursorMode, PatrolCursorMode, CommandFireCursorMode, CaptureCursorMode, LoadCursorMode, UnloadCursorMode, BuildCursorMode, NormalCursorMode>;

    struct UnitGuiInfo
    {
        enum class Section
        {
            Build,
            Orders,
        };

        Section section;
        unsigned int currentBuildPage;
    };

    struct HoverBuildInfo
    {
        DiscreteRect rect;
        bool isValid;
        SimScalar baseY;
    };

    /**
     * Transient linear-build drag state (RWE QoL extension, not stock
     * TA 3.1). Anchored on left mouse-down in BuildCursorMode and
     * resolved on mouse-up; `engaged` latches once the snapped
     * placement cell under the cursor leaves the anchor cell.
     * Kept out of CursorMode because dragging is transient input
     * state, not a distinct cursor mode.
     */
    struct BuildDragState
    {
        std::string unitType;
        DiscreteRect anchorRect;
        bool engaged{false};
    };

    struct BuildDragCandidate
    {
        DiscreteRect rect;
        bool valid;
        SimScalar baseY;
    };

    class GameScene : public Scene
    {
    public:
        static inline const SimScalar SecondsPerTick = SimScalar(SceneTickInterval) / 1000_ss;

        static constexpr int GuiSizeLeft = 128;
        static constexpr int GuiSizeRight = 0;
        static constexpr int GuiSizeTop = 32;
        static constexpr int GuiSizeBottom = 32;

    private:
        static const unsigned int UnitSelectChannel = 0;

        static const unsigned int reservedChannelsCount = 1;

        /**
         * Speed the camera pans via the arrow keys
         * in world units/second.
         */
        static constexpr float CameraPanSpeed = 1000.0f;

        static const Rectangle2f minimapViewport;

        SceneContext sceneContext;

        CroppedViewport worldViewport;

        std::unique_ptr<PlayerCommandService> playerCommandService;

        GameCameraState worldCameraState;

        GameMediaDatabase gameMediaDatabase;
        SharedTextureHandle unitTextureAtlas;
        std::vector<SharedTextureHandle> unitTeamTextureAtlases;

        UiRenderService worldUiRenderService;
        UiRenderService chromeUiRenderService;

        /** 640x480 logical UI space used by the in-game menu dialogs. */
        UiRenderService menuUiRenderService;

        GameSimulation simulation;

        MapTerrainGraphics terrainGraphics;

        BuilderGuisDatabase builderGuisDatabase;

        std::unique_ptr<GameNetworkService> gameNetworkService;

        std::shared_ptr<Sprite> minimap;
        std::shared_ptr<SpriteSeries> minimapDots;
        std::shared_ptr<Sprite> minimapDotHighlight;
        Rectangle2f minimapRect;

        std::unique_ptr<UiPanel> currentPanel;
        std::optional<std::unique_ptr<UiPanel>> nextPanel;

        InGameSoundsInfo sounds;

        std::shared_ptr<SpriteSeries> guiFont;

        PlayerId localPlayerId;

        SceneTime sceneTime{0};

        bool left{false};
        bool right{false};
        bool up{false};
        bool down{false};

        struct CameraControlStateFree
        {
        };
        struct CameraControlStateTrackingUnit
        {
        };
        struct CameraControlStateMiddleMousePan
        {
            Point previousCursorPosition;
        };

        using CameraControlState = std::variant<CameraControlStateFree, CameraControlStateTrackingUnit, CameraControlStateMiddleMousePan>;

        CameraControlState cameraControlState{CameraControlStateFree()};
        // We hold onto trackedUnitId outside of CameraControlStateTrackingUnit
        // because if you exit and re-enter tracking while having
        // a group of units selected, the original game will resume
        // tracking the next unit in the group.
        std::optional<UnitId> trackedUnitId;

        std::optional<UnitId> hoveredUnit;
        std::optional<FeatureId> hoveredFeature;
        std::unordered_set<UnitId> selectedUnits;

        std::optional<HoverBuildInfo> hoverBuildInfo;

        std::optional<BuildDragState> buildDragState;
        std::vector<BuildDragCandidate> buildDragCandidates;

        bool occupiedGridVisible{false};
        bool pathfindingVisualisationVisible{false};
        bool movementClassGridVisible{false};
        bool cursorTerrainDotVisible{false};

        bool healthBarsVisible{false};

        BehaviorSubject<CursorMode> cursorMode{NormalCursorMode()};

        std::deque<std::optional<GameSceneTimeAction>> actions;

        std::vector<PlayerCommand> localPlayerCommandBuffer;
        bool commandWasQueued{false};

        BehaviorSubject<UnitFireOrders> fireOrders{UnitFireOrders::HoldFire};
        BehaviorSubject<bool> onOff{false};

        /**
         * The CLOAK strip's display stage (retail mask bits 3:4 at
         * +0x37ec0): 0 decloaked, 1 cloaking requested, 2 mixed
         * (any 2+ cloakable units selected), 3 no cloakable unit
         * (the strip is greyed via the capability pass, not staged).
         */
        BehaviorSubject<unsigned int> cloakStage{0};

        UiFactory uiFactory;

        std::unordered_map<UnitId, UnitGuiInfo> unitGuiInfos;

        std::unordered_map<UnitId, std::unordered_map<std::string, int>> unconfirmedBuildQueueDelta;

        std::unordered_map<UnitId, int> unconfirmedWeaponQueueDelta;

        std::vector<std::pair<GameTime, GameHash>> gameHashes;

        std::optional<std::ofstream> stateLogStream;

        /** Latched once a campaign mission result has been declared. */
        bool missionResultDeclared{false};

        /**
         * The committed campaign result and its in-game banner sprite
         * (anims/igtitles.gaf igvictory/igdefeat, retail ctx+0x14813/
         * +0x14817 selected by player-dead state). The sprite is drawn
         * over the running simulation until the ENDMSN transition
         * fires, matching retail's +0x3923b bit5/bit6 overlay window.
         */
        bool missionResultWon{false};
        std::shared_ptr<Sprite> missionResultSprite;

        /**
         * Set when the completed simulation + parameters are moved
         * into the EndMissionScene at debrief handoff (retail keeps
         * the finished game context alive beneath ENDMSN so its
         * SaveGame writer can serialize it). The scene manager only
         * swaps scenes at the next loop iteration, so the trailing
         * update/render of this scene must not touch the moved-from
         * simulation.
         */
        bool simulationTransferredToDebrief{false};

        /**
         * True while the simulation is paused. Set by processing
         * PlayerPauseGameCommand/PlayerUnpauseGameCommand so that pause
         * state stays in lockstep for networked games.
         */
        bool gamePaused{false};

        /**
         * In-game menu dialog stack (stock ARMOPT.EXITMENU/YESORNO chain).
         * Empty when the menu is closed. The back of the vector is the
         * topmost dialog.
         */
        std::vector<std::unique_ptr<UiPanel>> inGameMenuDialogs;

        /**
         * Parallel to inGameMenuDialogs: true marks a non-modal
         * companion pane. Retail PREFS composes a right-hand category
         * pane (SPEEDSRT) beside its left strip -- both stay
         * interactive, while real modal dialogs (YESORNO, HELP,
         * BRIEFING) keep exclusive input.
         */
        std::vector<bool> inGameMenuDialogCompanions;

        /**
         * Dialogs popped or closed while their own event dispatch is
         * still unwinding. A child's key/mouse broadcast emits the
         * activation that removes its dialog (Enter on crdefault,
         * gadget quickkeys, button clicks); holding the removed panel
         * here until the outermost dispatch returns keeps the
         * in-flight UiPanel loop from iterating freed storage.
         */
        std::vector<std::unique_ptr<UiPanel>> inGameMenuRetiredDialogs;

        /** In-flight in-game-menu event dispatch nesting depth. */
        unsigned int inGameMenuDispatchDepth{0};

        /** Dialog that received the last menu mouse-down, if any. */
        UiPanel* inGameMenuMouseTarget{nullptr};

        /**
         * True when opening the in-game menu issued the active pause.
         * Single-player only; used to unpause on close without disturbing
         * a pause the player set before opening the menu.
         */
        bool inGameMenuPauseOwned{false};

        /** Action run when the user answers Yes on the top YESORNO dialog. */
        std::function<void()> inGameMenuConfirmAction;

        /** Active page of the in-game HELP dialog. */
        unsigned int inGameMenuHelpPage{0};

        /**
         * leftClickInterfaceMode value captured when the options screen
         * (PREFS) is opened, for SPEEDSRT's Undo Changes and PREFS's
         * Cancel (TotalA.exe snapshots all settings at options entry,
         * restores on Undo/Cancel, commits+persists on OK/PREV).
         */
        bool inGameMenuInterfaceSnapshot{true};

        /**
         * In-game briefing pager state: the sanitized campaign brief
         * text wrapped into TextRegion-sized pages. Retail pages the
         * text through TextRegion/MOREBAR clicks rather than scrolling.
         */
        std::vector<std::string> inGameMenuBriefingPages;
        unsigned int inGameMenuBriefingPage{0};

        /**
         * Save/load dialog (SAVELIST.GUI) state: true while the top
         * dialog is the save-entry variant, false for load-pick.
         * `inGameMenuSaveListEntries` maps GAMES list indices to the
         * decoded metadata of each save file.
         */
        bool inGameMenuSaveListIsSave{false};
        std::vector<SavedGameMetadata> inGameMenuSaveListEntries;

        TdfBlock* audioLookup;

        /** Launch parameters retained so Restart can rebuild the game. */
        GameParameters gameParameters;

        bool showDebugWindow{false};
        char unitSpawnText[20]{""};
        int unitSpawnPlayer{0};

        std::mutex playingUnitChannelsLock;
        std::unordered_set<int> playingUnitChannels;

        /**
         * Declared after the members its callback touches so it is
         * destroyed first: ~audioSub unsubscribes, which blocks until
         * any in-flight audio-thread delivery of channelFinished
         * returns, guaranteeing onChannelFinished can never observe
         * destroyed state.
         */
        std::unique_ptr<Subscription> audioSub = sceneContext.audioService->getChannelFinished().subscribe([this](int channel) { onChannelFinished(channel); });

        std::vector<Particle> particles;

        /**
         * Presentation-only RNG for the nanolathe spray effect
         * (particle scatter/endpoint jitter). Deliberately separate
         * from `simulation.rng` so visuals can never perturb game
         * state or hashes. Seeded per-process like retail's rand().
         */
        std::mt19937 nanolatheRng{std::random_device{}()};

        SceneTickAccumulator tickAccumulator;

        /**
         * Retail's game-speed word (TotalA.exe 0x496512/0x496570,
         * range 1..20, single-player only): 10 is real-time 30tps,
         * each step scales the wall-clock -> sim-tick rate by a tenth.
         * Runtime-only -- it rescales elapsed time, not sim state, so
         * it never affects determinism or saves.
         */
        int gameSpeedLevel{10};

        std::vector<FlashEffect> flashes;
        bool guiVisible{true};

        FrameBufferInfo worldFrameBuffer;

        TextureHandle dodgeMask;

        struct ProjectileRenderInfo
        {
            GameTime lastSmoke{GameTime(0)};
        };

        std::unordered_map<ProjectileId, ProjectileRenderInfo> projectileRenderInfos;

        /**
         * Opt-in frame profiler, enabled with RWE_PERF=1.
         * Accumulates wall-clock time spent in update(), tryTickGame()
         * and render() and logs a rolling summary every ~5 seconds.
         * Presentation-side only; never affects sim state or hashes.
         */
        bool perfEnabled{false};
        std::uint64_t perfUpdateNs{0};
        std::uint64_t perfTickNs{0};
        std::uint64_t perfRenderNs{0};
        unsigned int perfTickCount{0};
        unsigned int perfFrameCount{0};
        std::chrono::steady_clock::time_point perfWindowStart;

        /**
         * Per-frame perf samples for percentile reporting; the 5s
         * window summary logs p50/p95/p99/max alongside the averages
         * so frame spikes don't dissolve into the mean. renderWorld
         * sub-phase accumulators are reported as per-frame averages.
         */
        struct PerfSample
        {
            std::uint64_t updateNs;
            std::uint64_t tickNs;
            std::uint64_t renderNs;
        };
        std::vector<PerfSample> perfSamples;
        std::uint64_t perfRwTerrainNs{0};
        std::uint64_t perfRwShadowsNs{0};
        std::uint64_t perfRwUnitEmitNs{0};
        std::uint64_t perfRwUnitSubmitNs{0};
        std::uint64_t perfRwFogNs{0};
        std::uint64_t perfUiNs{0};
        std::string perfWindowLabel;
        unsigned int perfUnitsEmitted{0};
        unsigned int perfUnitsCulled{0};
        unsigned int perfShadowsEmitted{0};
        unsigned int perfShadowsCulled{0};

        /**
         * Long-run health counters for the progressive-slowdown probe:
         * peak accumulator backlog, ticks actually executed, sim-phase
         * totals (from GameSimulation::tickPhaseTimings) and workload
         * counters. Inert when perfEnabled is false.
         */
        float perfAccMax{0.0f};
        unsigned int perfTicksExec{0};
        unsigned int perfTicksBlocked{0};
        std::uint64_t perfPfReqBase{0};
        std::uint64_t perfPfExpBase{0};

        /** seconds to run before auto-exit (RWE_SOAK); 0 = run forever */
        unsigned int soakSeconds{0};
        std::chrono::steady_clock::time_point soakStart;
        bool soakStarted{false};

        void reportPerfWindow(double windowS);

        /**
         * Render-only frustum culling master switch (RWE_DISABLE_CULL=1
         * disables). Simulation state is never affected; the flag exists
         * so a single binary can run culled vs unculled benchmarks and
         * screenshot comparisons.
         */
        bool cullingEnabled{true};

        /**
         * Deterministic screenshot capture (RWE_SHOT_DIR/RWE_SHOT_TICKS).
         * While non-empty, interpolation freezes at tick boundaries so
         * the same sim tick renders identically across runs.
         */
        std::string shotDir;
        std::vector<unsigned int> shotTicks;
        std::size_t shotIndex{0};
        /** set during the instancing-off A/B re-render pass */
        bool shotInstancingDisabled{false};
        void captureWorldShot(const std::string& name);

        /**
         * Scripted render-bench driver, enabled with
         * RWE_SCENE_BENCH=<count>:<unitType>. Spawns `count` units
         * near the camera anchor, then walks a frame-count timeline:
         * idle -> select -> group-move on-screen -> pan off-screen ->
         * exit. Presentation-side only.
         */
        struct SceneBench
        {
            bool active{false};
            unsigned int count{0};
            std::string unitType;
            /** comma-list expansion of unitType; '~'-suffixed entries spawn under construction */
            std::vector<std::pair<std::string, bool>> unitTypes;
            std::vector<UnitId> spawned;
            int step{0};
            // on-screen camera anchor for phases A-C; edge-scroll would
            // otherwise drift the view away from the spawned cluster
            std::optional<Vector3f> cameraFocus;
        };
        SceneBench sceneBench;
        unsigned int sceneBenchFrame{0};
        void updateSceneBench();

    public:
        GameScene(
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
            std::optional<std::ofstream>&& stateLogStream);

        ~GameScene() override;

        void init() override;

        void render() override;

        void onKeyDown(const SDL_KeyboardEvent& keysym) override;

        void onKeyUp(const SDL_KeyboardEvent& keysym) override;

        void onMouseDown(MouseButtonEvent event) override;

        void onMouseUp(MouseButtonEvent event) override;

        void onMouseMove(MouseMoveEvent event) override;

        void onMouseWheel(MouseWheelEvent event) override;

        void update(int millisecondsElapsed) override;

        std::optional<UnitId> spawnUnit(const std::string& unitType, PlayerId owner, const SimVector& position, std::optional<const std::reference_wrapper<SimAngle>> rotation, bool allowOverlap = false);

        std::optional<std::reference_wrapper<UnitState>> spawnCompletedUnit(const std::string& unitType, PlayerId owner, const SimVector& position, bool allowOverlap = false);

        void setCameraPosition(const Vector3f& newPosition);

        const MapTerrain& getTerrain() const;

    private:
        GameTime getGameTime() const;

        void playUiSound(const AudioService::SoundHandle& sound);

        void playNotificationSound(const PlayerId& playerId, const AudioService::SoundHandle& sound);

        void playUnitNotificationSound(const PlayerId& playerId, const std::string& unitType, UnitSoundType soundType);

        void playSoundAt(const Vector3f& position, const AudioService::SoundHandle& sound);

        void playWeaponStartSound(const Vector3f& position, const std::string& weaponType);

        void playWeaponImpactSound(const Vector3f& position, const std::string& weaponType, ImpactType impactType);

        void spawnWeaponImpactExplosion(const Vector3f& position, const std::string& weaponType, ImpactType impactType);

        void doProjectileImpact(const SimVector& position, const std::string& weaponType, ImpactType impactType);

        void createLightSmoke(const Vector3f& position);

        void createWeaponSmoke(const Vector3f& position);

        void emitLightSmokeFromPiece(UnitId unitId, const std::string& pieceName);

        void emitBlackSmokeFromPiece(UnitId unitId, const std::string& pieceName);

        void emitWake1FromPiece(UnitId unitId, const std::string& pieceName);

        void modifyBuildQueue(UnitId unitId, const std::string& unitType, int count);

        /**
         * Applies a stockpile weapon build-button click to the unit's
         * background order queue (retail `commonattribs=8` gadget, e.g.
         * ARMMAKENUKE/ARMMAKEANTI): positive counts append rounds,
         * negative counts cancel pending rounds.
         */
        void modifyWeaponQueue(UnitId unitId, int count);

        void onChannelFinished(int channel);

        static Matrix4f worldToMinimapMatrix(const MapTerrain& terrain, const Rectangle2f& minimapRect);

        static Matrix4f minimapToWorldMatrix(const MapTerrain& terrain, const Rectangle2f& minimapRect);

        void tryTickGame();

        /**
         * Darkens the minimap where the local player lacks current
         * LOS: opaque black over unexplored cells, translucent black
         * over explored-but-not-visible cells (OTA `mapping`).
         */
        void drawMinimapFog();

        std::optional<UnitId> getUnitUnderCursor() const;
        std::optional<FeatureId> getFeatureUnderCursor() const;

        Vector2f screenToWorldClipSpace(Point p) const;

        bool isCursorOverMinimap() const;

        bool isCursorOverWorld() const;

        Point getMousePosition() const;

        std::optional<UnitId> getFirstCollidingUnit(const Ray3f& ray) const;
        std::optional<FeatureId> getFirstCollidingFeature(const Ray3f& ray) const;

        /**
         * Returns a value if the given ray intersects this unit
         * for the purposes of unit selection.
         * The value returned is the distance along the ray
         * where the intersection occurred.
         */
        std::optional<float> selectionIntersect(const UnitState& unit, const CollisionMesh& mesh, const Ray3f& ray) const;

        std::optional<SimVector> getMouseTerrainCoordinate() const;

        /**
         * Retail shift+click order-queue cancellation: with an armed
         * cursor, clicking on one of the selected units' queued order
         * markers (or a queued build ghost in build mode) removes that
         * order. Returns true when the click was consumed by a removal.
         */
        bool tryRemoveQueuedOrderAtCursor();

        bool tryRemoveQueuedOrderAtMarker(CursorType markerType);

        bool tryRemoveQueuedBuildOrderAtCursor();

        /**
         * Resolves a BuildCursorMode mouse-up (linear-build QoL):
         * a same-cell click places one BuildOrder with stock
         * semantics; an engaged drag emits one BuildOrder per valid
         * line candidate (first immediate/replace when Shift is up,
         * the rest queued). Always clears the drag state.
         */
        void completeBuildPlacement(const std::string& unitType);

        void localPlayerIssueUnitOrder(UnitId unitId, const UnitOrder& order);

        void localPlayerEnqueueUnitOrder(UnitId unitId, const UnitOrder& order);

        void localPlayerStopUnit(UnitId unitId);

        void localPlayerSetFireOrders(UnitId unitId, UnitFireOrders orders);

        void localPlayerSetOnOff(UnitId unitId, bool on);

        void localPlayerSetCloak(UnitId unitId, bool on);

        void localPlayerModifyBuildQueue(UnitId unitId, const std::string& unitType, int count);

        void localPlayerModifyWeaponQueue(UnitId unitId, int count);

        void localPlayerRemoveUnitOrder(UnitId unitId, int index);

        /**
         * Issues the retail SELFDESTRUCT unit command (Ctrl/Alt+D):
         * toggles the unit's interactive self-destruct countdown.
         */
        void localPlayerToggleSelfDestruct(UnitId unitId);

        void issueUnitOrder(UnitId unitId, const UnitOrder& order);

        void enqueueUnitOrder(UnitId unitId, const UnitOrder& order);

        void stopUnit(UnitId unitId);

        void removeUnitOrderAt(UnitId unitId, int index);

        void setFireOrders(UnitId unitId, UnitFireOrders orders);

        void setCloak(UnitId unitId, bool on);

        /**
         * Toggles the unit's interactive self-destruct state: arms the
         * countdown if idle, cancels it if armed (retail SELFDESTRUCT /
         * canceldestruct; TotalA.exe 0x49641d, 0x439f80).
         */
        void toggleSelfDestruct(UnitId unitId);

        void startTrack();

        void startTrackInternal(const std::vector<UnitId>& unitIds);

        bool isCtrlDown() const;

        bool isAltDown() const;

        bool isShiftDown() const;

        /**
         * True when the key is a letter or digit pressed with Ctrl or
         * Alt held — retail translates these to command codes that
         * bypass GUI quickkey matching (TotalA.exe 0x4c1c70).
         */
        bool isModifiedLetterOrDigit(SDL_Keycode key) const;

        void handleEscapeDown();

        bool inGameMenuOpen() const;

        void openInGameMenu();

        /**
         * Retail F1: opens the help screen directly (TotalA.exe
         * 0x4960ea), without going through the options menu.
         */
        void openInGameMenuHelp();

        void closeInGameMenu();

        void pushInGameMenuDialog(std::unique_ptr<UiPanel>&& panel, bool companionPane = false);

        void popInGameMenuDialog();

        /**
         * Pops the named dialog and everything stacked above it
         * (a companion pane's owning screen is below the pane).
         */
        void popInGameMenuDialogThrough(const std::string& panelName);

        /** Destroys retired dialogs once no dialog dispatch is in flight. */
        void reapInGameMenuDialogs();

        /**
         * Marks a call into a dialog's event methods; popped or closed
         * dialogs are reaped when the outermost scope unwinds.
         */
        class DialogDispatchScope
        {
            GameScene& scene;

        public:
            explicit DialogDispatchScope(GameScene& scene)
                : scene(scene)
            {
                ++scene.inGameMenuDispatchDepth;
            }

            ~DialogDispatchScope()
            {
                if (--scene.inGameMenuDispatchDepth == 0)
                {
                    scene.inGameMenuRetiredDialogs.clear();
                }
            }

            DialogDispatchScope(const DialogDispatchScope&) = delete;
            DialogDispatchScope& operator=(const DialogDispatchScope&) = delete;
        };

        /**
         * The dialog a menu-space point routes to: companion panes
         * yield to their owning screen while modal dialogs own the
         * point or block it entirely.
         */
        UiPanel* inGameMenuPointerTarget(int x, int y);

        /** The owning non-companion dialog of the current stack top. */
        UiPanel* inGameMenuKeyTarget();

        /** Commits the interface preference like retail PREV. */
        void commitInterfaceModePreference();

        void onInGameMenuMessage(const std::string& topic, const std::string& controlName, ActivateMessage::Type type);

        std::unique_ptr<UiPanel> createInGameMenuDialog(const std::string& guiName);

        std::unique_ptr<UiPanel> createYesNoDialog(const std::string& titleText);

        void renderInGameMenu();

        Point toInGameMenuCoordinates(int x, int y) const;

        void togglePause();

        void exitToMainMenu();

        void restartGame();

        void populateHelpDialog(UiPanel& panel);

        void populateInterfaceDialog(UiPanel& panel);

        void populateBriefingDialog(UiPanel& panel);

        /** Renders briefingPages[briefingPage] + the MOREBAR caption. */
        void applyBriefingPage(UiPanel& panel);

        /** Retail GAMEOPTIONS "GameSettings" summary (skirmish MISSION). */
        void populateGameOptionsDialog(UiPanel& panel);

        /**
         * Stock SAVELIST.GUI dialog in save or load mode: fills the GAMES
         * listbox from the save directory, wires GAMENAME/DELETE/OK.
         */
        std::unique_ptr<UiPanel> createSaveListDialog(bool saveMode);

        /** Refills the GAMES listbox + save-entry cache after a write/delete. */
        void refreshSaveListDialog(UiPanel& panel);

        /** Encode + write the save named by the dialog's GAMENAME field. */
        void performSaveGame(UiPanel& panel);

        /** Load the selected save: transitions to a fresh LoadingScene. */
        void performLoadGame(UiPanel& panel);

        bool isSingleHumanPlayer() const;

        void adjustGameSpeed(int delta);

        UnitState& getUnit(UnitId id);

        const UnitState& getUnit(UnitId id) const;

        std::optional<std::reference_wrapper<UnitState>> tryGetUnit(UnitId id);

        std::optional<std::reference_wrapper<const UnitState>> tryGetUnit(UnitId id) const;

        GamePlayerInfo& getPlayer(PlayerId player);

        const GamePlayerInfo& getPlayer(PlayerId player) const;

        bool isEnemy(UnitId id) const;

        bool isFriendly(UnitId id) const;

        void updateProjectiles();

        void processSimEvents();

        void updateFlashes();

        void processActions();

        void processPlayerCommands(const std::vector<std::pair<PlayerId, std::vector<PlayerCommand>>>& commands);

        void processPlayerCommand(const PlayerCommand& playerCommand);

        void processUnitCommand(const PlayerUnitCommand& unitCommand);

        template <typename T>
        void delay(SceneTime interval, T&& f)
        {
            actions.emplace_back(GameSceneTimeAction(sceneTime + interval, std::forward<T>(f)));
        }

        /**
         * Builds the ENDMSN debrief payload from the finished mission
         * and hands the frontend to EndMissionScene. Runs once, after
         * the retail-style banner delay.
         */
        void enterMissionDebrief();

        void renderUi();

        void renderMinimap();

        void renderWorld();

        void renderDebugWindow();

        /**
         * Renders waypoint sprites immediately (persistent meshes) and
         * appends route-line segments to `linesOut` for one batched draw.
         * `worldToUi` is the frame's world->UI transform, computed once
         * by the caller for all units.
         */
        void renderUnitOrders(UnitId unitId, bool drawLines, const Matrix4f& worldToUi, std::vector<GlColoredVertex>& linesOut);

        /**
         * Appends the footprint box outline of every queued BuildOrder on
         * `unit` to `trianglesOut` for one batched draw.
         */
        void renderBuildBoxes(const UnitState& unit, const Color& color, const Matrix4f& worldToUi, std::vector<GlColoredVertex>& trianglesOut);

        void attachOrdersMenuEventHandlers();

        void onMessage(const std::string& message, ActivateMessage::Type mode);

        bool matchesWithSidePrefix(const std::string& suffix, const std::string& value) const;

        /**
         * If there is exactly one unit selected, returns the unit ID.
         * Otherwise, returns nothing.
         */
        std::optional<UnitId> getSingleSelectedUnit() const;

        void selectUnitsInBandbox(const DiscreteRect& box);

        void toggleUnitSelection(const UnitId& unitId);

        void toggleUnitSelection(const std::unordered_set<UnitId>& units);

        void selectAdditionalUnit(const UnitId& unitId);

        void replaceUnitSelection(const UnitId& unitId);

        void replaceUnitSelection(const std::unordered_set<UnitId>& units);

        /**
         * Adds every unit selectable by the local player matching
         * `predicate` to the current selection (the retail Ctrl+A/Z/P/R/W
         * handlers set each matching unit's select bit without clearing
         * the rest).
         */
        void addSelectableUnitsToSelection(const std::function<bool(const UnitState&, const UnitDefinition&)>& predicate);

        /** Retail Ctrl+A: select every eligible owned unit. */
        void selectAllEligibleUnits();

        /** Retail Ctrl+S: replace the selection with eligible owned units on screen. */
        void selectUnitsOnScreen();

        /** Retail Ctrl+Z: add every eligible owned unit of a currently-selected type. */
        void selectUnitsOfSelectedTypes();

        /** Retail Ctrl+P: add all armed aircraft. */
        void selectArmedAircraft();

        /** Retail Ctrl+R: add all radar, radar-jammer, sonar and sonar-jammer units. */
        void selectRadarSonarUnits();

        /** Retail Ctrl+W: add all armed mobile units except the Commander. */
        void selectArmedMobileUnits();

        void deselectUnit(const UnitId& unitId);

        void clearUnitSelection();

        void onSelectedUnitsChanged();

        const UnitGuiInfo& getGuiInfo(const UnitId& unitId) const;

        void setNextPanel(std::unique_ptr<UiPanel>&& panel);

        void refreshBuildGuiTotal(UnitId unitId, const std::string& unitType);

        /**
         * Refreshes the stockpile counter label on the unit's weapon
         * build button (built rounds + pending queue + unconfirmed
         * clicks).
         */
        void refreshWeaponGuiTotal(UnitId unitId);

        /**
         * Finds the unit's weapon build gadget name (the
         * `commonattribs=8` entry on its current build panel page), if
         * any.
         */
        std::optional<std::string> getWeaponBuildGadgetName(UnitId unitId) const;

        /**
         * Built rounds stockpiled + rounds still queued in the unit's
         * background weapon orders.
         */
        unsigned int getWeaponStockpileTotal(UnitId unitId) const;

        void updateUnconfirmedBuildQueueDelta(UnitId unitId, const std::string& unitType, int count);

        int getUnconfirmedBuildQueueCount(UnitId unitId, const std::string& unitType) const;

        std::unique_ptr<UiPanel> createBuildPanel(const std::string& guiname, const std::vector<GuiEntry>& panelDefinition, const std::unordered_map<std::string, int>& totals);

        template <typename T>
        std::optional<std::reference_wrapper<T>> findWithSidePrefix(UiPanel& p, const std::string& name)
        {
            for (const auto& [_, side] : *sceneContext.sideData)
            {
                auto control = p.find<T>(side.namePrefix + name);
                if (control)
                {
                    return control;
                }
            }

            return std::nullopt;
        }

        bool leftClickMode() const;

        void spawnExplosion(const Vector3f& position, const AnimLocation& anim);

        void spawnFlash(const Vector3f& position);

        void spawnSmoke(const Vector3f& position, const std::string& gaf, const std::string& anim, ParticleFinishTime duration, GameTime frameDuration);

        void spawnWake(const Vector3f& position, const Vector3f& velocity, GameTime duration);

        void recreateWorldRenderTextures();

        /**
         * Handler for when the player nudges the camera
         * e.g. via arrow keys or by moving the mouse cursor
         * to the edge of the screen.
         */
        void nudgeCamera(int millisecondsElapsed, const Rectangle2f& cameraConstraint, int directionX, int directionZ);

        /**
         * Handler for when the player relocates the camera
         * e.g. by clicking a location on the minimap.
         */
        void relocateCamera(const Rectangle2f& cameraConstraint, float x, float z);

        std::optional<std::string> getUnitBuildButtonUnderCursor() const;

        /**
         * The weapon type of the selected unit's stockpile weapon when
         * the cursor is over its weapon-build gadget (GUI
         * `commonattribs=8`) -- drives the weapon-name+M/E hover info.
         */
        std::optional<std::string> getStockpileWeaponUnderCursor() const;
    };
}
