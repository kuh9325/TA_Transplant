#include "EndMissionScene.h"

#include <rwe/LoadingScene.h>
#include <rwe/MainMenuScene.h>
#include <rwe/camera_util.h>
#include <rwe/io/gui/gui.h>
#include <rwe/observable/BehaviorSubject.h>
#include <rwe/save/SaveGameService.h>
#include <rwe/save/SaveListDialog.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/UiTextBox.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>

#include <algorithm>
#include <ctime>
#include <filesystem>

namespace rwe
{
    namespace
    {
        const Viewport EndMissionViewport(0, 0, 640, 480);

        /**
         * Stats-table geometry measured from the baked headers on
         * outcome0/outcome1 PCX: column divider lines sit at
         * x = 12,107,181,255,329,403,477,551,626, header text runs
         * y~58-88 and the cell area y~92-294 (ten ~20px rows).
         * UiRenderService text is baseline-anchored: hattfont12
         * glyphs have posY=11, so a 12px glyph occupies rows
         * y-11..y. statsFirstRowBaselineY is therefore the first
         * row's baseline, centering it in the first cell.
         * Retail draws the rows programmatically (paint proc 0x468cf0
         * over the ctx+0xdcb stats block); its exact row pitch is not
         * decoded, so the pitch remains an approximation.
         */
        constexpr float statsCellCenterX[8] = {60, 144, 218, 292, 366, 440, 514, 589};
        constexpr float statsNameX = 18.0f;
        constexpr float statsFirstRowBaselineY = 107.0f;
        constexpr float statsRowHeight = 20.0f;

        /**
         * Per-player stats rows + result bitmap + game time, drawn over
         * the outcome PCX's baked table. Replaces the retail ENDMSN
         * panel paint proc (0x468cf0) which renders the same fields.
         */
        class DebriefStatsComponent : public UiComponent
        {
        private:
            const std::vector<DebriefPlayerRow>* rows;
            std::string gameTimeText;
            std::shared_ptr<Sprite> resultSprite;
            std::shared_ptr<SpriteSeries> font;

        public:
            DebriefStatsComponent(
                const std::vector<DebriefPlayerRow>* rows,
                const std::string& gameTimeText,
                std::shared_ptr<Sprite> resultSprite,
                std::shared_ptr<SpriteSeries> font)
                : UiComponent(0, 0, 640, 480),
                  rows(rows),
                  gameTimeText(gameTimeText),
                  resultSprite(std::move(resultSprite)),
                  font(std::move(font))
            {
            }

            /** Display-only overlay: clicks pass through to gadgets. */
            bool acceptsMouseInput() const override
            {
                return false;
            }

            void render(UiRenderService& graphics) const override
            {
                // result bitmap (ENDMSN.GAF victory/defeat, 129x29 /
                // 101x29): the GAF frames carry their authored canvas
                // position in posX/posY (320,240 for both outcomes --
                // centred over the stats table's lower rows). Sprite
                // bounds store the negated offsets, so drawing abs at
                // (-left, -top) reproduces the authored position;
                // drawSprite would land the frame mostly off-screen.
                if (resultSprite)
                {
                    graphics.drawSpriteAbs(
                        -resultSprite->bounds.left(),
                        -resultSprite->bounds.top(),
                        *resultSprite);
                }

                // "Game Time : %02d:%02d:%02d" (retail fmt 0x507794
                // with the "Game Time" label at 0x504910). Retail
                // draws it left-aligned on the info line under the
                // stats table: x = tableRect.left + 25, baseline =
                // tableRect.bottom + scroll + 10 (0x468af8-0x468b9e
                // calling 0x4a50e0). The exact table rect isn't
                // decoded; using the measured edges (left 12, bottom
                // ~297) with scroll taken as 0.
                graphics.drawText(37.0f, 310.0f, gameTimeText, *font);

                // stats rows under the baked column headers; numeric
                // values center under their headers, names hug the
                // left edge of the Name cell
                for (std::size_t i = 0; i < rows->size(); ++i)
                {
                    const auto& r = (*rows)[i];
                    float y = statsFirstRowBaselineY + i * statsRowHeight;
                    graphics.drawText(statsNameX, y, r.name, *font, Color(255, 255, 255));
                    graphics.drawTextCenteredX(statsCellCenterX[1], y, std::to_string(r.kills), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[2], y, std::to_string(r.losses), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[3], y, std::to_string(r.energyProduced), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[4], y, std::to_string(r.metalProduced), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[5], y, std::to_string(r.energyExcess), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[6], y, std::to_string(r.metalExcess), *font);
                    graphics.drawTextCenteredX(statsCellCenterX[7], y, std::to_string(r.score), *font);
                }
            }
        };

