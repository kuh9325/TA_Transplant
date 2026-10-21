#include <catch2/catch_test_macros.hpp>
#include <rwe/io/cob/Cob.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim(Grid<unsigned char>&& heights, unsigned char seaLevel)
        {
            return GameSimulation(MapTerrain(std::move(heights), SimScalar(seaLevel)), 255, 0, 0);
        }

        GameSimulation makeSim(unsigned char height, unsigned char seaLevel)
        {
            return makeSim(Grid<unsigned char>(16, 16, height), seaLevel);
        }

        GamePlayerInfo makePlayer()
        {
            return GamePlayerInfo{
                std::nullopt,
                GamePlayerType::Human,
                PlayerColorIndex(0),
                GamePlayerStatus::Alive,
                "ARM",
                std::nullopt,
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000)};
        }

        MovementClassDefinition makeMovementClass(unsigned int minWaterDepth, unsigned int maxWaterDepth, unsigned int maxSlope = 10)
        {
            MovementClassDefinition mc;
            mc.name = "TEST";
            mc.footprintX = 2;
            mc.footprintZ = 2;
            mc.minWaterDepth = minWaterDepth;
            mc.maxWaterDepth = maxWaterDepth;
            mc.maxSlope = maxSlope;
            mc.maxWaterSlope = 255;
            return mc;
        }

        Grid<YardMapCell> allCells(YardMapCell cell)
        {
            return Grid<YardMapCell>(2, 2, cell);
        }

        UnitDefinition makeBuildingDef(const std::string& name, Grid<YardMapCell>&& yardMap, unsigned int waterline, unsigned int minWaterDepth, unsigned int maxWaterDepth)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.isMobile = false;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 10, 255, minWaterDepth, maxWaterDepth};
            def.yardMap = yardMap;
            def.waterline = waterline;
            return def;
        }

        void registerDef(GameSimulation& sim, const UnitDefinition& def)
        {
            sim.unitDefinitions.insert_or_assign(def.unitName, def);
            sim.unitModelDefinitions.insert_or_assign(def.unitName, createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign(def.unitName, CobScript{{}, {}, {}, 0});
        }

        std::size_t countProjectiles(const GameSimulation& sim)
        {
            std::size_t n = 0;
            for (const auto& _ : sim.projectiles)
            {
                ++n;
            }
            return n;
        }
    }

    TEST_CASE("overlapping mission units do not erase another units occupied cells", "[mission-regression][save]")
    {
        auto sim = makeSim(64, 0);
        const UnitId departing(1), remaining(2);
        sim.occupiedGrid.mutate(GridCoordinates(3, 3), [&](auto& cell) { cell.mobileUnitId = departing; });
        sim.occupiedGrid.mutate(GridCoordinates(4, 3), [&](auto& cell) { cell.mobileUnitId = remaining; });
        sim.moveUnitOccupiedArea(DiscreteRect(3, 3, 2, 1), DiscreteRect(7, 7, 2, 1), departing);
        CHECK_FALSE(sim.occupiedGrid.get(3, 3).mobileUnitId);
        CHECK(sim.occupiedGrid.get(4, 3).mobileUnitId == remaining);
        CHECK(sim.occupiedGrid.get(7, 7).mobileUnitId == departing);
    }

    TEST_CASE("computeBuildingPlacement")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        SECTION("land structure is valid on flat land above sea level")
        {
            auto sim = makeSim(64, 32);
            auto mc = makeMovementClass(0, 0);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 64_ss);
        }

        SECTION("land structure is rejected in deep water")
        {
            auto sim = makeSim(8, 32);
            auto mc = makeMovementClass(0, 0);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("land structure may sit in water no deeper than MaxWaterDepth")
        {
            auto sim = makeSim(31, 32);
            auto mc = makeMovementClass(0, 1);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 31_ss);
        }

        SECTION("land structure is rejected just beyond MaxWaterDepth")
        {
            auto sim = makeSim(30, 32);
            auto mc = makeMovementClass(0, 1);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("water structure is rejected on dry land")
        {
            auto sim = makeSim(64, 32);
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 8, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("tidal-like structure is valid at sufficient depth and floats at seaLevel - waterline")
        {
            auto sim = makeSim(8, 32);
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 8, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 24_ss);
        }

        SECTION("water structure is rejected in water that is too shallow")
        {
            auto sim = makeSim(20, 32);
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 8, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("floating structure with explicit MaxWaterDepth=0 is placeable")
        {
            // ARMFMKR/CORFMKR ship with MinWaterDepth=11, MaxWaterDepth=0:
            // an all-water yardmap never applies MaxWaterDepth to the
            // floating base, it only bounds how shallow the seabed may be
            auto sim = makeSim(20, 32);
            auto mc = makeMovementClass(11, 0);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 4, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 28_ss);
        }

        SECTION("keel clearance binds when waterline exceeds minimum depth")
        {
            // sonar-like: MinWaterDepth=8 but Waterline=12, so the seabed
            // must actually be at least 12 deep for the base to clear it
            auto sim = makeSim(21, 32);
            auto mc = makeMovementClass(8, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 12, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("keel clearance passes at exactly waterline depth")
        {
            auto sim = makeSim(20, 32);
            auto mc = makeMovementClass(8, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 12, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 20_ss);
        }

        SECTION("seabed structure rests on the terrain, not the surface")
        {
            // underwater-support yardmaps (CORUWMS-style 'o' cells) place
            // their origin on the seabed rather than at sea level
            auto sim = makeSim(8, 32);
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 8_ss);
        }

        SECTION("seabed structure obeys explicit MaxWaterDepth")
        {
            auto sim = makeSim(8, 32);
            auto mc = makeMovementClass(20, 20);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("footprint height range is checked against MaxSlope")
        {
            Grid<unsigned char> heights(16, 16, 64);
            // drop one corner vertex of cell (5,5) by 14
            heights.get(6, 6) = 50;
            auto sim = makeSim(std::move(heights), 32);
            auto mc = makeMovementClass(0, 0, 10);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("footprint height range at exactly MaxSlope is allowed")
        {
            Grid<unsigned char> heights(16, 16, 64);
            heights.get(6, 6) = 54;
            auto sim = makeSim(std::move(heights), 32);
            auto mc = makeMovementClass(0, 0, 10);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 54_ss);
        }

        SECTION("ignored '.' cells do not participate in terrain checks")
        {
            Grid<unsigned char> heights(16, 16, 8);
            // a spike of land pokes above the surface but only under
            // the '.' corner of the footprint
            heights.get(6, 4) = 64;
            auto sim = makeSim(std::move(heights), 32);

            Grid<YardMapCell> yardMap(2, 2, YardMapCell::Water);
            yardMap.get(1, 0) = YardMapCell::Passable;
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, yardMap, false, 8, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 24_ss);
        }

        SECTION("the same spike rejects when the cell is a water cell")
        {
            Grid<unsigned char> heights(16, 16, 8);
            heights.get(6, 4) = 64;
            auto sim = makeSim(std::move(heights), 32);
            auto mc = makeMovementClass(20, 255);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Water), false, 8, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("mixed footprints rest on their support cells")
        {
            Grid<unsigned char> heights(16, 16, 64);
            // cell (4,4) is the water cell, cell (5,4) the support cell
            heights.get(4, 4) = 20;
            heights.get(4, 5) = 20;
            heights.get(5, 4) = 28;
            heights.get(5, 5) = 28;
            heights.get(6, 4) = 28;
            heights.get(6, 5) = 28;
            auto sim = makeSim(std::move(heights), 32);

            MovementClassDefinition mc;
            mc.name = "MIXED";
            mc.footprintX = 2;
            mc.footprintZ = 1;
            mc.minWaterDepth = 0;
            mc.maxWaterDepth = 10;
            mc.maxSlope = 10;
            mc.maxWaterSlope = 255;

            Grid<YardMapCell> yardMap(2, 1, YardMapCell::Water);
            yardMap.get(1, 0) = YardMapCell::Ground;

            auto placement = sim.computeBuildingPlacement(mc, yardMap, false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(placement->valid);
            REQUIRE(placement->baseY == 28_ss);
        }

        SECTION("mixed footprints reject water cells above the support base")
        {
            Grid<unsigned char> heights(16, 16, 64);
            heights.get(4, 4) = 30;
            heights.get(4, 5) = 30;
            heights.get(5, 4) = 28;
            heights.get(5, 5) = 28;
            heights.get(6, 4) = 28;
            heights.get(6, 5) = 28;
            auto sim = makeSim(std::move(heights), 32);

            MovementClassDefinition mc;
            mc.name = "MIXED";
            mc.footprintX = 2;
            mc.footprintZ = 1;
            mc.minWaterDepth = 0;
            mc.maxWaterDepth = 10;
            mc.maxSlope = 10;
            mc.maxWaterSlope = 255;

            Grid<YardMapCell> yardMap(2, 1, YardMapCell::Water);
            yardMap.get(1, 0) = YardMapCell::Ground;

            auto placement = sim.computeBuildingPlacement(mc, yardMap, false, 0, 4, 4);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }

        SECTION("out-of-bounds footprints report no placement")
        {
            auto sim = makeSim(64, 32);
            auto mc = makeMovementClass(0, 0);

            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, 15, 15);

            REQUIRE(!placement);
        }

        SECTION("occupied cells reject the placement")
        {
            auto sim = makeSim(64, 32);
            auto blocker = makeBuildingDef("BLOCKER", allCells(YardMapCell::Ground), 0, 0, 0);
            registerDef(sim, blocker);
            auto playerId = sim.addPlayer(makePlayer());

            auto pos = sim.terrain.heightmapIndexToWorldCenter(5, 4);
            auto spawned = sim.trySpawnUnit("BLOCKER", playerId, pos, std::nullopt);
            REQUIRE(spawned);

            auto blockerRect = sim.computeFootprintRegion(sim.getUnitState(*spawned).position, blocker.movementCollisionInfo);
            auto mc = makeMovementClass(0, 0);
            auto placement = sim.computeBuildingPlacement(mc, allCells(YardMapCell::Ground), false, 0, blockerRect.x, blockerRect.y);

            REQUIRE(placement);
            REQUIRE(!placement->valid);
        }
    }

    TEST_CASE("trySpawnUnit structure heights")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        SECTION("floating structure spawns at seaLevel - waterline")
        {
            auto sim = makeSim(8, 32);
            registerDef(sim, makeBuildingDef("FLOATER", allCells(YardMapCell::Water), 8, 20, 255));
            auto playerId = sim.addPlayer(makePlayer());

            // the order position carries the terrain height at the cursor;
            // the spawn must still put the origin at the floating height
            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            pos.y = sim.terrain.getHeightAt(pos.x, pos.z);
            auto spawned = sim.trySpawnUnit("FLOATER", playerId, pos, std::nullopt);

            REQUIRE(spawned);
            REQUIRE(sim.getUnitState(*spawned).position.y == 24_ss);
        }

        SECTION("seabed structure spawns on the terrain")
        {
            auto sim = makeSim(8, 32);
            registerDef(sim, makeBuildingDef("SEABED", allCells(YardMapCell::Ground), 0, 20, 255));
            auto playerId = sim.addPlayer(makePlayer());

            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            auto spawned = sim.trySpawnUnit("SEABED", playerId, pos, std::nullopt);

            REQUIRE(spawned);
            REQUIRE(sim.getUnitState(*spawned).position.y == 8_ss);
        }

        SECTION("land structure spawns at terrain height")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, makeBuildingDef("LANDBLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(makePlayer());

            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            auto spawned = sim.trySpawnUnit("LANDBLDG", playerId, pos, std::nullopt);

            REQUIRE(spawned);
            REQUIRE(sim.getUnitState(*spawned).position.y == 64_ss);
        }
    }

    TEST_CASE("trySpawnUnit allowOverlap")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        SECTION("authored overlapping units both spawn")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(makePlayer());

            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            auto first = sim.trySpawnUnit("BLDG", playerId, pos, std::nullopt, true);
            auto second = sim.trySpawnUnit("BLDG", playerId, pos, std::nullopt, true);

            REQUIRE(first);
            REQUIRE(second);
        }

        SECTION("default spawn still rejects the overlap")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(makePlayer());

            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            auto first = sim.trySpawnUnit("BLDG", playerId, pos, std::nullopt);
            auto second = sim.trySpawnUnit("BLDG", playerId, pos, std::nullopt);

            REQUIRE(first);
            REQUIRE(!second);
        }

        SECTION("authored unit hanging off the map edge clips instead of failing")
        {
            auto sim = makeSim(64, 32);
            UnitDefinition mobile{};
            mobile.unitName = "EDGEUNIT";
            mobile.objectName = "EDGEUNIT";
            mobile.isMobile = true;
            mobile.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            registerDef(sim, mobile);
            auto playerId = sim.addPlayer(makePlayer());

            // 16x16 heightmap -> 15x15 occupied grid; a 2x2 footprint
            // centred on the last cell hangs one row/column off the map
            auto pos = sim.terrain.heightmapIndexToWorldCenter(15, 15);
            auto spawned = sim.trySpawnUnit("EDGEUNIT", playerId, pos, std::nullopt, true);

            REQUIRE(spawned);
        }
    }

    TEST_CASE("meteorShower")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        // makeSim(64,32) -> 16x16 heightmap cells -> 256x256px world,
        // so retail cell-space target coords must land in [0,16)
        WeaponDefinition meteor{};
        meteor.velocity = SimScalar(10);
        meteor.isMeteor = true;
        meteor.damage.insert_or_assign("DEFAULT", 5);
        meteor.damageRadius = SimScalar(20);

        MeteorShowerParams params;
        params.weaponName = "Meteor";
        params.radius = 300;
        params.density = 2.0f;
        params.duration = 5.0f;
        params.interval = 60.0f;

        auto configure = [&meteor, &params](GameSimulation& sim) {
            sim.weaponDefinitions.insert_or_assign("METEOR", meteor);
            sim.configureMeteorShower(params, true);
        };

        SECTION("retail tick conversion and first-strike schedule")
        {
            auto sim = makeSim(64, 32);
            configure(sim);

            const auto& m = sim.meteorShower;
            REQUIRE(m.enabled);
            REQUIRE(m.weaponType == "METEOR");
            // int(30/density), int(duration*30), int(interval*30)
            REQUIRE(m.hitStepTicks == GameTime(15));
            REQUIRE(m.durationTicks == GameTime(150));
            REQUIRE(m.intervalTicks == GameTime(1800));
            // retail init: first strike begins one hit-step in
            REQUIRE(m.nextStrikeTime == GameTime(15));
            REQUIRE(!m.active);
        }

        SECTION("strike schedule and hit cadence")
        {
            auto sim = makeSim(64, 32);
            configure(sim);
            const auto& m = sim.meteorShower;

            for (int i = 0; i < 14; ++i)
            {
                sim.tick();
                REQUIRE(!m.active);
            }

            // tick 15: strike starts, first hit fires immediately
            sim.tick();
            REQUIRE(sim.gameTime == GameTime(15));
            REQUIRE(m.active);
            REQUIRE(m.nextHitTime == GameTime(30));
            REQUIRE(m.timeStrikeEnds == GameTime(165));
            // next strike = strike end + interval gap
            REQUIRE(m.nextStrikeTime == GameTime(1965));
            REQUIRE(countProjectiles(sim) == 1);

            // target uniformly over [0,16) cells; retail origin offsets
            REQUIRE(m.targetX >= 0);
            REQUIRE(m.targetX < 16);
            REQUIRE(m.targetZ >= 0);
            REQUIRE(m.targetZ < 16);
            REQUIRE(m.originX - m.targetX >= -15);
            REQUIRE(m.originX - m.targetX <= 14);
            REQUIRE(m.originZ - m.targetZ >= -15);
            REQUIRE(m.originZ - m.targetZ <= -6);

            // hits at t=15,30,...,165 inclusive -> 11 total; each
            // hit advances nextHitTime exactly once (projectiles may
            // die inside the window, so count by scheduler, not size)
            unsigned int hits = 1;
            auto prevNextHit = m.nextHitTime;
            while (m.active)
            {
                sim.tick();
                if (m.nextHitTime != prevNextHit)
                {
                    ++hits;
                    prevNextHit = m.nextHitTime;
                }
            }
            REQUIRE(hits == 11);
            REQUIRE(sim.gameTime == GameTime(165));
            REQUIRE(!m.active);
        }

        SECTION("hit projectile matches the retail 90-tick descent")
        {
            auto sim = makeSim(64, 32);
            configure(sim);
            const auto& m = sim.meteorShower;

            for (int i = 0; i < 15; ++i)
            {
                sim.tick();
            }
            REQUIRE(countProjectiles(sim) == 1);

            const auto& p = sim.projectiles.begin()->second;
            REQUIRE(p.weaponType == "METEOR");
            // environmental: non-player owner, no kill attribution
            REQUIRE(p.owner == PlayerIdNone);
            REQUIRE(!p.firedBy.has_value());
            // fixed descent: spawned at 1350px, already one tick
            // into the -15px/tick fall when observed
            REQUIRE(p.previousPosition.y == 1350_ss);
            REQUIRE(p.velocity.y == -15_ss);
            // cleared lifetime: the descent itself is the clock
            REQUIRE(!p.dieOnFrame.has_value());
            // lands within `radius` px of the target cell's world
            // point (world space is centre-origin -- the scheduler's
            // cell coords must go through heightmapToWorldSpace)
            auto landingX = p.position.x + (p.velocity.x * 90_ss);
            auto landingZ = p.position.z + (p.velocity.z * 90_ss);
            auto targetWorld = sim.terrain.heightmapToWorldSpace(
                SimVector(intToSimScalar(m.targetX), 0_ss, intToSimScalar(m.targetZ)));
            REQUIRE(rweAbs(landingX - targetWorld.x) <= 300_ss);
            REQUIRE(rweAbs(landingZ - targetWorld.z) <= 300_ss);
        }

        SECTION("hits spawn and land inside the world bounds")
        {
            // regression for the centre-origin fix: uncentered cell*16
            // coords shifted every meteor half a map east/south, so
            // most spawned OOB and the rest landed in one quadrant
            auto scatterParams = params;
            scatterParams.radius = 16;
            auto sim = makeSim(64, 32);
            sim.weaponDefinitions.insert_or_assign("METEOR", meteor);
            sim.configureMeteorShower(scatterParams, true);

            for (int i = 0; i < 15; ++i)
            {
                sim.tick();
            }
            // pin the strike's origin/target to an interior cell so
            // the spawn/impact positions are deterministic
            auto& m = sim.meteorShower;
            m.targetX = m.originX = 8;
            m.targetZ = m.originZ = 8;

            for (int i = 0; i < 15; ++i)
            {
                sim.tick(); // hit at t=30
            }
            // find the projectile spawned this tick (the t=15 meteor
            // is still descending alongside it)
            std::optional<ProjectileId> pinnedId;
            SimVector spawnPos;
            for (const auto& [id, proj] : sim.projectiles)
            {
                if (proj.previousPosition.y == 1350_ss)
                {
                    pinnedId = id;
                    spawnPos = proj.previousPosition;
                }
            }
            REQUIRE(pinnedId.has_value());
            // cell 8's centred world corner is (0,0); scatter <= 16px
            auto left = sim.terrain.leftInWorldUnits();
            auto top = sim.terrain.topInWorldUnits();
            auto right = left + sim.terrain.getWidthInWorldUnits();
            auto bottom = top + sim.terrain.getHeightInWorldUnits();
            REQUIRE(spawnPos.x >= left);
            REQUIRE(spawnPos.x < right);
            REQUIRE(spawnPos.z >= top);
            REQUIRE(spawnPos.z < bottom);
            REQUIRE(rweAbs(spawnPos.x) <= 17_ss);
            REQUIRE(rweAbs(spawnPos.z) <= 17_ss);

            // the pinned projectile must impact inside the map
            // (terrain or sea), not silently die out of bounds
            for (int i = 0; i < 95; ++i)
            {
                sim.tick();
            }
            std::optional<ProjectileDiedEvent::DeathType> death;
            for (const auto& e : sim.events)
            {
                if (const auto* died = std::get_if<ProjectileDiedEvent>(&e); died && died->projectileId == *pinnedId)
                {
                    death = died->deathType;
                }
            }
            REQUIRE(death.has_value());
            REQUIRE(*death != ProjectileDiedEvent::DeathType::OutOfBounds);
        }

        SECTION("impacts damage units of every player through the normal AoE path")
        {
            WeaponDefinition bigRadius = meteor;
            bigRadius.damageRadius = SimScalar(5000);

            auto sim = makeSim(64, 32);
            sim.weaponDefinitions.insert_or_assign("METEOR", bigRadius);
            sim.configureMeteorShower(params, true);
            auto playerA = sim.addPlayer(makePlayer());
            auto computer = makePlayer();
            computer.type = GamePlayerType::Computer;
            auto playerB = sim.addPlayer(computer);

            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto bldA = sim.trySpawnUnit("BLDG", playerA, sim.terrain.heightmapIndexToWorldCenter(4, 4), std::nullopt);
            auto bldB = sim.trySpawnUnit("BLDG", playerB, sim.terrain.heightmapIndexToWorldCenter(12, 12), std::nullopt);
            REQUIRE(bldA);
            REQUIRE(bldB);
            // fixture defs carry no authored hp; give enough to survive
            // the whole first strike so hp deltas are observable
            sim.getUnitState(*bldA).hitPoints = 100000;
            sim.getUnitState(*bldB).hitPoints = 100000;
            auto hpA = sim.getUnitState(*bldA).hitPoints;
            auto hpB = sim.getUnitState(*bldB).hitPoints;

            // let the first strike's hits land (~15 + 100 descent)
            for (int i = 0; i < 150; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*bldA).hitPoints < hpA);
            REQUIRE(sim.getUnitState(*bldB).hitPoints < hpB);
        }

        SECTION("missing or non-meteor weapon arms nothing but still schedules")
        {
            auto sim = makeSim(64, 32);

            WeaponDefinition plain;
            plain.velocity = SimScalar(10);
            sim.weaponDefinitions.insert_or_assign("METEOR", plain);
            sim.weaponDefinitions.insert_or_assign("OTHER", meteor);

            auto otherParams = params;
            otherParams.weaponName = "Other";
            sim.configureMeteorShower(otherParams, true);
            REQUIRE(sim.meteorShower.enabled);
            REQUIRE(sim.meteorShower.weaponType == "OTHER");

            sim.configureMeteorShower(params, true);
            // METEOR exists but lacks the meteor flag -> no hits
            REQUIRE(sim.meteorShower.weaponType.empty());

            for (int i = 0; i < 200; ++i)
            {
                sim.tick();
            }
            REQUIRE(countProjectiles(sim) == 0);
            // the scheduler still advanced through the strike
            REQUIRE(sim.meteorShower.nextStrikeTime > GameTime(15));
        }

        SECTION("disabled shower rolls target but spawns nothing")
        {
            auto sim = makeSim(64, 32);
            configure(sim);
            sim.configureMeteorShower(params, false);

            for (int i = 0; i < 200; ++i)
            {
                sim.tick();
            }
            REQUIRE(countProjectiles(sim) == 0);
            REQUIRE(!sim.meteorShower.active);
        }

        SECTION("same seed reproduces the identical strike sequence")
        {
            auto simA = makeSim(64, 32);
            auto simB = makeSim(64, 32);
            configure(simA);
            configure(simB);

            for (int i = 0; i < 200; ++i)
            {
                simA.tick();
                simB.tick();
            }
            REQUIRE(simA.computeHash() == simB.computeHash());
        }

        SECTION("different rng seed diverges the target rolls")
        {
            auto simA = makeSim(64, 32);
            auto simB = makeSim(64, 32);
            configure(simA);
            configure(simB);
            simB.rng.seed(12345);

            for (int i = 0; i < 15; ++i)
            {
                simA.tick();
                simB.tick();
            }
            const auto& a = simA.meteorShower;
            const auto& b = simB.meteorShower;
            REQUIRE((a.targetX != b.targetX || a.targetZ != b.targetZ
                || a.originX != b.originX || a.originZ != b.originZ));
        }

        SECTION("resolveMeteorShowerParams mirrors retail validation")
        {
            MeteorShowerParams defaults;
            defaults.weaponName = "Meteor";
            defaults.radius = 300;
            defaults.density = 2.0f;
            defaults.duration = 5.0f;
            defaults.interval = 60.0f;

            SECTION("fully armed schema passes through")
            {
                auto r = resolveMeteorShowerParams(params, defaults);
                REQUIRE(r.enabled);
                REQUIRE(r.params.weaponName == "Meteor");
                REQUIRE(r.params.density == 2.0f);
            }

            SECTION("empty weapon name disables the shower")
            {
                auto schema = params;
                schema.weaponName = "";
                auto r = resolveMeteorShowerParams(schema, defaults);
                REQUIRE(!r.enabled);
            }

            SECTION("any zero numeric field swaps in ALL the defaults")
            {
                // retail EXP1AC07-style record: authored earthquake
                // weapon but MeteorDensity=0 -> whole config becomes
                // the meteor.tdf [Default] block
                auto schema = params;
                schema.weaponName = "earthquake";
                schema.radius = 800;
                schema.density = 0.0f;
                schema.duration = 6.0f;
                schema.interval = 612.0f;
                auto r = resolveMeteorShowerParams(schema, defaults);
                REQUIRE(r.enabled);
                REQUIRE(r.params.weaponName == "Meteor");
                REQUIRE(r.params.radius == 300);
                REQUIRE(r.params.density == 2.0f);
                REQUIRE(r.params.duration == 5.0f);
                REQUIRE(r.params.interval == 60.0f);
            }
        }
    }

    TEST_CASE("createProjectileFromWeapon")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto sim = makeSim(64, 32);
        auto playerId = sim.addPlayer(makePlayer());

        WeaponDefinition boom;
        boom.velocity = SimScalar(10);
        sim.weaponDefinitions.insert({"LARGE_BUILDINGEX", boom});

        SECTION("case-mangled authored names resolve to the canonical definition")
        {
            auto projectile = sim.createProjectileFromWeapon(
                playerId, "lARGE_BUILDINGEX", SimVector(10_ss, 64_ss, 10_ss), SimVector(0_ss, -1_ss, 0_ss), 0_ss, std::nullopt);

            REQUIRE(projectile.weaponType == "LARGE_BUILDINGEX");
        }

        SECTION("genuinely unknown names still fail")
        {
            REQUIRE_THROWS(sim.createProjectileFromWeapon(
                playerId, "NOSUCHWEAPON", SimVector(10_ss, 64_ss, 10_ss), SimVector(0_ss, -1_ss, 0_ss), 0_ss, std::nullopt));
        }
    }

    TEST_CASE("addScaledResourceDelta")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto computerPlayer = []() {
            auto p = makePlayer();
            p.type = GamePlayerType::Computer;
            return p;
        };

        SECTION("computer-owned units get the campaign coefficient")
        {
            auto sim = makeSim(64, 32);
            sim.computerResourceScale = 0.5f;
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(computerPlayer());
            auto spawned = sim.trySpawnUnit("BLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(4, 4), std::nullopt);
            REQUIRE(spawned);

            sim.addScaledResourceDelta(*spawned, Energy(100), Metal(50));
            REQUIRE(sim.getPlayer(playerId).energyProductionBuffer == Energy(50));
            REQUIRE(sim.getPlayer(playerId).metalProductionBuffer == Metal(25));
        }

        SECTION("human-owned units stay unscaled")
        {
            auto sim = makeSim(64, 32);
            sim.computerResourceScale = 0.5f;
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("BLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(4, 4), std::nullopt);
            REQUIRE(spawned);

            sim.addScaledResourceDelta(*spawned, Energy(100), Metal(50));
            REQUIRE(sim.getPlayer(playerId).energyProductionBuffer == Energy(100));
            REQUIRE(sim.getPlayer(playerId).metalProductionBuffer == Metal(50));
        }

        SECTION("scale 1.0 leaves computer deltas unchanged")
        {
            auto sim = makeSim(64, 32);
            sim.computerResourceScale = 1.0f;
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(computerPlayer());
            auto spawned = sim.trySpawnUnit("BLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(4, 4), std::nullopt);
            REQUIRE(spawned);

            sim.addScaledResourceDelta(*spawned, Energy(100), Metal(50));
            REQUIRE(sim.getPlayer(playerId).energyProductionBuffer == Energy(100));
            REQUIRE(sim.getPlayer(playerId).metalProductionBuffer == Metal(50));
        }

        SECTION("apparent and actual deltas both scale")
        {
            auto sim = makeSim(64, 32);
            sim.computerResourceScale = 0.7f;
            registerDef(sim, makeBuildingDef("BLDG", allCells(YardMapCell::Ground), 0, 0, 0));
            auto playerId = sim.addPlayer(computerPlayer());
            auto spawned = sim.trySpawnUnit("BLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(4, 4), std::nullopt);
            REQUIRE(spawned);

            // nanolathe-class draw: desired rate -100/-50, real cost -80/-40
            sim.addScaledResourceDelta(*spawned, Energy(-100), Metal(-50), Energy(-80), Metal(-40));
            REQUIRE(sim.getPlayer(playerId).desiredEnergyConsumptionBuffer == Energy(70));
            REQUIRE(sim.getPlayer(playerId).desiredMetalConsumptionBuffer == Metal(35));
            REQUIRE(sim.getPlayer(playerId).actualEnergyConsumptionBuffer == Energy(56));
            REQUIRE(sim.getPlayer(playerId).actualMetalConsumptionBuffer == Metal(28));
        }
    }

    /**
     * computerResourceScale steers future deterministic economy
     * behavior (addScaledResourceDelta), so it is part of the
     * authoritative simulation hash -- two otherwise-identical
     * states at Easy (0.5) vs Hard (1.0) must hash differently.
     */
    TEST_CASE("GameHash distinguishes computerResourceScale")
    {
        auto simEasy = makeSim(64, 32);
        simEasy.computerResourceScale = 0.5f;
        auto simHard = makeSim(64, 32);
        simHard.computerResourceScale = 1.0f;

        REQUIRE(computeHashOf(simEasy) != computeHashOf(simHard));

        // and identical scales produce identical hashes
        auto simEasyB = makeSim(64, 32);
        simEasyB.computerResourceScale = 0.5f;
        REQUIRE(computeHashOf(simEasy) == computeHashOf(simEasyB));
    }

    TEST_CASE("corrosive water")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        // cells x<8 sit at height 8 under a sea level of 32
        // (submerged), cells x>=8 are dry land at 64
        auto splitHeights = [] {
            Grid<unsigned char> g(16, 16, 64);
            for (std::size_t y = 0; y < 16; ++y)
            {
                for (std::size_t x = 0; x < 8; ++x)
                {
                    g.get(x, y) = 8;
                }
            }
            return g;
        };

        auto waterBuilding = [] {
            // seabed structure: rests on the terrain under water
            return makeBuildingDef("SUBBLDG", allCells(YardMapCell::Ground), 0, 0, 255);
        };

        SECTION("waterDoesDamage=0 leaves submerged units alone")
        {
            auto sim = makeSim(splitHeights(), 32);
            registerDef(sim, waterBuilding());
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("SUBBLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(2, 8), std::nullopt);
            REQUIRE(spawned);
            auto& unit = sim.getUnitState(*spawned);
            unit.hitPoints = 1000;
            REQUIRE(unit.position.y <= sim.terrain.getSeaLevel());

            for (int i = 0; i < 90; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 1000);
        }

        SECTION("submerged units take the exact damage at the 30-tick cadence")
        {
            auto sim = makeSim(splitHeights(), 32);
            sim.waterDoesDamage = true;
            sim.waterDamage = 6;
            registerDef(sim, waterBuilding());
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("SUBBLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(2, 8), std::nullopt);
            REQUIRE(spawned);
            sim.getUnitState(*spawned).hitPoints = 1000;

            for (int i = 0; i < 29; ++i)
            {
                sim.tick();
            }
            // retail ticks the corrosion every 30th sim tick
            // (gameTime % 30 == 0): nothing before t=30
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 1000);

            sim.tick();
            REQUIRE(sim.gameTime == GameTime(30));
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 994);

            for (int i = 0; i < 29; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 994);

            sim.tick();
            REQUIRE(sim.gameTime == GameTime(60));
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 988);
        }

        SECTION("armed with zero damage applies nothing")
        {
            auto sim = makeSim(splitHeights(), 32);
            sim.waterDoesDamage = true;
            sim.waterDamage = 0;
            registerDef(sim, waterBuilding());
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("SUBBLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(2, 8), std::nullopt);
            REQUIRE(spawned);
            sim.getUnitState(*spawned).hitPoints = 1000;

            for (int i = 0; i < 60; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 1000);
        }

        SECTION("units above the surface are unaffected")
        {
            auto sim = makeSim(splitHeights(), 32);
            sim.waterDoesDamage = true;
            sim.waterDamage = 6;
            registerDef(sim, waterBuilding());
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("SUBBLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
            REQUIRE(spawned);
            auto& unit = sim.getUnitState(*spawned);
            unit.hitPoints = 1000;
            REQUIRE(unit.position.y > sim.terrain.getSeaLevel());

            for (int i = 0; i < 60; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 1000);
        }

        SECTION("hovercraft are exempt (retail unitdef +0x241 bit 12)")
        {
            auto hoverDef = makeBuildingDef("HOVER", allCells(YardMapCell::Ground), 0, 0, 0);
            hoverDef.canHover = true;
            hoverDef.isMobile = true;

            auto sim = makeSim(splitHeights(), 32);
            sim.waterDoesDamage = true;
            sim.waterDamage = 6;
            registerDef(sim, hoverDef);
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("HOVER", playerId, sim.terrain.heightmapIndexToWorldCenter(2, 8), std::nullopt);
            REQUIRE(spawned);
            auto& unit = sim.getUnitState(*spawned);
            unit.hitPoints = 1000;
            // floaters/hovercraft snap to the surface, i.e. exactly
            // at sea level -- still inside retail's <= test, so only
            // the canHover exemption keeps them safe
            REQUIRE(unit.position.y == sim.terrain.getSeaLevel());

            for (int i = 0; i < 60; ++i)
            {
                sim.tick();
            }
            REQUIRE(sim.getUnitState(*spawned).hitPoints == 1000);
        }

        SECTION("lethal corrosion kills through the normal damage path")
        {
            auto sim = makeSim(splitHeights(), 32);
            sim.waterDoesDamage = true;
            sim.waterDamage = 100;
            registerDef(sim, waterBuilding());
            auto playerId = sim.addPlayer(makePlayer());
            auto spawned = sim.trySpawnUnit("SUBBLDG", playerId, sim.terrain.heightmapIndexToWorldCenter(2, 8), std::nullopt);
            REQUIRE(spawned);
            sim.getUnitState(*spawned).hitPoints = 50;

            for (int i = 0; i < 30; ++i)
            {
                sim.tick();
            }
            // the unit died at t=30 and was reclaimed by
            // deleteDeadUnits in the same tick
            REQUIRE(!sim.tryGetUnitState(*spawned));
        }
    }

    TEST_CASE("projectile sea-surface collision")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        auto seaWeapon = [](bool waterWeapon) {
            WeaponDefinition w{};
            w.velocity = SimScalar(15);
            w.waterWeapon = waterWeapon;
            // ballistic: no auto dieOnFrame, the projectile lives
            // until terrain/sea/unit collision
            w.physicsType = ProjectilePhysicsTypeBallistic{};
            w.damage.insert_or_assign("DEFAULT", 5);
            w.damageRadius = SimScalar(0);
            return w;
        };

        auto dropProjectile = [](GameSimulation& sim, const std::string& weaponType, SimVector position) {
            auto p = sim.createProjectileFromWeapon(
                PlayerIdNone, weaponType, position,
                SimVector(0_ss, -1_ss, 0_ss), 0_ss, std::nullopt);
            return sim.projectiles.emplace(std::move(p));
        };

        // makeSim(8, 32): every cell is 24 under sea level
        SECTION("an ordinary projectile dies on the surface")
        {
            auto sim = makeSim(8, 32);
            sim.weaponDefinitions.insert_or_assign("SHELL", seaWeapon(false));
            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            pos.y = 40_ss;
            auto id = dropProjectile(sim, "SHELL", pos);
            sim.tick();
            REQUIRE(!sim.projectiles.tryGet(id));
        }

        SECTION("a waterweapon projectile survives the surface")
        {
            auto sim = makeSim(8, 32);
            sim.weaponDefinitions.insert_or_assign("TORP", seaWeapon(true));
            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            pos.y = 40_ss;
            auto id = dropProjectile(sim, "TORP", pos);
            sim.tick();
            // y: 40 -> 25, below sea level but still above the seabed
            REQUIRE(sim.projectiles.tryGet(id));
            sim.tick();
            REQUIRE(sim.projectiles.tryGet(id));
            // the next descent reaches the seabed and dies on terrain
            sim.tick();
            REQUIRE(!sim.projectiles.tryGet(id));
        }

        SECTION("nosealeveltrigger suppresses the surface destroy")
        {
            auto sim = makeSim(8, 32);
            sim.noSeaLevelTrigger = true;
            sim.weaponDefinitions.insert_or_assign("SHELL", seaWeapon(false));
            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            pos.y = 40_ss;
            auto id = dropProjectile(sim, "SHELL", pos);
            sim.tick();
            // crosses below sea level unharmed, then dies on the seabed
            REQUIRE(sim.projectiles.tryGet(id));
            sim.tick();
            REQUIRE(sim.projectiles.tryGet(id));
            sim.tick();
            REQUIRE(!sim.projectiles.tryGet(id));
        }

        SECTION("a projectile under dry terrain still hits ground")
        {
            auto sim = makeSim(64, 32);
            sim.noSeaLevelTrigger = true;
            sim.weaponDefinitions.insert_or_assign("SHELL", seaWeapon(false));
            auto pos = sim.terrain.heightmapIndexToWorldCenter(4, 4);
            pos.y = 70_ss;
            auto id = dropProjectile(sim, "SHELL", pos);
            sim.tick();
            REQUIRE(!sim.projectiles.tryGet(id));
        }
    }

    TEST_CASE("skirmish victory semantics")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        UnitDefinition commander{};
        commander.unitName = "COMMANDER";
        commander.objectName = "COMMANDER";
        commander.isMobile = true;
        commander.commander = true;
        commander.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};

        UnitDefinition tank{};
        tank.unitName = "TANK";
        tank.objectName = "TANK";
        tank.isMobile = true;
        tank.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};

        auto makePlayerWithGroup = [](PlayerColorIndex color, const std::string& side, std::optional<unsigned int> group) {
            auto p = makePlayer();
            p.color = color;
            p.side = side;
            p.allyGroup = group;
            return p;
        };

        auto spawn = [](GameSimulation& sim, const char* type, PlayerId owner, int x, int z) {
            auto id = sim.trySpawnUnit(type, owner, sim.terrain.heightmapIndexToWorldCenter(x, z), std::nullopt, true);
            REQUIRE(id);
            auto& unit = sim.getUnitState(*id);
            unit.finishBuilding(sim.unitDefinitions.at(unit.unitType));
            return *id;
        };

        SECTION("Game Ends: a dead commander eliminates its owner")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, commander);
            registerDef(sim, tank);
            sim.commanderDeathContinues = false;

            auto a = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(0), "ARM", std::nullopt));
            auto b = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(1), "CORE", std::nullopt));
            auto c = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(2), "CORE", std::nullopt));

            auto comA = spawn(sim, "COMMANDER", a, 2, 2);
            auto comB = spawn(sim, "COMMANDER", b, 8, 2);
            auto comC = spawn(sim, "COMMANDER", c, 12, 2);
            spawn(sim, "TANK", a, 4, 4);

            sim.killUnit(comB);
            sim.processVictoryCondition();

            // commander death killed the whole side: B's status and
            // every B unit are dead, the game continues with A and C
            REQUIRE(sim.getPlayer(b).status == GamePlayerStatus::Dead);
            REQUIRE(sim.getUnitState(comB).isDead());
            REQUIRE(std::holds_alternative<WinStatusUndecided>(sim.computeWinStatus()));

            sim.killUnit(comC);
            sim.processVictoryCondition();
            REQUIRE(sim.getPlayer(c).status == GamePlayerStatus::Dead);
            auto status = sim.computeWinStatus();
            REQUIRE(std::holds_alternative<WinStatusWon>(status));
            REQUIRE(std::get<WinStatusWon>(status).winner == a);

            (void)comA;
        }

        SECTION("Continues: commander loss alone does not eliminate")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, commander);
            registerDef(sim, tank);
            sim.commanderDeathContinues = true;

            auto a = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(0), "ARM", std::nullopt));
            auto b = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(1), "CORE", std::nullopt));

            spawn(sim, "COMMANDER", a, 2, 2);
            auto comB = spawn(sim, "COMMANDER", b, 8, 2);
            auto tankB = spawn(sim, "TANK", b, 10, 2);

            sim.killUnit(comB);
            sim.processVictoryCondition();
            // the commander is gone but B lives on with its tank
            REQUIRE(sim.getPlayer(b).status == GamePlayerStatus::Alive);
            REQUIRE(std::holds_alternative<WinStatusUndecided>(sim.computeWinStatus()));

            sim.killUnit(tankB);
            sim.processVictoryCondition();
            // the last live unit fell -> B is eliminated, A wins
            REQUIRE(sim.getPlayer(b).status == GamePlayerStatus::Dead);
            auto status = sim.computeWinStatus();
            REQUIRE(std::holds_alternative<WinStatusWon>(status));
            REQUIRE(std::get<WinStatusWon>(status).winner == a);
        }

        SECTION("allied survivors count as one alliance")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, commander);
            sim.commanderDeathContinues = false;

            // B and C share ally group 0: they win and lose together
            auto a = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(0), "ARM", std::nullopt));
            auto b = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(1), "CORE", 0u));
            auto c = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(2), "CORE", 0u));

            auto comA = spawn(sim, "COMMANDER", a, 2, 2);
            spawn(sim, "COMMANDER", b, 8, 2);
            spawn(sim, "COMMANDER", c, 12, 2);

            REQUIRE(sim.areAllied(b, c));
            REQUIRE(!sim.areAllied(a, b));
            REQUIRE(sim.areAllied(a, a));

            // two alliances still stand
            REQUIRE(std::holds_alternative<WinStatusUndecided>(sim.computeWinStatus()));

            // allied survivors are one alliance: kill A -> the BC
            // alliance wins even though two players remain
            sim.killUnit(comA);
            sim.processVictoryCondition();
            REQUIRE(sim.getPlayer(a).status == GamePlayerStatus::Dead);
            auto status = sim.computeWinStatus();
            REQUIRE(std::holds_alternative<WinStatusWon>(status));
            auto winner = std::get<WinStatusWon>(status).winner;
            REQUIRE((winner == b || winner == c));
        }

        SECTION("allies are not sensed as enemies and share sight")
        {
            auto sim = makeSim(64, 32);
            registerDef(sim, tank);

            auto a = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(0), "ARM", 1u));
            auto b = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(1), "CORE", 1u));
            auto e = sim.addPlayer(makePlayerWithGroup(PlayerColorIndex(2), "CORE", std::nullopt));

            auto tankB = spawn(sim, "TANK", b, 5, 5);
            auto tankE = spawn(sim, "TANK", e, 7, 7);

            // allied units are always LOS-visible to each other and
            // never land in any contact set
            sim.updateVisibility();
            REQUIRE(sim.isLosVisibleToPlayer(tankB, a));
            auto& vis = sim.playerVisibility[a.value];
            REQUIRE(vis.losVisibleUnits.count(tankB) == 0);
            REQUIRE(vis.radarDetectedUnits.count(tankB) == 0);
            REQUIRE(vis.sonarDetectedUnits.count(tankB) == 0);
            (void)tankE;
        }
    }
}
