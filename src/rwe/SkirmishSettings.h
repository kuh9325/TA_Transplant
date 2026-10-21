#pragma once

#include <string>

namespace rwe
{
    // Retail SKIRMISH.GUI staged toggles. Enum order matches the retail
    // gadget stage order exactly (TotalA.exe SKIRMISH.GUI definition).
    enum class SkirmishStartLocation
    {
        Fixed,
        Random
    };

    enum class SkirmishCommanderDeath
    {
        GameEnds,
        Continues
    };

    enum class SkirmishMapping
    {
        Unmapped,
        Mapped
    };

    enum class SkirmishLineOfSight
    {
        Permanent,
        True,
        Circular
    };

    enum class SkirmishDifficulty
    {
        Easy,
        Medium,
        Hard
    };

    struct SkirmishSettings
    {
        SkirmishStartLocation startLocation{SkirmishStartLocation::Fixed};
        SkirmishCommanderDeath commanderDeath{SkirmishCommanderDeath::GameEnds};
        SkirmishMapping mapping{SkirmishMapping::Unmapped};
        SkirmishLineOfSight lineOfSight{SkirmishLineOfSight::Permanent};
        SkirmishDifficulty difficulty{SkirmishDifficulty::Easy};
    };

    std::string toString(SkirmishStartLocation value);
    std::string toString(SkirmishCommanderDeath value);
    std::string toString(SkirmishMapping value);
    std::string toString(SkirmishLineOfSight value);
    std::string toString(SkirmishDifficulty value);
}
