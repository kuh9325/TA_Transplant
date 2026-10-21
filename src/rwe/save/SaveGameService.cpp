#include "SaveGameService.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <rwe/util.h>

namespace rwe
{
    namespace
    {
        constexpr std::size_t MaxSaveNameLength = 99; // retail SAVEGAME.GUI maxchars

        bool isNameCharAllowed(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c))
                || c == ' '
                || c == '_'
                || c == '-'
                || c == '.'
                || c == '('
                || c == ')';
        }
    }

    std::optional<std::filesystem::path> saveGameDirectory()
    {
        auto local = getLocalDataPath();
        if (!local)
        {
            return std::nullopt;
        }
        return *local / "savegame";
    }

    std::string sanitizeSaveName(const std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            out.push_back(isNameCharAllowed(c) ? c : '_');
        }

        // trim surrounding whitespace/dots (both are awkward in file names)
        auto first = out.find_first_not_of(" .");
        if (first == std::string::npos)
        {
            return "";
        }
        auto last = out.find_last_not_of(" .");
        out = out.substr(first, last - first + 1);

        if (out.size() > MaxSaveNameLength)
        {
            out.resize(MaxSaveNameLength);
        }
        return out;
    }

    std::optional<std::filesystem::path> saveGamePath(
        const std::filesystem::path& directory,
        const std::string& name)
    {
        auto stem = sanitizeSaveName(name);
        if (stem.empty())
        {
            return std::nullopt;
        }
        return directory / (stem + ".sav");
    }

    std::vector<SavedGameMetadata> listSavedGames(const std::filesystem::path& directory)
    {
        std::vector<SavedGameMetadata> out;
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec))
        {
            return out;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".sav")
            {
                continue;
            }
            std::ifstream stream(entry.path(), std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            try
            {
                out.push_back(readSaveMetadata(bytes));
            }
            catch (const SaveFormatException&)
            {
                // unreadable saves are skipped in the list
            }
        }

        std::sort(out.begin(), out.end(), [](const SavedGameMetadata& a, const SavedGameMetadata& b) {
            return a.unixTimeSeconds > b.unixTimeSeconds;
        });
        return out;
    }

    std::vector<char> readSavedGameFile(const std::filesystem::path& directory, const std::string& name)
    {
        auto path = saveGamePath(directory, name);
        if (!path)
        {
            throw std::runtime_error("Invalid save name");
        }
        std::ifstream stream(*path, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Save file not found: " + name);
        }
        return std::vector<char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    void writeSavedGameFile(
        const std::filesystem::path& directory,
        const std::string& name,
        const std::vector<char>& bytes)
    {
        auto path = saveGamePath(directory, name);
        if (!path)
        {
            throw std::runtime_error("Invalid save name");
        }

        std::filesystem::create_directories(directory);

        auto tmpPath = *path;
        tmpPath += ".tmp";
        {
            std::ofstream stream(tmpPath, std::ios::binary | std::ios::trunc);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream)
            {
                std::error_code ec;
                std::filesystem::remove(tmpPath, ec);
                throw std::runtime_error("Failed to write save file");
            }
        }
        std::filesystem::rename(tmpPath, *path);
    }

    bool deleteSavedGameFile(const std::filesystem::path& directory, const std::string& name)
    {
        auto path = saveGamePath(directory, name);
        if (!path)
        {
            return false;
        }
        std::error_code ec;
        return std::filesystem::remove(*path, ec);
    }
}