        /**
         * Retail status colours: the ENDMSN listbox builder (0x41eaa0)
         * prefixes each entry with 0xfe (won), 0xff (lost) or 0xfd
         * (unplayed). The exact palette indices those escapes select
         * are not decoded; these are approximations.
         */
        Color statusColor(CampaignMissionStatus status)
        {
            switch (status)
            {
                case CampaignMissionStatus::Won:
                    return Color(80, 220, 80);
                case CampaignMissionStatus::Lost:
                    return Color(220, 80, 80);
                case CampaignMissionStatus::Unplayed:
                default:
                    return Color(160, 160, 160);
            }
        }
    }

    EndMissionScene::EndMissionScene(
        const SceneContext& sceneContext,
        TdfBlock* audioLookup,
        const EndMissionInfo& info,
        std::unique_ptr<GameSimulation> completedSimulation,
        GameParameters&& completedParameters)
        : sceneContext(sceneContext),
          soundLookup(audioLookup),
          info(info),
          scaledUiRenderService(sceneContext.graphics, sceneContext.shaders, &EndMissionViewport),
          nativeUiRenderService(sceneContext.graphics, sceneContext.shaders, sceneContext.viewport),
          uiFactory(sceneContext.textureService, sceneContext.audioService, soundLookup, sceneContext.vfs, sceneContext.pathMapping, 640, 480),
          difficulty(info.campaign.difficulty),
          completedSimulation(std::move(completedSimulation)),
          completedParameters(std::move(completedParameters))
    {
    }

    EndMissionScene::~EndMissionScene() = default;

