#pragma once

#include <optional>
#include <rwe/sim/Energy.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/Metal.h>
#include <rwe/sim/ProjectilePhysicsType.h>
#include <rwe/sim/SimAngle.h>
#include <rwe/sim/SimScalar.h>
#include <string>

namespace rwe
{
    struct WeaponDefinition
    {
        /** TDF `name` display string -- the stockpile build button's
         * lower-left hover info shows the weapon's name, not a unit's. */
        std::string name;

        ProjectilePhysicsType physicsType;

        SimScalar maxRange;

        SimScalar reloadTime;

        /** The number of shots in a burst. */
        int burst;

        /** The amount of time between shots in the same burst, in seconds. */
        SimScalar burstInterval;

        /** Maximum angle deviation of projectiles shot in a burst. */
        SimAngle sprayAngle;

        bool turret{false};
        uint16_t accuracy{0};

        SimAngle tolerance;

        SimAngle pitchTolerance;

        /** Projectile velocity in pixels/tick. */
        SimScalar velocity;

        /** If true, the weapon only fires on command and does not auto-target. */
        bool commandFire;

        /**
         * If true (TDF `noexplode`), the weapon runs no area-of-effect
         * damage pass on impact; only the entity it directly struck
         * takes the hit.
         */
        bool noExplode;
        bool paralyzer{false};
        bool unitsOnly{false};
        bool fireStarter{false};

        std::unordered_map<std::string, unsigned int> damage;

        SimScalar damageRadius;

        /** Number of ticks projectiles fired from this weapon live for */
        std::optional<GameTime> weaponTimer;

        /**
         * TDF `guidance` flag: the projectile steers toward its target
         * -- for command-fired ground targets this guides the shot
         * onto the clicked point (strategic missiles are
         * `lineofsight`+`guidance`, not `tracks`).
         */
        bool guidance{false};

        /**
         * TDF `turnrate` (world angular units/tick): the guidance
         * steering limit -- also copied into Tracking physics when
         * `tracks=1`.
         */
        SimScalar turnRate{0_ss};

        /**
         * TDF `vlaunch` flag: the projectile launches vertically and
         * then steers toward its target -- firing straight at the aim
         * point from inside a silo footprint would bury the shot in
         * the terrain (stock: all stockpile missiles are vlaunch).
         */
        bool vLaunch{false};

        /**
         * TDF `cruise` flag: the missile bounds its flight by
         * `flightTime` rather than `weaponTimer`. Stock data: only the
         * strategic missiles (`NUCLEAR_MISSILE`/`CRBLMSSL`,
         * `flighttime=400`) set it -- honouring `weapontimer=5` for
         * them would cap flight at 150 ticks, a fraction of the
         * authored `range=32000` the target gate validates against.
         */
        bool cruise{false};

        /**
         * TDF `flighttime` (frames): a cruise weapon's maximum flight
         * duration (TotalA.exe projectile lifetime for cruise shots).
         */
        GameTime flightTime{0};

        /**
         * TDF `weaponacceleration` (world units/tick^2): the
         * projectile's speed grows by this each tick after launch --
         * strategic missiles start slow and accelerate through their
         * cruise (stock `weaponacceleration=50` px/s^2).
         */
        SimScalar acceleration{0_ss};

        /** The range in frames that projectile lifetime may randomly vary by. */
        std::optional<GameTime> randomDecay;

        /** If true, projectile does not explode when hitting the ground but instead continues travelling. */
        bool groundBounce;

        /**
         * TDF `waterweapon` flag (retail weapontdf +0x111 bit 16):
         * the projectile is not destroyed on crossing below sea level
         * (TotalA.exe 0x49b3a1).
         */
        bool waterWeapon{false};

        /** If true, the weapon consumes ammunition stockpiled via the build-weapon order. */
        bool stockpile;

        /**
         * TDF `interceptor` flag (retail weapontdf +0x111 bit30): the
         * weapon only acquires enemy `targetable` projectiles inside
         * `coverage`; it never attacks units (TotalA.exe acquire
         * path 0x408b38 -> 0x49d120).
         */
        bool interceptor{false};

        /**
         * TDF `coverage` (retail weapontdf +0xe0): the interceptor's
         * engagement box, in world units; a targetable projectile must
         * be within this distance of the defender on each axis
         * independently to be acquired.
         */
        SimScalar coverage{0_ss};

        /**
         * TDF `targetable` flag (retail weapontdf +0x111 bit29): live
         * projectiles of this weapon type are valid interceptor
         * targets (stock strategic missiles only).
         */
        bool targetable{false};

        /**
         * TDF `meteor` flag. The OTA meteor-shower scheduler only fires
         * weapons carrying it (retail TotalA.exe 0x418e82).
         */
        bool isMeteor{false};

        /** Energy cost of stockpiling one round of ammunition. */
        Energy energyPerShot;

        /** Metal cost of stockpiling one round of ammunition. */
        Metal metalPerShot;
    };
}
