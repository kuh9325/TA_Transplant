#pragma once

#include <rwe/sim/FeatureDefinition.h>
#include <rwe/sim/SimAngle.h>
#include <rwe/sim/SimVector.h>

namespace rwe
{
    struct MapFeature
    {
        FeatureDefinitionId featureName;
        SimVector position;
        SimAngle rotation{0};

        /**
         * Authoritative reclaim progress, in worker-time units applied against
         * the feature's total reclaim value (definition metal + energy).
         * Persists when reclaim is interrupted so a later order resumes.
         */
        unsigned int reclaimWorkDone{0};

        /**
         * Resource amounts already credited to reclaiming builders.
         * Tracked separately from reclaimWorkDone so that per-tick crediting
         * via integer division never duplicates or loses resources; at
         * completion the totals exactly equal the definition's metal/energy.
         * A death/burn successor carries lineage credits, which may exceed its
         * smaller authored pool; consumers saturate remaining resources at zero.
         */
        unsigned int reclaimMetalGiven{0};
        unsigned int reclaimEnergyGiven{0};

        uint16_t damageAccumulated{0};
        FeatureEvent event{FeatureEvent::None};
        unsigned int animationFrame{0};
        unsigned int animationDelay{0};
        unsigned int shadowFrame{0};
        unsigned int shadowDelay{0};
        uint8_t sparkCountdown{0};
    };
}
