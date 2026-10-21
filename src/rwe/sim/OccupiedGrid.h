#pragma once

#include <rwe/grid/StateHashGrid.h>
#include <rwe/sim/FeatureId.h>
#include <rwe/sim/UnitId.h>
#include <variant>

namespace rwe
{
    struct OccupiedCellBuildingInfo
    {
        UnitId unit;
        bool passable;
    };

    struct OccupiedCell
    {
        std::optional<UnitId> mobileUnitId;
        std::optional<OccupiedCellBuildingInfo> buildingInfo;
        std::optional<FeatureId> featureId;
    };

    struct OccupiedCellHash
    {
        std::uint32_t operator()(const OccupiedCell& c) const
        {
            if (!c.mobileUnitId && !c.buildingInfo && !c.featureId) { return 0; }
            std::uint32_t h = 2166136261u;
            auto add = [&](std::uint32_t v) { h = (h ^ v) * 16777619u; };
            add(c.mobileUnitId.has_value()); if (c.mobileUnitId) { add(c.mobileUnitId->value); }
            add(c.buildingInfo.has_value()); if (c.buildingInfo) { add(c.buildingInfo->unit.value); add(c.buildingInfo->passable); }
            add(c.featureId.has_value()); if (c.featureId) { add(c.featureId->value); }
            return h;
        }
    };
    using OccupiedGrid = StateHashGrid<OccupiedCell, OccupiedCellHash>;
}
