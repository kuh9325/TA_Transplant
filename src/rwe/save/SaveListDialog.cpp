#include "SaveListDialog.h"

#include <cstdio>
#include <ctime>
#include <rwe/ui/UiFactory.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiTextBox.h>
#include <rwe/vfs/AbstractVirtualFileSystem.h>
#include <rwe/TextureService.h>

namespace rwe
{
    namespace
    {
        void bindBitmap(UiPanel& panel, TextureService* textureService, AbstractVirtualFileSystem* vfs, const std::string& name)
        {
            if (vfs->readFile("bitmaps/" + name + ".pcx"))
            {
                panel.setBackground(textureService->getBitmapRegion(name, 0, 0, panel.getWidth(), panel.getHeight()));
            }
        }

        void setLabelText(UiPanel& panel, const std::string& name, const std::string& text)
        {
            if (auto label = panel.find<UiLabel>(name))
            {
                label->get().setText(text);
            }
        }

        std::string formatGameTime(GameTime t)
        {
            // same presentation as the debrief Game Time row
            auto totalSeconds = t.value / 30;
            auto minutes = totalSeconds / 60;
            auto seconds = totalSeconds % 60;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%u:%02u", minutes, seconds);
            return buf;
        }

        /**
         * Loads the metadata labels unique to LOADGAME.GUI (GAMETYPE,
         * MISSION, TIME, SIDE, DIFF). RADAR has no decoded meaning yet
         * and is left as authored.
         */
        void showMetadata(UiPanel& panel, const SavedGameMetadata& meta)
        {
            setLabelText(panel, "GAMETYPE", meta.isCampaign ? "Campaign" : "Normal");
            setLabelText(panel, "MISSION", meta.isCampaign ? meta.missionName : meta.mapName);
            setLabelText(panel, "TIME", formatGameTime(meta.gameTime));
            setLabelText(panel, "SIDE", meta.localSide);
            setLabelText(panel, "DIFF", meta.isCampaign ? meta.difficulty : "");
        }

        void clearMetadata(UiPanel& panel)
        {
            for (const auto* name : {"GAMETYPE", "MISSION", "TIME", "SIDE", "DIFF"})
            {
                setLabelText(panel, name, "");
            }
        }

        void wireSelectionSync(UiPanel& panel, const std::vector<SavedGameMetadata>* entries)
        {
            auto games = panel.find<UiListBox>("GAMES");
            if (!games)
            {
                return;
            }
            UiListBox* listBox = &games->get();
            UiTextBox* nameBox = nullptr;
            if (auto box = panel.find<UiTextBox>("GAMENAME"))
            {
                nameBox = &box->get();
            }
            UiPanel* panelPtr = &panel;
            listBox->addSubscription(listBox->selectedIndex().subscribe([listBox, nameBox, panelPtr, entries](const auto& selected) {
                if (!selected || !entries || *selected >= entries->size())
                {
                    clearMetadata(*panelPtr);
                    return;
                }
                if (nameBox != nullptr)
                {
                    nameBox->setText((*entries)[*selected].saveName);
                }
                showMetadata(*panelPtr, (*entries)[*selected]);
            }));
        }
    }

    std::unique_ptr<UiPanel> createInGameSaveListPanel(
        UiFactory& uiFactory,
        TextureService* textureService,
        AbstractVirtualFileSystem* vfs,
        bool saveMode,
        const std::vector<SavedGameMetadata>* entries)
    {
        auto panel = uiFactory.panelFromGuiFile(saveMode ? "SAVELIST" : "LOADLIST");
        bindBitmap(*panel, textureService, vfs, saveMode ? "DSaveList" : "DLoadList");
        wireSelectionSync(*panel, entries);

        // the name field is editable only in save mode; in load mode
        // it is a read-only display of the selected entry
        if (auto box = panel->find<UiTextBox>("GAMENAME"))
        {
            box->get().setEnabled(saveMode);
        }

        return panel;
    }

    std::unique_ptr<UiPanel> createFrontendLoadGamePanel(
        UiFactory& uiFactory,
        TextureService* textureService,
        AbstractVirtualFileSystem* vfs,
        bool saveMode,
        const std::vector<SavedGameMetadata>* entries)
    {
        auto panel = uiFactory.panelFromGuiFile("LOADGAME");

        // both the fullscreen frontend screen and the ENDMSN overlay
        // use the dialog bitmap pair; Loadgame2bg.pcx is the mission-
        // loading splash (LoadingScene's background), not this screen
        bindBitmap(*panel, textureService, vfs, saveMode ? "DSavegame2" : "DLoadgame2");
        wireSelectionSync(*panel, entries);

        if (auto box = panel->find<UiTextBox>("GAMENAME"))
        {
            box->get().setEnabled(saveMode);
        }

        // the frontend screen's SaveGame button switches to the
        // save-side screen, which has no meaning without a live game
        if (auto button = panel->find<UiStagedButton>("SaveGame"))
        {
            button->get().setEnabled(false);
        }

        return panel;
    }

    void populateSaveList(UiPanel& panel, const std::vector<SavedGameMetadata>& entries)
    {
        if (auto listBox = panel.find<UiListBox>("GAMES"))
        {
            auto& games = listBox->get();
            games.clearItems();
            for (const auto& entry : entries)
            {
                games.appendItem(entry.saveName);
            }
        }
        clearMetadata(panel);
    }
}
