#pragma once

#include <memory>
#include <string>
#include <vector>
#include <rwe/save/SavedGame.h>

namespace rwe
{
    class AbstractVirtualFileSystem;
    class TextureService;
    class UiFactory;
    class UiPanel;

    /**
     * Builders for the stock save-list dialogs (all declared with
     * `panel=NULL` in the GUI data; the exe binds a mode bitmap at
     * runtime, so the same is done here):
     *
     * - createInGameSaveListPanel: F2 save/load — SAVELIST.GUI with
     *   bitmaps/DSaveList.pcx, or LOADLIST.GUI with bitmaps/DLoadList.pcx.
     *   Topic emitted = "SAVELIST"/"LOADLIST".
     *
     * - createFrontendLoadGamePanel: SINGLE menu / ENDMSN load+save —
     *   LOADGAME.GUI with bitmaps/DLoadgame2.pcx in load mode or
     *   bitmaps/DSavegame2.pcx in save mode. (bitmaps/Loadgame2bg.pcx
     *   is the mission-loading splash, owned by LoadingScene, not this
     *   screen.) Topic emitted = "LOADGAME".
     *
     * In load mode the GAMENAME field is read-only and displays the
     * selected entry's name; in save mode it is the editable target
     * for the new save's name.
     *
     * `entries` must outlive the panel (listbox selection syncs the
     * GAMENAME field and metadata labels from it); callers populate it
     * via populateSaveList.
     */
    std::unique_ptr<UiPanel> createInGameSaveListPanel(
        UiFactory& uiFactory,
        TextureService* textureService,
        AbstractVirtualFileSystem* vfs,
        bool saveMode,
        const std::vector<SavedGameMetadata>* entries);

    std::unique_ptr<UiPanel> createFrontendLoadGamePanel(
        UiFactory& uiFactory,
        TextureService* textureService,
        AbstractVirtualFileSystem* vfs,
        bool saveMode,
        const std::vector<SavedGameMetadata>* entries);

    /**
     * Refills the GAMES listbox from `entries` (names only) and clears
     * any selection. Call after building and after save/delete writes.
     */
    void populateSaveList(UiPanel& panel, const std::vector<SavedGameMetadata>& entries);
}
