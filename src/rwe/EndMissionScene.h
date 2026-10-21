#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <rwe/AudioService.h>
#include <rwe/EndMissionScene_util.h>
#include <rwe/GameParameters.h>
#include <rwe/SceneContext.h>
#include <rwe/UiRenderService.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/tdf/TdfBlock.h>
#include <rwe/save/SavedGame.h>
#include <rwe/render/Sprite.h>
#include <rwe/render/SpriteSeries.h>
#include <rwe/scene/Scene.h>
#include <rwe/ui/UiFactory.h>
#include <rwe/ui/UiPanel.h>
#include <string>
#include <vector>

namespace rwe
{
    struct GameSimulation;

    /**
     * Everything the ENDMSN debrief needs from the finished mission,
     * captured at result-commit time in GameScene.
     */
    struct EndMissionInfo
    {
        /** True on Victory (retail ctx+0x3923b bit4), false on Defeat. */
        bool won;

        /** Launch parameters of the mission that just ended. */
        CampaignMissionParameters campaign;

        /** Elapsed mission time in sim ticks. */
        GameTime gameTime;

        /** OTA header score multipliers (killmul/timemul). */
        double killMul;
        double timeMul;

        /** One row per player that participated, in player order. */
        std::vector<DebriefPlayerRow> stats;
    };

    /**
     * The retail ENDMSN debrief screen (TotalA.exe 0x41f0a0 setup,
     * 0x41ec50 message handler, drawn over outcome0/outcome1 PCX).
     *
     * Retail semantics implemented here:
     *  - the mission status array (+0x391cf) is stamped W/L for the
     *    mission that just ended (via CampaignProgress);
     *  - the "Missions" listbox lists every campaign mission with a
     *    status colour mark (retail 0x41eaa0 prefixes 0xfd/0xfe/0xff);
     *  - the selection preselects the next mission on victory, the
     *    current mission on defeat (0x41f2e6-0x41f311);
     *  - "Start" relaunches the selected mission; "MainMenu" returns
     *    to the campaign frontend; "Difficulty" cycles Easy/Medium/Hard;
     *  - on a campaign-final victory there is no next mission, so the
     *    screen runs in the outcome0/MainMenu-only mode (0x41f400:
     *    Start/LoadGame/SaveGame/controls disabled, MainMenu only);
     *  - SaveGame is functional on defeat and non-final victory:
     *    retail keeps the finished game context alive beneath the
     *    debrief and its SaveGame handler (0x493060 -> save dialog
     *    0x492df0 -> writer 0x4326b0) serializes that completed
     *    state, including the just-stamped W/L mission-status array.
     *    RWE mirrors this by moving the completed GameSimulation and
     *    its GameParameters into this scene at handoff.
     *
     *  Retail additionally shows a "Click to continue." campaign-end
     *  interstitial before ENDMSN on final victory (sub-state 5,
     *  0x41fc12); that interstitial is not yet implemented and is a
     *  documented gap.
     */
    class EndMissionScene : public Scene
    {
    private:
        SceneContext sceneContext;
        TdfBlock* soundLookup;
        EndMissionInfo info;

        UiRenderService scaledUiRenderService;
        UiRenderService nativeUiRenderService;
        UiFactory uiFactory;

        std::unique_ptr<UiPanel> panel;

        /** The resolved campaign descriptor, if the mission belongs to one. */
        std::optional<CampaignDescriptorFile> campaignFile;

        /** Index of the finished mission within the descriptor. */
        unsigned int missionIndex{0};

        /** Retail "has continuation" flag: outcome1+Start vs outcome0+MainMenu. */
        bool hasContinuation{false};

        /** Currently staged difficulty for the next launch. */
        std::string difficulty;

        /** Missions listbox selection, mirrored via subscription. */
        std::optional<unsigned int> selectedMission;

        std::shared_ptr<SpriteSeries> font;

        /**
         * The completed mission's authoritative state, moved in from
         * GameScene and frozen (never ticked again). It exists solely
         * so that ENDMSN SaveGame can snapshot the finished mission
         * exactly as retail's 0x4326b0 writer reads the still-live
         * game context beneath the debrief. `completedParameters`
         * are the GameParameters the mission was launched with.
         */
        std::unique_ptr<GameSimulation> completedSimulation;
        GameParameters completedParameters;

        /**
         * ENDMSN's LoadGame/SaveGame open the stock LOADGAME screen
         * as an overlay (retail shows it over the debrief;
         * 0x4931d0/0x493060). `saveListEntries` maps GAMES list
         * indices to save metadata. `saveDialogPanel` is non-null
         * while the dialog on top of the stack is in save mode.
         */
        std::vector<std::unique_ptr<UiPanel>> dialogStack;
        std::vector<SavedGameMetadata> saveListEntries;
        UiPanel* saveDialogPanel{nullptr};

        /**
         * Dialogs popped while their own event dispatch is still
         * unwinding (YESORNO's y/n quickkeys activate mid-broadcast);
         * held until the outermost dispatch returns so the in-flight
         * UiPanel loop does not iterate freed storage.
         */
        std::vector<std::unique_ptr<UiPanel>> retiredDialogs;

        /** In-flight panel event dispatch nesting depth. */
        unsigned int uiDispatchDepth{0};

        /** Action deferred by a YESORNO confirm (e.g. overwrite). */
        std::function<void()> pendingConfirmAction;

    public:
        EndMissionScene(
            const SceneContext& sceneContext,
            TdfBlock* audioLookup,
            const EndMissionInfo& info,
            std::unique_ptr<GameSimulation> completedSimulation,
            GameParameters&& completedParameters);

        ~EndMissionScene() override;

        void init() override;

        void render() override;

        void onMouseDown(MouseButtonEvent event) override;

        void onMouseUp(MouseButtonEvent event) override;

        void onMouseMove(MouseMoveEvent event) override;

        void onMouseWheel(MouseWheelEvent event) override;

        void onKeyDown(const SDL_KeyboardEvent& keysym) override;

        void update(int millisecondsElapsed) override;

        /** Test seam: the debrief's ENDMSN panel (null until init). */
        UiPanel* getPanelForTest()
        {
            return panel.get();
        }

    private:
        void message(const std::string& topic, const std::string& message, const ActivateMessage& details);

        void startSelectedMission();

        void openLoadGameDialog();

        void openSaveGameDialog();

        void performSaveGame();

        void loadSelectedSave();

        void deleteSelectedSave();

        void pushConfirmDialog(const std::string& titleText);

        void popDialog();

        /** Destroys retired dialogs once no panel dispatch is in flight. */
        void reapRetiredDialogs();

        /**
         * Marks a call into a panel's event methods; popped dialogs are
         * reaped when the outermost scope unwinds.
         */
        class PanelDispatchScope
        {
            EndMissionScene& scene;

        public:
            explicit PanelDispatchScope(EndMissionScene& scene)
                : scene(scene)
            {
                ++scene.uiDispatchDepth;
            }

            ~PanelDispatchScope()
            {
                if (--scene.uiDispatchDepth == 0)
                {
                    scene.retiredDialogs.clear();
                }
            }

            PanelDispatchScope(const PanelDispatchScope&) = delete;
            PanelDispatchScope& operator=(const PanelDispatchScope&) = delete;
        };

        void cycleDifficulty();

        void goToMainMenu();

        MouseButtonEvent toScaledEvent(MouseButtonEvent event) const;
    };
}
