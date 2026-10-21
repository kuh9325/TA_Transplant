#include "SkirmishSettings.h"

namespace rwe
{
    std::string toString(SkirmishStartLocation value)
    {
        switch (value)
        {
            case SkirmishStartLocation::Fixed:
                return "Fixed";
            case SkirmishStartLocation::Random:
                return "Random";
        }
        return "Fixed";
    }

    std::string toString(SkirmishCommanderDeath value)
    {
        switch (value)
        {
            case SkirmishCommanderDeath::GameEnds:
                return "Game ends";
            case SkirmishCommanderDeath::Continues:
                return "Continues";
        }
        return "Game ends";
    }

    std::string toString(SkirmishMapping value)
    {
        switch (value)
        {
            case SkirmishMapping::Unmapped:
                return "Unmapped";
            case SkirmishMapping::Mapped:
                return "Mapped";
        }
        return "Unmapped";
    }

    std::string toString(SkirmishLineOfSight value)
    {
        switch (value)
        {
            case SkirmishLineOfSight::Permanent:
                return "Permanent";
            case SkirmishLineOfSight::True:
                return "True";
            case SkirmishLineOfSight::Circular:
                return "Circular";
        }
        return "Permanent";
    }

    std::string toString(SkirmishDifficulty value)
    {
        switch (value)
        {
            case SkirmishDifficulty::Easy:
                return "Easy";
            case SkirmishDifficulty::Medium:
                return "Medium";
            case SkirmishDifficulty::Hard:
                return "Hard";
        }
        return "Easy";
    }
}
