#pragma once

#include <optional>
#include <array>
#include <vector>
#include <cstdint>
#include <rwe/sim/FeatureDefinitionId.h>
#include <rwe/sim/SimScalar.h>
#include <string>

namespace rwe
{
    enum class FeatureEvent : uint8_t { None, Dying, Reclaiming, Burning };

    struct FeatureAnimation
    {
        std::vector<uint16_t> frames;
        std::vector<uint16_t> shadows;
    };

    struct FeatureDefinition
    {
        std::string name;

        int footprintX;
        int footprintZ;
        SimScalar height;

        bool reclaimable;
        bool autoreclaimable;
        std::optional<FeatureDefinitionId> featureReclamate;
        unsigned int metal;
        unsigned int energy;

        bool flamable;
        std::optional<FeatureDefinitionId> featureBurnt;
        unsigned int burnMin;
        unsigned int burnMax;
        float sparkTime;
        unsigned int spreadChance;
        std::string burnWeapon;

        bool geothermal;

        unsigned int hitDensity;

        bool reproduce;
        unsigned int reproduceArea;

        bool noDisplayInfo;

        bool permanent;

        bool blocking;

        bool indestructible;
        unsigned int damage;
        std::optional<FeatureDefinitionId> featureDead;

        bool object{false};
        // Event cursors use authored GAF delays at 30 Hz, including in headless play.
        std::array<FeatureAnimation, 3> animations;

        bool isStanding() const;
    };
}
