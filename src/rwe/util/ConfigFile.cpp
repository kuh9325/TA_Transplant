#include "ConfigFile.h"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace rwe
{
    namespace
    {
        std::string trimLeft(std::string s)
        {
            auto start = s.find_first_not_of(" \t\r\n");
            return start == std::string::npos ? "" : s.substr(start);
        }

        std::string trimRight(std::string s)
        {
            auto end = s.find_last_not_of(" \t\r\n");
            return end == std::string::npos ? "" : s.substr(0, end + 1);
        }
    }

    std::optional<std::map<std::string, std::string>> readConfigFile(const std::filesystem::path& path)
    {
        std::map<std::string, std::string> values;

        std::ifstream file(path);
        if (!file.is_open())
        {
            return values;
        }

        std::string line;
        while (std::getline(file, line))
        {
            auto trimmed = trimLeft(line);
            if (trimmed.empty() || trimmed[0] == '#')
            {
                continue;
            }

            auto eqPos = trimmed.find('=');
            if (eqPos == std::string::npos)
            {
                return std::nullopt;
            }

            values[trimRight(trimmed.substr(0, eqPos))] = trimLeft(trimmed.substr(eqPos + 1));
        }

        return values;
    }

    void writeConfigValue(const std::filesystem::path& path, const std::string& key, const std::string& value)
    {
        std::vector<std::string> lines;
        bool replaced = false;

        {
            std::ifstream file(path);
            std::string line;
            while (std::getline(file, line))
            {
                auto trimmed = trimLeft(line);
                if (!replaced && !trimmed.empty() && trimmed[0] != '#')
                {
                    auto eqPos = trimmed.find('=');
                    if (eqPos != std::string::npos && trimRight(trimmed.substr(0, eqPos)) == key)
                    {
                        line = key + "=" + value;
                        replaced = true;
                    }
                }
                lines.push_back(line);
            }
        }

        if (!replaced)
        {
            lines.push_back(key + "=" + value);
        }

        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        auto tmpPath = path;
        tmpPath += ".tmp";

        {
            std::ofstream file(tmpPath, std::ios::trunc);
            if (!file.is_open())
            {
                throw std::runtime_error("Failed to write config file: " + tmpPath.string());
            }
            for (const auto& line : lines)
            {
                file << line << '\n';
            }
            if (!file.good())
            {
                throw std::runtime_error("Failed to write config file: " + tmpPath.string());
            }
        }

        // POSIX rename replaces atomically; Windows refuses to rename
        // over an existing file, so fall back to remove+rename there
        std::error_code ec;
        std::filesystem::rename(tmpPath, path, ec);
        if (ec)
        {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(tmpPath, path);
        }
    }
}
