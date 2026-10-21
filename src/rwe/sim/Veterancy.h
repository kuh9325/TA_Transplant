#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>

namespace rwe
{
    // TotalA.exe unit+0xb8 is a wrapping kill word, not a separate XP pool.
    inline unsigned int veteranTier(uint16_t kills)
    {
        return std::min<unsigned int>(kills / 5, 5);
    }

    inline unsigned int veteranPercent(unsigned int amount, unsigned int percent)
    {
        // Retail multiplies in a wrapping 32-bit word before signed division.
        return static_cast<uint32_t>(std::bit_cast<int32_t>(uint32_t(amount) * uint32_t(percent)) / 100);
    }

    inline unsigned int veteranAttackDamage(unsigned int amount, uint16_t kills)
    {
        return kills < 5 ? amount : veteranPercent(amount, 100 + 6 * veteranTier(kills));
    }

    inline unsigned int veteranDefenceDamage(unsigned int amount, uint16_t kills)
    {
        return kills < 5 ? amount : veteranPercent(amount, 100 - 4 * veteranTier(kills));
    }

    inline unsigned int veteranReloadTicks(unsigned int ticks, uint16_t kills)
    {
        return kills < 5 ? ticks : veteranPercent(static_cast<uint16_t>(ticks), 100 - 6 * veteranTier(kills));
    }

    inline uint16_t veteranSpread(unsigned int accuracy, unsigned int health, unsigned int maximumHealth, uint16_t kills)
    {
        // Turret executor only. Non-turret/dropped/vlaunch callers must not draw.
        auto healthTerm = maximumHealth == 0 ? 0 : (uint32_t(health) << 11) / maximumHealth;
        auto bound = static_cast<uint16_t>(accuracy - healthTerm + 2048);
        auto divisor = kills / 12;
        return divisor > 1 ? bound / divisor : bound;
    }

    inline std::string veteranLabel(uint16_t kills)
    {
        auto text = std::to_string(kills) + (kills == 1 ? " kill" : " kills");
        return kills >= 5 ? text + " - Veteran" : text;
    }
}
