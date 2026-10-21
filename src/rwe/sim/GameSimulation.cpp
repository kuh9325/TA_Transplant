#include "GameSimulation.h"
#include "Veterancy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/sim/cob.h>
#include <rwe/sim/movement.h>
#include <rwe/sim/util.h>
#include <rwe/util/Index.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/collection_util.h>
#include <rwe/util/match.h>
#include <rwe/util/rwe_string.h>
#include <type_traits>
#include <unordered_set>

namespace rwe
{
    namespace
    {
        /**
         * Accumulates wall-clock nanoseconds into `dst` on destruction.
         * A null `dst` makes the timer an inert no-op — used to make
         * tick() phase profiling opt-in without restructuring the tick.
         */
        class ScopedPhaseTimer
        {
        public:
            explicit ScopedPhaseTimer(uint64_t* dst) : dst(dst)
            {
                if (dst != nullptr)
                {
                    start = std::chrono::steady_clock::now();
                }
            }

            ~ScopedPhaseTimer()
            {
                if (dst != nullptr)
                {
                    *dst += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count());
                }
            }

        private:
            uint64_t* dst;
            std::chrono::steady_clock::time_point start;
        };
    }

    bool GamePlayerInfo::addResourceDelta(const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal)
    {
        if (recordAndCheckDesire(apparentEnergy) && recordAndCheckDesire(apparentMetal))
        {
            acceptResource(actualEnergy);
            acceptResource(actualMetal);
            return true;
        }

        return false;
    }

    bool GamePlayerInfo::recordAndCheckDesire(const rwe::Energy& energy)
    {
        if (energy >= Energy(0))
        {
            return true;
        }

        desiredEnergyConsumptionBuffer -= energy;
        return !energyStalled;
    }

    bool GamePlayerInfo::recordAndCheckDesire(const rwe::Metal& metal)
    {
        if (metal >= Metal(0))
        {
            return true;
        }

        desiredMetalConsumptionBuffer -= metal;
        return !metalStalled;
    }

    void GamePlayerInfo::acceptResource(const rwe::Energy& energy)
    {
        if (energy >= Energy(0))
        {
            energyProductionBuffer += energy;
        }
        else
        {
            actualEnergyConsumptionBuffer -= energy;
        }
    }

    void GamePlayerInfo::acceptResource(const rwe::Metal& metal)
    {
        if (metal >= Metal(0))
        {
            metalProductionBuffer += metal;
        }
        else
        {
            actualMetalConsumptionBuffer -= metal;
        }
    }

    bool PathRequest::operator==(const PathRequest& rhs) const
    {
        return unitId == rhs.unitId;
    }

    bool PathRequest::operator!=(const PathRequest& rhs) const
    {
        return !(rhs == *this);
    }

    GameSimulation::GameSimulation(MapTerrain&& terrain, unsigned char surfaceMetal, int minWindSpeed, int maxWindSpeed, int tidalStrength)
        : terrain(std::move(terrain)),
          occupiedGrid(this->terrain.getHeightMap().getWidth() - 1, this->terrain.getHeightMap().getHeight() - 1, OccupiedCell()),
          metalGrid(this->terrain.getHeightMap().getWidth() - 1, this->terrain.getHeightMap().getHeight() - 1, surfaceMetal),
          geoGrid(this->terrain.getHeightMap().getWidth() - 1, this->terrain.getHeightMap().getHeight() - 1, false),
          minWindSpeed(minWindSpeed),
          maxWindSpeed(maxWindSpeed),
          tidalStrength(tidalStrength),
          nextWindSpeedChange(gameTime)
    {
    }

    std::optional<FeatureId> GameSimulation::addFeature(MapFeature&& newFeature)
    {
        const auto& featureDefinition = getFeatureDefinition(newFeature.featureName);

        auto footprintRegion = computeFootprintRegion(newFeature.position, featureDefinition.footprintX, featureDefinition.footprintZ);

        if (anyFeatureOccupies(footprintRegion))
        {
            return std::nullopt;
        }

        auto featureId = FeatureId(features.emplace(std::move(newFeature)));

        auto& f = features.tryGet(featureId)->get();

        occupiedGrid.forEach(occupiedGrid.clipRegion(footprintRegion), [&](auto& cell) {
            cell.featureId = featureId;
        });

        if (!featureDefinition.blocking && featureDefinition.indestructible && featureDefinition.metal)
        {
            metalGrid.set(metalGrid.clipRegion(footprintRegion), featureDefinition.metal);
        }

        if (!featureDefinition.blocking && featureDefinition.indestructible && featureDefinition.geothermal)
        {
            geoGrid.set(geoGrid.clipRegion(footprintRegion), true);
        }

        return featureId;
    }

    int computeMidpointHeight(const Grid<unsigned char>& heightmap, int x, int y)
    {
        assert(x < heightmap.getWidth() - 1);
        assert(y < heightmap.getHeight() - 1);

        auto p1 = static_cast<int>(heightmap.get(x, y));
        auto p2 = static_cast<int>(heightmap.get(x + 1, y));
        auto p3 = static_cast<int>(heightmap.get(x, y + 1));
        auto p4 = static_cast<int>(heightmap.get(x + 1, y + 1));
        return (p1 + p2 + p3 + p4) / 4;
    }

    SimVector computeFeaturePosition(
        const MapTerrain& terrain,
        const FeatureDefinition& featureDefinition,
        int x,
        int y)
    {
        auto position = terrain.heightmapIndexToWorldCorner(x, y);
        position.x += (intToSimScalar(featureDefinition.footprintX) * MapTerrain::HeightTileWidthInWorldUnits) / 2_ss;
        position.z += (intToSimScalar(featureDefinition.footprintZ) * MapTerrain::HeightTileHeightInWorldUnits) / 2_ss;
        // Retail static feature reference points use the footprint centre's
        // bilinear terrain height, including below sea level.
        position.y = terrain.getHeightAt(position.x, position.z);

        return position;
    }

    std::optional<FeatureId> GameSimulation::addFeature(FeatureDefinitionId featureType, int heightmapX, int heightmapZ)
    {
        const auto& featureDefinition = getFeatureDefinition(featureType);
        auto resolvedPos = computeFeaturePosition(terrain, featureDefinition, heightmapX, heightmapZ);
        auto featureInstance = MapFeature{featureType, resolvedPos, fromRadians(RadiansAngle::fromUnwrappedAngle(Pif))};
        return addFeature(std::move(featureInstance));
    }

    PlayerId GameSimulation::addPlayer(const GamePlayerInfo& info)
    {
        PlayerId id(players.size());
        players.push_back(info);
        return id;
    }

    std::optional<UnitWeapon> tryCreateWeapon(const GameSimulation& sim, const std::string& weaponType)
    {
        if (sim.weaponDefinitions.find(toUpper(weaponType)) == sim.weaponDefinitions.end())
        {
            return std::nullopt;
        }

        UnitWeapon weapon;
        weapon.weaponType = toUpper(weaponType);
        return weapon;
    }

    std::vector<UnitMesh> createUnitMeshes(const GameSimulation& sim, const std::string& objectName)
    {
        const auto& def = sim.unitModelDefinitions.at(objectName);

        const auto& pieceDefs = def.pieces;

        std::vector<UnitMesh> pieces(pieceDefs.size());
        for (Index i = 0; i < getSize(pieces); ++i)
        {
            pieces[i].name = pieceDefs[i].name;
        }

        return pieces;
    }

    UnitState createUnit(
        GameSimulation& simulation,
        const std::string& unitType,
        PlayerId owner,
        const SimVector& position,
        std::optional<SimAngle> rotation)
    {
        const auto& unitDefinition = simulation.unitDefinitions.at(unitType);

        auto meshes = createUnitMeshes(simulation, unitDefinition.objectName);
        auto modelDefinition = simulation.unitModelDefinitions.at(unitDefinition.objectName);

        if (unitDefinition.isMobile)
        {
            // don't shade mobile units
            for (auto& m : meshes)
            {
                m.shaded = false;
            }
        }

        const auto& script = simulation.unitScriptDefinitions.at(unitType);
        auto cobEnv = std::make_unique<CobEnvironment>(&script);
        UnitState unit(meshes, std::move(cobEnv));
        unit.unitType = toUpper(unitType);
        unit.owner = owner;
        unit.position = position;
        unit.previousPosition = position;
        unit.fireOrders = unitDefinition.standingFireOrder;
        unit.moveOrders = unitDefinition.standingMoveOrder;

        // FBI init_cloaked: the unit spawns with the cloak request on;
        // effective cloak follows on the first upkeep pass
        unit.cloakRequested = unitDefinition.initCloaked;

        if (rotation)
        {
            unit.rotation = *rotation;
            unit.previousRotation = *rotation;
        }
        else if (unitDefinition.isMobile)
        {
            // spawn the unit facing the other way
            unit.rotation = HalfTurn;
            unit.previousRotation = HalfTurn;
        }

        // add weapons
        if (!unitDefinition.weapon1.empty())
        {
            unit.weapons[0] = tryCreateWeapon(simulation, unitDefinition.weapon1);
        }
        if (!unitDefinition.weapon2.empty())
        {
            unit.weapons[1] = tryCreateWeapon(simulation, unitDefinition.weapon2);
        }
        if (!unitDefinition.weapon3.empty())
        {
            unit.weapons[2] = tryCreateWeapon(simulation, unitDefinition.weapon3);
        }

        return unit;
    }

    std::optional<UnitId> GameSimulation::trySpawnUnit(const std::string& unitType, PlayerId owner, const SimVector& position, std::optional<SimAngle> rotation, bool allowOverlap, std::optional<UnitId> ignoreUnit, bool bypassUnitLimit)
    {
        if (!bypassUnitLimit && countLiveUnitsOwnedBy(owner) >= maxUnitsPerPlayer)
        {
            // retail per-player maxunits gate (slot ranges allocated
            // per player at mission start; creation past the cap is
            // refused)
            return std::nullopt;
        }

        auto unit = createUnit(*this, unitType, owner, position, rotation);
        const auto& unitDefinition = unitDefinitions.at(unitType);
        if (!unitDefinition.isMobile)
        {
            // structures take the Y the placement rules compute for
            // their footprint -- terrain support height, or floating
            // at seaLevel - waterline for all-water yardmaps
            auto mc = getAdHocMovementClass(unitDefinition.movementCollisionInfo);
            auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
            if (auto placement = computeBuildingPlacement(mc, unitDefinition.yardMap, unitDefinition.yardMapContainsGeo, unitDefinition.waterline, footprintRect.x, footprintRect.y))
            {
                unit.position.y = placement->baseY;
                unit.previousPosition.y = placement->baseY;
            }
        }
        else if (unitDefinition.floater || unitDefinition.canHover)
        {
            unit.position.y = rweMax(terrain.getSeaLevel(), unit.position.y);
            unit.previousPosition.y = unit.position.y;
        }

        // TODO: if we failed to add the unit throw some warning
        auto unitId = tryAddUnit(std::move(unit), allowOverlap, ignoreUnit);

        if (unitId)
        {
            UnitBehaviorService(this).onCreate(*unitId);
            events.push_back(UnitSpawnedEvent{*unitId});
        }

        return unitId;
    }

    unsigned int GameSimulation::countLiveUnitsOwnedBy(PlayerId owner) const
    {
        unsigned int count = 0;
        for (const auto& [id, unit] : units)
        {
            if (!unit.isDead() && unit.isOwnedBy(owner))
            {
                ++count;
            }
        }
        return count;
    }

    std::optional<UnitId> GameSimulation::tryAddUnit(UnitState&& unit, bool allowOverlap, std::optional<UnitId> ignoreUnit)
    {
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);

        // set footprint area as occupied by the unit
        auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        // a factory nanoframe spawns inside its parent's open yard:
        // the parent's own cells are excluded from the collision test
        if (!allowOverlap && (ignoreUnit ? isCollisionAt(footprintRect, *ignoreUnit) : isCollisionAt(footprintRect)))
        {
            return std::nullopt;
        }

        auto unitId = units.emplace(std::move(unit));
        if (unitDefinition.isAirBase) { airServiceFacilities.insert(unitId); }
        // a mid-tick spawn is a new potential acquisition candidate;
        // force the per-tick candidate lists to rebuild on next query
        invalidateWeaponTargetCandidates();
        const auto& insertedUnit = units.tryGet(unitId)->get();

        if (allowOverlap)
        {
            // authored mission units can legitimately overlap each
            // other, sit on blocking features, or hang off the map
            // edge; mark what fits instead of rejecting the unit
            auto clipped = occupiedGrid.clipRegion(footprintRect);
            if (unitDefinition.isMobile)
            {
                occupiedGrid.forEach(clipped, [unitId](auto& cell) { cell.mobileUnitId = unitId; });
            }
            else
            {
                assert(!!unitDefinition.yardMap);
                clipped.forEach([&](const auto& c) {
                    auto yardCell = unitDefinition.yardMap->tryGetValue(Point(c.x - footprintRect.x, c.y - footprintRect.y));
                    if (yardCell)
                    {
                        occupiedGrid.mutate(c, [&](auto& cell) { cell.buildingInfo = OccupiedCellBuildingInfo{unitId, isPassable(*yardCell, insertedUnit.yardOpen)}; });
                    }
                });
            }

            return unitId;
        }

        auto footprintRegion = occupiedGrid.tryToRegion(footprintRect);
        assert(!!footprintRegion);

        if (unitDefinition.isMobile)
        {
            occupiedGrid.forEach(*footprintRegion, [unitId](auto& cell) { cell.mobileUnitId = unitId; });
        }
        else
        {
            assert(!!unitDefinition.yardMap);
            occupiedGrid.forEach2(footprintRegion->x, footprintRegion->y, *unitDefinition.yardMap, [&](auto& cell, const auto& yardMapCell) {
                cell.buildingInfo = OccupiedCellBuildingInfo{unitId, isPassable(yardMapCell, insertedUnit.yardOpen)};
            });
        }

        return unitId;
    }

    void GameSimulation::rebuildOccupancyAfterLoad()
    {
        occupiedGrid = OccupiedGrid(occupiedGrid.getWidth(), occupiedGrid.getHeight(), OccupiedCell());
        flyingUnitsSet.clear();

        for (const auto& [featureId, feature] : features)
        {
            const auto& featureDefinition = getFeatureDefinition(feature.featureName);
            auto footprintRegion = computeFootprintRegion(feature.position, featureDefinition.footprintX, featureDefinition.footprintZ);
            occupiedGrid.forEach(occupiedGrid.clipRegion(footprintRegion), [&](auto& cell) {
                cell.featureId = featureId;
            });
        }

        for (const auto& [unitId, unit] : units)
        {
            // carried units own no footprint cells (tryBoardTransport
            // cleared them on boarding)
            if (unit.carriedBy)
            {
                continue;
            }
            const auto& unitDefinition = unitDefinitions.at(unit.unitType);
            auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
            auto footprintRegion = occupiedGrid.tryToRegion(footprintRect);
            if (unitDefinition.isMobile)
            {
                if (!footprintRegion)
                {
                    continue;
                }
                if (isFlying(unit.physics))
                {
                    flyingUnitsSet.insert(unitId);
                }
                else
                {
                    occupiedGrid.forEach(*footprintRegion, [unitId](auto& cell) { cell.mobileUnitId = unitId; });
                }
            }
            else if (unitDefinition.yardMap)
            {
                // Authored buildings may straddle the map boundary. Match
                // tryAddUnit's clipped placement, including the yard offset.
                occupiedGrid.clipRegion(footprintRect).forEach([&](const auto& cell) {
                    auto yardCell = unitDefinition.yardMap->tryGetValue(
                        Point(cell.x - footprintRect.x, cell.y - footprintRect.y));
                    if (yardCell)
                    {
                        occupiedGrid.mutate(cell, [&](auto& value) { value.buildingInfo = OccupiedCellBuildingInfo{
                            unitId, isPassable(*yardCell, unit.yardOpen)}; });
                    }
                });
            }
        }
    }

    std::optional<GameSimulation::BuildingPlacement> GameSimulation::computeBuildingPlacement(const rwe::MovementClassDefinition& mc, const std::optional<Grid<YardMapCell>>& yardMap, bool yardMapContainsGeo, unsigned int waterline, unsigned int x, unsigned int y) const
    {
        const auto& heights = terrain.getHeightMap();
        const int seaLevel = static_cast<int>(simScalarToUInt(terrain.getSeaLevel()));

        // the terrain scan needs the footprint's corner vertices,
        // which extend one cell past the footprint itself
        if (!heights.tryToRegion(DiscreteRect(x, y, mc.footprintX + 1, mc.footprintZ + 1)))
        {
            return std::nullopt;
        }

        bool valid = !isCollisionAt(DiscreteRect(x, y, mc.footprintX, mc.footprintZ));

        // retail tracks terrain extrema separately for support cells
        // (o f O c y G) and water cells (w C Y); '.' cells are ignored
        int minSupportLow = 255;
        int maxSupportHigh = 0;
        int maxWaterHigh = 0;

        for (unsigned int dy = 0; dy < mc.footprintZ; ++dy)
        {
            for (unsigned int dx = 0; dx < mc.footprintX; ++dx)
            {
                auto cell = yardMap ? yardMap->get(dx, dy) : YardMapCell::Ground;
                if (cell == YardMapCell::Passable)
                {
                    continue;
                }

                int cellLow = 255;
                int cellHigh = 0;
                for (unsigned int vy = 0; vy < 2; ++vy)
                {
                    for (unsigned int vx = 0; vx < 2; ++vx)
                    {
                        int h = heights.get(x + dx + vx, y + dy + vy);
                        cellLow = std::min(cellLow, h);
                        cellHigh = std::max(cellHigh, h);
                    }
                }

                if (isWater(cell))
                {
                    maxWaterHigh = std::max(maxWaterHigh, cellHigh);
                }
                else
                {
                    minSupportLow = std::min(minSupportLow, cellLow);
                    maxSupportHigh = std::max(maxSupportHigh, cellHigh);
                }
            }
        }

        int baseY;
        if (maxSupportHigh >= minSupportLow)
        {
            // the footprint has support cells: the base sits on the
            // lowest terrain corner and the footprint's height range
            // must satisfy the unit's slope limit
            if (maxSupportHigh - minSupportLow > static_cast<int>(mc.maxSlope))
            {
                valid = false;
            }
            baseY = minSupportLow;

            // MaxWaterDepth bounds the deepest support corner
            if (minSupportLow < seaLevel - static_cast<int>(mc.maxWaterDepth))
            {
                valid = false;
            }
        }
        else
        {
            // an all-water footprint floats with its base below the
            // surface; MaxWaterDepth is not applied to it
            baseY = seaLevel - static_cast<int>(waterline);
        }

        // keel clearance: no water cell's terrain may poke above the base
        if (maxWaterHigh > baseY)
        {
            valid = false;
        }

        // MinWaterDepth: every occupied cell's highest corner must be
        // at least this deep; depths clamp at 0 so a default of 0
        // never rejects dry land
        int shallowDepth = std::max(0, seaLevel - std::max(maxSupportHigh, maxWaterHigh));
        if (shallowDepth < static_cast<int>(mc.minWaterDepth))
        {
            valid = false;
        }

        if (yardMapContainsGeo && yardMap && !containsAnyGeoMatch(*yardMap, x, y))
        {
            valid = false;
        }

        return BuildingPlacement{intToSimScalar(baseY), valid};
    }

    DiscreteRect GameSimulation::computeFootprintRegion(const SimVector& position, unsigned int footprintX, unsigned int footprintZ) const
    {
        auto halfFootprintX = SimScalar(footprintX * MapTerrain::HeightTileWidthInWorldUnits.value / 2);
        auto halfFootprintZ = SimScalar(footprintZ * MapTerrain::HeightTileHeightInWorldUnits.value / 2);
        SimVector topLeft(
            position.x - halfFootprintX,
            position.y,
            position.z - halfFootprintZ);

        auto cell = terrain.worldToHeightmapCoordinateNearest(topLeft);

        return DiscreteRect(cell.x, cell.y, footprintX, footprintZ);
    }

    DiscreteRect GameSimulation::computeFootprintRegion(const SimVector& position, const UnitDefinition::MovementCollisionInfo& collisionInfo) const
    {
        auto [footprintX, footprintZ] = getFootprintXZ(collisionInfo);
        return computeFootprintRegion(position, footprintX, footprintZ);
    }

    bool GameSimulation::anyFeatureOccupies(const DiscreteRect& rect) const
    {
        auto region = occupiedGrid.tryToRegion(rect);
        if (!region)
        {
            return true;
        }

        return occupiedGrid.any(*region, [&](const auto& cell) {
            return cell.featureId.has_value();
        });
    }

    bool GameSimulation::containsAnyGeoMatch(const Grid<YardMapCell>& yardMap, unsigned int x, unsigned int y) const
    {
        return geoGrid.any2(x, y, yardMap, [](const bool& geo, const YardMapCell& cell) {
            return geo && isGeo(cell);
        });
    }

    bool GameSimulation::isCollisionAt(const DiscreteRect& rect) const
    {
        auto region = occupiedGrid.tryToRegion(rect);
        if (!region)
        {
            return true;
        }

        return isCollisionAt(*region);
    }

    bool GameSimulation::isCollisionAt(const GridRegion& region) const
    {
        return occupiedGrid.any(region, [&](const auto& cell) {
            if (cell.mobileUnitId)
            {
                return true;
            }
            if (cell.buildingInfo && !cell.buildingInfo->passable)
            {
                return true;
            }
            if (cell.featureId)
            {
                const auto& f = getFeature(*cell.featureId);
                const auto& def = getFeatureDefinition(f.featureName);
                if (def.blocking)
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool GameSimulation::isCollisionAt(const DiscreteRect& rect, UnitId self) const
    {
        auto region = occupiedGrid.tryToRegion(rect);
        if (!region)
        {
            return true;
        }

        return occupiedGrid.any(*region, [&](const auto& cell) {
            if (cell.mobileUnitId && *cell.mobileUnitId != self)
            {
                return true;
            }
            if (cell.buildingInfo && !cell.buildingInfo->passable && cell.buildingInfo->unit != self)
            {
                return true;
            }
            if (cell.featureId)
            {
                const auto& f = getFeature(*cell.featureId);
                const auto& def = getFeatureDefinition(f.featureName);
                if (def.blocking)
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool GameSimulation::isYardmapBlocked(unsigned int x, unsigned int y, const Grid<YardMapCell>& yardMap, bool open, UnitId self) const
    {
        return occupiedGrid.any2(x, y, yardMap, [&](const auto& cell, const auto& yardMapCell) {
            if (isPassable(yardMapCell, open))
            {
                return false;
            }

            if (cell.mobileUnitId)
            {
                return true;
            }
            if (cell.buildingInfo && !cell.buildingInfo->passable && cell.buildingInfo->unit != self)
            {
                return true;
            }
            if (cell.featureId)
            {
                const auto& f = getFeature(*cell.featureId);
                const auto& def = getFeatureDefinition(f.featureName);
                if (def.blocking)
                {
                    return true;
                }
            }

            return false;
        });
    }

    bool GameSimulation::isAdjacentToObstacle(const DiscreteRect& rect) const
    {
        DiscreteRect top(rect.x - 1, rect.y - 1, rect.width + 2, 1);
        DiscreteRect bottom(rect.x - 1, rect.y + rect.width, rect.width + 2, 1);
        DiscreteRect left(rect.x - 1, rect.y, 1, rect.height);
        DiscreteRect right(rect.x + rect.width, rect.y, 1, rect.height);
        return isCollisionAt(top)
            || isCollisionAt(bottom)
            || isCollisionAt(left)
            || isCollisionAt(right);
    }

    void GameSimulation::showObject(UnitId unitId, const std::string& name)
    {
        auto mesh = getUnitState(unitId).findPiece(name);
        if (mesh)
        {
            mesh->get().visible = true;
        }
    }

    void GameSimulation::hideObject(UnitId unitId, const std::string& name)
    {
        auto mesh = getUnitState(unitId).findPiece(name);
        if (mesh)
        {
            mesh->get().visible = false;
        }
    }

    void GameSimulation::enableShading(UnitId unitId, const std::string& name)
    {
        auto mesh = getUnitState(unitId).findPiece(name);
        if (mesh)
        {
            mesh->get().shaded = true;
        }
    }

    void GameSimulation::disableShading(UnitId unitId, const std::string& name)
    {
        auto mesh = getUnitState(unitId).findPiece(name);
        if (mesh)
        {
            mesh->get().shaded = false;
        }
    }

    UnitState& GameSimulation::getUnitState(UnitId id)
    {
        auto it = units.find(id);
        assert(it != units.end());
        return it->second;
    }

    const UnitState& GameSimulation::getUnitState(UnitId id) const
    {
        auto it = units.find(id);
        assert(it != units.end());
        return it->second;
    }

    UnitInfo GameSimulation::getUnitInfo(UnitId id)
    {
        auto& state = getUnitState(id);
        const auto& definition = unitDefinitions.at(state.unitType);
        return UnitInfo(id, &state, &definition);
    }

    ConstUnitInfo GameSimulation::getUnitInfo(UnitId id) const
    {
        auto& state = getUnitState(id);
        const auto& definition = unitDefinitions.at(state.unitType);
        return ConstUnitInfo(id, &state, &definition);
    }

    std::optional<std::reference_wrapper<UnitState>> GameSimulation::tryGetUnitState(UnitId id)
    {
        return tryFind(units, id);
    }

    std::optional<std::reference_wrapper<const UnitState>> GameSimulation::tryGetUnitState(UnitId id) const
    {
        return tryFind(units, id);
    }

    std::optional<std::reference_wrapper<const UnitState>> GameSimulation::tryGetUnitState(CobUnitId id) const
    {
        return tryFind(units, UnitId(id.value));
    }

    bool GameSimulation::unitExists(UnitId id) const
    {
        auto it = units.find(id);
        return it != units.end();
    }

    namespace
    {
        /**
         * True when the order's target is the given unit. Used by the
         * ownership transfer to drop foreign references — retail
         * rebuilds the unit as a fresh entity, so references held by
         * other units die with it (TotalA.exe 0x488570).
         */
        bool orderTargetsUnit(const UnitOrder& order, UnitId unitId)
        {
            return match(
                order,
                [&](const AttackOrder& o) { return std::holds_alternative<UnitId>(o.target) && std::get<UnitId>(o.target) == unitId; },
                [&](const CompleteBuildOrder& o) { return o.target == unitId; },
                [&](const GuardOrder& o) { return o.target == unitId; },
                [&](const RepairOrder& o) { return o.target == unitId; },
                [&](const ReclaimOrder& o) { return std::holds_alternative<UnitId>(o.target) && std::get<UnitId>(o.target) == unitId; },
                [&](const CaptureOrder& o) { return o.target == unitId; },
                [&](const LoadOrder& o) { return o.target == unitId; },
                [&](const CommandFireOrder& o) { return std::holds_alternative<UnitId>(o.target) && std::get<UnitId>(o.target) == unitId; },
                [](const auto&) { return false; });
        }
    }

    bool GameSimulation::transferUnitOwnership(UnitId unitId, PlayerId newOwner, std::optional<UnitId> performedBy)
    {
        auto unitRef = tryGetUnitState(unitId);
        if (!unitRef)
        {
            return false;
        }
        auto& unit = unitRef->get();

        // retail early-outs on a same-owner transfer (TotalA.exe
        // 0x488570 reads unit+0x96 against the requested owner)
        if (unit.isDead() || unit.carriedBy || unit.owner == newOwner)
        {
            return false;
        }

        // dispatched before the owner field mutates so the evaluator
        // observes the previous owner (CaptureUnitType latch)
        auto previousOwner = unit.owner;
        if (missionObjectives)
        {
            missionObjectives->onUnitOwnerChanged(*this, unit, previousOwner);
        }

        unit.owner = newOwner;
        unit.kills = 0; // retail ownership transfer constructs a fresh unit record
        // the transfer changes every player's enemy set; rebuild the
        // per-tick acquisition candidate lists on next query
        invalidateWeaponTargetCandidates();

        // retail respawns the unit as a fresh entity for its new owner:
        // the mission selectability gate must not survive the transfer,
        // or a unit captured while its scripted queue still gates it
        // (or whose `p`/`d` suppressed the implicit make-selectable)
        // can never be selected by its new owner
        if (unit.missionState)
        {
            unit.missionState->selectable = true;
        }

        // volatile state discarded by the retail respawn
        unit.clearOrders();
        unit.backgroundOrders.clear();
        unit.buildQueue.clear();
        unit.buildOrderUnitId = std::nullopt;
        unit.factoryState = FactoryBehaviorStateIdle();
        unit.behaviourState = UnitBehaviorStateIdle();
        unit.inBuildStance = false;
        unit.navigationState = NavigationStateInfo();
        unit.lastDamagedBy = std::nullopt;
        unit.lastDamagedByPlayer = std::nullopt;
        unit.fireOrders = UnitFireOrders::FireAtWill;
        unit.moveOrders = UnitMoveOrders::Maneuver;
        unit.cloakRequested = false;
        unit.cloaked = false;
        unit.cloakBlockedUntil = GameTime(0);

        // foreign references die with the retail respawn: drop other
        // units' weapon locks and orders aimed at the unit so a former
        // enemy does not keep attacking a now-friendly unit
        for (auto& [otherId, other] : units)
        {
            if (otherId == unitId)
            {
                continue;
            }

            for (Index i = 0; i < getSize(other.weapons); ++i)
            {
                auto& weapon = other.weapons[i];
                if (!weapon)
                {
                    continue;
                }
                auto attacking = std::get_if<UnitWeaponStateAttacking>(&weapon->state);
                if (attacking
                    && std::holds_alternative<UnitId>(attacking->target)
                    && std::get<UnitId>(attacking->target) == unitId)
                {
                    other.clearWeaponTarget(i);
                }
            }

            if (other.lastDamagedBy && *other.lastDamagedBy == unitId)
            {
                other.lastDamagedBy = std::nullopt;
            }

            if (performedBy && otherId == *performedBy)
            {
                // the capturer's running CaptureOrder completes
                // normally when its handler returns
                continue;
            }
            for (std::size_t i = other.orders.size(); i-- > 0;)
            {
                if (orderTargetsUnit(other.orders[i], unitId))
                {
                    other.removeOrderAt(i);
                }
            }
        }

        return true;
    }

    MapFeature& GameSimulation::getFeature(FeatureId id)
    {
        auto it = features.find(id);
        assert(it != features.end());
        return it->second;
    }

    const MapFeature& GameSimulation::getFeature(FeatureId id) const
    {
        auto it = features.find(id);
        assert(it != features.end());
        return it->second;
    }

    std::optional<std::reference_wrapper<MapFeature>> GameSimulation::tryGetFeature(FeatureId id)
    {
        return tryFind(features, id);
    }

    std::optional<std::reference_wrapper<const MapFeature>> GameSimulation::tryGetFeature(FeatureId id) const
    {
        return tryFind(features, id);
    }

    void GameSimulation::removeFeature(FeatureId id)
    {
        auto featureIt = features.find(id);
        if (featureIt == features.end())
        {
            return;
        }

        const auto& feature = featureIt->second;
        const auto& featureDefinition = getFeatureDefinition(feature.featureName);
        auto footprintRegion = computeFootprintRegion(feature.position, featureDefinition.footprintX, featureDefinition.footprintZ);
        occupiedGrid.forEach(occupiedGrid.clipRegion(footprintRegion), [&](auto& cell) {
            if (cell.featureId == id)
            {
                cell.featureId = std::nullopt;
            }
        });

        activeFeatures.erase(std::remove(activeFeatures.begin(), activeFeatures.end(), id), activeFeatures.end());
        features.remove(id);
    }

    bool GameSimulation::reclaimComplete(const MapFeature& feature) const
    {
        const auto& featureDefinition = getFeatureDefinition(feature.featureName);
        return feature.reclaimWorkDone >= featureDefinition.metal + featureDefinition.energy;
    }

    std::optional<std::pair<Energy, Metal>> GameSimulation::applyReclaimProgress(FeatureId id, unsigned int workerTimePerTick)
    {
        auto feature = tryGetFeature(id);
        if (!feature || feature->get().event != FeatureEvent::None)
        {
            return std::nullopt;
        }

        auto& f = feature->get();
        const auto& featureDefinition = getFeatureDefinition(f.featureName);
        const auto totalPool = featureDefinition.metal + featureDefinition.energy;

        auto newWorkDone = f.reclaimWorkDone + workerTimePerTick;
        if (newWorkDone > totalPool)
        {
            newWorkDone = totalPool;
        }
        f.reclaimWorkDone = newWorkDone;

        auto entitledMetal = totalPool == 0
            ? 0u
            : static_cast<unsigned int>((static_cast<unsigned long long>(featureDefinition.metal) * newWorkDone) / totalPool);
        auto entitledEnergy = totalPool == 0
            ? 0u
            : static_cast<unsigned int>((static_cast<unsigned long long>(featureDefinition.energy) * newWorkDone) / totalPool);

        entitledMetal = std::max(entitledMetal, f.reclaimMetalGiven);
        entitledEnergy = std::max(entitledEnergy, f.reclaimEnergyGiven);
        auto metalCredit = entitledMetal - f.reclaimMetalGiven;
        auto energyCredit = entitledEnergy - f.reclaimEnergyGiven;
        f.reclaimMetalGiven = entitledMetal;
        f.reclaimEnergyGiven = entitledEnergy;

        return std::make_pair(Energy(energyCredit), Metal(metalCredit));
    }

    GamePlayerInfo& GameSimulation::getPlayer(PlayerId player)
    {
        return players.at(player.value);
    }

    const GamePlayerInfo& GameSimulation::getPlayer(PlayerId player) const
    {
        return players.at(player.value);
    }

    void GameSimulation::updateVisibility()
    {
        playerVisibility.resize(players.size());

        if (!lineOfSightEnabled)
        {
            return;
        }

        // visibility grids are per-cell like occupiedGrid: the
        // heightmap holds vertices, so cells are one fewer per side
        const auto& heightMap = terrain.getHeightMap();
        const auto mapW = heightMap.getWidth() - 1;
        const auto mapH = heightMap.getHeight() - 1;

        struct ViewerUnit
        {
            UnitId unitId;
            SimVector position;
            SimScalar sightDistance;
            SimScalar sightSquared;
            SimScalar radarSquared;
            SimScalar sonarSquared;
        };

        struct JammerUnit
        {
            SimVector position;
            SimScalar radarJamSquared;
            SimScalar sonarJamSquared;
        };

        const auto seaLevel = terrain.getSeaLevel();

        struct DetectableUnit
        {
            UnitId unitId;
            PlayerId owner;
            SimVector position;
            bool cloaked;
            bool stealth;
            bool radarEligible;
            bool sonarEligible;
        };

        // per-player scratch vectors are hoisted so capacity is reused
        // across players and ticks instead of re-allocating each pass
        std::vector<ViewerUnit> viewers;
        std::vector<JammerUnit> jammers;
        std::unordered_set<UnitId> seenViewers;

        // detection targets are gathered once per tick: the structural
        // predicates and the two hash lookups (unit + model defs) would
        // otherwise repeat for every player's scan
        std::vector<DetectableUnit> detectables;
        for (const auto& [unitId, unit] : units)
        {
            if (unit.isDead() || unit.carriedBy)
            {
                continue;
            }
            const auto& def = unitDefinitions.at(unit.unitType);
            // radar requires the target's top edge at or above
            // sea level -- fully submerged units are
            // radar-invisible (retail 0x467840); sonar requires
            // the target at or below sea level
            auto modelHeight = unitModelDefinitions.at(def.objectName).height;
            detectables.push_back(DetectableUnit{
                unitId,
                unit.owner,
                unit.position,
                unit.cloaked,
                def.stealth,
                unit.position.y + modelHeight >= seaLevel,
                unit.position.y <= seaLevel});
        }

        // per-viewer column deltas for the LOS disc stamp: the cell
        // center transform is linear, so the XZ delta for column dx is
        // computed once per viewer instead of per (dx, dy) cell
        std::vector<SimScalar> viewerDdx;

        for (Index playerIndex = 0; playerIndex < players.size(); ++playerIndex)
        {
            auto& pv = playerVisibility[playerIndex];
            pv.losVisibleUnits.clear();
            pv.radarDetectedUnits.clear();
            pv.sonarDetectedUnits.clear();
            pv.hasTargetingUpgrade = false;

            // allocate/refresh grids for human players; AI players only
            // need the unit sets for acquisition gating
            const auto& playerInfo = players[playerIndex];
            const bool trackGrids = playerInfo.type == GamePlayerType::Human;
            if (trackGrids)
            {
                // refcounted per-viewer stamps persist across ticks, so
                // there is no per-tick grid clear; a dimension change
                // (first pass, load, map swap) rebuilds them together
                if (pv.losGrid.getWidth() != mapW || pv.losGrid.getHeight() != mapH
                    || pv.losStampCount.getWidth() != mapW || pv.losStampCount.getHeight() != mapH)
                {
                    pv.losGrid = Grid<unsigned char>(mapW, mapH, 0);
                    pv.losStampCount = Grid<unsigned short>(mapW, mapH, 0);
                    pv.losStampCache.clear();
                }
                // exploredGrid carries accumulated history restored by
                // save/load -- losGrid is the one absent from the save
                // (always empty post-load), so its dimension mismatch
                // must not reset the restored history; reallocate
                // exploredGrid only on a genuine map-size change
                if (pv.exploredGrid.getWidth() != mapW || pv.exploredGrid.getHeight() != mapH)
                {
                    pv.exploredGrid = Grid<unsigned char>(mapW, mapH, 0);
                }
            }

            // gather this player's sensing units and every other
            // player's powered jammers (retail 0x467440 loops 2 and 3:
            // emitters and jammers must both be on -- unit+0x10e bit0,
            // the powered-and-active composite)
            viewers.clear();
            jammers.clear();
            PlayerId viewerId(playerIndex);
            for (const auto& [unitId, unit] : units)
            {
                if (unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                const auto& def = unitDefinitions.at(unit.unitType);
                if (unit.isBeingBuilt(def))
                {
                    continue;
                }
                const bool ownedByViewer = unit.isOwnedBy(viewerId);
                if (ownedByViewer || areAllied(unit.owner, viewerId))
                {
                    if (ownedByViewer && def.isTargetingUpgrade && unit.activated && unit.isSufficientlyPowered)
                    {
                        // retail `istargetingupgrade`: an activated,
                        // powered facility arms the player's radar-target
                        // upgrade (TotalA.exe 0x40abf9 -> +0x79)
                        pv.hasTargetingUpgrade = true;
                    }
                    // sight is a passive sense -- it is not gated by
                    // the powered-on state the way radar/sonar are.
                    // Allied players share line of sight/radar/sonar:
                    // retail unions each ally group's sensing units at
                    // game start via the +0x108 alliedWith table
                    // (TotalA.exe 0x46c620/0x46c3d7).
                    SimScalar radarSquared = 0_ss;
                    SimScalar sonarSquared = 0_ss;
                    if (isUnitSensorOn(unit, def))
                    {
                        // retail edge term: radar coverage reaches
                        // radarDistance beyond the emitter's bounding
                        // edge -- +2*unit+0x70 where +0x70 is the mean
                        // footprint half-extent, i.e. +8*(fx+fz) wu
                        auto [fx, fz] = getFootprintXZ(def.movementCollisionInfo);
                        auto radarEdge = def.radarDistance + SimScalar(8 * (fx + fz));
                        radarSquared = radarEdge * radarEdge;
                        sonarSquared = def.sonarDistance * def.sonarDistance;
                    }
                    if (def.sightDistance <= 0_ss && radarSquared <= 0_ss && sonarSquared <= 0_ss)
                    {
                        continue;
                    }
                    viewers.push_back(ViewerUnit{unitId, unit.position, def.sightDistance, def.sightDistance * def.sightDistance, radarSquared, sonarSquared});
                }
                else if (isUnitSensorOn(unit, def) && (def.radarDistanceJam > 0_ss || def.sonarDistanceJam > 0_ss))
                {
                    // enemy jammer coverage is stamped AFTER friendly
                    // sensor coverage and clears the contact bits
                    jammers.push_back(JammerUnit{unit.position, def.radarDistanceJam * def.radarDistanceJam, def.sonarDistanceJam * def.sonarDistanceJam});
                }
            }

            // mark LOS terrain discs and accumulate explored cells.
            // Each viewer's disc is refcounted into losStampCount: a
            // stationary viewer keeps last tick's stamp untouched and
            // only moved or newly-viewing units recompute; units that
            // left the viewer set (dead, captured, carried off) release
            // their cells. Explored cells are marked at stamp time,
            // replacing the old full-map explored |= los pass.
            if (trackGrids)
            {
                auto& losData = pv.losGrid.getVector();
                auto& countData = pv.losStampCount.getVector();
                auto& exploredData = pv.exploredGrid.getVector();
                seenViewers.clear();
                for (const auto& viewer : viewers)
                {
                    if (viewer.sightSquared <= 0_ss)
                    {
                        continue;
                    }
                    seenViewers.insert(viewer.unitId);

                    auto stampIt = pv.losStampCache.find(viewer.unitId);
                    if (stampIt != pv.losStampCache.end()
                        && stampIt->second.position == viewer.position
                        && stampIt->second.sightSquared == viewer.sightSquared)
                    {
                        // disc unchanged since last tick -- the stamp is
                        // still in place, nothing to recompute
                        continue;
                    }
                    if (stampIt != pv.losStampCache.end())
                    {
                        for (auto idx : stampIt->second.cells)
                        {
                            if (--countData[idx] == 0)
                            {
                                losData[idx] = 0;
                            }
                        }
                        stampIt->second.cells.clear();
                    }
                    else
                    {
                        stampIt = pv.losStampCache.emplace(viewer.unitId, PlayerVisibility::LosViewerStamp()).first;
                    }
                    stampIt->second.position = viewer.position;
                    stampIt->second.sightSquared = viewer.sightSquared;
                    auto& cells = stampIt->second.cells;

                    auto center = terrain.worldToHeightmapCoordinate(viewer.position);
                    auto radiusCells = static_cast<int>(simScalarToFloat(viewer.sightDistance) / 16.0f) + 1;

                    // the cell-center transform is linear in the cell
                    // index, so the x-delta for each column is computed
                    // once per viewer (same expression as the per-cell
                    // heightmapIndexToWorldCenter call it replaces)
                    viewerDdx.clear();
                    viewerDdx.reserve(2 * radiusCells + 1);
                    auto cx0 = center.x - radiusCells;
                    for (int dx = 0; dx <= 2 * radiusCells; ++dx)
                    {
                        viewerDdx.push_back(terrain.heightmapIndexToWorldCenter(cx0 + dx, 0).x - viewer.position.x);
                    }

                    auto cy0 = center.y - radiusCells;
                    auto cy1 = center.y + radiusCells;
                    for (int cy = cy0; cy <= cy1; ++cy)
                    {
                        if (cy < 0 || cy >= mapH)
                        {
                            continue;
                        }
                        auto ddz = terrain.heightmapIndexToWorldCenter(0, cy).z - viewer.position.z;
                        auto rowBase = cy * mapW;
                        for (int dx = 0; dx <= 2 * radiusCells; ++dx)
                        {
                            auto cx = cx0 + dx;
                            if (cx < 0 || cx >= mapW)
                            {
                                continue;
                            }
                            auto ddx = viewerDdx[dx];
                            if (ddx * ddx + ddz * ddz <= viewer.sightSquared)
                            {
                                auto idx = rowBase + cx;
                                if (countData[idx]++ == 0)
                                {
                                    losData[idx] = 1;
                                }
                                cells.push_back(idx);
                                if (mappingEnabled)
                                {
                                    exploredData[idx] = 1;
                                }
                            }
                        }
                    }
                }

                // release cells of viewers that left the set this tick
                for (auto it = pv.losStampCache.begin(); it != pv.losStampCache.end();)
                {
                    if (seenViewers.find(it->first) == seenViewers.end())
                    {
                        for (auto idx : it->second.cells)
                        {
                            if (--countData[idx] == 0)
                            {
                                losData[idx] = 0;
                            }
                        }
                        it = pv.losStampCache.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            // detect enemy units: LOS wins over radar/sonar, and enemy
            // jammer coverage clears radar/sonar contacts but never
            // LOS (retail ordering: coverage -> jam -> LOS re-marks)
            for (const auto& target : detectables)
            {
                // allied units are shared senses, never contacts
                if (target.owner == viewerId || areAllied(target.owner, viewerId))
                {
                    continue;
                }
                bool radarContact = false;
                bool sonarContact = false;
                bool losVisible = false;
                for (const auto& viewer : viewers)
                {
                    // retail measures sensor coverage in the XZ plane
                    // only (0x467840: dx^2 + dz^2)
                    auto ddx = target.position.x - viewer.position.x;
                    auto ddz = target.position.z - viewer.position.z;
                    auto distSquared = ddx * ddx + ddz * ddz;
                    // effective cloak removes the unit from the LOS set
                    // but not from radar (retail 0x465ac0 gates only the
                    // sight predicate; cloak is not Stealth)
                    if (!target.cloaked && distSquared <= viewer.sightSquared)
                    {
                        losVisible = true;
                        break;
                    }
                    // FBI stealth (+0x241 bit8) skips the shared
                    // radar/sonar coverage callback entirely
                    if (target.stealth)
                    {
                        continue;
                    }
                    if (target.radarEligible && distSquared <= viewer.radarSquared)
                    {
                        radarContact = true;
                    }
                    if (target.sonarEligible && distSquared <= viewer.sonarSquared)
                    {
                        sonarContact = true;
                    }
                }
                if (!losVisible)
                {
                    for (const auto& jammer : jammers)
                    {
                        auto ddx = target.position.x - jammer.position.x;
                        auto ddz = target.position.z - jammer.position.z;
                        auto distSquared = ddx * ddx + ddz * ddz;
                        if (distSquared <= jammer.radarJamSquared)
                        {
                            radarContact = false;
                        }
                        if (distSquared <= jammer.sonarJamSquared)
                        {
                            sonarContact = false;
                        }
                    }
                    if (radarContact)
                    {
                        pv.radarDetectedUnits.insert(target.unitId);
                    }
                    if (sonarContact)
                    {
                        pv.sonarDetectedUnits.insert(target.unitId);
                    }
                }
                else
                {
                    pv.losVisibleUnits.insert(target.unitId);
                }
            }
        }
    }

    bool GameSimulation::isLosVisibleToPlayer(UnitId unitId, PlayerId viewer) const
    {
        const auto& unit = getUnitState(unitId);
        if (unit.isOwnedBy(viewer) || areAllied(unit.owner, viewer))
        {
            return true;
        }
        // retail 0x465ac0: owner -> visible, effectively cloaked ->
        // invisible, else the LOS grid -- cloak is checked before the
        // sight test, so a cloaked enemy is hidden even with LOS off
        if (unit.cloaked)
        {
            return false;
        }
        if (!lineOfSightEnabled)
        {
            return true;
        }
        if (viewer.value >= playerVisibility.size())
        {
            return false;
        }
        return playerVisibility[viewer.value].losVisibleUnits.contains(unitId);
    }

    bool GameSimulation::isRadarDetectedByPlayer(UnitId unitId, PlayerId viewer) const
    {
        if (!lineOfSightEnabled || viewer.value >= playerVisibility.size())
        {
            return false;
        }
        const auto& unit = getUnitState(unitId);
        if (unit.isOwnedBy(viewer))
        {
            return false;
        }
        return playerVisibility[viewer.value].radarDetectedUnits.contains(unitId);
    }

    bool GameSimulation::isSonarDetectedByPlayer(UnitId unitId, PlayerId viewer) const
    {
        if (!lineOfSightEnabled || viewer.value >= playerVisibility.size())
        {
            return false;
        }
        const auto& unit = getUnitState(unitId);
        if (unit.isOwnedBy(viewer))
        {
            return false;
        }
        return playerVisibility[viewer.value].sonarDetectedUnits.contains(unitId);
    }

    bool GameSimulation::isDetectedByPlayer(UnitId unitId, PlayerId viewer) const
    {
        return isLosVisibleToPlayer(unitId, viewer)
            || isRadarDetectedByPlayer(unitId, viewer)
            || isSonarDetectedByPlayer(unitId, viewer);
    }

    bool GameSimulation::isUnitSensorOn(const UnitState& unit, const UnitDefinition& def)
    {
        // retail unit+0x10e bit0 is the powered-and-active composite;
        // its force-clear path only applies to onoffable units, so
        // non-onoffable emitters/jammers are always on
        if (!def.onOffable)
        {
            return true;
        }
        return unit.activated && unit.isSufficientlyPowered;
    }

    const std::vector<UnitId>& GameSimulation::getWeaponTargetCandidates(PlayerId viewer)
    {
        if (weaponTargetCandidatesComputedAt != gameTime)
        {
            weaponTargetCandidatesComputedAt = gameTime;
            weaponTargetCandidatesByPlayer.clear();
            weaponTargetCandidatesByPlayer.resize(players.size());
            for (const auto& [unitId, unit] : units)
            {
                // build-time exclusions are limited to predicates that
                // cannot flip mid-tick (dead stays dead, immunity is
                // never cleared); cargo stays so a mid-tick transport
                // drop still reaches the scan's own re-checks
                if (unit.isDead()
                    || (unit.missionState && unit.missionState->immunity))
                {
                    continue;
                }
                for (Index p = 0; p < players.size(); ++p)
                {
                    PlayerId player(p);
                    if (player != unit.owner && !areAllied(unit.owner, player))
                    {
                        weaponTargetCandidatesByPlayer[p].push_back(unitId);
                    }
                }
            }
        }
        if (viewer.value >= weaponTargetCandidatesByPlayer.size())
        {
            static const std::vector<UnitId> empty;
            return empty;
        }
        return weaponTargetCandidatesByPlayer[viewer.value];
    }

    void GameSimulation::invalidateWeaponTargetCandidates()
    {
        weaponTargetCandidatesComputedAt = std::nullopt;
    }

    bool GameSimulation::isWeaponTargetableByPlayer(UnitId unitId, PlayerId viewer) const
    {
        if (isLosVisibleToPlayer(unitId, viewer))
        {
            return true;
        }
        if (!lineOfSightEnabled || viewer.value >= playerVisibility.size())
        {
            return false;
        }
        // retail: an activated targeting facility admits radar-only
        // contacts into the weapon-target candidate list
        return playerVisibility[viewer.value].hasTargetingUpgrade
            && isRadarDetectedByPlayer(unitId, viewer);
    }

    bool isUnitMovingForCloak(const UnitState& unit)
    {
        // retail selects cloakCostMoving from the in-motion pair
        // (unit+0x110 bits 2-3); actual displacement speed is the
        // deterministic equivalent
        return match(
            unit.physics,
            [&](const UnitPhysicsInfoGround& p) { return p.currentSpeed > 0_ss; },
            [&](const UnitPhysicsInfoAir& p) {
                if (auto flying = std::get_if<AirMovementStateFlying>(&p.movementState))
                {
                    return flying->currentVelocity.lengthSquared() > 0_ss * 0_ss;
                }
                // taking off / landing count as in motion
                return true;
            });
    }

    void GameSimulation::updateCloak()
    {
        // retail's cloak upkeep runs in the per-second bookkeeping
        // pass alongside the sensor sweep (TotalA.exe 0x401360 inside
        // the loop that drives 0x467440)
        if (gameTime % GameTime(SimTicksPerSecond) != GameTime(0))
        {
            return;
        }

        for (auto& [unitId, unit] : units)
        {
            const auto& unitDefinition = unitDefinitions.at(unit.unitType);

            if (!unit.cloakRequested
                || unit.isDead()
                || unit.carriedBy
                || unit.isBeingBuilt(unitDefinition)
                || !(unitDefinition.cloakCost > 0.0f))
            {
                // no request (or the unit cannot cloak right now):
                // retail clears unit+0x10e bit2 without touching the
                // request bit -- cargo and units under construction
                // simply stay decloaked
                unit.cloaked = false;
                continue;
            }

            // proximity suppression: the sensor pass scans the owner's
            // detected non-allied units inside mincloakdistance and
            // pushes the blocked-until tick 90 out (retail 0x4676b6).
            // Cloaked enemies are absent from the visible set and
            // carried cargo never triggers a decloak.
            if (unitDefinition.minCloakDistance > 0_ss)
            {
                const auto radiusSquared = unitDefinition.minCloakDistance * unitDefinition.minCloakDistance;
                for (const auto& [otherId, other] : units)
                {
                    if (otherId == unitId
                        || other.isOwnedBy(unit.owner)
                        || areAllied(other.owner, unit.owner)
                        || other.isDead()
                        || other.carriedBy)
                    {
                        continue;
                    }
                    if (!isLosVisibleToPlayer(otherId, unit.owner))
                    {
                        continue;
                    }
                    if (unit.position.distanceSquared(other.position) <= radiusSquared)
                    {
                        unit.cloakBlockedUntil = gameTime + GameTime(90);
                        break;
                    }
                }
            }

            if (gameTime < unit.cloakBlockedUntil)
            {
                // still inside the decloak dwell
                unit.cloaked = false;
                continue;
            }

            // retail truncates the float cost to an integer before
            // comparing it against the player's energy pool (0x4e43a0)
            const auto cost = isUnitMovingForCloak(unit)
                ? unitDefinition.cloakCostMoving
                : unitDefinition.cloakCost;
            const auto intCost = Energy(static_cast<float>(static_cast<unsigned int>(cost)));

            auto& player = getPlayer(unit.owner);
            if (player.energy >= intCost)
            {
                player.energy -= intCost;
                unit.cloaked = true;
            }
            else
            {
                // energy shortfall drops effective cloak; the request
                // persists so the unit recloaks once energy recovers
                unit.cloaked = false;
            }
        }
    }

    void GameSimulation::moveObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar position, SimScalar speed)
    {
        getUnitState(unitId).moveObject(name, axis, position, speed);
    }

    void GameSimulation::moveObjectNow(UnitId unitId, const std::string& name, SimAxis axis, SimScalar position)
    {
        getUnitState(unitId).moveObjectNow(name, axis, position);
    }

    void GameSimulation::turnObject(UnitId unitId, const std::string& name, SimAxis axis, SimAngle angle, SimScalar speed)
    {
        getUnitState(unitId).turnObject(name, axis, angle, speed);
    }

    void GameSimulation::turnObjectNow(UnitId unitId, const std::string& name, SimAxis axis, SimAngle angle)
    {
        getUnitState(unitId).turnObjectNow(name, axis, angle);
    }

    void GameSimulation::spinObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar speed, SimScalar acceleration)
    {
        getUnitState(unitId).spinObject(name, axis, speed, acceleration);
    }

    void GameSimulation::stopSpinObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar deceleration)
    {
        getUnitState(unitId).stopSpinObject(name, axis, deceleration);
    }

    bool GameSimulation::isPieceMoving(UnitId unitId, const std::string& name, SimAxis axis) const
    {
        return getUnitState(unitId).isMoveInProgress(name, axis);
    }

    bool GameSimulation::isPieceTurning(UnitId unitId, const std::string& name, SimAxis axis) const
    {
        return getUnitState(unitId).isTurnInProgress(name, axis);
    }

    std::optional<SimVector> GameSimulation::intersectLineWithTerrain(const Line3x<SimScalar>& line) const
    {
        return terrain.intersectLine(line);
    }

    void GameSimulation::moveUnitOccupiedArea(const DiscreteRect& oldRect, const DiscreteRect& newRect, UnitId unitId)
    {
        auto oldRegion = occupiedGrid.tryToRegion(oldRect);
        assert(!!oldRegion);
        auto newRegion = occupiedGrid.tryToRegion(newRect);
        assert(!!newRegion);

        occupiedGrid.forEach(*oldRegion, [unitId](auto& cell) {
            if (cell.mobileUnitId == unitId) { cell.mobileUnitId = std::nullopt; }
        });
        occupiedGrid.forEach(*newRegion, [unitId](auto& cell) { cell.mobileUnitId = unitId; });
    }

    SimVector GameSimulation::computeAttachPosition(const UnitState& transport, int slot) const
    {
        // retail 0x43def0: slot < 0 or >= script piece count -> {0,0,0}
        // offset from the carrier, i.e. it rides at the carrier origin
        const auto* script = transport.cobEnvironment ? transport.cobEnvironment->_script : nullptr;
        if (!script || slot < 0 || static_cast<unsigned int>(slot) >= script->pieces.size())
        {
            return transport.position;
        }
        const auto& transportDefinition = unitDefinitions.at(transport.unitType);
        const auto& modelDef = unitModelDefinitions.at(transportDefinition.objectName);
        auto pieceTransform = getPieceTransform(script->pieces[slot], modelDef, transport.pieces);
        return transport.getTransform() * pieceTransform * SimVector(0_ss, 0_ss, 0_ss);
    }

    bool GameSimulation::tryBoardTransport(UnitId unitId, UnitId transportId, int slot)
    {
        auto unitRef = tryGetUnitState(unitId);
        auto transportRef = tryGetUnitState(transportId);
        if (!unitRef || !transportRef || unitId == transportId)
        {
            return false;
        }

        auto& unit = unitRef->get();
        auto& transport = transportRef->get();
        if (unit.isDead() || transport.isDead())
        {
            return false;
        }

        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        const auto& transportDefinition = unitDefinitions.at(transport.unitType);

        // retail group-join gates: target must be a transport that is not
        // itself carried; the unit must not already be aboard or carrying
        // others. Retail's `i` does not check TransMaxUnits capacity —
        // scripts routinely board more units than the nominal limit.
        if (transportDefinition.transportCapacity == 0
            || transport.carriedBy
            || unit.carriedBy
            || !unit.passengers.empty())
        {
            return false;
        }

        // detach the unit from terrain occupancy
        auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        if (auto footprintRegion = occupiedGrid.tryToRegion(footprintRect))
        {
            if (unitDefinition.isMobile && !isFlying(unit.physics))
            {
                occupiedGrid.forEach(*footprintRegion, [unitId](auto& cell) {
                    if (cell.mobileUnitId == unitId) { cell.mobileUnitId = std::nullopt; }
                });
            }
            else if (!unitDefinition.isMobile)
            {
                occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
                    if (cell.buildingInfo && cell.buildingInfo->unit == unitId)
                    {
                        cell.buildingInfo = std::nullopt;
                    } });
            }
        }

        unit.carriedBy = transportId;
        unit.attachSlot = slot;
        // cargo is inert: it takes no work ticks, so any nanolathe work
        // in flight ends here
        unit.activeNanolatheTarget = std::nullopt;
        // scripted `i` attaches with slot -1, so the member rides at
        // the carrier's position verbatim (retail 0x43def0 returns a
        // zero offset for an invalid slot); interactive loads ride at
        // the queried slot piece's world position
        unit.position = computeAttachPosition(transport, slot);
        transport.passengers.push_front(unitId);
        return true;
    }

    bool GameSimulation::tryDisembarkUnit(UnitId unitId, const SimVector& position)
    {
        auto unitRef = tryGetUnitState(unitId);
        if (!unitRef)
        {
            return false;
        }
        auto& unit = unitRef->get();
        if (!unit.carriedBy)
        {
            return false;
        }
        auto transportId = *unit.carriedBy;

        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        auto footprintRect = computeFootprintRegion(position, unitDefinition.movementCollisionInfo);
        // the carrier's own footprint may legitimately overlap the drop
        // cell -- a VTOL hovers over it and a ground transport stands on
        // the shoreline -- so exclude the carrier from the check
        if (isCollisionAt(footprintRect, transportId))
        {
            return false;
        }

        // unlink from the carrier's passenger list
        if (auto transport = tryGetUnitState(transportId))
        {
            auto& list = transport->get().passengers;
            list.erase(std::remove(list.begin(), list.end(), unitId), list.end());
        }
        unit.carriedBy = std::nullopt;
        unit.attachSlot = -1;

        unit.position = position;
        unit.position.y = terrain.getHeightAt(unit.position.x, unit.position.z);

        // restore terrain occupancy
        if (auto footprintRegion = occupiedGrid.tryToRegion(footprintRect))
        {
            if (unitDefinition.isMobile && !isFlying(unit.physics))
            {
                occupiedGrid.forEach(*footprintRegion, [unitId](auto& cell) { cell.mobileUnitId = unitId; });
            }
            else if (!unitDefinition.isMobile)
            {
                occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
                    cell.buildingInfo = OccupiedCellBuildingInfo{unitId, false}; });
            }
        }

        return true;
    }

    void GameSimulation::requestPath(UnitId unitId)
    {
        PathRequest request{unitId};

        // If the unit is already in the queue for a path,
        // we'll assume that they no longer care about their old request
        // and that their new request is for some new path,
        // so we'll move them to the back of the queue for fairness.
        auto it = std::find(pathRequests.begin(), pathRequests.end(), request);
        if (it != pathRequests.end())
        {
            pathRequests.erase(it);
        }

        pathRequests.push_back(PathRequest{unitId});
    }

    Projectile GameSimulation::createProjectileFromWeapon(
        PlayerId owner, const UnitWeapon& weapon, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit)
    {
        return createProjectileFromWeapon(owner, weapon.weaponType, position, direction, distanceToTarget, targetUnit);
    }

    Projectile GameSimulation::createProjectileFromWeapon(PlayerId owner, const std::string& weaponType, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit, std::optional<SimVector> targetPosition)
    {
        // retail FBIs contain case-mangled names (e.g. lARGE_BUILDINGEX)
        auto upperWeaponType = toUpper(weaponType);
        const auto& weaponDefinition = weaponDefinitions.at(upperWeaponType);

        Projectile projectile;
        projectile.weaponType = upperWeaponType;
        projectile.owner = owner;
        projectile.position = position;
        projectile.previousPosition = position;
        projectile.origin = position;
        // vlaunch shots rise vertically out of their silo first; the
        // guidance pass then steers them onto the target
        projectile.velocity = weaponDefinition.vLaunch
            ? SimVector(0_ss, weaponDefinition.velocity, 0_ss)
            : direction * weaponDefinition.velocity;
        projectile.targetPosition = targetPosition;

        projectile.lastSmoke = gameTime;

        projectile.damage = weaponDefinition.damage;

        projectile.damageRadius = weaponDefinition.damageRadius;

        if (std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinition.physicsType))
        {
            projectile.velocity = SimVector(0_ss, 0_ss, 0_ss);
            projectile.targetPosition = std::nullopt;
        }
        else if (weaponDefinition.cruise && weaponDefinition.flightTime.value > 0)
        {
            // cruise missiles (strategic nukes) bound flight by
            // `flighttime` -- `weapontimer=5` on stock data would kill
            // the shot at 150 ticks, far inside the validated range
            projectile.dieOnFrame = gameTime + weaponDefinition.flightTime;
        }
        else if (weaponDefinition.weaponTimer)
        {
            auto randomDecay = weaponDefinition.randomDecay.value().value;
            std::uniform_int_distribution<unsigned int> dist(0, randomDecay);
            auto randomVal = dist(rng);
            projectile.dieOnFrame = gameTime + *weaponDefinition.weaponTimer - GameTime(randomDecay / 2) + GameTime(randomVal);
        }
        else if (std::holds_alternative<ProjectilePhysicsTypeLineOfSight>(weaponDefinition.physicsType))
        {
            projectile.dieOnFrame = gameTime + GameTime(simScalarToUInt(distanceToTarget / weaponDefinition.velocity) + 1);
        }

        projectile.createdAt = gameTime;
        projectile.groundBounce = weaponDefinition.groundBounce;

        projectile.targetUnit = std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinition.physicsType) ? std::nullopt : targetUnit;

        return projectile;
    }

    void GameSimulation::spawnProjectile(PlayerId owner, const UnitWeapon& weapon, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit, std::optional<UnitId> firedBy, std::optional<ProjectileId> targetProjectile, std::optional<SimVector> targetPosition)
    {
        auto projectile = createProjectileFromWeapon(owner, weapon, position, direction, distanceToTarget, targetUnit);
        projectile.firedBy = firedBy;
        projectile.targetProjectile = targetProjectile;
        projectile.targetPosition = targetPosition;
        if (std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinitions.at(projectile.weaponType).physicsType))
        {
            projectile.targetPosition = std::nullopt;
            projectile.targetProjectile = std::nullopt;
            if (firedBy)
            {
                if (auto source = tryGetUnitState(*firedBy))
                {
                    if (auto* air = std::get_if<UnitPhysicsInfoAir>(&source->get().physics))
                    {
                        if (auto* flying = std::get_if<AirMovementStateFlying>(&air->movementState))
                        {
                            projectile.velocity = SimVector(flying->currentVelocity.x, 0_ss, flying->currentVelocity.z);
                        }
                    }
                }
            }
        }
        projectiles.emplace(std::move(projectile));
    }

    std::optional<ProjectileId> GameSimulation::findInterceptorTarget(const UnitState& unit, const WeaponDefinition& weaponDefinition) const
    {
        for (const auto& [projectileId, projectile] : projectiles)
        {
            if (projectile.isDead || projectile.owner == unit.owner)
            {
                continue;
            }
            if (!weaponDefinitions.at(projectile.weaponType).targetable)
            {
                continue;
            }
            // retail's coverage test is a per-axis box, not a radius
            if (rweAbs(unit.position.x - projectile.position.x) > weaponDefinition.coverage
                || rweAbs(unit.position.z - projectile.position.z) > weaponDefinition.coverage)
            {
                continue;
            }
            // one defender per missile: skip missiles a live
            // interceptor projectile is already tracking
            bool claimed = false;
            for (const auto& [otherId, other] : projectiles)
            {
                if (!other.isDead && other.targetProjectile && *other.targetProjectile == projectileId)
                {
                    claimed = true;
                    break;
                }
            }
            if (claimed)
            {
                continue;
            }
            return projectileId;
        }
        return std::nullopt;
    }

    WinStatus GameSimulation::computeWinStatus() const
    {
        // campaign missions do not use skirmish last-player-standing;
        // mission victory evaluation is a later campaign phase
        if (missionInfo)
        {
            return WinStatusUndecided();
        }



        std::optional<PlayerId> livingPlayer;
        std::set<unsigned int> livingAllianceKeys;
        for (Index i = 0; i < getSize(players); ++i)
        {
            const auto& p = players[i];

            if (p.status == GamePlayerStatus::Alive)
            {
                // retail's win tally counts surviving *alliances*:
                // players in the same ally group (+0x13f) win and lose
                // together. Ungrouped players each form their own
                // alliance -- 0x100+i can never collide with a real
                // group value (0-4).
                livingAllianceKeys.insert(p.allyGroup ? *p.allyGroup : 0x100u + i);
                if (!livingPlayer)
                {
                    livingPlayer = PlayerId(i);
                }
            }
        }

        if (livingAllianceKeys.size() > 1)
        {
            // multiple alliances still alive, the game is not over
            return WinStatusUndecided();
        }

        if (livingPlayer)
        {
            // one alliance is alive, declare a member the winner
            return WinStatusWon{*livingPlayer};
        }

        // no players are alive, the game is a draw
        return WinStatusDraw();
    }

    bool GameSimulation::areAllied(PlayerId a, PlayerId b) const
    {
        if (a == b)
        {
            return true;
        }
        if (a.value >= players.size() || b.value >= players.size())
        {
            return false;
        }
        const auto& ga = players[a.value].allyGroup;
        const auto& gb = players[b.value].allyGroup;
        return ga && gb && *ga == *gb;
    }

    void GameSimulation::initMissionObjectives()
    {
        if (!missionInfo)
        {
            return;
        }
        missionObjectives.emplace(missionInfo->objectives, missionInfo->localPlayerId);
    }

    MissionResult GameSimulation::getMissionResult() const
    {
        return missionObjectives ? missionObjectives->result() : MissionResult::InProgress;
    }

    bool GameSimulation::addResourceDelta(const UnitId& unitId, const Energy& energy, const Metal& metal)
    {
        return addResourceDelta(unitId, energy, metal, energy, metal);
    }

    bool GameSimulation::addResourceDelta(const UnitId& unitId, const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal)
    {
        auto& unit = getUnitState(unitId);
        auto& player = getPlayer(unit.owner);

        unit.addEnergyDelta(apparentEnergy);
        unit.addMetalDelta(apparentMetal);
        return player.addResourceDelta(apparentEnergy, apparentMetal, actualEnergy, actualMetal);
    }

    bool GameSimulation::addScaledResourceDelta(const UnitId& unitId, const Energy& energy, const Metal& metal)
    {
        return addScaledResourceDelta(unitId, energy, metal, energy, metal);
    }

    bool GameSimulation::addScaledResourceDelta(const UnitId& unitId, const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal)
    {
        // retail scales the contribution into the unit's production
        // accumulators only for computer-owned units
        // (TotalA.exe owner type+0x73==2)
        const auto scale = getPlayer(getUnitState(unitId).owner).type == GamePlayerType::Computer
            ? computerResourceScale
            : 1.0f;
        return addResourceDelta(
            unitId,
            Energy(apparentEnergy.value * scale),
            Metal(apparentMetal.value * scale),
            Energy(actualEnergy.value * scale),
            Metal(actualMetal.value * scale));
    }

    bool GameSimulation::trySetYardOpen(const UnitId& unitId, bool open)
    {
        auto& unit = getUnitState(unitId);
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        auto footprintRegion = occupiedGrid.tryToRegion(footprintRect);
        assert(!!footprintRegion);

        assert(!!unitDefinition.yardMap);
        if (isYardmapBlocked(footprintRegion->x, footprintRegion->y, *unitDefinition.yardMap, open, unitId))
        {
            return false;
        }

        occupiedGrid.forEach2(footprintRegion->x, footprintRegion->y, *unitDefinition.yardMap, [&](auto& cell, const auto& yardMapCell) {
            cell.buildingInfo = OccupiedCellBuildingInfo{unitId, isPassable(yardMapCell, open)};
        });

        unit.yardOpen = open;

        return true;
    }

    void GameSimulation::emitBuggerOff(const UnitId& unitId)
    {
        auto& unit = getUnitState(unitId);
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        auto footprintRegion = occupiedGrid.tryToRegion(footprintRect);
        assert(!!footprintRegion);

        occupiedGrid.forEach(*footprintRegion, [&](const auto& e) {
            if (e.mobileUnitId)
            {
                tellToBuggerOff(*e.mobileUnitId, footprintRect);
            }
        });
    }

    void GameSimulation::tellToBuggerOff(const UnitId& unitId, const DiscreteRect& rect)
    {
        auto& unit = getUnitState(unitId);
        if (unit.orders.empty())
        {
            unit.addOrder(BuggerOffOrder(rect));
        }
    }

    GameHash GameSimulation::computeHash() const
    {
        return computeHashOf(*this);
    }

    void GameSimulation::activateUnit(UnitId unitId)
    {
        auto& unit = getUnitState(unitId);
        unit.activate();
        events.push_back(UnitActivatedEvent{unitId});
    }

    void GameSimulation::deactivateUnit(UnitId unitId)
    {
        auto& unit = getUnitState(unitId);
        unit.deactivate();
        events.push_back(UnitDeactivatedEvent{unitId});
    }

    void GameSimulation::quietlyKillUnit(UnitId unitId)
    {
        auto& unit = getUnitState(unitId);
        if (unit.isDead()) { return; }
        unit.markAsDeadNoCorpse();
        releaseAirService(unitId);
        destroyAirServiceDependents(unitId, UnitDeathCause::Normal);
        // still emit the death event so listeners can drop the
        // unit id from selection, hover and gui bookkeeping;
        // otherwise the id goes stale when the unit is swept.
        events.push_back(UnitDiedEvent{unitId, unit.unitType, unit.position, UnitDiedEvent::DeathType::Deleted});
        if (missionObjectives)
        {
            missionObjectives->onUnitDied(*this, unit);
        }
    }

    Matrix4x<SimScalar> GameSimulation::getUnitPieceLocalTransform(UnitId unitId, const std::string& pieceName) const
    {
        const auto& unit = getUnitState(unitId);
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        const auto& modelDef = unitModelDefinitions.at(unitDefinition.objectName);
        return getPieceTransform(pieceName, modelDef, unit.pieces);
    }

    Matrix4x<SimScalar> GameSimulation::getUnitPieceTransform(UnitId unitId, const std::string& pieceName) const
    {
        const auto& unit = getUnitState(unitId);
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        const auto& modelDef = unitModelDefinitions.at(unitDefinition.objectName);
        auto pieceTransform = getPieceTransform(pieceName, modelDef, unit.pieces);
        return unit.getTransform() * pieceTransform;
    }

    SimVector GameSimulation::getUnitPiecePosition(UnitId unitId, const std::string& pieceName) const
    {
        const auto& unit = getUnitState(unitId);
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        const auto& modelDef = unitModelDefinitions.at(unitDefinition.objectName);
        auto pieceTransform = getPieceTransform(pieceName, modelDef, unit.pieces);
        return unit.getTransform() * pieceTransform * SimVector(0_ss, 0_ss, 0_ss);
    }

    void GameSimulation::setBuildStance(UnitId unitId, bool value)
    {
        getUnitState(unitId).inBuildStance = value;
    }

    void GameSimulation::setYardOpen(UnitId unitId, bool value)
    {
        trySetYardOpen(unitId, value);
    }

    void GameSimulation::setBuggerOff(UnitId unitId, bool value)
    {
        // retail SET BUGGER_OFF flips unit+0x10f bit3 (TotalA.exe
        // 0x480bbe); raising it also emits the scatter order to units
        // inside the yard footprint
        getUnitState(unitId).buggerOff = value;
        if (value)
        {
            emitBuggerOff(unitId);
        }
    }

    MovementClassDefinition GameSimulation::getAdHocMovementClass(const UnitDefinition::MovementCollisionInfo& info) const
    {
        return match(
            info,
            [&](const UnitDefinition::AdHocMovementClass& mc) {
                return MovementClassDefinition{
                    "",
                    mc.footprintX,
                    mc.footprintZ,
                    mc.minWaterDepth,
                    mc.maxWaterDepth,
                    mc.maxSlope,
                    mc.maxWaterSlope};
            },
            [&](const UnitDefinition::NamedMovementClass& mc) {
                return movementClassDatabase.getMovementClass(mc.movementClassId);
            });
    }

    std::pair<unsigned int, unsigned int> GameSimulation::getFootprintXZ(const UnitDefinition::MovementCollisionInfo& info) const
    {
        return match(
            info,
            [&](const UnitDefinition::AdHocMovementClass& mc) {
                return std::make_pair(mc.footprintX, mc.footprintZ);
            },
            [&](const UnitDefinition::NamedMovementClass& mc) {
                const auto& mcDef = movementClassDatabase.getMovementClass(mc.movementClassId);
                return std::make_pair(mcDef.footprintX, mcDef.footprintZ);
            });
    }

    SimVector rotateTowards(const SimVector& v, const SimVector& target, SimScalar maxAngle)
    {
        auto normV = v.normalizedOr(SimVector(0_ss, 0_ss, 1_ss));
        auto targetDirection = target.normalizedOr(SimVector(0_ss, 0_ss, 1_ss));
        auto cross = normV.cross(targetDirection);
        auto dot = normV.dot(targetDirection);
        // normalized vectors can produce a dot product slightly outside
        // [-1, 1] due to floating-point error, and acos is NaN there
        auto angle = rweMin(rweAcos(rweMin(rweMax(dot, SimScalar(-1)), SimScalar(1))), angularToRadians(maxAngle));

        return Matrix4x<SimScalar>::rotationAxisAngle(cross, angle) * v;
    }

    std::optional<std::variant<UnitId, FeatureId>> projectileCollides(const GameSimulation& sim, const Projectile& projectile, const OccupiedCell& cellValue)
    {
        if (cellValue.mobileUnitId)
        {
            const auto& unit = sim.getUnitState(*cellValue.mobileUnitId);

            if (unit.isOwnedBy(projectile.owner))
            {
                return std::nullopt;
            }

            const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
            const auto& modelDefinition = sim.unitModelDefinitions.at(unitDefinition.objectName);

            // ignore if the projectile is above or below the unit
            if (projectile.position.y < unit.position.y || projectile.position.y > unit.position.y + modelDefinition.height)
            {
                return std::nullopt;
            }

            return *cellValue.mobileUnitId;
        }

        if (cellValue.buildingInfo && !cellValue.buildingInfo->passable)
        {
            const auto& unit = sim.getUnitState(cellValue.buildingInfo->unit);

            if (unit.isOwnedBy(projectile.owner))
            {
                return std::nullopt;
            }

            const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);
            const auto& modelDefinition = sim.unitModelDefinitions.at(unitDefinition.objectName);

            // ignore if the projectile is above or below the unit
            if (projectile.position.y < unit.position.y || projectile.position.y > unit.position.y + modelDefinition.height)
            {
                return std::nullopt;
            }

            return cellValue.buildingInfo->unit;
        }

        if (cellValue.featureId && !sim.weaponDefinitions.at(projectile.weaponType).unitsOnly)
        {
            const auto& feature = sim.getFeature(*cellValue.featureId);
            const auto& featureDefinition = sim.getFeatureDefinition(feature.featureName);

            // ignore if the projectile is above or below the feature
            if (projectile.position.y < feature.position.y || projectile.position.y > feature.position.y + featureDefinition.height)
            {
                return std::nullopt;
            }

            return *cellValue.featureId;
        }


        return std::nullopt;
    }

    bool projectileCollidesWithUnit(const GameSimulation& sim, const Projectile& projectile, UnitId unitId)
    {
        const auto& unit = sim.getUnitState(unitId);

        if (unit.isOwnedBy(projectile.owner))
        {
            return false;
        }

        const auto& unitDefinition = sim.unitDefinitions.at(unit.unitType);

        auto footprintRect = sim.computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        auto heightMapPos = sim.terrain.worldToHeightmapCoordinate(projectile.position);

        if (!footprintRect.contains(heightMapPos))
        {
            return false;
        }

        const auto& modelDefinition = sim.unitModelDefinitions.at(unitDefinition.objectName);

        // ignore if the projectile is above or below the unit
        if (projectile.position.y < unit.position.y || projectile.position.y > unit.position.y + modelDefinition.height)
        {
            return false;
        }

        return true;
    }

    struct ProjectileCollisionInfoTerrain
    {
    };
    struct ProjectileCollisionInfoOutOfBounds
    {
    };
    struct ProjectileCollisionInfoSea
    {
    };
    struct ProjectileCollisionInfoUnitOrFeatureOrBuilding
    {
        /** The entity the projectile directly struck. */
        std::variant<UnitId, FeatureId> hit;
    };
    using ProjectileCollisionInfo = std::variant<
        ProjectileCollisionInfoOutOfBounds,
        ProjectileCollisionInfoTerrain,
        ProjectileCollisionInfoSea,
        ProjectileCollisionInfoUnitOrFeatureOrBuilding>;

    std::optional<ProjectileCollisionInfo> checkProjectileCollision(const GameSimulation& simulation, const Projectile& projectile)
    {
        // a projectile with a degenerate position is off the map;
        // NaN coordinates would poison any downstream geometry query
        if (!std::isfinite(projectile.position.x.value)
            || !std::isfinite(projectile.position.y.value)
            || !std::isfinite(projectile.position.z.value))
        {
            return ProjectileCollisionInfoOutOfBounds();
        }

        // test collision with terrain
        auto terrainHeight = simulation.terrain.tryGetHeightAt(projectile.position.x, projectile.position.z);
        if (!terrainHeight)
        {
            return ProjectileCollisionInfoOutOfBounds();
        }

        auto seaLevel = simulation.terrain.getSeaLevel();

        // terrain first, matching retail's projectile/terrain-water
        // ordering (TotalA.exe 0x49b090): a projectile under the
        // terrain height hits ground, not water
        if (projectile.position.y <= *terrainHeight)
        {
            return ProjectileCollisionInfoTerrain();
        }
        // test collision with sea: below sea level over a water cell.
        // Retail exempts waterWeapon projectiles entirely (0x49b3a1)
        // and suppresses the destroy when the OTA sets
        // `nosealeveltrigger` (0x49b3c0)
        else if (seaLevel > *terrainHeight && projectile.position.y <= seaLevel)
        {
            const auto& weaponDefinition = simulation.weaponDefinitions.at(projectile.weaponType);
            if (!weaponDefinition.waterWeapon && !simulation.noSeaLevelTrigger)
            {
                return ProjectileCollisionInfoSea();
            }
            // fall through: the projectile survives the surface and
            // may still hit something below it
        }

        {
            // detect collision with something's footprint
            auto heightMapPos = simulation.terrain.worldToHeightmapCoordinate(projectile.position);
            auto cellValue = simulation.occupiedGrid.tryGet(heightMapPos);
            if (cellValue)
            {
                auto collides = projectileCollides(simulation, projectile, cellValue->get());
                if (collides)
                {
                    return ProjectileCollisionInfoUnitOrFeatureOrBuilding{*collides};
                }
            }

            // detect collision with flying unit footprint
            for (auto unitId : simulation.flyingUnitsSet)
            {
                if (projectileCollidesWithUnit(simulation, projectile, unitId))
                {
                    return ProjectileCollisionInfoUnitOrFeatureOrBuilding{unitId};
                }
            }
        }

        return std::nullopt;
    }

    BoundingBox3x<SimScalar> GameSimulation::createBoundingBox(const UnitState& unit) const
    {
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);
        const auto& modelDefinition = unitModelDefinitions.at(unitDefinition.objectName);
        auto footprint = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
        auto min = SimVector(SimScalar(footprint.x), unit.position.y, SimScalar(footprint.y));
        auto max = SimVector(SimScalar(footprint.x + footprint.width), unit.position.y + modelDefinition.height, SimScalar(footprint.y + footprint.height));
        auto worldMin = terrain.heightmapToWorldSpace(min);
        auto worldMax = terrain.heightmapToWorldSpace(max);
        return BoundingBox3x<SimScalar>::fromMinMax(worldMin, worldMax);
    }

    void GameSimulation::recordUnitKilledForStats(const UnitState& unit)
    {
        // ENDMSN debrief accounting: every dead unit is a loss for its
        // owner and a kill for the player whose unit last damaged it
        // (TotalA.exe player record +0xfc/+0xfe).
        if (unit.owner.value < players.size())
        {
            ++players[unit.owner.value].unitsLost;
        }
        if (unit.lastDamagedByPlayer)
        {
            auto attackerOwner = *unit.lastDamagedByPlayer;
            if (attackerOwner != unit.owner && attackerOwner.value < players.size())
            {
                ++players[attackerOwner.value].unitsKilled;
                creditUnitVeterancy(unit);
            }
        }
    }

    void GameSimulation::selfDestructUnit(UnitId unitId)
    {
        applyDamage(unitId, 30000, unitId, true, false, UnitDeathCause::SelfDestruct);
    }

    void GameSimulation::creditUnitVeterancy(const UnitState& unit)
    {
        ScopedPhaseTimer timer(collectTickPhaseTimings ? &tickPhaseTimings.veterancy : nullptr);
        // Callers have latched death. The owner snapshot is compared by owner
        // byte, not alliance: retail credits a different allied player's unit.
        if (!unit.lastDamagedBy || !unit.lastDamagedByPlayer
            || *unit.lastDamagedByPlayer == unit.owner || unit.lastDamagedByPlayer->value >= players.size()
            || unit.isBeingBuilt(unitDefinitions.at(unit.unitType))) { return; }
        if (auto killer = tryGetUnitState(*unit.lastDamagedBy))
        {
            const auto oldTier = veteranTier(killer->get().kills);
            killer->get().kills = static_cast<uint16_t>(killer->get().kills + 1);
            if (collectTickPhaseTimings)
            {
                ++tickPhaseTimings.veterancyAwards;
                tickPhaseTimings.veterancyPromotions += veteranTier(killer->get().kills) > oldTier;
            }
        }
    }

    void GameSimulation::killUnit(UnitId unitId, UnitDeathCause cause)
    {
        auto& unit = getUnitState(unitId);
        if (unit.isDead()) { return; }
        const auto& unitDefinition = unitDefinitions.at(unit.unitType);

        // Latch BEFORE cargo or area damage can re-enter this unit's death path.
        unit.markAsDead();
        if (cause == UnitDeathCause::SelfDestruct)
        {
            unit.lastDamagedBy = std::nullopt;
            unit.lastDamagedByPlayer = std::nullopt;
        }
        recordUnitKilledForStats(unit);
        releaseAirService(unitId);
        destroyAirServiceDependents(unitId, cause);

        // Cargo inherits the carrier death packet through the existing damage path.
        auto passengers = std::move(unit.passengers);
        unit.passengers.clear();
        for (const auto& passengerId : passengers)
        {
            if (auto passenger = tryGetUnitState(passengerId))
            {
                passenger->get().carriedBy = std::nullopt;
                passenger->get().attachSlot = -1;
                // Retail cargo receives a fixed 30000 packet, not weapon
                // amplification. Defender veterancy can leave a high-HP cargo
                // unit alive; ordinary cargo still dies and credits the killer.
                applyDamage(passengerId, 30000, unit.lastDamagedBy, true, false, cause, uint16_t(0));
            }
        }

        // detach from any carrier this unit was aboard
        if (unit.carriedBy)
        {
            if (auto carrier = tryGetUnitState(*unit.carriedBy))
            {
                auto& list = carrier->get().passengers;
                list.erase(std::remove(list.begin(), list.end(), unitId), list.end());
            }
            unit.carriedBy = std::nullopt;
            unit.attachSlot = -1;
        }

        auto explosion = toUpper(cause == UnitDeathCause::SelfDestruct ? unitDefinition.selfDestructAs : unitDefinition.explodeAs);
        if (weaponDefinitions.find(explosion) == weaponDefinitions.end()) { explosion.clear(); }
        auto deathType = unit.position.y < terrain.getSeaLevel() ? UnitDiedEvent::DeathType::WaterExploded : UnitDiedEvent::DeathType::NormalExploded;
        events.push_back(UnitDiedEvent{unitId, unit.unitType, unit.position, deathType, explosion});
        if (missionObjectives)
        {
            missionObjectives->onUnitDied(*this, unit);
        }

        // TODO: spawn debris particles (from Killed script)
        // Retail data gap: several Core Contingency FBIs (CORLEVLR,
        // the seaplanes, the radar jammers) set ExplodeAs=
        // MEDIUM_UNITEX, a name Cavedog never defined in UNITS.TDF.
        // Retail tolerates the missing lookup, so the unit dies
        // without a death explosion instead of aborting the tick.
        if (!explosion.empty() && !unit.isBeingBuilt(unitDefinition))
        {
            // Impact record only: no launch lifetime, velocity division or random
            // projectile decay draw. Retail death blasts carry no shooter.
            const auto& weapon = weaponDefinitions.at(explosion);
            Projectile impact{};
            impact.weaponType = explosion;
            impact.owner = unit.owner;
            impact.position = impact.previousPosition = impact.origin = unit.position;
            impact.damage = weapon.damage;
            impact.damageRadius = weapon.damageRadius;
            deathExplosions.push_back(std::move(impact));
        }
        if (!drainingDeathExplosions)
        {
            drainingDeathExplosions = true;
            while (!deathExplosions.empty())
            {
                auto projectile = std::move(deathExplosions.front());
                deathExplosions.pop_front();
                doProjectileImpact(projectile, projectile.position.y < terrain.getSeaLevel() ? ImpactType::Water : ImpactType::Normal);
            }
            drainingDeathExplosions = false;
        }
    }

    void GameSimulation::applyDamage(UnitId unitId, unsigned int damagePoints, std::optional<UnitId> attacker, bool bypassImmunity, bool paralyzer, UnitDeathCause cause, std::optional<uint16_t> attackerKills)
    {
        auto& unit = getUnitState(unitId);
        if (unit.isDead()) { return; }
        if (unit.carriedBy && cause != UnitDeathCause::SelfDestruct)
        {
            // units aboard a transport cannot be damaged directly;
            // their independent owner self-destruct still applies.
            return;
        }
        if (!bypassImmunity && unit.missionState && unit.missionState->immunity)
        {
            // retail OTA `Immunity=1` is consulted where a unit is
            // *picked* for damage -- target selection and area sweeps
            // (TotalA.exe 0x40ab24: unit word +0x110 bit 15 skips the
            // iterated candidate). The damage routine itself
            // (0x489bb0->0x489ce0) never tests it, so forced damage
            // such as the self-destruct countdown bypasses immunity.
            return;
        }
        unit.lastDamagedAt = gameTime;
        // The lethal packet owns credit. Neutral/environmental damage cannot
        // inherit an earlier weapon or EMP assistant's attribution.
        unit.lastDamagedBy = attacker;
        unit.lastDamagedByPlayer = std::nullopt;
        if (attacker)
        {
            unit.lastDamagedBy = attacker;
            if (auto attackerState = tryGetUnitState(*attacker))
            {
                unit.lastDamagedByPlayer = attackerState->get().owner;
                if (cause != UnitDeathCause::SelfDestruct)
                {
                    damagePoints = veteranAttackDamage(damagePoints, attackerKills.value_or(attackerState->get().kills));
                }
            }
        }
        if (unit.armored && damagePoints < 30000)
        {
            // retail armored rule (TotalA.exe 0x489bc3): while the
            // COB-set armored flag is up, hits below 30000 raw points
            // are scaled by the unitdef DamageModifier (16.16 fixed)
            const auto& armoredDef = unitDefinitions.at(unit.unitType);
            damagePoints = static_cast<unsigned int>(
                (static_cast<unsigned long long>(damagePoints) * armoredDef.damageModifier) >> 16);
        }
        damagePoints = veteranDefenceDamage(damagePoints, unit.kills);
        if (paralyzer)
        {
            // Retail kind 2 is a primary-queue interrupt, never HP damage
            // (0x489deb). All peers simulate every owner in RWE lockstep.
            if (unit.isDead() || unitDefinitions.at(unit.unitType).immuneToParalyzer)
            {
                return;
            }
            const auto credit = static_cast<std::uint16_t>(damagePoints);
            auto* order = unit.orders.empty() ? nullptr : std::get_if<ParalyzeOrder>(&unit.orders.front());
            if (order)
            {
                order->credit += credit;
            }
            else
            {
                unit.orders.emplace_front(ParalyzeOrder{credit, std::nullopt});
            }
            return;
        }
        if (unit.hitPoints <= damagePoints)
        {
            const auto& unitDefinition = unitDefinitions.at(unit.unitType);
            if (unit.isBeingBuilt(unitDefinition))
            {
                // Units that are still under construction
                // die quietly without a corpse.
                // FIXME: units in TA that are not actively receiving build input
                // die with an explosion, even though they leave no corpse.
                recordUnitKilledForStats(unit);
                quietlyKillUnit(unitId);
            }
            else
            {
                killUnit(unitId, cause);
            }
        }
        else
        {
            unit.hitPoints -= damagePoints;
        }
    }

    void GameSimulation::applyDamageInRadius(const SimVector& position, SimScalar radius, const Projectile& projectile)
    {
        if (radius <= 0_ss) { return; }
        const bool paralyzer = weaponDefinitions.at(projectile.weaponType).paralyzer;
        // Retail resolves deaths after the impact sweep. Native deaths are
        // immediate, so snapshot the shooter here: a fifth kill in this blast
        // must not strengthen later recipients of the very same impact.
        std::optional<uint16_t> attackerKills;
        if (projectile.firedBy)
        {
            if (auto shooter = tryGetUnitState(*projectile.firedBy)) { attackerKills = shooter->get().kills; }
        }
        auto minX = position.x - radius;
        auto maxX = position.x + radius;
        auto minZ = position.z - radius;
        auto maxZ = position.z + radius;

        auto minPoint = terrain.worldToHeightmapCoordinate(SimVector(minX, position.y, minZ));
        auto maxPoint = terrain.worldToHeightmapCoordinate(SimVector(maxX, position.y, maxZ));
        auto minCell = occupiedGrid.clampToCoords(minPoint);
        auto maxCell = occupiedGrid.clampToCoords(maxPoint);

        assert(minCell.x <= maxCell.x);
        assert(minCell.y <= maxCell.y);

        auto radiusSquared = radius * radius;

        std::unordered_set<UnitId> seenUnits;

        auto region = GridRegion::fromCoordinates(minCell, maxCell);

        auto damageUnit = [&](UnitId id) {
            // check if the unit was seen/mark as seen
            auto pair = seenUnits.insert(id);
            if (!pair.second) // the unit was already present
            {
                return;
            }

            auto& unit = getUnitState(id);

            // skip dead units
            if (unit.isDead() || unit.carriedBy || (unit.missionState && unit.missionState->immunity))
            {
                return;
            }

            // add in the third dimension component to distance,
            // check if we are still in range
            auto unitDistanceSquared = createBoundingBox(unit).distanceSquared(position);
            if (unitDistanceSquared > radiusSquared)
            {
                return;
            }

            // apply appropriate damage
            auto damageScale = std::clamp(1_ss - (rweSqrt(unitDistanceSquared) / radius), 0_ss, 1_ss);
            auto rawDamage = projectile.getDamage(unit.unitType);
            auto scaledDamage = simScalarToUInt(SimScalar(rawDamage) * damageScale);
            if (!projectile.firedBy && !paralyzer && scaledDamage > 0)
            {
                unit.lastDamagedBy = std::nullopt;
                unit.lastDamagedByPlayer = std::nullopt;
            }
            applyDamage(id, scaledDamage, projectile.firedBy, false, paralyzer, UnitDeathCause::Normal, attackerKills);
        };
        region.forEach([&](const auto& coords) {
            const auto& cell = occupiedGrid.get(coords);
            // Passable mines still receive blast damage, including when a
            // mobile occupies the same tile. Deduplicate multi-cell footprints.
            if (cell.mobileUnitId) { damageUnit(*cell.mobileUnitId); }
            if (cell.buildingInfo) { damageUnit(cell.buildingInfo->unit); }
        });

        // Snapshot anchors before any feature transition. New successor identities
        // are never re-hit by the same blast, even above retail's 64-anchor limit.
        if (!weaponDefinitions.at(projectile.weaponType).unitsOnly)
        {
            std::unordered_set<FeatureId> seenFeatures;
            std::vector<FeatureId> victims;
            region.forEach([&](const auto& coords) {
                auto id = occupiedGrid.get(coords).featureId;
                if (id && seenFeatures.insert(*id).second)
                {
                    auto f = tryGetFeature(*id);
                    if (f && SimScalar(simScalarToUInt((f->get().position - position).length())) < radius)
                    {
                        victims.push_back(*id);
                    }
                }
            });
            for (auto id : victims) { damageFeature(id, weaponDefinitions.at(projectile.weaponType)); }
        }

        // Apply damage to flying units
        for (const auto& flyingUnitId : flyingUnitsSet)
        {
            auto& unit = getUnitState(flyingUnitId);

            // skip units that are dying or dead
            if (!unit.isAlive() || unit.carriedBy || (unit.missionState && unit.missionState->immunity))
            {
                continue;
            }

            // check if the unit is in range
            auto unitDistanceSquared = createBoundingBox(unit).distanceSquared(position);
            if (unitDistanceSquared > radiusSquared)
            {
                continue;
            }

            // apply appropriate damage
            auto damageScale = std::clamp(1_ss - (rweSqrt(unitDistanceSquared) / radius), 0_ss, 1_ss);
            auto rawDamage = projectile.getDamage(unit.unitType);
            auto scaledDamage = simScalarToUInt(SimScalar(rawDamage) * damageScale);
            if (!projectile.firedBy && !paralyzer && scaledDamage > 0)
            {
                unit.lastDamagedBy = std::nullopt;
                unit.lastDamagedByPlayer = std::nullopt;
            }
            applyDamage(flyingUnitId, scaledDamage, projectile.firedBy, false, paralyzer, UnitDeathCause::Normal, attackerKills);
        }
    }

    void GameSimulation::doProjectileImpact(const Projectile& projectile, ImpactType impactType)
    {
        // `noexplode` weapons never run the area-of-effect damage pass
        // (retail gates the AoE grid walk on the weapon flag);
        // a `noexplode` projectile damages only the entity it struck
        if (!weaponDefinitions.at(projectile.weaponType).noExplode)
        {
            applyDamageInRadius(projectile.position, projectile.damageRadius, projectile);
        }
    }

    void GameSimulation::updateProjectiles()
    {
        for (auto& projectileEntry : projectiles)
        {
            const auto& id = projectileEntry.first;
            auto& projectile = projectileEntry.second;
            if (projectile.isDead) { continue; }

            const auto& weaponDefinition = weaponDefinitions.at(projectile.weaponType);

            const bool dropped = std::holds_alternative<ProjectilePhysicsTypeDropped>(weaponDefinition.physicsType);

            // remove if it's time to die
            if (projectile.dieOnFrame && *projectile.dieOnFrame <= gameTime)
            {
                projectile.isDead = true;
                events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position});
                continue;
            }

            match(
                weaponDefinition.physicsType,
                [&](const ProjectilePhysicsTypeBallistic&) {
                    projectile.velocity.y -= 112_ss / (30_ss * 30_ss);
                },
                [&](const ProjectilePhysicsTypeLineOfSight&) {},
                [&](const ProjectilePhysicsTypeDropped&) {},
                [&](const ProjectilePhysicsTypeTracking& t) {
                    if (projectile.targetProjectile)
                    {
                        // interceptor steering toward its claimed missile
                        auto target = projectiles.tryGet(*projectile.targetProjectile);
                        if (!target || target->get().isDead)
                        {
                            projectile.targetProjectile = std::nullopt;
                            return;
                        }
                        auto vectorToTarget = (target->get().position - projectile.position);
                        projectile.velocity = rotateTowards(projectile.velocity, vectorToTarget, t.turnRate);
                        return;
                    }
                    if (projectile.targetUnit)
                    {
                        auto targetUnit = tryGetUnitState(*projectile.targetUnit);
                        if (!targetUnit || targetUnit->get().isDead())
                        {
                            projectile.targetUnit = std::nullopt;
                            return;
                        }
                        auto vectorToTarget = (targetUnit->get().position - projectile.position);
                        projectile.velocity = rotateTowards(projectile.velocity, vectorToTarget, t.turnRate);
                        return;
                    }
                    if (!projectile.targetPosition)
                    {
                        return;
                    }
                    {
                        auto vectorToTarget = (*projectile.targetPosition - projectile.position);
                        projectile.velocity = rotateTowards(projectile.velocity, vectorToTarget, t.turnRate);
                    }
                });

            // TDF `guidance`: steer a ground-target shot onto its aim
            // point even without `tracks` (strategic missiles are
            // lineofsight+guidance); turn at the weapon's turnrate.
            // Retail vlaunch/twophase shots spend their first
            // `weapontimer` frames in an unsteered vertical climb
            // (projectile phase counter at +0x69&0x30, phase expiry
            // timer +0x46 seeded from weapontimer at spawn); steering
            // only starts once phase 0 ends, so the shot pitches over
            // at altitude instead of burying itself at the silo.
            if (!dropped && weaponDefinition.guidance && projectile.targetPosition && !projectile.targetUnit && !projectile.targetProjectile)
            {
                bool climbing = weaponDefinition.vLaunch
                    && weaponDefinition.weaponTimer
                    && gameTime - projectile.createdAt < *weaponDefinition.weaponTimer;
                if (!climbing)
                {
                    auto vectorToTarget = (*projectile.targetPosition - projectile.position);
                    projectile.velocity = rotateTowards(projectile.velocity, vectorToTarget, weaponDefinition.turnRate);
                }
            }

            // TDF `weaponacceleration`: the shot's speed grows each
            // tick in its current direction (strategic missiles
            // accelerate through their cruise)
            if (!dropped && weaponDefinition.acceleration > 0_ss)
            {
                auto speed = projectile.velocity.length();
                if (speed > 0_ss)
                {
                    projectile.velocity = (projectile.velocity / speed) * (speed + weaponDefinition.acceleration);
                }
            }

            projectile.previousPosition = projectile.position;
            projectile.position += projectile.velocity;
            if (dropped)
            {
                // Retail dropped mover integrates position and wind before gravity.
                projectile.position += projectileWind;
                projectile.velocity.y -= droppedProjectileGravity();
            }

            // an interceptor projectile kills its claimed missile on
            // contact; the interception still lands the defender's
            // own area damage, while the missile dies mid-flight
            if (projectile.targetProjectile)
            {
                auto target = projectiles.tryGet(*projectile.targetProjectile);
                if (target && !target->get().isDead
                    && projectile.position.distanceSquared(target->get().position) <= projectile.damageRadius * projectile.damageRadius)
                {
                    auto& targetRef = target->get();
                    targetRef.isDead = true;
                    events.push_back(ProjectileDiedEvent{*projectile.targetProjectile, targetRef.weaponType, targetRef.position, ProjectileDiedEvent::DeathType::EndOfLife});
                    doProjectileImpact(projectile, ImpactType::Normal);
                    projectile.isDead = true;
                    events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position, ProjectileDiedEvent::DeathType::NormalImpact});
                    continue;
                }
            }

            auto collisionInfo = checkProjectileCollision(*this, projectile);
            if (collisionInfo)
            {
                match(
                    *collisionInfo,
                    [&](const ProjectileCollisionInfoOutOfBounds&) {
                        // silently remove projectiles that go outside the map
                        projectile.isDead = true;
                        events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position, ProjectileDiedEvent::DeathType::OutOfBounds});
                    },
                    [&](const ProjectileCollisionInfoSea&) {
                        doProjectileImpact(projectile, ImpactType::Water);
                        projectile.isDead = true;
                        events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position, ProjectileDiedEvent::DeathType::WaterImpact});
                    },
                    [&](const ProjectileCollisionInfoTerrain&) {
                        if (projectile.groundBounce)
                        {
                            projectile.velocity.y = 0_ss;
                            projectile.position.y = projectile.previousPosition.y;
                        }
                        else
                        {
                            doProjectileImpact(projectile, ImpactType::Normal);
                            projectile.isDead = true;
                            events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position, ProjectileDiedEvent::DeathType::NormalImpact});
                        }
                    },
                    [&](const ProjectileCollisionInfoUnitOrFeatureOrBuilding& collision) {
                        if (weaponDefinition.noExplode)
                        {
                            // `noexplode` weapons skip the AoE pass;
                            // only the struck entity takes the hit
                            if (auto hitUnit = std::get_if<UnitId>(&collision.hit))
                            {
                                const auto& target = getUnitState(*hitUnit);
                                applyDamage(*hitUnit, projectile.getDamage(target.unitType), projectile.firedBy, false, weaponDefinition.paralyzer);
                            }
                            else if (auto hitFeature = std::get_if<FeatureId>(&collision.hit))
                            {
                                damageFeature(*hitFeature, weaponDefinition);
                            }
                        }
                        else
                        {
                            doProjectileImpact(projectile, ImpactType::Normal);
                        }
                        projectile.isDead = true;
                        events.push_back(ProjectileDiedEvent{id, projectile.weaponType, projectile.position, ProjectileDiedEvent::DeathType::NormalImpact});
                    });
            }
        }
    }

    void GameSimulation::killPlayer(PlayerId playerId)
    {
        getPlayer(playerId).status = GamePlayerStatus::Dead;
        for (auto& p : units)
        {
            auto& unit = p.second;
            if (unit.isDead())
            {
                continue;
            }

            if (!unit.isOwnedBy(playerId))
            {
                continue;
            }

            killUnit(p.first);
        }
    }

    void GameSimulation::processVictoryCondition()
    {
        // campaign missions have their own victory semantics;
        // the skirmish commander-death rule does not apply
        if (missionInfo)
        {
            return;
        }

        if (commanderDeathContinues)
        {
            // CommanderDeath=Continues: losing the commander does not
            // eliminate the player. A player is defeated only when
            // their last live unit is gone (retail all-units-killed
            // elimination, the same check DestroyAllUnits/AllUnitsKilled
            // map objectives are built on).
            for (Index i = 0; i < getSize(players); ++i)
            {
                if (players[i].status != GamePlayerStatus::Alive)
                {
                    continue;
                }
                bool hasLiveUnit = false;
                for (const auto& p : units)
                {
                    if (p.second.isOwnedBy(PlayerId(i)) && !p.second.isDead())
                    {
                        hasLiveUnit = true;
                        break;
                    }
                }
                if (!hasLiveUnit)
                {
                    killPlayer(PlayerId(i));
                }
            }
            return;
        }

        // CommanderDeath=Game ends: if a commander died this frame,
        // kill the player that owns it
        for (const auto& p : units)
        {
            const auto& unitDefinition = unitDefinitions.at(p.second.unitType);
            if (unitDefinition.commander && p.second.isDead())
            {
                killPlayer(p.second.owner);
            }
        }
    }

    SimScalar GameSimulation::droppedProjectileGravity() const
    {
        // Modern TNT fallback is 8155/65536; OTA uses 30 Hz squared.
        const double raw = mapGravity < 0 ? 8155.0 : std::trunc(static_cast<double>(mapGravity) * 65536.0 / 900.0);
        return SimScalar(static_cast<float>(raw / 65536.0));
    }

    void GameSimulation::updateWind()
    {
        if (gameTime >= nextWindSpeedChange)
        {
            // the wind speed will last between 5 and 14 seconds before changing
            std::uniform_int_distribution<int> durationDist(5, 14);
            nextWindSpeedChange = gameTime + GameTime(durationDist(rng) * SimTicksPerSecond);

            // the new wind speed is taken from a uniform distribution between the min and max speeds
            std::uniform_int_distribution<int> speedDist(minWindSpeed, maxWindSpeed);
            auto currentWindSpeed = speedDist(rng);

            // the new wind direction is a random angle
            std::uniform_int_distribution<int> directionDist(MinAngle.value, MaxAngle.value);
            auto currentWindDirection = SimAngle(directionDist(rng));

            currentWindGenerationFactor = SimScalar(currentWindSpeed) / SimScalar(MaxUtilizableWindSpeed);
            projectileWind = UnitState::toDirection(currentWindDirection) * SimScalar(static_cast<float>(currentWindSpeed) * 2.0f / 65536.0f);

            UnitBehaviorService(this).updateWind(currentWindGenerationFactor, currentWindDirection);
        }
    }

    void GameSimulation::updateCorrosiveWater()
    {
        // retail unit-tick path (TotalA.exe 0x48aed3..0x48af32): when
        // `waterdoesdamage` is armed and `waterdamage` is nonzero,
        // every 30th tick each live unit standing at or below sea
        // level takes the damage (damage type 0xb, no attacker);
        // hovercraft are exempt (unitdef +0x241 bit 12)
        if (!waterDoesDamage || waterDamage == 0)
        {
            return;
        }
        if (gameTime % GameTime(30) != GameTime(0))
        {
            return;
        }

        auto seaLevel = terrain.getSeaLevel();
        for (auto& entry : units)
        {
            auto& unit = entry.second;
            // retail tests the unit's effective footprint height
            // (terrain height averaged over the footprint,
            // unit+0x70), not its altitude: a unit standing on the
            // seabed or flying over water is eligible, a unit on
            // dry land is not
            if (!unit.isAlive() || unit.carriedBy
                || terrain.getHeightAt(unit.position.x, unit.position.z) > seaLevel)
            {
                continue;
            }
            if (unitDefinitions.at(unit.unitType).canHover)
            {
                continue;
            }
            applyDamage(entry.first, static_cast<unsigned int>(waterDamage));
        }
    }

    void GameSimulation::updateResources()
    {
        // run resource updates once per second
        if (gameTime % GameTime(SimTicksPerSecond) == GameTime(0))
        {
            // recalculate max energy and metal storage
            for (auto& player : players)
            {
                player.maxEnergy = Energy(0);
                player.maxMetal = Metal(0);
            }

            for (auto& entry : units)
            {
                auto& unit = entry.second;
                const auto& unitDefinition = unitDefinitions.at(unit.unitType);
                if (!unit.carriedBy && !unit.isBeingBuilt(unitDefinition))
                {
                    auto& playerInfo = getPlayer(unit.owner);
                    if (unitDefinition.commander)
                    {
                        playerInfo.maxMetal += playerInfo.startingMetal;
                        playerInfo.maxEnergy += playerInfo.startingEnergy;
                    }
                    else
                    {
                        playerInfo.maxMetal += unitDefinition.metalStorage;
                        playerInfo.maxEnergy += unitDefinition.energyStorage;
                    }
                }
            }

            for (auto& player : players)
            {
                // Debrief stats: "Produced" is the gross production this
                // tick; "Excess" is production discarded by the storage
                // clamp below (TotalA.exe player record +0xac/+0xb4/+0xcc/+0xd4).
                player.metalProduced += player.metalProductionBuffer.value;
                player.energyProduced += player.energyProductionBuffer.value;

                player.metal += player.metalProductionBuffer;
                player.metalProductionBuffer = Metal(0);
                player.energy += player.energyProductionBuffer;
                player.energyProductionBuffer = Energy(0);

                if (player.metal > Metal(0))
                {
                    player.metal -= player.actualMetalConsumptionBuffer;
                    player.actualMetalConsumptionBuffer = Metal(0);
                    player.metalStalled = false;
                }
                else
                {
                    player.metalStalled = true;
                }

                player.previousDesiredMetalConsumptionBuffer = player.desiredMetalConsumptionBuffer;
                player.desiredMetalConsumptionBuffer = Metal(0);

                if (player.energy > Energy(0))
                {
                    player.energy -= player.actualEnergyConsumptionBuffer;
                    player.actualEnergyConsumptionBuffer = Energy(0);
                    player.energyStalled = false;
                }
                else
                {
                    player.energyStalled = true;
                }

                player.previousDesiredEnergyConsumptionBuffer = player.desiredEnergyConsumptionBuffer;
                player.desiredEnergyConsumptionBuffer = Energy(0);

                if (player.metal > player.maxMetal)
                {
                    player.metalExcess += (player.metal - player.maxMetal).value;
                    player.metal = player.maxMetal;
                }

                if (player.energy > player.maxEnergy)
                {
                    player.energyExcess += (player.energy - player.maxEnergy).value;
                    player.energy = player.maxEnergy;
                }
            }

            for (auto& entry : units)
            {
                const auto& unitId = entry.first;
                auto& unit = entry.second;
                const auto& unitDefinition = unitDefinitions.at(unit.unitType);

                unit.resetResourceBuffers();

                if (unit.carriedBy)
                {
                    // cargo aboard a transport produces and consumes nothing
                    continue;
                }

                if (!unit.isBeingBuilt(unitDefinition))
                {
                    addScaledResourceDelta(unitId, unitDefinition.energyMake, unitDefinition.metalMake);
                }

                if (unit.activated)
                {
                    if (unitDefinition.windGenerator != Energy(0))
                    {
                        // generate energy from wind
                        addScaledResourceDelta(unitId, unitDefinition.windGenerator * currentWindGenerationFactor, Metal(0));
                    }

                    if (unitDefinition.tidalGenerator)
                    {
                        // tidal generators produce the map's tidal strength
                        addScaledResourceDelta(unitId, Energy(tidalStrength), Metal(0));
                    }

                    if (unit.isSufficientlyPowered)
                    {
                        // extract metal
                        if (unitDefinition.extractsMetal != Metal(0))
                        {
                            auto footprint = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
                            auto metalValue = metalGrid.accumulate(metalGrid.clipRegion(footprint), 0u, std::plus<>());
                            addScaledResourceDelta(unitId, Energy(0), Metal(metalValue * unitDefinition.extractsMetal.value));
                        }

                        // make metal
                        if (unitDefinition.makesMetal != Metal(0))
                        {
                            addScaledResourceDelta(unitId, Energy(0), unitDefinition.makesMetal);
                        }
                    }

                    // a negative use value is income: retail routes it
                    // through the scaled production accumulator while
                    // positive use costs are unscaled consumption
                    // (TotalA.exe 0x4013de/0x401429 vs 0x4013eb)
                    const auto poweredIncome = addScaledResourceDelta(
                        unitId,
                        Energy(std::max(-unitDefinition.energyUse.value, 0.0f)),
                        Metal(std::max(-unitDefinition.metalUse.value, 0.0f)));
                    const auto poweredCost = addResourceDelta(
                        unitId,
                        Energy(std::min(-unitDefinition.energyUse.value, 0.0f)),
                        Metal(std::min(-unitDefinition.metalUse.value, 0.0f)));
                    unit.isSufficientlyPowered = poweredIncome && poweredCost;
                }
            }
        }
    }

    struct CorpseSpawnInfo
    {
        std::string featureName;
        SimVector position;
        SimAngle rotation;
    };

    void GameSimulation::trySpawnFeature(const std::string& featureType, const SimVector& position, SimAngle rotation)
    {
        auto featureId = tryGetFeatureDefinitionId(featureType).value();
        auto feature = MapFeature{featureId, position, rotation};

        addFeature(std::move(feature));
    }

    void GameSimulation::deleteDeadUnits()
    {
        std::vector<CorpseSpawnInfo> corpsesToSpawn;

        for (auto it = units.begin(); it != units.end();)
        {
            const auto& unit = it->second;
            const auto& unitDefinition = unitDefinitions.at(unit.unitType);
            auto deadState = std::get_if<UnitState::LifeStateDead>(&unit.lifeState);
            if (deadState == nullptr)
            {
                ++it;
                continue;
            }

            if (deadState->leaveCorpse && !unitDefinition.corpse.empty())
            {
                corpsesToSpawn.push_back(CorpseSpawnInfo{
                    unitDefinition.corpse,
                    unit.position,
                    unit.rotation});
            }

            auto footprintRect = computeFootprintRegion(unit.position, unitDefinition.movementCollisionInfo);
            auto footprintRegion = occupiedGrid.tryToRegion(footprintRect);
            if (!footprintRegion)
            {
                // a unit that died with its footprint overhanging the
                // map edge has no fully-contained region; clip to the
                // on-grid cells it actually occupied
                int x0 = std::max(0, footprintRect.x);
                int y0 = std::max(0, footprintRect.y);
                int x1 = std::min(occupiedGrid.getWidth(), footprintRect.x + footprintRect.width);
                int y1 = std::min(occupiedGrid.getHeight(), footprintRect.y + footprintRect.height);
                if (x1 > x0 && y1 > y0)
                {
                    footprintRegion = GridRegion(x0, y0, x1 - x0, y1 - y0);
                }
            }
            if (unitDefinition.isMobile)
            {
                if (isFlying(unit.physics))
                {
                    flyingUnitsSet.erase(it->first);
                }
                else if (footprintRegion)
                {
                    occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
                        if (cell.mobileUnitId == it->first) { cell.mobileUnitId = std::nullopt; }
                    });
                }
            }
            else if (footprintRegion)
            {
                occupiedGrid.forEach(*footprintRegion, [&](auto& cell) {
                  if (cell.buildingInfo && cell.buildingInfo->unit == it->first)
                  {
                      cell.buildingInfo = std::nullopt;
                  } });
            }

            it = units.erase(it);
        }

        for (const auto& spawnInfo : corpsesToSpawn)
        {
            trySpawnFeature(spawnInfo.featureName, spawnInfo.position, spawnInfo.rotation);
        }
    }

    void GameSimulation::deleteDeadProjectiles()
    {
        for (auto it = projectiles.begin(); it != projectiles.end();)
        {
            const auto& projectile = it->second;
            if (projectile.isDead)
            {
                it = projectiles.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void GameSimulation::spawnNewUnits()
    {
        for (const auto& unitId : unitCreationRequests)
        {
            auto unit = tryGetUnitState(unitId);
            if (!unit)
            {
                continue;
            }

            if (auto s = std::get_if<UnitBehaviorStateCreatingUnit>(&unit->get().behaviourState); s != nullptr)
            {

                if (!std::holds_alternative<UnitCreationStatusPending>(s->status))
                {
                    continue;
                }

                auto newUnitId = trySpawnUnit(s->unitType, s->owner, s->position, std::nullopt);
                if (!newUnitId)
                {
                    s->status = UnitCreationStatusFailed();
                    continue;
                }

                events.push_back(UnitStartedBuildingEvent{unitId});

                s->status = UnitCreationStatusDone{*newUnitId};
            }

            if (auto s = std::get_if<FactoryBehaviorStateCreatingUnit>(&unit->get().factoryState); s != nullptr)
            {
                if (!std::holds_alternative<UnitCreationStatusPending>(s->status))
                {
                    continue;
                }

                auto newUnitId = trySpawnUnit(s->unitType, s->owner, s->position, s->rotation, false, unitId);
                if (!newUnitId)
                {
                    LOG_DEBUG << "factory spawn failed for " << s->unitType
                              << " at (" << simScalarToFloat(s->position.x)
                              << "," << simScalarToFloat(s->position.z) << ")";
                    s->status = UnitCreationStatusFailed();
                    continue;
                }

                s->status = UnitCreationStatusDone{*newUnitId};
            }
        }

        unitCreationRequests.clear();
    }

    void GameSimulation::configureMeteorShower(const MeteorShowerParams& params, bool enabled)
    {
        auto& m = meteorShower;

        // retail apply: seconds -> 30Hz ticks via truncation
        // (TotalA.exe 0x418ed0); cast through int to reproduce the
        // __ftol result for negative inputs
        m.radius = params.radius;
        m.hitStepTicks = params.density > 0.0f
            ? GameTime(static_cast<unsigned int>(static_cast<int>(30.0f / params.density)))
            : GameTime(0);
        m.durationTicks = GameTime(static_cast<unsigned int>(static_cast<int>(params.duration * 30.0f)));
        m.intervalTicks = GameTime(static_cast<unsigned int>(static_cast<int>(params.interval * 30.0f)));

        // retail init (0x418e50): first strike one hit-step in
        m.enabled = enabled;
        m.active = false;
        m.nextStrikeTime = m.hitStepTicks;
        m.timeStrikeEnds = GameTime(0);
        m.nextHitTime = GameTime(0);
        m.originX = m.originZ = m.targetX = m.targetZ = 0;

        // retail resolves by weapon name and requires the TDF `meteor`
        // flag (0x418e50 + 0x418e82); a missing or non-meteor weapon
        // means hits spawn nothing
        auto weaponType = toUpper(params.weaponName);
        auto it = weaponDefinitions.find(weaponType);
        if (it != weaponDefinitions.end() && it->second.isMeteor)
        {
            m.weaponType = weaponType;
        }
        else
        {
            if (enabled)
            {
                LOG_WARN << "meteor shower weapon '" << params.weaponName << "' is missing or lacks the meteor flag; hazard projectiles disabled";
            }
            m.weaponType.clear();
        }
    }

    void GameSimulation::updateMeteorShower()
    {
        auto& m = meteorShower;

        if (m.nextStrikeTime <= gameTime)
        {
            m.active = true;
            m.timeStrikeEnds = gameTime + m.durationTicks;
            m.nextStrikeTime = m.timeStrikeEnds + m.intervalTicks;
            m.nextHitTime = gameTime;

            // retail 0x418f92..0x419043: uniform cell-space target,
            // then origin = target + (rand*30/32768 - 15, rand*10/32768 - 15)
            auto cellsX = std::max(1, static_cast<int>(terrain.getWidthInWorldUnits().value) / 16);
            auto cellsZ = std::max(1, static_cast<int>(terrain.getHeightInWorldUnits().value) / 16);
            m.targetZ = std::uniform_int_distribution<int>(0, cellsZ - 1)(rng);
            m.targetX = std::uniform_int_distribution<int>(0, cellsX - 1)(rng);
            m.originZ = m.targetZ + std::uniform_int_distribution<int>(0, 9)(rng) - 15;
            m.originX = m.targetX + std::uniform_int_distribution<int>(0, 29)(rng) - 15;

            if (!m.enabled)
            {
                m.active = false;
            }
        }

        if (m.active)
        {
            if (m.nextHitTime <= gameTime)
            {
                m.nextHitTime += m.hitStepTicks;

                if (!m.weaponType.empty() && m.radius > 0)
                {
                    // retail 0x4190d1..0x4191a8: impact lands at the
                    // target cell (world px) plus a random offset in a
                    // disc of `radius` px; the projectile spawns 1350px
                    // up on a fixed 90-tick descent at -15px/tick.
                    // retail stores cell-space coords <<20; RWE world
                    // space is centre-origin, so shift by half the map
                    auto originWorld = terrain.heightmapToWorldSpace(
                        SimVector(intToSimScalar(m.originX), 0_ss, intToSimScalar(m.originZ)));
                    auto magnitude = intToSimScalar(std::uniform_int_distribution<int>(0, m.radius - 1)(rng));
                    SimAngle angle(static_cast<uint16_t>(std::uniform_int_distribution<unsigned int>(0, 65534)(rng)));

                    SimVector position(
                        originWorld.x - magnitude * cos(angle),
                        1350_ss,
                        originWorld.z - magnitude * sin(angle));
                    SimVector velocity(
                        floatToSimScalar(static_cast<float>(m.targetX - m.originX) * 16.0f / 90.0f),
                        -15_ss,
                        floatToSimScalar(static_cast<float>(m.targetZ - m.originZ) * 16.0f / 90.0f));

                    // environmental projectile: no firing unit and a
                    // non-player owner so it can damage every player
                    auto projectile = createProjectileFromWeapon(PlayerIdNone, m.weaponType, position, SimVector(0_ss, -1_ss, 0_ss), 0_ss, std::nullopt);
                    projectile.velocity = velocity;
                    // the configured descent is already exact; the
                    // line-of-sight lifetime estimate would kill it
                    projectile.dieOnFrame = std::nullopt;
                    projectiles.emplace(std::move(projectile));
                }
            }

            if (m.timeStrikeEnds <= gameTime)
            {
                m.active = false;
            }
        }
    }

    void GameSimulation::tick()
    {
        gameTime += GameTime(1);

        // opt-in per-phase profiling; each timer is a no-op unless
        // collectTickPhaseTimings was enabled by the benchmark harness
        const bool timing = collectTickPhaseTimings;
        auto& pt = tickPhaseTimings;
        if (timing)
        {
            ++pt.tickCount;
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.visibility : nullptr);
            updateVisibility();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.wind : nullptr);
            updateWind();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.meteorShower : nullptr);
            updateMeteorShower();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.resources : nullptr);
            updateResources();
        }

        // after visibility + resources so the proximity scan sees this
        // tick's LOS sets and the debit lands on the updated pool
        {
            ScopedPhaseTimer timer(timing ? &pt.cloak : nullptr);
            updateCloak();
        }

        // campaign AI sees this tick's resource state; orders it
        // issues run through the same unit behavior pass as any other
        {
            ScopedPhaseTimer timer(timing ? &pt.campaignAi : nullptr);
            updateCampaignAi(*this);
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.pathfinding : nullptr);
            pathFindingService.update(*this);
        }

        // run unit scripts
        for (auto& entry : units)
        {
            auto unitId = entry.first;
            auto& unit = entry.second;

            if (unit.isDead()) { continue; }

            if (unit.carriedBy)
            {
                // Units aboard a transport are inert cargo: no behavior,
                // piece or COB updates. A valid attach slot rides at the
                // carrier's slot piece world position; an invalid slot
                // (-1) rides at the carrier origin (retail 0x43def0).
                if (auto carrier = tryGetUnitState(*unit.carriedBy); carrier && carrier->get().isAlive())
                {
                    unit.position = computeAttachPosition(carrier->get(), unit.attachSlot);
                }
                UnitBehaviorService(this).updateSelfDestruct(unitId);
                continue;
            }

            {
                ScopedPhaseTimer timer(timing ? &pt.unitBehavior : nullptr);
                UnitBehaviorService(this).update(unitId);
            }

            if (unit.isDead()) { continue; }

            {
                ScopedPhaseTimer timer(timing ? &pt.pieces : nullptr);
                for (auto& piece : unit.pieces)
                {
                    piece.update(SimScalar(SimMillisecondsPerTick) / 1000_ss);
                }
            }

            {
                ScopedPhaseTimer timer(timing ? &pt.cob : nullptr);
                runUnitCobScripts(*this, unitId);
            }
        }

        // Resolve pad attachments after every carrier and COB piece has moved.
        // This avoids unit-ID ordering producing a one-tick trailing guest.
        for (const auto& [slot, aircraft] : airServiceOccupants)
        {
            auto patient = tryGetUnitState(aircraft);
            auto base = tryGetUnitState(slot.first);
            if (patient && base && patient->get().isAlive() && base->get().isAlive())
            {
                patient->get().position = computeAttachPosition(base->get(), slot.second);
                patient->get().rotation = base->get().rotation;
            }
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.projectiles : nullptr);
            updateProjectiles();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.featureUpdates : nullptr);
            updateFeatures();
        }

        // retail applies corrosive water inside the per-unit update;
        // running it here keeps it once per tick on the same cadence
        {
            ScopedPhaseTimer timer(timing ? &pt.corrosiveWater : nullptr);
            updateCorrosiveWater();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.victoryAndObjectives : nullptr);
            processVictoryCondition();

            if (missionObjectives)
            {
                missionObjectives->tick(*this);
            }
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.deadUnitCleanup : nullptr);
            deleteDeadUnits();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.deadProjectileCleanup : nullptr);
            deleteDeadProjectiles();
        }

        {
            ScopedPhaseTimer timer(timing ? &pt.spawn : nullptr);
            spawnNewUnits();
        }
    }

    std::optional<FeatureDefinitionId> GameSimulation::tryGetFeatureDefinitionId(const std::string& featureName) const
    {
        if (auto it = featureNameIndex.find(toUpper(featureName)); it != featureNameIndex.end())
        {
            return it->second;
        }

        return std::nullopt;
    }

    const FeatureDefinition& GameSimulation::getFeatureDefinition(FeatureDefinitionId featureDefinitionId) const
    {
        return featureDefinitions.get(featureDefinitionId);
    }
}
