#pragma once

#include <rwe/sim/GameTime.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/sim/ProjectileId.h>
#include <rwe/sim/ProjectilePhysicsType.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitId.h>
#include <variant>

namespace rwe
{
    struct Projectile
    {
        std::string weaponType;

        PlayerId owner;

        SimVector position;
        SimVector previousPosition;

        SimVector origin;

        /** Velocity in game pixels/tick */
        SimVector velocity;

        /** The last time the projectile emitted smoke. */
        GameTime lastSmoke;

        std::unordered_map<std::string, unsigned int> damage;

        std::optional<GameTime> dieOnFrame;

        SimScalar damageRadius;

        bool groundBounce;

        bool isDead{false};

        /** The game time at which the projectile was created. */
        GameTime createdAt;

        /** The unit that this projectile is tracking, if any. */
        std::optional<UnitId> targetUnit;

        /**
         * The world position this projectile is tracking toward, if
         * any. Command-fire ground targets steer the shot (nukes
         * vlaunch then guide onto the clicked point).
         */
        std::optional<SimVector> targetPosition;

        /**
         * The projectile this projectile is intercepting, if any
         * (retail projectile +0x56). Set on interceptor weapons'
         * shots; presence on a live missile marks it claimed so no
         * second defender locks it (TotalA.exe dedup at 0x49d1a8).
         */
        std::optional<ProjectileId> targetProjectile;

        /** The unit that fired this projectile, if known. */
        std::optional<UnitId> firedBy;

        SimVector getBackPosition(SimScalar duration) const;

        SimVector getPreviousBackPosition(SimScalar duration) const;

        unsigned int getDamage(const std::string& unitType) const;
    };
}
