#pragma once

#include <filesystem>

namespace rwe
{
    class GlobalConfig
    {
    public:
        bool leftClickInterfaceMode{true};

        /**
         * The `key=value` config file the options screen commits to
         * when OK/PREV is pressed (retail writes "Interface Type" to
         * the registry at the same point). Empty disables persistence.
         */
        std::filesystem::path configFilePath;
    };
}
