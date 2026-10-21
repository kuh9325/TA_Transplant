#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace rwe
{
    /**
     * Reads a `key=value` config file (the format OpaqueArgs::parseConfig
     * consumes): one entry per line, '#' comments and blank lines
     * ignored. Returns nullopt when the file contains a malformed line
     * (no '='); a missing file yields an empty map.
     */
    std::optional<std::map<std::string, std::string>> readConfigFile(const std::filesystem::path& path);

    /**
     * Atomically sets `key=value` inside the config file at `path`,
     * preserving unrelated lines and ordering. Writes to a sibling
     * temp file then renames so a crash cannot leave a half-written
     * config. Creates parent directories when needed.
     */
    void writeConfigValue(const std::filesystem::path& path, const std::string& key, const std::string& value);
}