    void EndMissionScene::init()
    {
        auto campaigns = loadCampaignDescriptors(*sceneContext.vfs, sceneContext.pathMapping->camps);

        // Resolve the descriptor this mission belongs to. Frontend
        // launches carry the descriptor's display name; direct
        // --campaign launches carry the mission file instead, so fall
        // back to matching missionfile+side.
        const CampaignDescriptorFile* match = nullptr;
        for (const auto& c : campaigns)
        {
            if (toUpper(c.descriptor.campaignSide) != toUpper(info.campaign.campaignSide))
            {
                continue;
            }
            if (c.displayName == info.campaign.campaignName)
            {
                match = &c;
                break;
            }
        }
        if (!match)
        {
            for (const auto& c : campaigns)
            {
                if (toUpper(c.descriptor.campaignSide) != toUpper(info.campaign.campaignSide))
                {
                    continue;
                }
                auto it = std::find_if(c.descriptor.missions.begin(), c.descriptor.missions.end(), [&](const auto& m) {
                    return toUpper(m.missionFile) == toUpper(info.campaign.missionFile);
                });
                if (it != c.descriptor.missions.end())
                {
                    match = &c;
                    break;
                }
            }
        }

        unsigned int missionCount = 0;
        if (match)
        {
            campaignFile = *match;
            missionCount = match->descriptor.missions.size();

            // index the finished mission inside the descriptor; the
            // stored index wins unless the file maps elsewhere
            // (e.g. --campaign always passes missionIndex 0)
            missionIndex = info.campaign.missionIndex;
            auto it = std::find_if(match->descriptor.missions.begin(), match->descriptor.missions.end(), [&](const auto& m) {
                return toUpper(m.missionFile) == toUpper(info.campaign.missionFile);
            });
            if (it != match->descriptor.missions.end())
            {
                missionIndex = it->index;
            }

            // retail stamps the status at mission teardown (0x41dc20)
            sceneContext.campaignProgress->recordResult(
                match->displayName, match->descriptor.campaignSide, missionIndex, info.won, missionCount);
        }

        // Retail outcome selection (0x41f115-0x41f1e4): continuation
        // requires a campaign context; on victory a further mission
        // must exist, on defeat the screen always continues (retry).
        bool nextExists = match && missionIndex + 1 < missionCount;
        hasContinuation = endmsnHasContinuation(match != nullptr, info.won, nextExists);

        auto guiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/ENDMSN.GUI");
        if (!guiRaw)
        {
            throw std::runtime_error("Couldn't read ENDMSN.GUI");
        }
        auto parsedGui = parseGuiFromBytes(*guiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse ENDMSN.GUI");
        }

        panel = uiFactory.panelFromGuiFile("ENDMSN", hasContinuation ? "outcome1" : "outcome0", *parsedGui);

        // the bitmap-background overload does not wire the GUI header's
        // key defaults; ENDMSN declares crdefault=Start (Enter = Start),
        // but on the outcome0 path retail moves the default action to
        // MainMenu (0x41f1d2-0x41f1e4 activates the MainMenu button)
        if (hasContinuation && (*parsedGui)[0].crDefault)
        {
            panel->setCrDefault(*(*parsedGui)[0].crDefault);
        }
        else
        {
            panel->setCrDefault("MainMenu");
        }

        font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        // result bitmap from ENDMSN.GAF (victory/defeat entries)
        std::shared_ptr<Sprite> resultSprite;
        if (auto series = sceneContext.textureService->getGuiTexture("ENDMSN", info.won ? "victory" : "defeat"))
        {
            if (!(*series)->sprites.empty())
            {
                resultSprite = (*series)->sprites.front();
            }
        }

        panel->appendChild(std::make_unique<DebriefStatsComponent>(
            &info.stats,
            "Game Time : " + debriefGameTimeText(info.gameTime),
            std::move(resultSprite),
            font));

        // Missions listbox + KNOB are only populated when the screen
        // offers continuation (retail fills the list under flag==1).
        if (hasContinuation)
        {
            if (auto listBox = panel->find<UiListBox>("Missions"))
            {
                auto& lb = listBox->get();
                const auto* run = sceneContext.campaignProgress->findRun(match->displayName, match->descriptor.campaignSide);
                for (const auto& m : match->descriptor.missions)
                {
                    auto status = run && m.index < run->status.size() ? run->status[m.index] : CampaignMissionStatus::Unplayed;
                    lb.appendItem(m.missionName, statusColor(status));
                }
                lb.setSelectedIndex(debriefSelectedMissionIndex(missionIndex, info.won, missionCount));
                auto sub = lb.selectedIndex().subscribe([this](const auto& sel) {
                    selectedMission = sel;
                });
                lb.addSubscription(std::move(sub));
            }
        }
        else
        {
            // outcome0 mode: no continuation -- retail enables
            // MainMenu only (0x41f400-0x41f532): Start, LoadGame,
            // SaveGame, the difficulty controls and the mission
            // list are all inert.
            if (auto listBox = panel->find<UiListBox>("Missions"))
            {
                listBox->get().clearItems();
            }
            for (const auto* name : {"Start", "LoadGame", "SaveGame", "Difficulty", "AdjustDiff"})
            {
                if (auto b = panel->find<UiStagedButton>(name))
                {
                    b->get().setEnabled(false);
                }
            }
            panel->setFocusByName("MainMenu");
        }

        // Difficulty staged button: Easy|Medium|Hard
        if (auto diff = panel->find<UiStagedButton>("Difficulty"))
        {
            static const std::vector<std::string> stages{"Easy", "Medium", "Hard"};
            auto it = std::find_if(stages.begin(), stages.end(), [&](const auto& s) { return toUpper(s) == toUpper(difficulty); });
            if (it != stages.end())
            {
                diff->get().setStage(static_cast<unsigned int>(it - stages.begin()));
            }
        }

        panel->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    void EndMissionScene::render()
    {
        panel->render(scaledUiRenderService);
        if (!dialogStack.empty())
        {
            scaledUiRenderService.fillScreen(Color(0, 0, 0, 63));
            dialogStack.back()->render(scaledUiRenderService);
        }
    }

    void EndMissionScene::update(int millisecondsElapsed)
    {
        PanelDispatchScope dispatchScope(*this);
        (dialogStack.empty() ? *panel : *dialogStack.back())
            .update(static_cast<float>(millisecondsElapsed) / 1000.0f);
    }

    MouseButtonEvent EndMissionScene::toScaledEvent(MouseButtonEvent event) const
    {
        auto clip = screenToWorldRayUtil(
            scaledUiRenderService.getInverseViewProjectionMatrix(),
            sceneContext.viewport->toClipSpace(event.x, event.y));
        event.x = static_cast<int>(clip.origin.x);
        event.y = static_cast<int>(clip.origin.y);
        return event;
    }

    void EndMissionScene::onMouseDown(MouseButtonEvent event)
    {
        PanelDispatchScope dispatchScope(*this);
        (dialogStack.empty() ? *panel : *dialogStack.back()).mouseDown(toScaledEvent(event));
    }

    void EndMissionScene::onMouseUp(MouseButtonEvent event)
    {
        PanelDispatchScope dispatchScope(*this);
        (dialogStack.empty() ? *panel : *dialogStack.back()).mouseUp(toScaledEvent(event));
    }

    void EndMissionScene::onMouseMove(MouseMoveEvent event)
    {
        auto clip = screenToWorldRayUtil(
            scaledUiRenderService.getInverseViewProjectionMatrix(),
            sceneContext.viewport->toClipSpace(event.x, event.y));
        event.x = static_cast<int>(clip.origin.x);
        event.y = static_cast<int>(clip.origin.y);
        PanelDispatchScope dispatchScope(*this);
        (dialogStack.empty() ? *panel : *dialogStack.back()).mouseMove(event);
    }

    void EndMissionScene::onMouseWheel(MouseWheelEvent event)
    {
        PanelDispatchScope dispatchScope(*this);
        (dialogStack.empty() ? *panel : *dialogStack.back()).mouseWheel(event);
    }

    void EndMissionScene::onKeyDown(const SDL_KeyboardEvent& keysym)
    {
        PanelDispatchScope dispatchScope(*this);
        auto& target = dialogStack.empty() ? *panel : *dialogStack.back();
        // Esc/Cr defaults come from the GUI file, as in retail; Escape
        // pops only when nothing on the dialog consumed the key -- a
        // focused TextBox uses it to drop focus, and an Escape quickkey
        // (LOADGAME's CANCEL=27) may dismiss instead
        if (target.keyDown(KeyEvent(keysym.key)))
        {
            return;
        }
        if (!dialogStack.empty() && keysym.key == SDLK_ESCAPE)
        {
            popDialog();
        }
    }

    void EndMissionScene::message(const std::string& topic, const std::string& message, const ActivateMessage& /*details*/)
    {
        if (topic == "LOADGAME")
        {
            if (message == "LOAD")
            {
                // the stock LOADGAME.GUI's OK button is named LOAD;
                // in save mode it confirms the save instead
                // (retail 0x492df0)
                if (saveDialogPanel != nullptr)
                {
                    performSaveGame();
                }
                else
                {
                    loadSelectedSave();
                }
            }
            else if (message == "CANCEL")
            {
                popDialog();
            }
            else if (message == "DELETE")
            {
                deleteSelectedSave();
            }
            return;
        }

        if (topic == "YESORNO")
        {
            if (message == "CHOICE1")
            {
                auto action = std::move(pendingConfirmAction);
                pendingConfirmAction = nullptr;
                popDialog();
                if (action)
                {
                    action();
                }
            }
            else if (message == "CHOICE2")
            {
                pendingConfirmAction = nullptr;
                popDialog();
            }
            return;
        }

        if (message == "Start")
        {
            startSelectedMission();
        }
        else if (message == "MainMenu")
        {
            goToMainMenu();
        }
        else if (message == "Difficulty")
        {
            cycleDifficulty();
        }
        else if (message == "LoadGame")
        {
            openLoadGameDialog();
        }
        else if (message == "SaveGame")
        {
            openSaveGameDialog();
        }
    }

    void EndMissionScene::openLoadGameDialog()
    {
        auto dialog = createFrontendLoadGamePanel(
            uiFactory,
            sceneContext.textureService,
            sceneContext.vfs,
            false,
            &saveListEntries);

        saveListEntries.clear();
        if (auto dir = saveGameDirectory())
        {
            saveListEntries = listSavedGames(*dir);
        }
        populateSaveList(*dialog, saveListEntries);

        auto& p = dialogStack.emplace_back(std::move(dialog));
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    void EndMissionScene::openSaveGameDialog()
    {
        // retail ENDMSN SaveGame (0x493060) opens LOADGAME.GUI in
        // save mode: GAMENAME is the editable target and the OK
        // button writes the save. The simulation underneath is the
        // completed mission handed over by GameScene.
        auto dialog = createFrontendLoadGamePanel(
            uiFactory,
            sceneContext.textureService,
            sceneContext.vfs,
            true,
            &saveListEntries);

        saveListEntries.clear();
        if (auto dir = saveGameDirectory())
        {
            saveListEntries = listSavedGames(*dir);
        }
        populateSaveList(*dialog, saveListEntries);

        // save mode starts with an empty entry field focused, same
        // as the in-game SAVELIST dialog
        if (auto box = dialog->find<UiTextBox>("GAMENAME"))
        {
            box->get().setText("");
        }
        dialog->setFocusByName("GAMENAME");

        saveDialogPanel = dialog.get();
        auto& p = dialogStack.emplace_back(std::move(dialog));
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    void EndMissionScene::performSaveGame()
    {
        if (saveDialogPanel == nullptr || !completedSimulation)
        {
            return;
        }

        std::string typed;
        if (auto box = saveDialogPanel->find<UiTextBox>("GAMENAME"))
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

        auto doSave = [this, name, dir]() {
            // the campaign thumbs were stamped by recordResult during
            // init() -- before any dialog could open -- matching the
            // retail order (mission teardown stamps thumbs at
            // 0x41dc97, then the ENDMSN save writer serializes them
            // at 0x432863)
            auto bytes = encodeSavedGame(
                *completedSimulation,
                completedParameters,
                sceneContext.campaignProgress,
                name,
                static_cast<std::uint64_t>(std::time(nullptr)));
            writeSavedGameFile(*dir, name, bytes);

            // retail leaves the dialog open after writing; refresh
            // the list so the new entry shows
            if (saveDialogPanel != nullptr)
            {
                saveListEntries = listSavedGames(*dir);
                populateSaveList(*saveDialogPanel, saveListEntries);
            }
        };

        auto path = saveGamePath(*dir, name);
        if (path && std::filesystem::exists(*path))
        {
            pendingConfirmAction = std::move(doSave);
            pushConfirmDialog("Overwrite existing saved game?");
            return;
        }

        doSave();
    }

    void EndMissionScene::pushConfirmDialog(const std::string& titleText)
    {
        auto dialog = uiFactory.panelFromGuiFile("YESORNO");
        if (auto title = dialog->find<UiLabel>("TITLE"))
        {
            title->get().setText(titleText);
        }
        auto& p = dialogStack.emplace_back(std::move(dialog));
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    void EndMissionScene::popDialog()
    {
        if (dialogStack.empty())
        {
            return;
        }
        if (dialogStack.back().get() == saveDialogPanel)
        {
            saveDialogPanel = nullptr;
        }
        // a popped dialog may be mid-dispatch; retire it until the
        // outermost event scope unwinds
        retiredDialogs.push_back(std::move(dialogStack.back()));
        dialogStack.pop_back();
        reapRetiredDialogs();
    }

    void EndMissionScene::reapRetiredDialogs()
    {
        if (uiDispatchDepth == 0)
        {
            retiredDialogs.clear();
        }
    }

    void EndMissionScene::loadSelectedSave()
    {
        if (dialogStack.empty())
        {
            return;
        }
        auto listBox = dialogStack.back()->find<UiListBox>("GAMES");
        if (!listBox)
        {
            return;
        }
        auto selected = listBox->get().getSelectedIndex();
        if (!selected || *selected >= saveListEntries.size())
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
            auto bytes = readSavedGameFile(*dir, saveListEntries[*selected].saveName);
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
            soundLookup,
            AudioService::LoopToken(),
            params,
            std::move(decoded)));
    }

    void EndMissionScene::deleteSelectedSave()
    {
        if (dialogStack.empty())
        {
            return;
        }
        auto listBox = dialogStack.back()->find<UiListBox>("GAMES");
        if (!listBox)
        {
            return;
        }
        auto selected = listBox->get().getSelectedIndex();
        if (!selected || *selected >= saveListEntries.size())
        {
            return;
        }
        if (auto dir = saveGameDirectory())
        {
            deleteSavedGameFile(*dir, saveListEntries[*selected].saveName);
        }
        saveListEntries.clear();
        if (auto dir = saveGameDirectory())
        {
            saveListEntries = listSavedGames(*dir);
        }
        populateSaveList(*dialogStack.back(), saveListEntries);
    }

    void EndMissionScene::startSelectedMission()
    {
        if (!campaignFile || !hasContinuation)
        {
            return;
        }

        if (!selectedMission || *selectedMission >= campaignFile->descriptor.missions.size())
        {
            return;
        }

        const auto& mission = campaignFile->descriptor.missions[*selectedMission];

        // The staged difficulty is carried through unchanged; the
        // LoadingScene resolves it to a schema using retail's ordered
        // fallback (staged, then remaining ranks ascending), which is
        // how a staged Easy/Medium still launches stock "Krogoth
        // Encounter"'s single Type=Hard schema.
        auto mapName = missionFileToMapName(mission.missionFile);

        GameParameters params{mapName, 0};
        params.campaign = CampaignMissionParameters{
            campaignFile->displayName,
            campaignFile->descriptor.campaignSide,
            mission.index,
            mission.missionFile,
            mission.missionName,
            difficulty};

        auto scene = std::make_unique<LoadingScene>(
            sceneContext,
            soundLookup,
            AudioService::LoopToken(),
            params);

        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }

    void EndMissionScene::cycleDifficulty()
    {
        static const std::vector<std::string> stages{"Easy", "Medium", "Hard"};
        auto it = std::find_if(stages.begin(), stages.end(), [&](const auto& s) { return toUpper(s) == toUpper(difficulty); });
        auto idx = it == stages.end() ? 1 : static_cast<unsigned int>(it - stages.begin());
        idx = (idx + 1) % stages.size();
        difficulty = stages[idx];
    }

    void EndMissionScene::goToMainMenu()
    {
        auto scene = std::make_unique<MainMenuScene>(
            sceneContext,
            soundLookup,
            static_cast<float>(sceneContext.viewport->width()),
            static_cast<float>(sceneContext.viewport->height()));
        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }
}
