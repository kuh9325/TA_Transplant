#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <rwe/save/SavedGame.h>

namespace rwe
{
    /**
     * File-level operations for saved games, rooted at the retail-style
     * `savegame/` directory under the local data path (getLocalDataPath()).
     *
     * Retail contract: files are `savegame\<name>.sav` under the install
     * directory, named by the save's display name, listed by the stock
     * save/load dialogs, and deletable from the same dialog. RWE keeps
     * the directory + `.sav` extension contract but places it under the
     * user-writable local data path (the install directory is not
     * guaranteed writable and may be shared).
     *
     * All writes are atomic (write to `<name>.sav.tmp`, flush, rename)
     * so a failed save never destroys an existing good file.
     */

    /** `<localData>/savegame` — created on first write, not on list. */
    std::optional<std::filesystem::path> saveGameDirectory();

    /**
     * Reduce an arbitrary display name to a safe save filename stem:
     * trims whitespace, replaces filesystem-hostile characters with '_',
     * and caps the result (retail SAVELIST caps entry at 20 chars).
     * Returns empty for an unusable name.
     */
    std::string sanitizeSaveName(const std::string& name);

    /** `<name>.sav` path for a display name, or nullopt when unusable. */
    std::optional<std::filesystem::path> saveGamePath(
        const std::filesystem::path& directory,
        const std::string& name);

    /**
     * Metadata for each readable save in the directory, newest first.
     * Corrupt/foreign entries are skipped (the retail dialog shows the
     * "Invalid savegame file" error instead of listing them; listing
     * only readable saves is the honest equivalent for RWE's own dir).
     */
    std::vector<SavedGameMetadata> listSavedGames(const std::filesystem::path& directory);

    /**
     * Read a save file's bytes by display name.
     * Throws std::runtime_error on missing/unreadable files.
     */
    std::vector<char> readSavedGameFile(const std::filesystem::path& directory, const std::string& name);

    /**
     * Write a save file atomically: tmp sibling + rename.
     * Throws std::runtime_error on failure; an existing save of the
     * same name is only replaced on success.
     */
    void writeSavedGameFile(
        const std::filesystem::path& directory,
        const std::string& name,
        const std::vector<char>& bytes);

    /**
     * Delete a save by display name (retail DELETE gadget).
     * Returns false when the file did not exist.
     */
    bool deleteSavedGameFile(const std::filesystem::path& directory, const std::string& name);
}
