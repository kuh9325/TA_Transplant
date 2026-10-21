#include <rwe/sim/MeteorShower.h>

namespace rwe
{
    ResolvedMeteorShowerParams resolveMeteorShowerParams(const MeteorShowerParams& schema, const MeteorShowerParams& defaults)
    {
        ResolvedMeteorShowerParams result;
        result.enabled = !schema.weaponName.empty();

        // retail discards the entire schema record when the weapon
        // name is empty or any numeric field is zero -- the defaults
        // apply wholesale, including the weapon name
        if (schema.weaponName.empty()
            || schema.radius == 0
            || schema.density == 0.0f
            || schema.duration == 0.0f
            || schema.interval == 0.0f)
        {
            result.params = defaults;
        }
        else
        {
            result.params = schema;
        }

        return result;
    }
}
