#include "GameSimulation.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    namespace
    {
        struct FeatureTimer
        {
            uint64_t* output;
            std::chrono::steady_clock::time_point start;
            explicit FeatureTimer(uint64_t* output) : output(output)
            {
                if (output) { start = std::chrono::steady_clock::now(); }
            }
            ~FeatureTimer()
            {
                if (output) { *output += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(); }
            }
        };

        const FeatureAnimation& animation(const FeatureDefinition& definition, FeatureEvent event)
        {
            return definition.animations.at(static_cast<unsigned int>(event) - 1);
        }

        bool advance(const std::vector<uint16_t>& frames, unsigned int& frame, unsigned int& delay)
        {
            if (frame >= frames.size()) { return false; }
            if (delay >= 2) { --delay; return true; }
            if (++frame >= frames.size()) { delay = 0; return false; }
            delay = frames[frame];
            return true;
        }

        // Explicit mapping from the existing lockstep RNG; no library-dependent
        // distribution or draw for ranges < 2. Retail's feature bounded draw.
        unsigned int featureDraw(std::minstd_rand& rng, unsigned int bound)
        {
            return bound < 2 ? 0 : static_cast<unsigned int>(rng() % bound);
        }
    }

    void GameSimulation::damageFeature(FeatureId id, const WeaponDefinition& weapon)
    {
        FeatureTimer timer(collectTickPhaseTimings ? &tickPhaseTimings.featureDamage : nullptr);
        auto feature = tryGetFeature(id);
        if (!feature || weapon.unitsOnly) { return; }
        auto& f = feature->get();
        const auto& def = getFeatureDefinition(f.featureName);
        if (def.indestructible || f.event != FeatureEvent::None) { return; }
        if (!def.object && def.flamable && weapon.fireStarter)
        {
            // A missing burn sequence still suppresses damage in this branch.
            igniteFeature(id);
            return;
        }
        auto damage = weapon.damage.find("DEFAULT");
        auto amount = damage == weapon.damage.end() ? 0u : static_cast<uint16_t>(damage->second);
        unsigned int sum = f.damageAccumulated + amount;
        if (def.object) { sum = static_cast<uint16_t>(sum); }
        if (sum >= static_cast<uint16_t>(def.damage)) { transitionFeature(id, FeatureEvent::Dying); }
        else { f.damageAccumulated = static_cast<uint16_t>(sum); }
    }

    void GameSimulation::transitionFeature(FeatureId id, FeatureEvent cause)
    {
        auto feature = tryGetFeature(id);
        if (!feature || cause == FeatureEvent::None) { return; }
        auto& f = feature->get();
        const auto& def = getFeatureDefinition(f.featureName);
        if (f.event != FeatureEvent::None || def.indestructible) { return; }
        const auto& anim = animation(def, cause);
        if (def.object || anim.frames.empty())
        {
            completeFeatureTransition(id, cause);
            return;
        }
        f.event = cause;
        f.animationFrame = f.shadowFrame = 0;
        f.animationDelay = anim.frames.front();
        f.shadowDelay = anim.shadows.empty() ? 0 : anim.shadows.front();
        activeFeatures.push_back(id);
    }

    bool GameSimulation::igniteFeature(FeatureId id)
    {
        auto feature = tryGetFeature(id);
        if (!feature) { return false; }
        const auto& def = getFeatureDefinition(feature->get().featureName);
        if (def.object || def.indestructible || feature->get().event != FeatureEvent::None
            || animation(def, FeatureEvent::Burning).frames.empty()) { return false; }
        transitionFeature(id, FeatureEvent::Burning);
        auto half = static_cast<uint16_t>(static_cast<int64_t>(std::trunc(static_cast<double>(def.sparkTime) * 30.0))) >> 1;
        feature->get().sparkCountdown = static_cast<uint8_t>(featureDraw(rng, half) + half);
        auto anchor = computeFootprintRegion(feature->get().position, def.footprintX, def.footprintZ);
        auto soundPosition = terrain.heightmapIndexToWorldCorner(anchor.x, anchor.y);
        soundPosition.y = feature->get().position.y;
        events.emplace_back(FeatureEffectEvent{soundPosition, true});
        return true;
    }

    void GameSimulation::completeFeatureTransition(FeatureId id, FeatureEvent cause)
    {
        auto feature = tryGetFeature(id);
        if (!feature) { return; }
        const auto old = feature->get(); // removal invalidates the slot reference
        const auto& def = getFeatureDefinition(old.featureName);
        auto successor = cause == FeatureEvent::Burning ? def.featureBurnt
            : cause == FeatureEvent::Reclaiming ? def.featureReclamate : def.featureDead;
        auto anchor = computeFootprintRegion(old.position, def.footprintX, def.footprintZ);
        FeatureTimer timer(collectTickPhaseTimings ? &tickPhaseTimings.featureCollision : nullptr);
        removeFeature(id);
        if (!successor) { return; }
        const auto& next = getFeatureDefinition(*successor);
        MapFeature replacement;
        replacement.featureName = *successor;
        replacement.position = old.position;
        replacement.rotation = old.rotation;
        if (!def.object || cause == FeatureEvent::Burning)
        {
            replacement.position = terrain.heightmapIndexToWorldCorner(anchor.x, anchor.y);
            replacement.position.x += SimScalar(next.footprintX * 8);
            replacement.position.z += SimScalar(next.footprintZ * 8);
            replacement.position.y = terrain.getHeightAt(replacement.position.x, replacement.position.z);
        }
        // Preserve progressive native reclaim credits across a lineage. Retail
        // pays only on completion; resetting native credits would create resources.
        replacement.reclaimMetalGiven = old.reclaimMetalGiven;
        replacement.reclaimEnergyGiven = old.reclaimEnergyGiven;
        const auto region = computeFootprintRegion(replacement.position, next.footprintX, next.footprintZ);
        if (next.blocking)
        {
            bool obstructed = false;
            occupiedGrid.forEach(occupiedGrid.clipRegion(region), [&](const auto& cell) {
                obstructed |= cell.mobileUnitId.has_value() || cell.buildingInfo.has_value();
            });
            // Safety for malformed/expanding successors: never entomb a unit.
            if (obstructed) { return; }
        }
        addFeature(std::move(replacement)); // generation changes; old orders cannot hit the successor
    }

    void GameSimulation::featureBurnEvent(FeatureId id)
    {
        auto feature = tryGetFeature(id);
        if (!feature) { return; }
        auto position = feature->get().position;
        const auto& def = getFeatureDefinition(feature->get().featureName);
        auto anchor = computeFootprintRegion(position, def.footprintX, def.footprintZ);
        auto probe = [&](int x, int z) {
            if (x < 0 || z < 0 || x >= occupiedGrid.getWidth() || z >= occupiedGrid.getHeight()) { return; }
            auto candidate = occupiedGrid.get(x, z).featureId;
            if (!candidate) { return; }
            auto f = tryGetFeature(*candidate);
            if (!f || f->get().event != FeatureEvent::None) { return; }
            const auto& d = getFeatureDefinition(f->get().featureName);
            auto a = computeFootprintRegion(f->get().position, d.footprintX, d.footprintZ);
            if (a.x != x || a.y != z || !d.flamable || d.object || d.indestructible) { return; }
            if (static_cast<int>(featureDraw(rng, 100)) < static_cast<uint8_t>(d.spreadChance)) { igniteFeature(*candidate); }
        };
        for (int z = -3; z <= 3; ++z)
        {
            for (int x = -3; x <= 3; ++x)
            {
                if (x != 0 || z != 0) { probe(anchor.x + x, anchor.y + z); }
            }
        }
        // projectileWind stores the same retail raw wind words divided by 65536.
        // Fire interprets twice that vector in tile space, for five cumulative probes.
        auto x = SimScalar(anchor.x), z = SimScalar(anchor.y);
        Point previous(anchor.x, anchor.y);
        for (int i = 0; i < 5; ++i)
        {
            x += 2_ss * projectileWind.x; z += 2_ss * projectileWind.z;
            Point tile(static_cast<int>(std::floor(simScalarToFloat(x))), static_cast<int>(std::floor(simScalarToFloat(z))));
            if (tile != previous) { previous = tile; probe(tile.x, tile.y); }
        }
        auto weapon = weaponDefinitions.find(toUpper(def.burnWeapon));
        if (weapon != weaponDefinitions.end())
        {
            Projectile packet{};
            packet.weaponType = weapon->first;
            packet.position = position;
            packet.position.y = terrain.getHeightAt(position.x, position.z);
            packet.owner = PlayerId(10); // neutral feature producer; no shooter or kill credit
            packet.damage = weapon->second.damage;
            packet.damageRadius = weapon->second.damageRadius;
            doProjectileImpact(packet, ImpactType::Normal);
            events.emplace_back(ProjectileDiedEvent{ProjectileId(0), packet.weaponType, packet.position, ProjectileDiedEvent::DeathType::NormalImpact});
        }
    }

    void GameSimulation::updateFeatures()
    {
        // Snapshot only the active instances: spread appends new fires for next
        // tick; slot reuse cannot turn a captured ID into a different feature.
        const auto active = activeFeatures;
        for (auto it = active.rbegin(); it != active.rend(); ++it)
        {
            auto feature = tryGetFeature(*it);
            if (!feature || feature->get().event == FeatureEvent::None) { continue; }
            auto& f = feature->get();
            const auto& anim = animation(getFeatureDefinition(f.featureName), f.event);
            if (f.event == FeatureEvent::Burning && gameTime.value % 3 == 0)
            {
                events.emplace_back(FeatureEffectEvent{f.position, false});
            }
            bool alive = advance(anim.frames, f.animationFrame, f.animationDelay);
            advance(anim.shadows, f.shadowFrame, f.shadowDelay);
            if (!alive) { completeFeatureTransition(*it, f.event); }
            else if (f.event == FeatureEvent::Burning && f.sparkCountdown != 0 && --f.sparkCountdown == 0)
            {
                featureBurnEvent(*it);
            }
        }
    }
}
