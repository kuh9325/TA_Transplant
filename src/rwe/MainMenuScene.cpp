#include "MainMenuScene.h"
#include <algorithm>
#include <set>
#include <rwe/LoadingScene.h>
#include <rwe/LoadingScene_util.h>
#include <rwe/MainMenuModel.h>
#include <rwe/camera_util.h>
#include <rwe/config.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/gui/gui.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/resource_io.h>
#include <rwe/save/SaveGameService.h>
#include <rwe/save/SaveListDialog.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiSurface.h>
#include <rwe/ui/button_sound_util.h>
#include <rwe/ui/difficulty_util.h>
#include <rwe/ui/narration_util.h>
#include <rwe/util/Index.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    const Viewport MainMenuViewport(0, 0, 640, 480);

    MainMenuScene::MainMenuScene(
        const SceneContext& sceneContext,
        TdfBlock* audioLookup,
        float width,
        float height)
        : sceneContext(sceneContext),
          soundLookup(audioLookup),
          scaledUiRenderService(sceneContext.graphics, sceneContext.shaders, &MainMenuViewport),
          nativeUiRenderService(sceneContext.graphics, sceneContext.shaders, sceneContext.viewport),
          model(),
          uiFactory(sceneContext.textureService, sceneContext.audioService, soundLookup, sceneContext.vfs, sceneContext.pathMapping, 640, 480),
          panelStack(),
          dialogStack(),
          bgm()
    {
    }

    void MainMenuScene::init()
    {
        bgm = startBgm();
        goToMainMenu();
    }

    void MainMenuScene::render()
    {
        panelStack.back()->render(scaledUiRenderService);

        for (auto& e : dialogStack)
        {
            scaledUiRenderService.fillScreen(Color(0, 0, 0, 63));
            e->render(scaledUiRenderService);
        }
    }

    void MainMenuScene::onMouseDown(MouseButtonEvent event)
    {
        auto p = toScaledCoordinates(event.x, event.y);
        event.x = p.x;
        event.y = p.y;
        PanelDispatchScope dispatchScope(*this);
        topPanel().mouseDown(event);
    }

    void MainMenuScene::onMouseUp(MouseButtonEvent event)
    {
        auto p = toScaledCoordinates(event.x, event.y);
        event.x = p.x;
        event.y = p.y;
        PanelDispatchScope dispatchScope(*this);
        topPanel().mouseUp(event);
    }

    void MainMenuScene::onMouseMove(MouseMoveEvent event)
    {
        auto p = toScaledCoordinates(event.x, event.y);
        event.x = p.x;
        event.y = p.y;
        PanelDispatchScope dispatchScope(*this);
        topPanel().mouseMove(event);
    }

    AudioService::LoopToken MainMenuScene::startBgm()
    {
        auto bgmBlock = soundLookup->findBlock("BGM");
        if (!bgmBlock)
        {
            return AudioService::LoopToken();
        }

        auto bgmName = bgmBlock->get().findValue("sound");
        if (!bgmName)
        {
            return AudioService::LoopToken();
        }

        auto bgm = sceneContext.audioService->loadSound(*bgmName);
        if (!bgm)
        {
            return AudioService::LoopToken();
        }

        return sceneContext.audioService->loopSound(*bgm);
    }

    void MainMenuScene::goToPreviousMenu()
    {
        if (!dialogStack.empty())
        {
            // a popped panel may be mid-dispatch; retire it until the
            // outermost event scope unwinds
            retiredPanels.push_back(std::move(dialogStack.back()));
            dialogStack.pop_back();
            reapRetiredPanels();
            return;
        }

        // leaving the briefing screen stops mission narration
        if (narrationChannel >= 0)
        {
            sceneContext.audioService->stopChannel(narrationChannel);
            narrationChannel = -1;
        }

        if (panelStack.size() > 1)
        {
            retiredPanels.push_back(std::move(panelStack.back()));
            panelStack.pop_back();
            reapRetiredPanels();
            return;
        }

        exit();
    }

    void MainMenuScene::reapRetiredPanels()
    {
        if (uiDispatchDepth == 0)
        {
            retiredPanels.clear();
        }
    }

    void MainMenuScene::goToMenu(std::unique_ptr<UiPanel>&& panel)
    {
        auto& p = panelStack.emplace_back(std::move(panel));
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    void MainMenuScene::openDialog(std::unique_ptr<UiPanel>&& panel)
    {
        auto& p = dialogStack.emplace_back(std::move(panel));
        p->groupMessages().subscribe([this](const auto& msg) {
            if (auto activate = std::get_if<ActivateMessage>(&msg.message); activate != nullptr)
            {
                message(msg.topic, msg.controlName, *activate);
            }
        });
    }

    UiPanel& MainMenuScene::topPanel()
    {
        if (!dialogStack.empty())
        {
            return *(dialogStack.back());
        }

        return *(panelStack.back());
    }

    void MainMenuScene::update(int millisecondsElapsed)
    {
        PanelDispatchScope dispatchScope(*this);
        topPanel().update(static_cast<float>(millisecondsElapsed) / 1000.0f);

        // Narration finishing on its own is indistinguishable from a manual
        // stop: drop the channel and move SHUTUP to its "No Narration" stage.
        if (narrationChannel >= 0 && !sceneContext.audioService->isPlaying(narrationChannel))
        {
            narrationChannel = -1;
            if (auto b = topPanel().find<UiStagedButton>("SHUTUP"))
            {
                b->get().setStage(campaignNarrationStageIndex(false));
            }
        }
    }

    void MainMenuScene::onMouseWheel(MouseWheelEvent event)
    {
        PanelDispatchScope dispatchScope(*this);
        topPanel().mouseWheel(event);
    }

    void MainMenuScene::onKeyDown(const SDL_KeyboardEvent& keysym)
    {
        PanelDispatchScope dispatchScope(*this);
        topPanel().keyDown(KeyEvent(keysym.key));
    }

    void MainMenuScene::goToMainMenu()
    {
        auto mainMenuGuiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/MAINMENU.GUI");
        if (!mainMenuGuiRaw)
        {
            throw std::runtime_error("Couldn't read MAINMENU.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*mainMenuGuiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        auto panel = uiFactory.panelFromGuiFile("MAINMENU", "FrontendX", *parsedGui);
        if (auto debugStrLabel = panel->find<UiLabel>("DebugString"))
        {
            debugStrLabel->get().setText(ProjectNameVersion);
            debugStrLabel->get().setAlignment(UiLabel::Alignment::Center);
        }
        goToMenu(std::move(panel));
    }

    void MainMenuScene::exit()
    {
        sceneContext.sceneManager->requestExit();
    }

    std::optional<int> matchesPlayer(const std::string& pattern, const std::string& input)
    {
        // FIXME: this should probably be a regex match instead of this crude brute-force search
        for (int i = 0; i < 10; ++i)
        {
            std::string candidate = pattern;
            auto pos = candidate.find("{0}");
            if (pos != std::string::npos)
            {
                candidate.replace(pos, 3, std::to_string(i));
            }
            if (input == candidate)
            {
                return i;
            }
        }

        return std::nullopt;
    }

    void MainMenuScene::message(const std::string& topic, const std::string& message, const ActivateMessage& details)
    {
        if (message == "PrevMenu" || message == "PREVMENU")
        {
            goToPreviousMenu();
        }
        else if (topic == "MAINMENU")
        {
            if (message == "EXIT")
            {
                exit();
            }
            else if (message == "SINGLE")
            {
                goToSingleMenu();
            }
        }
        else if (topic == "SINGLE")
        {
            if (message == "Skirmish")
            {
                goToSkirmishMenu();
            }
            else if (message == "NewCamp")
            {
                goToNewCampaignMenu();
            }
            else if (message == "LoadGame")
            {
                openLoadGameScreen();
            }
            // AnyMsn and Options are not yet supported; their events
            // are intentionally dropped here.
        }
        else if (topic == "LOADGAME")
        {
            if (message == "LOAD")
            {
                loadSelectedSave();
            }
            else if (message == "CANCEL")
            {
                goToPreviousMenu();
            }
            else if (message == "DELETE")
            {
                deleteSelectedSave();
            }
        }
        else if (topic == "NEWGAME")
        {
            if (message == "Start")
            {
                startCampaignBriefing();
            }
            else if (message == "Side0" || message == "Arm")
            {
                setCampaignSide("ARM");
            }
            else if (message == "Side1" || message == "Core")
            {
                setCampaignSide("CORE");
            }
            else if (message == "Difficulty")
            {
                cycleCampaignDifficulty();
            }
        }
        else if (topic == "MSNBRIEF")
        {
            if (message == "Start")
            {
                startCampaignMission();
            }
            else if (message == "SHUTUP")
            {
                toggleCampaignNarration();
            }
        }
        else if (topic == "SKIRMISH")
        {
            if (message == "SelectMap")
            {
                resetCandidateSelectedMap();
                openMapSelectionDialog();
            }
            else if (message == "Start")
            {
                startGame();
            }
            else if (message == "StartLocation")
            {
                model.skirmish.startLocation = static_cast<SkirmishStartLocation>(cycleSkirmishStage("StartLocation"));
            }
            else if (message == "CommanderDeath")
            {
                model.skirmish.commanderDeath = static_cast<SkirmishCommanderDeath>(cycleSkirmishStage("CommanderDeath"));
            }
            else if (message == "Mapping")
            {
                model.skirmish.mapping = static_cast<SkirmishMapping>(cycleSkirmishStage("Mapping"));
            }
            else if (message == "LineOfSight")
            {
                model.skirmish.lineOfSight = static_cast<SkirmishLineOfSight>(cycleSkirmishStage("LineOfSight"));
            }
            else if (message == "Difficulty")
            {
                model.skirmish.difficulty = static_cast<SkirmishDifficulty>(cycleSkirmishStage("Difficulty"));
            }
            else if (auto num = matchesPlayer("PLAYER{0}", message))
            {
                togglePlayer(*num);
            }
            else if (auto num = matchesPlayer("PLAYER{0}_side", message))
            {
                togglePlayerSide(*num);
            }
            else if (auto num = matchesPlayer("PLAYER{0}_color", message))
            {
                switch (details.type)
                {
                    case ActivateMessage::Type::Primary:
                        cyclePlayerColor(*num);
                        break;
                    case ActivateMessage::Type::Secondary:
                        reverseCyclePlayerColor(*num);
                        break;
                    default:
                        throw std::logic_error("Invalid activation type");
                }
            }
            else if (auto num = matchesPlayer("PLAYER{0}_team", message))
            {
                cyclePlayerTeam(*num);
            }
            else if (auto num = matchesPlayer("PLAYER{0}_metal", message))
            {
                switch (details.type)
                {
                    case ActivateMessage::Type::Primary:
                        incrementPlayerMetal(*num);
                        break;
                    case ActivateMessage::Type::Secondary:
                        decrementPlayerMetal(*num);
                        break;
                    default:
                        throw std::logic_error("Invalid activation type");
                }
            }
            else if (auto num = matchesPlayer("PLAYER{0}_energy", message))
            {
                switch (details.type)
                {
                    case ActivateMessage::Type::Primary:
                        incrementPlayerEnergy(*num);
                        break;
                    case ActivateMessage::Type::Secondary:
                        decrementPlayerEnergy(*num);
                        break;
                    default:
                        throw std::logic_error("Invalid activation type");
                }
            }
        }
        else if (topic == "SELMAP")
        {
            if (message == "LOAD")
            {
                commitSelectedMap();
                goToPreviousMenu();
            }
        }
    }

    void MainMenuScene::goToSingleMenu()
    {
        auto mainMenuGuiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/SINGLE.GUI");
        if (!mainMenuGuiRaw)
        {
            throw std::runtime_error("Couldn't read SINGLE.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*mainMenuGuiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        auto panel = uiFactory.panelFromGuiFile("SINGLE", "SINGLEBG", *parsedGui);
        goToMenu(std::move(panel));
    }

    void MainMenuScene::goToSkirmishMenu()
    {
        auto mainMenuGuiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/SKIRMISH.GUI");
        if (!mainMenuGuiRaw)
        {
            throw std::runtime_error("Couldn't read SKIRMISH.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*mainMenuGuiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        auto panel = uiFactory.panelFromGuiFile("SKIRMISH", "Skirmsetup4x", *parsedGui);
        if (auto mapLabel = panel->find<UiLabel>("MapName"))
        {
            auto sub = model.selectedMap.subscribe([&l = mapLabel->get()](const auto& selectedMap) {
                if (selectedMap)
                {
                    l.setText(selectedMap->name);
                }
                else
                {
                    l.setText("");
                }
            });
            mapLabel->get().addSubscription(std::move(sub));
        }

        attachPlayerSelectionComponents("SKIRMISH", *panel);

        syncSkirmishToggleStages(*panel);

        goToMenu(std::move(panel));
    }

    void MainMenuScene::goToNewCampaignMenu()
    {
        auto guiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/NEWGAME.GUI");
        if (!guiRaw)
        {
            throw std::runtime_error("Couldn't read NEWGAME.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*guiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        // lazily load the stock campaign catalog once
        if (model.campaign.campaigns.empty())
        {
            model.campaign.campaigns = loadCampaignDescriptors(*sceneContext.vfs, sceneContext.pathMapping->camps);
        }

        auto panel = uiFactory.panelFromGuiFile("NEWGAME", "newcampaign4x", *parsedGui);

        refreshCampaignList();
        populateCampaignControls(*panel);
        uiFactory.playButtonSound(newGameCampaignSelectSoundKey);

        if (auto listBox = panel->find<UiListBox>("Campaign"))
        {
            auto& lb = listBox->get();
            auto sub = lb.selectedIndex().subscribe([this](const auto& sel) {
                if (updatingCampaignControls)
                {
                    return;
                }
                model.campaign.selectedIndex = sel;
                model.campaign.selectedMissionIndex = std::nullopt;
                populateCampaignControls(topPanel());
            });
            lb.addSubscription(std::move(sub));
        }

        if (auto missionsBox = panel->find<UiListBox>("Missions"))
        {
            auto& lb = missionsBox->get();
            auto sub = lb.selectedIndex().subscribe([this](const auto& sel) {
                if (updatingCampaignControls)
                {
                    return;
                }
                model.campaign.selectedMissionIndex = sel;
            });
            lb.addSubscription(std::move(sub));
        }

        goToMenu(std::move(panel));
    }

    void MainMenuScene::populateCampaignControls(UiPanel& panel)
    {
        auto& campaign = model.campaign;

        // repopulating the listboxes re-fires their selection
        // observables; suppress them so the model stays authoritative
        updatingCampaignControls = true;

        if (auto listBox = panel.find<UiListBox>("Campaign"))
        {
            auto& lb = listBox->get();
            auto oldSelected = campaign.selectedIndex;
            lb.clearItems();
            for (auto i : campaign.displayedCampaigns)
            {
                lb.appendItem(campaign.campaigns[i].displayName);
            }
            if (!campaign.displayedCampaigns.empty())
            {
                auto index = std::min<unsigned int>(oldSelected.value_or(0), static_cast<unsigned int>(campaign.displayedCampaigns.size() - 1));
                campaign.selectedIndex = index;
                lb.setSelectedItem(campaign.campaigns[campaign.displayedCampaigns[index]].displayName);
            }
        }

        const CampaignDescriptorFile* selectedCampaign = nullptr;
        if (!campaign.displayedCampaigns.empty())
        {
            auto index = std::min<unsigned int>(campaign.selectedIndex.value_or(0), static_cast<unsigned int>(campaign.displayedCampaigns.size() - 1));
            selectedCampaign = &campaign.campaigns[campaign.displayedCampaigns[index]];
        }

        if (auto missionsBox = panel.find<UiListBox>("Missions"))
        {
            auto& lb = missionsBox->get();
            auto oldSelected = campaign.selectedMissionIndex;
            lb.clearItems();
            if (selectedCampaign)
            {
                for (const auto& mission : selectedCampaign->descriptor.missions)
                {
                    lb.appendItem(mission.missionName);
                }
                if (!selectedCampaign->descriptor.missions.empty())
                {
                    auto index = std::min<unsigned int>(oldSelected.value_or(0), static_cast<unsigned int>(selectedCampaign->descriptor.missions.size() - 1));
                    campaign.selectedMissionIndex = index;
                    lb.setSelectedItem(selectedCampaign->descriptor.missions[index].missionName);
                }
            }
        }

        for (const auto& name : {"Side0", "Arm"})
        {
            if (auto b = panel.find<UiStagedButton>(name))
            {
                b->get().setToggledOn(campaign.side == "ARM");
            }
        }
        for (const auto& name : {"Side1", "Core"})
        {
            if (auto b = panel.find<UiStagedButton>(name))
            {
                b->get().setToggledOn(campaign.side == "CORE");
            }
        }

        if (auto label = panel.find<UiLabel>("SIDENAME"))
        {
            label->get().setText(campaign.side);
        }

        if (auto b = panel.find<UiStagedButton>("Difficulty"))
        {
            b->get().setStage(campaignDifficultyStageIndex(campaign.difficulty));
        }

        updatingCampaignControls = false;
    }

    void MainMenuScene::refreshCampaignList()
    {
        auto& campaign = model.campaign;
        campaign.displayedCampaigns.clear();
        campaign.selectedIndex = std::nullopt;
        campaign.selectedMissionIndex = std::nullopt;

        for (unsigned int i = 0; i < campaign.campaigns.size(); ++i)
        {
            if (campaign.campaigns[i].descriptor.campaignSide == campaign.side)
            {
                campaign.displayedCampaigns.push_back(i);
            }
        }
    }

    void MainMenuScene::setCampaignSide(const std::string& side)
    {
        if (model.campaign.side == side)
        {
            return;
        }

        model.campaign.side = side;
        refreshCampaignList();
        populateCampaignControls(topPanel());
        uiFactory.playButtonSound(newGameCampaignSelectSoundKey);
    }

    void MainMenuScene::cycleCampaignDifficulty()
    {
        auto& difficulty = model.campaign.difficulty;
        difficulty = nextCampaignDifficulty(difficulty);

        if (auto b = topPanel().find<UiStagedButton>("Difficulty"))
        {
            b->get().setStage(campaignDifficultyStageIndex(difficulty));
        }
    }

    void MainMenuScene::startCampaignBriefing()
    {
        auto& campaign = model.campaign;
        if (campaign.displayedCampaigns.empty())
        {
            return;
        }

        auto selected = campaign.selectedIndex.value_or(0);
        if (selected >= campaign.displayedCampaigns.size())
        {
            selected = 0;
        }

        const auto& descriptorFile = campaign.campaigns[campaign.displayedCampaigns[selected]];
        if (descriptorFile.descriptor.missions.empty())
        {
            return;
        }

        // a new campaign run starts from a clean status slate --
        // retail's NEWGAME Start handler re-initialises the mission
        // status array to all-'U' (TotalA.exe 0x477bc8 -> 0x41da30)
        sceneContext.campaignProgress->resetCampaign(
            descriptorFile.displayName,
            descriptorFile.descriptor.campaignSide,
            static_cast<unsigned int>(descriptorFile.descriptor.missions.size()));

        // a new campaign defaults to its first mission
        auto missionIndex = std::min<unsigned int>(campaign.selectedMissionIndex.value_or(0), static_cast<unsigned int>(descriptorFile.descriptor.missions.size() - 1));
        const auto& mission = descriptorFile.descriptor.missions[missionIndex];

        pendingMission = PendingCampaignMission{
            descriptorFile.displayName,
            descriptorFile.descriptor.campaignSide,
            mission.index,
            mission.missionFile,
            mission.missionName};

        goToMissionBriefing();
    }

    void MainMenuScene::goToMissionBriefing()
    {
        if (!pendingMission)
        {
            return;
        }

        const auto& mission = *pendingMission;
        auto mapName = missionFileToMapName(mission.missionFile);

        auto otaRaw = sceneContext.vfs->readFile("maps/" + mapName + ".ota");
        if (!otaRaw)
        {
            throw std::runtime_error("Failed to read mission OTA: " + mapName);
        }
        std::string otaStr(otaRaw->begin(), otaRaw->end());
        auto ota = parseOta(parseTdfFromString(otaStr));

        auto guiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/MSNBRIEF.GUI");
        if (!guiRaw)
        {
            throw std::runtime_error("Couldn't read MSNBRIEF.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*guiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        // the briefing background bitmap is named by the side's
        // nameprefix, not the full side name: mbriefarm / mbriefcor
        auto sidePrefix = toLower(mission.campaignSide);
        if (auto side = sceneContext.sideData->find(mission.campaignSide); side != sceneContext.sideData->end())
        {
            sidePrefix = toLower(side->second.namePrefix);
        }
        auto panel = uiFactory.panelFromGuiFile("MSNBRIEF", "mbrief" + sidePrefix, *parsedGui);

        // mission title + briefing text
        std::string briefText;
        if (!ota.brief.empty())
        {
            auto briefRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->camps + "/briefs/" + briefFileName(ota.brief));
            if (briefRaw)
            {
                briefText = sanitizeBriefText(std::string(briefRaw->begin(), briefRaw->end()));
            }
        }

        if (auto textRegion = panel->find<UiLabel>("TextRegion"))
        {
            auto text = mission.missionName;
            if (!briefText.empty())
            {
                text += "\n\n" + briefText;
            }
            textRegion->get().setAlignment(UiLabel::Alignment::LeftWrapped);
            textRegion->get().setText(text);
        }

        // mission panorama image
        if (auto panorama = panel->find<UiSurface>("PANORAMA"))
        {
            if (!ota.glamour.empty())
            {
                if (auto sprite = sceneContext.textureService->tryGetPcxSprite("bitmaps/glamour/" + toUpper(ota.glamour) + ".PCX"))
                {
                    panorama->get().setBackground(*sprite);
                }
            }
        }

        // mission narration
        narrationChannel = -1;
        narrationSound = std::nullopt;
        if (!ota.narration.empty())
        {
            narrationSound = sceneContext.audioService->loadSoundFromPath(sceneContext.pathMapping->camps + "/briefs/" + toUpper(ota.narration) + ".WAV");
            if (narrationSound)
            {
                narrationChannel = sceneContext.audioService->playSound(*narrationSound);
            }
        }

        // stock MSNBRIEF drives the SHUTUP staged button from narration state:
        // stage 1 ("Narration", lit) while narration plays,
        // stage 0 ("No Narration", dark) when nothing is playing.
        if (auto b = panel->find<UiStagedButton>("SHUTUP"))
        {
            b->get().setStage(campaignNarrationStageIndex(narrationChannel >= 0));
        }

        goToMenu(std::move(panel));
    }

    void MainMenuScene::toggleCampaignNarration()
    {
        // Stock SHUTUP is a one-way narration kill switch: it silences
        // playback but never restarts it. The staged button mirrors the
        // playback state rather than toggling freely.
        if (narrationChannel < 0)
        {
            return;
        }

        sceneContext.audioService->stopChannel(narrationChannel);
        narrationChannel = -1;

        if (auto b = topPanel().find<UiStagedButton>("SHUTUP"))
        {
            b->get().setStage(campaignNarrationStageIndex(false));
        }
    }

    void MainMenuScene::startCampaignMission()
    {
        if (!pendingMission)
        {
            return;
        }

        if (narrationChannel >= 0)
        {
            sceneContext.audioService->stopChannel(narrationChannel);
            narrationChannel = -1;
        }

        const auto& mission = *pendingMission;

        GameParameters params{missionFileToMapName(mission.missionFile), 0};
        params.campaign = CampaignMissionParameters{
            mission.campaignName,
            mission.campaignSide,
            mission.missionIndex,
            mission.missionFile,
            mission.missionName,
            model.campaign.difficulty};

        auto scene = std::make_unique<LoadingScene>(
            sceneContext,
            soundLookup,
            std::move(bgm),
            params);

        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }

    void MainMenuScene::openLoadGameScreen()
    {
        // stock frontend load screen (LOADGAME.GUI over the
        // DLoadgame2.pcx dialog art); it navigates as a screen, not a
        // dialog
        auto panel = createFrontendLoadGamePanel(
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
        populateSaveList(*panel, saveListEntries);

        goToMenu(std::move(panel));
    }

    void MainMenuScene::loadSelectedSave()
    {
        auto listBox = topPanel().find<UiListBox>("GAMES");
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
        auto scene = std::make_unique<LoadingScene>(
            sceneContext,
            soundLookup,
            AudioService::LoopToken(),
            params,
            std::move(decoded));
        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }

    void MainMenuScene::deleteSelectedSave()
    {
        auto listBox = topPanel().find<UiListBox>("GAMES");
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
        populateSaveList(topPanel(), saveListEntries);
    }

    void MainMenuScene::openMapSelectionDialog()
    {
        auto guiRaw = sceneContext.vfs->readFile(sceneContext.pathMapping->guis + "/SELMAP.GUI");
        if (!guiRaw)
        {
            throw std::runtime_error("Couldn't read SELMAP.GUI");
        }

        auto parsedGui = parseGuiFromBytes(*guiRaw);
        if (!parsedGui)
        {
            throw std::runtime_error("Failed to parse GUI file");
        }

        auto panel = uiFactory.panelFromGuiFile("SELMAP", "DSelectmap2", *parsedGui);

        if (auto descriptionLabel = panel->find<UiLabel>("DESCRIPTION"))
        {
            descriptionLabel->get().setAlignment(UiLabel::Alignment::LeftWrapped);
            auto sub = model.candidateSelectedMap.subscribe([&l = descriptionLabel->get()](const auto& selectedMap) {
                if (selectedMap)
                {
                    l.setText(selectedMap->description);
                }
                else
                {
                    l.setText("");
                }
            });
            descriptionLabel->get().addSubscription(std::move(sub));
        }
        if (auto sizeLabel = panel->find<UiLabel>("SIZE"))
        {
            auto sub = model.candidateSelectedMap.subscribe([&l = sizeLabel->get()](const auto& selectedMap) {
                if (selectedMap)
                {
                    l.setText(selectedMap->size);
                }
                else
                {
                    l.setText("");
                }
            });
            sizeLabel->get().addSubscription(std::move(sub));
        }

        if (auto listBox = panel->find<UiListBox>("MAPNAMES"))
        {
            auto mapNames = getMapNames();
            for (const auto& e : mapNames)
            {
                listBox->get().appendItem(e);
            }

            auto sub = model.selectedMap.subscribe([&l = listBox->get()](const auto& selectedMap) {
                if (selectedMap)
                {
                    l.setSelectedItem(selectedMap->name);
                }
                else
                {
                    l.clearSelectedItem();
                }
            });
            listBox->get().addSubscription(std::move(sub));

            listBox->get().selectedIndex().subscribe([&l = listBox->get(), &c = *this](const auto& selectedMap) {
                if (selectedMap)
                {
                    c.setCandidateSelectedMap(l.getItems()[*selectedMap]);
                }
                else
                {
                    c.clearCandidateSelectedMap();
                }
            });
        }

        if (auto surface = panel->find<UiSurface>("MAPPIC"))
        {
            auto sub = model.candidateSelectedMap.subscribe([&s = surface->get()](const auto& info) {
                if (info)
                {
                    s.setBackground(info->minimap);
                }
                else
                {
                    s.clearBackground();
                }
            });
            surface->get().addSubscription(std::move(sub));
        }

        openDialog(std::move(panel));
    }

    void MainMenuScene::setCandidateSelectedMap(const std::string& mapName)
    {
        auto otaRaw = sceneContext.vfs->readFile(std::string("maps/").append(mapName).append(".ota"));
        if (!otaRaw)
        {
            return;
        }

        std::string otaStr(otaRaw->begin(), otaRaw->end());

        auto ota = parseOta(parseTdfFromString(otaStr));

        auto minimap = sceneContext.textureService->getMinimap(mapName);

        // this is what TA shows in its map selection dialog
        auto sizeInfo = std::string().append(ota.memory).append("  Players: ").append(ota.numPlayers);

        MainMenuModel::SelectedMapInfo info(
            mapName,
            ota.missionDescription,
            sizeInfo,
            minimap);

        model.candidateSelectedMap.next(std::move(info));
    }

    void MainMenuScene::resetCandidateSelectedMap()
    {
        model.candidateSelectedMap.next(model.selectedMap.getValue());
    }

    void MainMenuScene::commitSelectedMap()
    {
        model.selectedMap.next(model.candidateSelectedMap.getValue());
        seedSkirmishResourceDefaults();
    }

    void MainMenuScene::syncSkirmishToggleStages(UiPanel& panel)
    {
        const std::pair<const char*, unsigned int> toggles[] = {
            {"StartLocation", static_cast<unsigned int>(model.skirmish.startLocation)},
            {"CommanderDeath", static_cast<unsigned int>(model.skirmish.commanderDeath)},
            {"Mapping", static_cast<unsigned int>(model.skirmish.mapping)},
            {"LineOfSight", static_cast<unsigned int>(model.skirmish.lineOfSight)},
            {"Difficulty", static_cast<unsigned int>(model.skirmish.difficulty)},
        };
        for (const auto& [name, stage] : toggles)
        {
            if (auto button = panel.find<UiStagedButton>(name))
            {
                button->get().setStage(stage);
            }
        }
    }

    unsigned int MainMenuScene::cycleSkirmishStage(const std::string& name)
    {
        // the click that delivered this message came through the menu
        // panel; staged buttons do not advance themselves
        for (auto it = panelStack.rbegin(); it != panelStack.rend(); ++it)
        {
            if (auto button = (*it)->template find<UiStagedButton>(name))
            {
                auto stage = (button->get().getStage() + 1) % button->get().getStageCount();
                button->get().setStage(stage);
                return stage;
            }
        }
        return 0;
    }

    void MainMenuScene::seedSkirmishResourceDefaults()
    {
        auto& defaults = model.skirmishResourceDefaults;
        defaults = MainMenuModel::SkirmishResourceDefaults{};

        const auto& selectedMap = model.selectedMap.getValue();
        if (!selectedMap)
        {
            return;
        }

        auto otaRaw = sceneContext.vfs->readFile(std::string("maps/").append(selectedMap->name).append(".ota"));
        if (!otaRaw)
        {
            return;
        }
        std::string otaStr(otaRaw->begin(), otaRaw->end());
        auto ota = parseOta(parseTdfFromString(otaStr));

        unsigned int occupied = 0;
        for (const auto& p : model.players)
        {
            if (p.type.getValue() != MainMenuModel::PlayerSettings::Type::Open)
            {
                ++occupied;
            }
        }

        unsigned int schemaIndex;
        try
        {
            schemaIndex = resolveSkirmishSchemaIndex(ota, occupied);
        }
        catch (const std::exception&)
        {
            return;
        }
        const auto& schema = ota.schemas.at(schemaIndex);

        defaults.humanMetal = Metal(schema.humanMetal);
        defaults.humanEnergy = Energy(schema.humanEnergy);
        defaults.computerMetal = Metal(schema.computerMetal);
        defaults.computerEnergy = Energy(schema.computerEnergy);
        defaults.valid = true;

        // switching maps reloads the authored grants; user edits made
        // after this point act as the per-slot overrides
        for (Index i = 0; i < getSize(model.players); ++i)
        {
            applySkirmishResourceDefaults(i);
        }
    }

    void MainMenuScene::applySkirmishResourceDefaults(unsigned int playerIndex)
    {
        const auto& defaults = model.skirmishResourceDefaults;
        if (!defaults.valid)
        {
            return;
        }
        auto& player = model.players[playerIndex];
        switch (player.type.getValue())
        {
            case MainMenuModel::PlayerSettings::Type::Human:
                player.metal.next(defaults.humanMetal);
                player.energy.next(defaults.humanEnergy);
                break;
            case MainMenuModel::PlayerSettings::Type::Computer:
                player.metal.next(defaults.computerMetal);
                player.energy.next(defaults.computerEnergy);
                break;
            default:
                break;
        }
    }

    void MainMenuScene::clearCandidateSelectedMap()
    {
        model.candidateSelectedMap.next(std::nullopt);
    }

    void MainMenuScene::incrementPlayerMetal(int playerIndex)
    {
        auto& player = model.players[playerIndex];

        if (player.metal.getValue() == Metal(200))
        {
            player.metal.next(Metal(500));
        }
        else if (player.metal.getValue() < Metal(10000))
        {
            player.metal.next(player.metal.getValue() + Metal(500));
        }
    }

    void MainMenuScene::decrementPlayerMetal(int playerIndex)
    {
        auto& player = model.players[playerIndex];

        if (player.metal.getValue() > Metal(500))
        {
            player.metal.next(player.metal.getValue() - Metal(500));
        }
        else if (player.metal.getValue() == Metal(500))
        {
            player.metal.next(Metal(200));
        }
    }

    void MainMenuScene::incrementPlayerEnergy(int playerIndex)
    {
        auto& player = model.players[playerIndex];

        if (player.energy.getValue() == Energy(200))
        {
            player.energy.next(Energy(500));
        }
        else if (player.energy.getValue() < Energy(10000))
        {
            player.energy.next(player.energy.getValue() + Energy(500));
        }
    }

    void MainMenuScene::decrementPlayerEnergy(int playerIndex)
    {
        auto& player = model.players[playerIndex];

        if (player.energy.getValue() > Energy(500))
        {
            player.energy.next(player.energy.getValue() - Energy(500));
        }
        else if (player.energy.getValue() == Energy(500))
        {
            player.energy.next(Energy(200));
        }
    }

    void MainMenuScene::togglePlayer(int playerIndex)
    {
        auto& player = model.players[playerIndex];

        switch (player.type.getValue())
        {
            case MainMenuModel::PlayerSettings::Type::Open:
            {
                auto color = player.colorIndex.getValue();
                if (model.isColorInUse(color))
                {
                    auto newColor = model.getFirstFreeColor();
                    if (!newColor)
                    {
                        throw std::logic_error("No free colors for the player");
                    }
                    color = *newColor;
                }

                bool humanAllowed = std::find_if(model.players.begin(), model.players.end(), [](const auto& p) {
                    return p.type.getValue() == MainMenuModel::PlayerSettings::Type::Human;
                }) == model.players.end();
                if (humanAllowed)
                {
                    player.type.next(MainMenuModel::PlayerSettings::Type::Human);
                }
                else
                {
                    player.type.next(MainMenuModel::PlayerSettings::Type::Computer);
                }
                player.colorIndex.next(color);
                applySkirmishResourceDefaults(playerIndex);
                break;
            }
            case MainMenuModel::PlayerSettings::Type::Human:
                player.type.next(MainMenuModel::PlayerSettings::Type::Computer);
                applySkirmishResourceDefaults(playerIndex);
                break;
            case MainMenuModel::PlayerSettings::Type::Computer:
                player.type.next(MainMenuModel::PlayerSettings::Type::Open);
                break;
        }
    }

    void MainMenuScene::togglePlayerSide(int playerIndex)
    {
        auto& player = model.players[playerIndex];
        switch (player.side.getValue())
        {
            case MainMenuModel::PlayerSettings::Side::Arm:
                player.side.next(MainMenuModel::PlayerSettings::Side::Core);
                break;
            case MainMenuModel::PlayerSettings::Side::Core:
                player.side.next(MainMenuModel::PlayerSettings::Side::Arm);
                break;
        }
    }

    void MainMenuScene::cyclePlayerColor(int playerIndex)
    {
        auto& player = model.players[playerIndex];
        auto currentColor = player.colorIndex.getValue();
        for (unsigned int i = 1; i < 10; ++i)
        {
            auto newColor = PlayerColorIndex((currentColor.value + i) % 10);
            if (!model.isColorInUse(newColor))
            {
                player.colorIndex.next(newColor);
                return;
            }
        }
    }

    void MainMenuScene::reverseCyclePlayerColor(int playerIndex)
    {
        auto& player = model.players[playerIndex];
        auto currentColor = player.colorIndex.getValue();
        for (unsigned int i = 9; i >= 1; --i)
        {
            auto newColor = PlayerColorIndex((currentColor.value + i) % 10);
            if (!model.isColorInUse(newColor))
            {
                player.colorIndex.next(newColor);
                return;
            }
        }
    }

    void MainMenuScene::cyclePlayerTeam(int playerIndex)
    {
        auto& player = model.players[playerIndex];
        if (!player.teamIndex.getValue())
        {
            player.teamIndex.next(0);
            model.teamChanges.next(0);
        }
        else
        {
            auto val = *(player.teamIndex.getValue());
            if (val == 4)
            {
                player.teamIndex.next(std::nullopt);
                model.teamChanges.next(val);
            }
            else
            {
                player.teamIndex.next(val + 1);
                model.teamChanges.next(val);
                model.teamChanges.next(val + 1);
            }
        }
    }

    std::string getSideName(const MainMenuModel::PlayerSettings::Side side)
    {
        switch (side)
        {
            case MainMenuModel::PlayerSettings::Side::Arm:
                return "ARM";
            case MainMenuModel::PlayerSettings::Side::Core:
                return "CORE";
        }

        throw std::logic_error("Invalid side");
    }

    PlayerControllerType playerSettingsTypeToPlayerControllerType(MainMenuModel::PlayerSettings::Type t)
    {
        switch (t)
        {
            case MainMenuModel::PlayerSettings::Type::Human:
                return PlayerControllerTypeHuman();
            case MainMenuModel::PlayerSettings::Type::Computer:
                return PlayerControllerTypeComputer();
            default:
                throw std::logic_error("Invalid player settings type");
        }
    }

    void MainMenuScene::startGame()
    {
        // retail Start handler (TotalA.exe 0x47ae60): no selected map
        // is a silent return; validation failures pop a message box
        if (!model.selectedMap.getValue())
        {
            return;
        }

        GameParameters params{model.selectedMap.getValue()->name, 0};
        params.skirmish = model.skirmish;

        for (Index i = 0; i < getSize(model.players); ++i)
        {
            const auto& playerSlot = model.players[i];
            if (playerSlot.type.getValue() == MainMenuModel::PlayerSettings::Type::Open)
            {
                params.players[i] = std::nullopt;
                continue;
            }

            auto controller = playerSettingsTypeToPlayerControllerType(playerSlot.type.getValue());

            std::optional<unsigned int> allyGroup;
            if (auto team = playerSlot.teamIndex.getValue(); team && *team >= 0)
            {
                allyGroup = static_cast<unsigned int>(*team);
            }

            PlayerInfo playerInfo{std::nullopt, controller, getSideName(playerSlot.side.getValue()), playerSlot.colorIndex.getValue(), playerSlot.metal.getValue(), playerSlot.energy.getValue(), allyGroup};
            params.players[i] = std::move(playerInfo);
        }

        // retail check 1: the map's data must load ("The terrain for
        // the selected map does not exist.")
        auto otaRaw = sceneContext.vfs->readFile(std::string("maps/").append(model.selectedMap.getValue()->name).append(".ota"));
        std::optional<OtaRecord> ota;
        if (otaRaw)
        {
            try
            {
                std::string otaStr(otaRaw->begin(), otaRaw->end());
                ota = parseOta(parseTdfFromString(otaStr));
            }
            catch (const std::exception& e)
            {
                LOG_WARN << "Cannot start skirmish: failed to parse map: " << e.what();
            }
        }
        if (!ota)
        {
            showSkirmishStartError("The terrain for the selected map does not exist.");
            return;
        }

        if (auto rejection = checkSkirmishStart(params.players, *ota))
        {
            LOG_WARN << "Cannot start skirmish: " << *rejection;
            showSkirmishStartError(*rejection);
            return;
        }

        auto scene = std::make_unique<LoadingScene>(
            sceneContext,
            soundLookup,
            std::move(bgm),
            params);

        sceneContext.sceneManager->setNextScene(std::shared_ptr<Scene>(std::move(scene)));
    }

    void MainMenuScene::showSkirmishStartError(const std::string& message)
    {
        // retail answers a rejected Start with a modal message box
        // (TotalA.exe 0x4c5740 creates the box, 0x4abd90 attaches it);
        // a centered label plus an OK button popped via PrevMenu is
        // the same contract
        auto panel = std::make_unique<UiPanel>(120, 190, 400, 100);
        panel->setName("SKIRMISH");

        auto font = sceneContext.textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");
        auto label = std::make_unique<UiLabel>(10, 15, 380, 50, message, font);
        label->setAlignment(UiLabel::Alignment::LeftWrapped);
        panel->appendChild(std::move(label));

        auto ok = uiFactory.createBasicButton(152, 65, 95, 20, "SKIRMISH", "PrevMenu", "OK");
        ok->setName("PrevMenu");
        ok->setTextAlign(UiStagedButton::TextAlign::Center);
        panel->appendChild(std::move(ok));

        openDialog(std::move(panel));
    }

    Point MainMenuScene::toScaledCoordinates(int x, int y) const
    {
        auto clip = screenToWorldRayUtil(scaledUiRenderService.getInverseViewProjectionMatrix(), sceneContext.viewport->toClipSpace(x, y));
        return Point(static_cast<int>(clip.origin.x), static_cast<int>(clip.origin.y));
    }

    std::vector<std::string> MainMenuScene::getMapNames()
    {
        auto mapNames = sceneContext.vfs->getFileNames("maps", ".ota");

        // Keep only maps that have a multiplayer schema
        mapNames.erase(std::remove_if(mapNames.begin(), mapNames.end(), [this](const auto& e) { return !hasMultiplayerSchema(e); }), mapNames.end());

        for (auto& e : mapNames)
        {
            // chop off the extension
            e.resize(e.size() - 4);
        }

        return mapNames;
    }

    void MainMenuScene::attachPlayerSelectionComponents(const std::string& guiName, UiPanel& panel)
    {
        unsigned int tableStart = 78;
        unsigned int rowHeight = 20;

        for (int i = 0; i < 10; ++i)
        {
            unsigned int rowStart = tableStart + (i * rowHeight);

            {
                // player name button
                unsigned int width = 112;
                unsigned int height = 20;

                auto b = uiFactory.createBasicButton(45, rowStart, width, height, guiName, "skirmname", "Player");
                b->setName("PLAYER" + std::to_string(i));
                b->setTextAlign(UiStagedButton::TextAlign::Center);

                auto sub = model.players[i].type.subscribe([b = b.get(), &panel, this, guiName, i](MainMenuModel::PlayerSettings::Type type) {
                    switch (type)
                    {
                        case MainMenuModel::PlayerSettings::Type::Open:
                            panel.removeChildrenWithPrefix("PLAYER" + std::to_string(i) + "_");
                            b->setLabel("Open");
                            break;
                        case MainMenuModel::PlayerSettings::Type::Human:
                            b->setLabel("Player");
                            panel.removeChildrenWithPrefix("PLAYER" + std::to_string(i) + "_");
                            attachDetailedPlayerSelectionComponents(guiName, panel, i);
                            break;
                        case MainMenuModel::PlayerSettings::Type::Computer:
                            b->setLabel("Computer");
                            panel.removeChildrenWithPrefix("PLAYER" + std::to_string(i) + "_");
                            attachDetailedPlayerSelectionComponents(guiName, panel, i);
                            break;
                    }
                });
                b->addSubscription(std::move(sub));

                panel.appendChild(std::move(b));
            }
        }
    }

    void MainMenuScene::attachDetailedPlayerSelectionComponents(const std::string& guiName, UiPanel& panel, int i)
    {
        unsigned int tableStart = 78;
        unsigned int rowHeight = 20;

        unsigned int rowStart = tableStart + (i * rowHeight);

        {
            // side button
            unsigned int width = 44;
            unsigned int height = 20;

            auto b = uiFactory.createStagedButton(163, rowStart, width, height, guiName, "SIDEx", std::vector<std::string>(2), 2);
            b->setName("PLAYER" + std::to_string(i) + "_side");

            auto sub = model.players[i].side.subscribe([b = b.get()](MainMenuModel::PlayerSettings::Side side) {
                switch (side)
                {
                    case MainMenuModel::PlayerSettings::Side::Arm:
                        b->setStage(0);
                        break;
                    case MainMenuModel::PlayerSettings::Side::Core:
                        b->setStage(1);
                        break;
                }
            });
            b->addSubscription(std::move(sub));

            panel.appendChild(std::move(b));
        }

        {
            // color
            unsigned int width = 19;
            unsigned int height = 19;

            auto graphics = sceneContext.textureService->getGafEntry("anims/LOGOS.GAF", "32xlogos");
            auto newSprites = std::make_shared<SpriteSeries>();
            newSprites->sprites.reserve(graphics->sprites.size());

            std::transform(graphics->sprites.begin(), graphics->sprites.end(), std::back_inserter(newSprites->sprites), [width, height](const auto& sprite) {
                auto bounds = Rectangle2f::fromTopLeft(0.0f, 0.0f, width, height);
                return std::make_shared<Sprite>(bounds, sprite->texture, sprite->mesh);
            });

            auto b = uiFactory.createButton(214, rowStart, width, height, guiName, "logo", "");
            b->setName("PLAYER" + std::to_string(i) + "_color");

            auto sub = model.players[i].colorIndex.subscribe([b = b.get(), newSprites](const auto& index) {
                b->setNormalSprite(newSprites->sprites[index.value]);
                b->setPressedSprite(newSprites->sprites[index.value]);
            });
            b->addSubscription(std::move(sub));

            panel.appendChild(std::move(b));
        }

        {
            // ally
            unsigned int width = 38;
            unsigned int height = 20;

            auto graphics = sceneContext.textureService->getGuiTexture(guiName, "TEAMICONSx");
            if (!graphics)
            {
                graphics = sceneContext.textureService->getGuiTexture(guiName, "ally icons");
            }
            if (!graphics)
            {
                throw std::runtime_error("Failed to load TEAMICONSx");
            }

            auto b = uiFactory.createButton(241, rowStart, width, height, guiName, "team", "");
            b->setName("PLAYER" + std::to_string(i) + "_team");

            auto sub = model.players[i].teamIndex.subscribe([b = b.get(), &m = model, g = *graphics](auto index) {
                if (!index)
                {
                    b->setNormalSprite(g->sprites[10]);
                    b->setPressedSprite(g->sprites[10]);
                    return;
                }

                auto stage = (*index) * 2;
                if (!m.isTeamShared(*index))
                {
                    ++stage;
                }

                b->setNormalSprite(g->sprites[stage]);
                b->setPressedSprite(g->sprites[stage]);
            });
            b->addSubscription(std::move(sub));

            auto teamSub = model.teamChanges.subscribe([b = b.get(), &m = model, g = *graphics, i](auto index) {
                if (index == m.players[i].teamIndex.getValue())
                {
                    auto stage = index * 2;
                    if (m.isTeamShared(index))
                    {
                        b->setNormalSprite(g->sprites[stage]);
                        b->setPressedSprite(g->sprites[stage]);
                    }
                    else
                    {
                        b->setNormalSprite(g->sprites[stage + 1]);
                        b->setPressedSprite(g->sprites[stage + 1]);
                    }
                }
            });
            b->addSubscription(std::move(teamSub));

            panel.appendChild(std::move(b));
        }

        {
            // metal
            unsigned int width = 46;
            unsigned int height = 20;

            auto b = uiFactory.createButton(286, rowStart, width, height, guiName, "skirmmet", "");
            b->setName("PLAYER" + std::to_string(i) + "_metal");
            b->setTextAlign(UiStagedButton::TextAlign::Center);

            auto sub = model.players[i].metal.subscribe([b = b.get()](const auto& newMetal) {
                b->setLabel(formatResource(newMetal));
            });
            b->addSubscription(std::move(sub));

            panel.appendChild(std::move(b));
        }

        {
            // energy
            unsigned int width = 46;
            unsigned int height = 20;

            auto b = uiFactory.createButton(337, rowStart, width, height, guiName, "skirmmet", "");
            b->setName("PLAYER" + std::to_string(i) + "_energy");
            b->setTextAlign(UiStagedButton::TextAlign::Center);

            auto sub = model.players[i].energy.subscribe([b = b.get()](const auto& newEnergy) {
                b->setLabel(formatResource(newEnergy));
            });
            b->addSubscription(std::move(sub));

            panel.appendChild(std::move(b));
        }
    }

    bool MainMenuScene::hasMultiplayerSchema(const std::string& mapName)
    {
        auto otaRaw = sceneContext.vfs->readFile(std::string("maps/").append(mapName));
        if (!otaRaw)
        {
            throw std::runtime_error("Failed to read OTA file");
        }

        return parseTdfHasNetworkSchema(*otaRaw);
    }
}
