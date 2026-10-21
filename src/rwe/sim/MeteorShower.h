#pragma once

#include <rwe/sim/GameTime.h>
#include <string>

namespace rwe
{
    /**
     * Raw hazard configuration values in OTA-schema units: radius in
     * world pixels, density in hits/second, duration and interval in
     * seconds. Same layout is used for the OTA schema fields
     * (MeteorWeapon/MeteorRadius/MeteorDensity/MeteorDuration/
     * MeteorInterval) and for gamedata/meteor.tdf [Default].
     */
    struct MeteorShowerParams
    {
        std::string weaponName;
        int radius{0};
        float density{0.0f};
        float duration{0.0f};
        float interval{0.0f};
    };

    /**
     * Authoritative meteor-shower state. Mirrors the retail globals at
     * TotalA.exe 0x502c38..0x502c88 which drive the per-tick update at
     * 0x418f50 and are saved under the [Meteor] save block.
     *
     * Retail semantics (all times in 30Hz sim ticks):
     *  - when now >= nextStrikeTime a strike begins: active is set,
     *    timeStrikeEnds = now + durationTicks, nextStrikeTime =
     *    timeStrikeEnds + intervalTicks (the interval is the gap AFTER
     *    a strike, not the period between strike starts), nextHitTime
     *    = now, and a fresh target/origin pair is rolled. If the
     *    shower is disabled the roll still happens but active is
     *    cleared again.
     *  - while active, each time now >= nextHitTime a hit is spawned
     *    and nextHitTime += hitStepTicks; the hit at timeStrikeEnds
     *    still fires because the end check runs after the hit check.
     */
    struct MeteorShower
    {
        /** Resolved weapon block name (uppercase); empty means the
         * configured weapon was missing or lacked the `meteor` flag,
         * so hits spawn nothing. */
        std::string weaponType;

        /** Impact scatter radius in world units (pixels). */
        int radius{0};

        /** Ticks between hits while a strike is active:
         * int(30 / density). */
        GameTime hitStepTicks{0};

        /** Strike length in ticks: int(duration * 30). */
        GameTime durationTicks{0};

        /** Gap after a strike ends in ticks: int(interval * 30). */
        GameTime intervalTicks{0};

        bool enabled{false};
        bool active{false};
        GameTime nextStrikeTime{0};
        GameTime timeStrikeEnds{0};
        GameTime nextHitTime{0};

        /** Spawn origin offset from the target, in heightmap cells
         * (retail stores both as cell coordinates). */
        int originX{0};
        int originZ{0};

        /** Strike target in heightmap cells. */
        int targetX{0};
        int targetZ{0};
    };

    /**
     * Retail schema-side validation (TotalA.exe 0x421a4b..0x421a85):
     * an empty weapon name disables the shower, and if any numeric
     * field is zero the WHOLE configuration is replaced by the
     * gamedata/meteor.tdf [Default] block. Returns the effective
     * params; `enabled` reflects the authored weapon name only.
     */
    struct ResolvedMeteorShowerParams
    {
        MeteorShowerParams params;
        bool enabled{false};
    };

    ResolvedMeteorShowerParams resolveMeteorShowerParams(const MeteorShowerParams& schema, const MeteorShowerParams& defaults);
}
