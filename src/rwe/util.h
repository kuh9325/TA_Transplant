#pragma once

#include <filesystem>
#include <optional>

namespace rwe
{
    std::optional<std::filesystem::path> getLocalDataPath();
    std::optional<std::filesystem::path> getSearchPath();

    /**
     * Root directory for project-owned runtime resources (e.g.
     * shaders/), resolved against the executable rather than the
     * process working directory. Inside a macOS .app bundle this is
     * `Contents/Resources`; otherwise it is the directory containing
     * the executable. Empty when the base path cannot be determined.
     */
    std::filesystem::path getResourceRoot();

    /**
     * Conservative check that `path` looks like a Total Annihilation
     * data root: an existing directory holding at least one TA
     * archive (.hpi/.ufo/.ccx/.gpf/.gp3, case-insensitive) or the
     * canonical loose-file layout (palettes/ + gamedata/
     * subdirectories). Mirrors what addToVfs() consumes.
     */
    bool isPlausibleGameDataRoot(const std::filesystem::path& path);

    float toleranceToRadians(unsigned int angle);
}
