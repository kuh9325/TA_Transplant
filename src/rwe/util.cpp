#include "util.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <rwe/math/rwe_math.h>
#include <rwe/util/rwe_string.h>

namespace fs = std::filesystem;

namespace rwe
{
#ifdef RWE_PLATFORM_WINDOWS
    std::optional<std::filesystem::path> getLocalDataPath()
    {
        auto appData = std::getenv("APPDATA");
        if (appData == nullptr)
        {
            return std::nullopt;
        }

        std::filesystem::path path(appData);
        path /= "RWE";

        return path;
    }
#endif

#ifdef RWE_PLATFORM_LINUX
    std::optional<std::filesystem::path> getLocalDataPath()
    {
        auto home = std::getenv("HOME");
        if (home == nullptr)
        {
            return std::nullopt;
        }

        std::filesystem::path path(home);
        path /= ".rwe";

        return path;
    }
#endif

    std::optional<std::filesystem::path> getSearchPath()
    {
        auto path = getLocalDataPath();
        if (!path)
        {
            return std::nullopt;
        }


        *path /= "Data";

        return *path;
    }

    fs::path getResourceRoot()
    {
        // SDL_GetBasePath returns the executable's directory, except
        // for a macOS .app bundle where it returns
        // <App>.app/Contents/Resources/. SDL caches the string
        // internally; it must not be freed.
        auto* base = SDL_GetBasePath();
        if (base == nullptr)
        {
            return fs::path();
        }
        return fs::path(base);
    }

    bool isPlausibleGameDataRoot(const fs::path& path)
    {
        std::error_code ec;
        if (!fs::is_directory(path, ec) || ec)
        {
            return false;
        }

        static const std::string archiveExtensions[]{".HPI", ".UFO", ".CCX", ".GPF", ".GP3"};
        bool hasPalettesDir = false;
        bool hasGamedataDir = false;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        {
            const auto& entry = *it;
            auto ext = toUpper(entry.path().extension().string());
            if (entry.is_regular_file(ec))
            {
                for (const auto& candidate : archiveExtensions)
                {
                    if (ext == candidate)
                    {
                        return true;
                    }
                }
            }
            else if (entry.is_directory(ec))
            {
                auto name = toUpper(entry.path().filename().string());
                if (name == "PALETTES")
                {
                    hasPalettesDir = true;
                }
                else if (name == "GAMEDATA")
                {
                    hasGamedataDir = true;
                }
            }
        }
        if (ec)
        {
            return false;
        }

        // a loose-file layout carries the canonical VFS directories
        return hasPalettesDir && hasGamedataDir;
    }

    float toleranceToRadians(unsigned int angle)
    {
        return static_cast<float>(angle) * (Pif / 32768.0f);
    }
}
