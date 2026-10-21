#pragma once

#include <limits>
#include <rwe/util/OpaqueId.h>

namespace rwe
{
    struct PlayerIdTag;
    using PlayerId = OpaqueId<unsigned int, PlayerIdTag>;

    /**
     * Owner value for entities that belong to no player --
     * e.g. meteor-shower hazard projectiles (retail spawns them with
     * no owning unit at all; TotalA.exe 0x4190d1..0x4191a8). It never
     * equals a real owner, so friendly-fire owner checks pass against
     * every player's units.
     */
    constexpr PlayerId PlayerIdNone{std::numeric_limits<unsigned int>::max()};
}
