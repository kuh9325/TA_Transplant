#pragma once

#include <memory>
#include <rwe/AudioService.h>
#include <rwe/CursorService.h>
#include <rwe/RenderService.h>
#include <rwe/SceneContext.h>
#include <rwe/TextureService.h>
#include <rwe/io/sidedatatdf/SideData.h>
#include <rwe/io/tdf/TdfBlock.h>
#include <rwe/save/SavedGame.h>
#include <rwe/scene/Scene.h>
#include <rwe/ui/UiFactory.h>
#include <rwe/ui/UiPanel.h>

namespace rwe
{
    class MainMenuScene : public Scene
    {
    private:
        SceneContext sceneContext;
        TdfBlock* soundLookup;

        UiRenderService scaledUiRenderService;
        UiRenderService nativeUiRenderService;

        MainMenuModel model;
        UiFactory uiFactory;

        std::vector<std::unique_ptr<UiPanel>> panelStack;
        std::vector<std::unique_ptr<UiPanel>> dialogStack;

        /**
         * Panels popped while their own event dispatch is still
         * unwinding. A child's key/mouse broadcast emits the
         * activation that pops its screen (gadget quickkeys); holding
         * the removed panel here until the outermost dispatch returns
         * keeps the in-flight UiPanel loop from iterating freed
         * storage.
         */
        std::vector<std::unique_ptr<UiPanel>> retiredPanels;

        /** In-flight panel event dispatch nesting depth. */
        unsigned int uiDispatchDepth{0};

        AudioService::LoopToken bgm;

    public:
        MainMenuScene(
            const SceneContext& sceneContext,
            TdfBlock* audioLookup,
            float width,
            float height);

        MainMenuScene(const MainMenuScene&) = delete;
        MainMenuScene& operator=(const MainMenuScene&) = delete;
        MainMenuScene(MainMenuScene&&) = delete;
        MainMenuScene& operator=(MainMenuScene&&) = delete;

        void init() override;

        void render() override;

        void onMouseDown(MouseButtonEvent event) override;

        void onMouseUp(MouseButtonEvent event) override;

        void onMouseMove(MouseMoveEvent event) override;

        void onMouseWheel(MouseWheelEvent event) override;

        void onKeyDown(const SDL_KeyboardEvent& keysym) override;

        void update(int millisecondsElapsed) override;

        void goToPreviousMenu();

        void goToMenu(std::unique_ptr<UiPanel>&& panel);

        void openDialog(std::unique_ptr<UiPanel>&& panel);

        void goToMainMenu();

        void goToSingleMenu();

        void goToSkirmishMenu();

        void goToNewCampaignMenu();

        void goToMissionBriefing();

        void openMapSelectionDialog();

        void exit();

        void message(const std::string& topic, const std::string& message, const ActivateMessage& details);

        void setCandidateSelectedMap(const std::string& mapName);

        void clearCandidateSelectedMap();

        void commitSelectedMap();

        void resetCandidateSelectedMap();

        void togglePlayer(int playerIndex);

        void incrementPlayerMetal(int playerIndex);

        void decrementPlayerMetal(int playerIndex);

        void incrementPlayerEnergy(int playerIndex);

        void decrementPlayerEnergy(int playerIndex);

        void togglePlayerSide(int playerIndex);

        void cyclePlayerColor(int playerIndex);

        void reverseCyclePlayerColor(int playerIndex);

        void cyclePlayerTeam(int playerIndex);

        void startGame();

        void setCampaignSide(const std::string& side);

        void cycleCampaignDifficulty();

        void startCampaignBriefing();

        void startCampaignMission();

        void toggleCampaignNarration();

        void openLoadGameScreen();

        void loadSelectedSave();

        void deleteSelectedSave();

    private:
        /**
         * The mission selected in NEWCAMP and presented on MSNBRIEF.
         * Set when the briefing opens; consumed by Start.
         */
        struct PendingCampaignMission
        {
            std::string campaignName;
            std::string campaignSide;
            unsigned int missionIndex;
            std::string missionFile;
            std::string missionName;
        };

        std::optional<PendingCampaignMission> pendingMission;

        std::optional<AudioService::SoundHandle> narrationSound;
        int narrationChannel{-1};

        /** LOADGAME screen's save list: maps GAMES indices to metadata. */
        std::vector<SavedGameMetadata> saveListEntries;

        /** Suppresses listbox selection callbacks while controls repopulate. */
        bool updatingCampaignControls{false};

        void refreshCampaignList();

        /** Repopulates the NEWGAME listboxes and labels from model state. */
        void populateCampaignControls(UiPanel& panel);

        AudioService::LoopToken startBgm();

        UiPanel& topPanel();

        /** Destroys retired panels once no panel dispatch is in flight. */
        void reapRetiredPanels();

        /**
         * Marks a call into a panel's event methods; popped panels are
         * reaped when the outermost scope unwinds.
         */
        class PanelDispatchScope
        {
            MainMenuScene& scene;

        public:
            explicit PanelDispatchScope(MainMenuScene& scene)
                : scene(scene)
            {
                ++scene.uiDispatchDepth;
            }

            ~PanelDispatchScope()
            {
                if (--scene.uiDispatchDepth == 0)
                {
                    scene.retiredPanels.clear();
                }
            }

            PanelDispatchScope(const PanelDispatchScope&) = delete;
            PanelDispatchScope& operator=(const PanelDispatchScope&) = delete;
        };

        Point toScaledCoordinates(int x, int y) const;

        std::vector<std::string> getMapNames();

        /**
         * Sets the five retail SKIRMISH.GUI staged toggles to the
         * stage indices stored in the model (stage order == enum
         * order for all five controls).
         */
        void syncSkirmishToggleStages(UiPanel& panel);

        /**
         * Advances the named skirmish staged button by one stage
         * (wrapping) and returns the new stage index. No-op returning
         * 0 when the button is absent.
         */
        unsigned int cycleSkirmishStage(const std::string& name);

        /**
         * Reloads the committed map's schema resource defaults into
         * the model and reseeds occupied player slots (retail's
         * schema HumanMetal/Energy, ComputerMetal/Energy grants;
         * later user edits act as per-slot overrides).
         */
        void seedSkirmishResourceDefaults();

        /** Reseeds one slot's metal/energy from the resolved defaults. */
        void applySkirmishResourceDefaults(unsigned int playerIndex);

        void attachPlayerSelectionComponents(const std::string& guiName, UiPanel& panel);

        void attachDetailedPlayerSelectionComponents(const std::string& guiName, UiPanel& panel, int i);

        /**
         * Retail-style modal message box for a rejected skirmish
         * Start (TotalA.exe pops the reason via its message-box
         * helper; the dialog dismisses through PrevMenu).
         */
        void showSkirmishStartError(const std::string& message);

        bool hasMultiplayerSchema(const std::string& mapName);
    };
}
