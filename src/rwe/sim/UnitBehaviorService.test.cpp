#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <rwe/LoadingScene_util.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/sim/FeatureDefinition.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/MissionObjectives.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/sim/util.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace fs = std::filesystem;

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0);
        }

        UnitDefinition makeBuilderDefinition(const std::string& name, unsigned int buildTime)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1024);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 1000;
            def.builder = true;
            def.workerTimePerTick = 10;
            def.buildDistance = SimScalar(300);
            def.buildTime = buildTime;
            // generous storage so reclaim income is never clamped by capacity
            def.metalStorage = Metal(1000000);
            def.energyStorage = Energy(1000000);
            return def;
        }

        FeatureDefinition makeFeatureDefinition(const std::string& name, unsigned int metal, unsigned int energy)
        {
            FeatureDefinition def{};
            def.name = name;
            def.footprintX = 2;
            def.footprintZ = 2;
            def.height = SimScalar(10);
            def.reclaimable = true;
            def.metal = metal;
            def.energy = energy;
            def.blocking = true;
            def.damage = 100;
            return def;
        }

        UnitDefinition makeTargetDefinition()
        {
            UnitDefinition def{};
            def.unitName = "damaged";
            def.objectName = "DAMAGED";
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1024);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 500;
            def.builder = false;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }

        UnitDefinition makeBigTargetDefinition()
        {
            auto def = makeTargetDefinition();
            def.unitName = "bigtarget";
            def.objectName = "BIGTARGET";
            // 6x6 footprint: a short-range builder cannot reach its center
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{6, 6, 0, 0, 0, 0};
            return def;
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

        struct Fixture
        {
            GameSimulation sim;
            PlayerId playerId;
            PlayerId enemyPlayerId;
            FeatureDefinitionId wreckDefId;
            FeatureDefinitionId balancedDefId;
            FeatureDefinitionId scrapDefId;
            FeatureDefinitionId smudgeDefId;
            FeatureDefinitionId noReclaimDefId;
            FeatureDefinitionId bigWreckDefId;

            Fixture()
                : sim(makeSim())
            {
                // navigation code logs; the test binary has no custom main
                // and other test cases clear the global logger, so install
                // a file logger on every fixture construction
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

                // an ordinary construction unit whose own build time is far
                // shorter than the target's, and a commander whose build time
                // is far longer -- construction continuation must not depend
                // on the builder's buildTime
                sim.unitDefinitions.insert_or_assign("REPAIRER", makeBuilderDefinition("REPAIRER", 10));
                auto commanderDef = makeBuilderDefinition("COMMANDER", 5000);
                commanderDef.commander = true;
                // stock truth: CanCapture=1 only on commanders; the same
                // flag on a target grants capture immunity
                commanderDef.canCapture = true;
                sim.unitDefinitions.insert_or_assign("COMMANDER", commanderDef);
                auto shortArm = makeBuilderDefinition("SHORTARM", 10);
                shortArm.buildDistance = SimScalar(60);
                sim.unitDefinitions.insert_or_assign("SHORTARM", shortArm);
                sim.unitDefinitions.insert_or_assign("DAMAGED", makeTargetDefinition());
                sim.unitDefinitions.insert_or_assign("BIGTARGET", makeBigTargetDefinition());
                auto factoryDef = makeTargetDefinition();
                factoryDef.unitName = "factory";
                factoryDef.objectName = "FACTORY";
                factoryDef.isMobile = false;
                factoryDef.canMove = false;
                factoryDef.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Ground);
                sim.unitDefinitions.insert_or_assign("FACTORY", factoryDef);
                // CLOAKER: the stock cloak block -- capability derives
                // from CloakCost > 0; sight=200 lets it detect its own
                // neighbours for the proximity-suppression tests
                auto cloakerDef = makeTargetDefinition();
                cloakerDef.unitName = "cloaker";
                cloakerDef.objectName = "CLOAKER";
                cloakerDef.cloakCost = 100.0f;
                cloakerDef.cloakCostMoving = 400.0f;
                cloakerDef.minCloakDistance = SimScalar(40);
                cloakerDef.sightDistance = SimScalar(200);
                // resource pools are clamped to the summed storage caps
                // every second -- without storage the cloak debit would
                // always starve
                cloakerDef.energyStorage = Energy(1000000);
                cloakerDef.metalStorage = Metal(1000000);
                sim.unitDefinitions.insert_or_assign("CLOAKER", cloakerDef);
                for (const auto& name : {"REPAIRER", "COMMANDER", "SHORTARM", "DAMAGED", "BIGTARGET", "FACTORY", "CLOAKER"})
                {
                    sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                    sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
                }

                playerId = sim.addPlayer(makePlayer());
                auto enemyInfo = makePlayer();
                enemyInfo.color = PlayerColorIndex(1);
                enemyPlayerId = sim.addPlayer(enemyInfo);

                // WRECK: metal-only corpse. BALANCED: even split for clean
                // per-tick credit assertions. SMUDGE: non-reclaimable remnant
                // left behind by SCRAP. NORECLAIM: looks juicy but refuses.
                wreckDefId = sim.featureDefinitions.insert(makeFeatureDefinition("WRECK", 90, 0));
                balancedDefId = sim.featureDefinitions.insert(makeFeatureDefinition("BALANCED", 60, 60));
                auto scrap = makeFeatureDefinition("SCRAP", 40, 0);
                scrapDefId = sim.featureDefinitions.insert(scrap);
                smudgeDefId = sim.featureDefinitions.insert(makeFeatureDefinition("SMUDGE", 0, 0));
                sim.featureDefinitions.get(scrapDefId).featureReclamate = smudgeDefId;
                noReclaimDefId = sim.featureDefinitions.insert(makeFeatureDefinition("NORECLAIM", 90, 0));
                sim.featureDefinitions.get(noReclaimDefId).reclaimable = false;
                bigWreckDefId = sim.featureDefinitions.insert(makeFeatureDefinition("BIGWRECK", 60, 0));
                sim.featureDefinitions.get(bigWreckDefId).footprintX = 6;
                sim.featureDefinitions.get(bigWreckDefId).footprintZ = 6;
            }

            FeatureId spawnFeature(FeatureDefinitionId defId, int x = 12, int y = 8)
            {
                auto id = sim.addFeature(defId, x, y);
                REQUIRE(id);
                return *id;
            }

            UnitId spawnBuilder(const std::string& unitType = "REPAIRER")
            {
                auto id = sim.trySpawnUnit(unitType, playerId, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
                REQUIRE(id);
                // units only process orders once construction has finished
                sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(unitType));
                return *id;
            }

            UnitId spawnTarget(PlayerId owner, bool complete = true, const std::string& unitType = "DAMAGED", int x = 12, int y = 8)
            {
                auto id = sim.trySpawnUnit(unitType, owner, sim.terrain.heightmapIndexToWorldCenter(x, y), std::nullopt);
                REQUIRE(id);
                if (complete)
                {
                    auto& unit = sim.getUnitState(*id);
                    unit.finishBuilding(sim.unitDefinitions.at(unitType));
                }
                return *id;
            }

            bool hasRepairStartedEvent() const
            {
                for (const auto& event : sim.events)
                {
                    if (std::holds_alternative<UnitStartedRepairingEvent>(event))
                    {
                        return true;
                    }
                }
                return false;
            }

            bool hasReclaimStartedEvent() const
            {
                for (const auto& event : sim.events)
                {
                    if (std::holds_alternative<UnitStartedReclaimingEvent>(event))
                    {
                        return true;
                    }
                }
                return false;
            }

            const UnitState* findUnitByTypeExcept(const std::string& unitType, UnitId except) const
            {
                for (const auto& [id, unit] : sim.units)
                {
                    if (unit.unitType == unitType && id != except)
                    {
                        return &unit;
                    }
                }
                return nullptr;
            }

            UnitId findUnitIdByTypeExcept(const std::string& unitType, UnitId except) const
            {
                for (const auto& [id, unit] : sim.units)
                {
                    if (unit.unitType == unitType && id != except)
                    {
                        return id;
                    }
                }
                throw std::logic_error("unit not found: " + unitType);
            }

            unsigned int countUnitsOfType(const std::string& unitType) const
            {
                unsigned int count = 0;
                for (const auto& [id, unit] : sim.units)
                {
                    if (unit.unitType == unitType)
                    {
                        ++count;
                    }
                }
                return count;
            }

            unsigned int countUnits() const
            {
                unsigned int count = 0;
                for (const auto& [id, unit] : sim.units)
                {
                    ++count;
                }
                return count;
            }
        };
    }

    TEST_CASE("repair order")
    {
        SECTION("repairs a damaged friendly unit to full health")
        {
            // repair must work for both commanders and ordinary builders
            for (const auto& builderType : {"REPAIRER", "COMMANDER"})
            {
                Fixture f;
                auto builderId = f.spawnBuilder(builderType);
                auto targetId = f.spawnTarget(f.playerId);

                auto& target = f.sim.getUnitState(targetId);
                target.hitPoints = 300;
                REQUIRE(target.isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")) == false);

                f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));

                // first tick enters the repairing state and deploys the build arm
                f.sim.tick();
                REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(f.sim.getUnitState(builderId).behaviourState));
                REQUIRE(target.hitPoints == 300);

                // the COB StartBuilding script signals the build stance
                f.sim.getUnitState(builderId).inBuildStance = true;

                // repair restores 50 hit points per tick at worker time 10
                f.sim.tick();
                REQUIRE(target.hitPoints == 350);
                REQUIRE(f.hasRepairStartedEvent());
                f.sim.tick();
                REQUIRE(target.hitPoints == 400);
                f.sim.tick();
                REQUIRE(target.hitPoints == 450);
                f.sim.tick();
                REQUIRE(target.hitPoints == 500);

                // hit points are capped exactly at maximum and the order completes
                REQUIRE(f.sim.getUnitState(builderId).orders.empty());
                REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));

                // consumed the damaged fraction of the build cost
                const auto& player = f.sim.getPlayer(f.playerId);
                REQUIRE(player.actualEnergyConsumptionBuffer == Energy(400));
                REQUIRE(player.actualMetalConsumptionBuffer == Metal(200));

                // no further resource consumption after completion
                f.sim.tick();
                REQUIRE(player.actualEnergyConsumptionBuffer == Energy(400));
                REQUIRE(player.actualMetalConsumptionBuffer == Metal(200));
            }
        }

        SECTION("rejects an enemy target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            f.sim.getUnitState(targetId).hitPoints = 300;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 300);
            REQUIRE(f.sim.getPlayer(f.playerId).actualEnergyConsumptionBuffer == Energy(0));
        }

        SECTION("completes safely on a dead target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.getUnitState(targetId).hitPoints = 300;
            f.sim.getUnitState(targetId).markAsDead();

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
        }

        SECTION("does not repair a unit still under construction")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, false);

            auto& target = f.sim.getUnitState(targetId);
            REQUIRE(target.isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();

            // the order completes without doing construction work
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(target.buildTimeCompleted == 0);
            REQUIRE(target.hitPoints == 0);
        }

        SECTION("completes on a full-health target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 500);
            REQUIRE(f.sim.getPlayer(f.playerId).actualEnergyConsumptionBuffer == Energy(0));
        }

        SECTION("waits when resources are stalled")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.getUnitState(targetId).hitPoints = 300;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            f.sim.getPlayer(f.playerId).metalStalled = true;
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 300);
            REQUIRE(f.sim.getPlayer(f.playerId).actualMetalConsumptionBuffer == Metal(0));
            REQUIRE_FALSE(f.sim.getUnitState(builderId).orders.empty());
        }

        SECTION("a non-builder cannot repair")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.getUnitState(targetId).hitPoints = 300;

            // the target orders a repair back at the builder (it is not a builder)
            f.sim.getUnitState(targetId).addOrder(RepairOrder(builderId));
            f.sim.getUnitState(builderId).hitPoints = 500;
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(targetId).orders.empty());
            REQUIRE(f.sim.getUnitState(builderId).hitPoints == 500);
        }

        SECTION("repairs a large-footprint target from its perimeter")
        {
            // regression: working range is measured to the target's footprint
            // edge, not its center. SHORTARM buildDistance=60; the 6x6 target
            // sits with center ~96 away (>60) but perimeter ~48 away (<=60).
            Fixture f;
            auto builderId = f.spawnBuilder("SHORTARM");
            auto targetId = f.spawnTarget(f.playerId, true, "BIGTARGET", 14, 8);

            auto& target = f.sim.getUnitState(targetId);
            target.hitPoints = 300;

            // sanity: center distance really does exceed buildDistance
            const auto& builder = f.sim.getUnitState(builderId);
            REQUIRE(builder.position.distanceSquared(target.position) > SimScalar(60) * SimScalar(60));

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            REQUIRE(target.hitPoints == 350);
        }

        SECTION("construction assistance uses footprint-edge working range")
        {
            Fixture f;
            auto builderId = f.spawnBuilder("SHORTARM");
            auto targetId = f.spawnTarget(f.playerId, false, "BIGTARGET", 14, 8);

            auto& target = f.sim.getUnitState(targetId);
            target.buildTimeCompleted = 50;
            target.hitPoints = 250;

            f.sim.getUnitState(builderId).addOrder(CompleteBuildOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateBuilding>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            REQUIRE(target.buildTimeCompleted == 60);
        }

        SECTION("construction assistance resumes regardless of builder build time")
        {
            // regression: buildExistingUnit previously evaluated the target's
            // isBeingBuilt against the builder's definition, so builders whose
            // own buildTime was shorter than the target's progress could not
            // resume construction. Target has buildTimeCompleted=50; the
            // ordinary builder's buildTime is 10, the commander's is 5000.
            for (const auto& builderType : {"REPAIRER", "COMMANDER"})
            {
                Fixture f;
                auto builderId = f.spawnBuilder(builderType);
                auto targetId = f.spawnTarget(f.playerId, false);

                auto& target = f.sim.getUnitState(targetId);
                target.buildTimeCompleted = 50;
                target.hitPoints = 250;
                REQUIRE(target.isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));

                f.sim.getUnitState(builderId).addOrder(CompleteBuildOrder(targetId));
                f.sim.tick();
                REQUIRE(std::holds_alternative<UnitBehaviorStateBuilding>(f.sim.getUnitState(builderId).behaviourState));

                f.sim.getUnitState(builderId).inBuildStance = true;
                f.sim.tick();

                REQUIRE(target.buildTimeCompleted == 60);
                REQUIRE(target.hitPoints == 300);
            }
        }
    }

    TEST_CASE("reclaim order")
    {
        SECTION("reclaims a feature and credits its full resource pool")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);

            // sink the player's stocks so income is observable
            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));

            // first tick enters the reclaiming state
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;

            // WRECK has metal=90, energy=0; workerTimePerTick=10 drains
            // the 90-unit pool in 9 working ticks
            f.sim.tick();
            REQUIRE(f.hasReclaimStartedEvent());
            for (int i = 0; i < 8; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(!f.sim.tryGetFeature(featureId));

            // the whole pool was credited, exactly once
            // (income accumulates in the production buffer until the
            // once-per-second resource settle)
            REQUIRE(player.metalProductionBuffer == Metal(90));
            REQUIRE(player.energyProductionBuffer == Energy(0));

            // no further income after completion
            f.sim.tick();
            REQUIRE(player.metalProductionBuffer == Metal(90));
        }

        SECTION("credits resources progressively each tick")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.balancedDefId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            // BALANCED has metal=60, energy=60; at 10 work/tick the pool of
            // 120 drains over 12 ticks crediting 5 metal + 5 energy each tick
            f.sim.tick();
            REQUIRE(player.metalProductionBuffer == Metal(5));
            REQUIRE(player.energyProductionBuffer == Energy(5));
            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 10);

            f.sim.tick();
            REQUIRE(player.metalProductionBuffer == Metal(10));
            REQUIRE(player.energyProductionBuffer == Energy(10));

            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(player.metalProductionBuffer == Metal(60));
            REQUIRE(player.energyProductionBuffer == Energy(60));
            REQUIRE(!f.sim.tryGetFeature(featureId));
        }

        SECTION("rejects a non-reclaimable feature")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.noReclaimDefId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();

            // the order completes immediately and the feature is untouched
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(f.sim.tryGetFeature(featureId));
            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 0);
        }

        SECTION("rejects reclaim for a non-builder")
        {
            Fixture f;
            auto targetId = f.spawnTarget(f.playerId);
            auto featureId = f.spawnFeature(f.wreckDefId, 20, 8);

            f.sim.getUnitState(targetId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(targetId).orders.empty());
            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 0);
        }

        SECTION("leaves a featurereclamate remnant at completion")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.scrapDefId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            // SCRAP pool is 40; 4 working ticks
            for (int i = 0; i < 4; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetFeature(featureId));

            // the remnant feature occupies the same footprint
            bool foundRemnant = false;
            for (const auto& [id, feature] : f.sim.features)
            {
                if (feature.featureName == f.smudgeDefId)
                {
                    foundRemnant = true;
                    auto cell = f.sim.occupiedGrid.tryGet(
                        f.sim.terrain.worldToHeightmapCoordinate(feature.position));
                    REQUIRE(cell);
                    REQUIRE(cell->get().featureId == id);
                }
            }
            REQUIRE(foundRemnant);
        }

        SECTION("keeps partial progress across STOP and resumes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            // 4 working ticks: 40/90 progress, 40 metal credited
            for (int i = 0; i < 4; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 40);
            REQUIRE(player.metalProductionBuffer == Metal(40));

            // STOP -- the order is cancelled mid-reclaim
            f.sim.getUnitState(builderId).clearOrders();
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));

            // partial progress persists on the feature, not the unit
            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 40);
            REQUIRE(player.metalProductionBuffer == Metal(40));

            // a fresh order resumes from the partial state
            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));
            f.sim.getUnitState(builderId).inBuildStance = true;
            for (int i = 0; i < 5; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetFeature(featureId));
            REQUIRE(player.metalProductionBuffer == Metal(90));
        }

        SECTION("completes safely when the target disappears before arrival")
        {
            Fixture f;
            auto builderId = f.spawnBuilder("SHORTARM");
            auto featureId = f.spawnFeature(f.wreckDefId, 40, 40);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(builderId).orders.size() == 1);

            f.sim.removeFeature(featureId);
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
        }

        SECTION("completes safely when the target disappears mid-reclaim")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.removeFeature(featureId);
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
        }

        SECTION("two builders share a pool without duplicating resources")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto builderId2 = f.sim.trySpawnUnit("COMMANDER", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(8, 10), std::nullopt);
            REQUIRE(builderId2);
            f.sim.getUnitState(*builderId2).finishBuilding(f.sim.unitDefinitions.at("COMMANDER"));
            auto featureId = f.spawnFeature(f.balancedDefId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.getUnitState(*builderId2).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.getUnitState(*builderId2).inBuildStance = true;

            // both drain the same 120-unit pool at 10/tick each -> 6 ticks
            for (int i = 0; i < 6; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetFeature(featureId));
            // the pool is split by contribution but never exceeded
            REQUIRE(player.metalProductionBuffer == Metal(60));
            REQUIRE(player.energyProductionBuffer == Energy(60));
        }

        SECTION("short-range builder reclaims via footprint edge")
        {
            Fixture f;
            auto builderId = f.spawnBuilder("SHORTARM");
            // 6x6 footprint: center is out of range, edge is in range
            auto featureId = f.spawnFeature(f.bigWreckDefId, 11, 8);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();

            REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone > 0);
        }

        SECTION("a stale feature id is safe")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);
            f.sim.removeFeature(featureId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
        }

        SECTION("queued reclaim orders run in sequence")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);
            auto featureId2 = f.spawnFeature(f.wreckDefId, 20, 8);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId2));

            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            for (int i = 0; i < 9; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetFeature(featureId));
            REQUIRE(f.sim.getUnitState(builderId).orders.size() == 1);

            // the second feature is reclaimed next
            for (int i = 0; i < 12; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetFeature(featureId2));
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(player.metalProductionBuffer == Metal(180));
        }
    }

    TEST_CASE("campaign difficulty computer economy payments")
    {
        // TotalA.exe scales every contribution into a computer-owned
        // unit's production accumulators by the staged difficulty
        // (Easy x0.5, Medium x0.7, Hard x1.0): resource income,
        // reclaim credits and nanolathe-class build/repair payments.
        // The work itself -- repair hit points, build progress,
        // reclaim ticks -- is not scaled.
        auto addComputerPlayer = [](Fixture& f) {
            auto info = makePlayer();
            info.type = GamePlayerType::Computer;
            info.color = PlayerColorIndex(9);
            return f.sim.addPlayer(info);
        };

        SECTION("a computer repairer's payment is scaled while the repair rate is not")
        {
            for (const auto& [difficulty, scale] : std::vector<std::pair<std::string, float>>{
                     {"Easy", 0.5f}, {"Medium", 0.7f}, {"Hard", 1.0f}})
            {
                CAPTURE(difficulty);
                Fixture f;
                f.sim.computerResourceScale = scale;
                auto computerId = addComputerPlayer(f);
                auto builderId = f.spawnTarget(computerId, true, "REPAIRER", 8, 8);
                auto targetId = f.spawnTarget(computerId);

                auto& target = f.sim.getUnitState(targetId);
                target.hitPoints = 300;

                f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
                f.sim.tick();
                f.sim.getUnitState(builderId).inBuildStance = true;

                // the repair rate is unchanged: 50 hit points per
                // working tick regardless of the coefficient
                f.sim.tick();
                REQUIRE(target.hitPoints == 350);
                for (int i = 0; i < 3; ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(target.hitPoints == 500);

                // but the nanolathe payment is scaled
                const auto& player = f.sim.getPlayer(computerId);
                REQUIRE(player.actualEnergyConsumptionBuffer == Energy(400 * scale));
                REQUIRE(player.actualMetalConsumptionBuffer == Metal(200 * scale));
            }
        }

        SECTION("a human repairer's payment is unscaled")
        {
            Fixture f;
            f.sim.computerResourceScale = 0.5f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            auto& target = f.sim.getUnitState(targetId);
            target.hitPoints = 300;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            for (int i = 0; i < 4; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(target.hitPoints == 500);

            const auto& player = f.sim.getPlayer(f.playerId);
            REQUIRE(player.actualEnergyConsumptionBuffer == Energy(400));
            REQUIRE(player.actualMetalConsumptionBuffer == Metal(200));
        }

        SECTION("a computer builder's construction payment is scaled while build progress is not")
        {
            for (const auto& [difficulty, scale] : std::vector<std::pair<std::string, float>>{
                     {"Easy", 0.5f}, {"Medium", 0.7f}, {"Hard", 1.0f}})
            {
                CAPTURE(difficulty);
                Fixture f;
                f.sim.computerResourceScale = scale;
                auto computerId = addComputerPlayer(f);
                auto builderId = f.spawnTarget(computerId, true, "REPAIRER", 8, 8);
                auto targetId = f.spawnTarget(computerId, false, "DAMAGED", 14, 8);

                auto& target = f.sim.getUnitState(targetId);
                target.buildTimeCompleted = 50;
                target.hitPoints = 250;

                f.sim.getUnitState(builderId).addOrder(CompleteBuildOrder(targetId));
                f.sim.tick();
                f.sim.getUnitState(builderId).inBuildStance = true;

                // build progress advances at the same 10 build-time
                // units per working tick at every coefficient
                f.sim.tick();
                REQUIRE(target.buildTimeCompleted == 60);
                for (int i = 0; i < 4; ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(target.buildTimeCompleted == 100);
                REQUIRE(!target.isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));

                // the nanolathe draw accumulates the scaled desired
                // rate: 100 energy / 50 metal per tick over 5 ticks
                const auto& player = f.sim.getPlayer(computerId);
                REQUIRE(player.desiredEnergyConsumptionBuffer == Energy(500 * scale));
                REQUIRE(player.desiredMetalConsumptionBuffer == Metal(250 * scale));
            }
        }

        SECTION("a computer builder's reclaim credit is scaled while reclaim progress is not")
        {
            for (const auto& [difficulty, scale] : std::vector<std::pair<std::string, float>>{
                     {"Easy", 0.5f}, {"Medium", 0.7f}, {"Hard", 1.0f}})
            {
                CAPTURE(difficulty);
                Fixture f;
                f.sim.computerResourceScale = scale;
                auto computerId = addComputerPlayer(f);
                auto builderId = f.spawnTarget(computerId, true, "REPAIRER", 8, 8);
                auto featureId = f.spawnFeature(f.wreckDefId);

                auto& player = f.sim.getPlayer(computerId);
                player.metal = Metal(0);
                player.energy = Energy(0);

                f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
                f.sim.tick();
                f.sim.getUnitState(builderId).inBuildStance = true;

                // WRECK holds 90 metal; reclaim work advances 10/tick
                // unscaled while the credited metal is scaled
                f.sim.tick();
                REQUIRE(f.sim.getFeature(featureId).reclaimWorkDone == 10);
                REQUIRE(player.metalProductionBuffer == Metal(10 * scale));

                for (int i = 0; i < 8; ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(!f.sim.tryGetFeature(featureId));
                REQUIRE(player.metalProductionBuffer == Metal(90 * scale));
            }
        }

        SECTION("a human builder's reclaim credit is unscaled")
        {
            Fixture f;
            f.sim.computerResourceScale = 0.5f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            for (int i = 0; i < 9; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetFeature(featureId));
            REQUIRE(player.metalProductionBuffer == Metal(90));
        }
    }

    TEST_CASE("reclaim unit order")
    {
        SECTION("reclaims an own completed unit and returns its full build cost")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            REQUIRE(f.hasReclaimStartedEvent());

            // DAMAGED: 500 HP, cost 500M/1000E; at 10 work/tick the unit
            // drains 10 HP per tick and is consumed after 50 working ticks
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 480);

            for (int i = 0; i < 20; ++i)
            {
                f.sim.tick();
            }
            // nothing is paid while the unit is still being drained --
            // the refund is a lump sum at completion
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 0);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);

            for (int i = 0; i < 28; ++i)
            {
                f.sim.tick();
            }

            // the unit is consumed: dead, swept, and no corpse left behind
            REQUIRE(!f.sim.tryGetUnitState(targetId));
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));

            // full metal cost paid out once at completion; no energy
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 500);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);

            // no further income after the unit is gone
            f.sim.tick();
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 500);
        }

        SECTION("reclaims an enemy completed structure")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "FACTORY");

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints < 500);
        }

        SECTION("reclaims an own completed structure")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, true, "FACTORY");

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints < 500);
        }

        SECTION("reclaims an enemy completed mobile unit")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED");

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints < 500);
        }

        SECTION("rejects a commander target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "COMMANDER");

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();

            // the order is rejected cleanly and the commander is untouched
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 1000);
            REQUIRE(f.sim.getPlayer(f.enemyPlayerId).status == GamePlayerStatus::Alive);
        }

        SECTION("reclaims a unit still under construction but pays nothing")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, false);

            // a half-built frame: 50/100 build progress, 250/500 HP
            f.sim.getUnitState(targetId).buildTimeCompleted = 50;
            f.sim.getUnitState(targetId).hitPoints = 250;

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            f.sim.getUnitState(builderId).inBuildStance = true;

            // reclaim drains the frame's partial HP but yields no resources
            for (int i = 0; i < 25; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetUnitState(targetId));
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 0);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);
        }

        SECTION("a damaged target completes faster but pays the same metal")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            // 250/500 HP: half the drain time, identical yield
            f.sim.getUnitState(targetId).hitPoints = 250;

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            for (int i = 0; i < 25; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetUnitState(targetId));
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 500);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);
        }

        SECTION("keeps drained progress on the target across STOP and resumes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;

            // 20 working ticks: 200 HP drained, nothing paid yet --
            // the lump sum only lands at completion
            for (int i = 0; i < 20; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 300);
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 0);

            // STOP cancels the order; the target keeps its reduced HP
            f.sim.getUnitState(builderId).clearOrders();
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 300);

            // a fresh order drains the rest; the full metal cost is paid
            // once at completion regardless of the interrupted drain
            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetUnitState(targetId));
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 500);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);
        }

        SECTION("stops reclaiming safely when the target dies mid-reclaim")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));

            // killed by weapons mid-reclaim: no reclaim payout is awarded
            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.killUnit(targetId);
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 0);
        }

        SECTION("stops draining while a moved target is out of range and resumes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 480);

            // the target drives far away; the builder chases without draining
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            for (int i = 0; i < 5; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(builderId).orders.size() == 1);
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 480);
            REQUIRE(!f.sim.getUnitState(builderId).getActiveNanolatheTarget());

            // the target stops in range again; reclaim resumes
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(12, 8);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints < 480);
        }

        SECTION("survives the builder dying mid-reclaim")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 480);

            f.sim.killUnit(builderId);
            f.sim.tick();

            // the target keeps its partially-drained hit points
            REQUIRE(f.sim.tryGetUnitState(targetId));
            REQUIRE(f.sim.getUnitState(targetId).hitPoints > 0);
            REQUIRE(f.sim.getUnitState(targetId).hitPoints <= 480);
        }

        SECTION("two builders share one target without duplicating resources")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto builderId2 = f.sim.trySpawnUnit("REPAIRER", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(8, 10), std::nullopt);
            REQUIRE(builderId2);
            f.sim.getUnitState(*builderId2).finishBuilding(f.sim.unitDefinitions.at("REPAIRER"));
            auto targetId = f.spawnTarget(f.playerId);

            auto& player = f.sim.getPlayer(f.playerId);
            player.metal = Metal(0);
            player.energy = Energy(0);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            f.sim.getUnitState(*builderId2).addOrder(ReclaimOrder(targetId));
            f.sim.tick();
            f.sim.getUnitState(builderId).inBuildStance = true;
            f.sim.getUnitState(*builderId2).inBuildStance = true;

            // both drain 10 HP/tick each -> the 500 HP unit is consumed in 25 ticks
            for (int i = 0; i < 25; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.sim.tryGetUnitState(targetId));
            // whichever builder lands the final drain takes the lump sum;
            // the payout is awarded exactly once regardless of participation
            REQUIRE(player.metal.value + player.metalProductionBuffer.value == 500);
            REQUIRE(player.energy.value + player.energyProductionBuffer.value == 0);
        }

        SECTION("a stale unit id is safe")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(UnitId(9999)));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
        }

        SECTION("a unit cannot reclaim itself")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(builderId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(f.sim.getUnitState(builderId).hitPoints == 1000);
        }
    }

    TEST_CASE("capture order")
    {
        SECTION("captures an enemy completed unit through the production path")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateCapturing>(f.sim.getUnitState(capturerId).behaviourState));

            // DAMAGED: E=1000 M=500 hp=500/500 -> base=min(1800,
            // 15+107.14+150)=272, hp-scaled 272, needed=544 at 2 work
            // per 6 ticks -> 1632 working ticks
            const auto& order = std::get<CaptureOrder>(f.sim.getUnitState(capturerId).orders.front());
            REQUIRE(order.workRequired == 544);

            for (int i = 0; i < 1600; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId));

            for (int i = 0; i < 40; ++i)
            {
                f.sim.tick();
            }
            // ownership transferred exactly once, order completed
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.playerId));
            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(capturerId).behaviourState));

            // volatile state reset: the captured unit idles empty
            const auto& target = f.sim.getUnitState(targetId);
            REQUIRE(target.isAlive());
            REQUIRE(target.hitPoints == 500);
            REQUIRE(target.orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(target.behaviourState));

            // selectability follows the new owner
            REQUIRE(target.isSelectableBy(f.sim.unitDefinitions.at(target.unitType), f.playerId));
            REQUIRE(!target.isSelectableBy(f.sim.unitDefinitions.at(target.unitType), f.enemyPlayerId));
        }

        SECTION("rejects capture by a unit without CanCapture")
        {
            Fixture f;
            auto builderId = f.spawnBuilder("REPAIRER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(builderId).addOrder(CaptureOrder(targetId));
            f.sim.tick();

            // the order drops immediately; the target is untouched
            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId));
        }

        SECTION("rejects a capture-capable (commander) target")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "COMMANDER");

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId));
        }

        SECTION("rejects an unfinished target")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId, false);

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId));
        }

        SECTION("rejects an own unit as a no-op")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.playerId);

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.playerId));
        }

        SECTION("cannot capture itself")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(capturerId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
        }

        SECTION("completes safely when the target dies mid-capture")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(std::holds_alternative<UnitBehaviorStateCapturing>(f.sim.getUnitState(capturerId).behaviourState));

            f.sim.killUnit(targetId);
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(capturerId).behaviourState));
        }

        SECTION("damaged targets capture faster per the retail HP factor")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            f.sim.getUnitState(targetId).hitPoints = 100;

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.tick();

            // hp=100/500 -> needed = 272 * (100+500)/1000 * 2 = 326
            const auto& order = std::get<CaptureOrder>(f.sim.getUnitState(capturerId).orders.front());
            REQUIRE(order.workRequired == 326);
        }

        SECTION("progress survives chasing a moved target but is lost with the order")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
            }
            const auto* order = std::get_if<CaptureOrder>(&f.sim.getUnitState(capturerId).orders.front());
            REQUIRE(order);
            auto progressBefore = order->progress;
            REQUIRE(progressBefore > 0);

            // moving the target out of range keeps the order (and its
            // progress) alive while the capturer chases
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(60, 60);
            f.sim.tick();
            REQUIRE(!f.sim.getUnitState(capturerId).orders.empty());
            REQUIRE(std::get<CaptureOrder>(f.sim.getUnitState(capturerId).orders.front()).progress == progressBefore);

            // cancelling the order discards progress; a fresh order
            // restarts from zero
            f.sim.getUnitState(capturerId).clearOrders();
            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(12, 8);
            f.sim.tick();
            const auto* fresh = std::get_if<CaptureOrder>(&f.sim.getUnitState(capturerId).orders.front());
            REQUIRE(fresh);
            REQUIRE(fresh->progress == 0);
        }

        SECTION("transfer clears the target's orders")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            // the target is mid-attack on a fellow enemy unit; after
            // capture its own order queue dies with the retail respawn
            auto enemy2 = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(targetId).addOrder(AttackOrder(enemy2));

            // shrink work for a fast transfer
            f.sim.unitDefinitions.at("DAMAGED").buildCostEnergy = Energy(0);
            f.sim.unitDefinitions.at("DAMAGED").buildCostMetal = Metal(0);
            // needed = min(1800, 150) * hp factor * 2 = 300 -> 900 ticks
            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }

            const auto& target = f.sim.getUnitState(targetId);
            REQUIRE(target.isOwnedBy(f.playerId));
            REQUIRE(target.orders.empty());
        }

        SECTION("capturing a unit drops enemy orders targeting it")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            auto attackerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(attackerId).addOrder(AttackOrder(targetId));

            f.sim.unitDefinitions.at("DAMAGED").buildCostEnergy = Energy(0);
            f.sim.unitDefinitions.at("DAMAGED").buildCostMetal = Metal(0);
            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.playerId));
            // the attacker's order died with the retail respawn
            REQUIRE(f.sim.getUnitState(attackerId).orders.empty());
        }

        SECTION("production capture satisfies a CaptureUnitType objective")
        {
            Fixture f;
            MissionInfo info;
            info.localPlayerId = f.playerId;
            info.objectives = {MissionObjective{"CaptureUnitType", {"DAMAGED"}}};
            f.sim.missionInfo = std::move(info);
            f.sim.initMissionObjectives();

            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.unitDefinitions.at("DAMAGED").buildCostEnergy = Energy(0);
            f.sim.unitDefinitions.at("DAMAGED").buildCostMetal = Metal(0);
            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.playerId));
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);
        }

        SECTION("an enemy capture does not satisfy a CaptureUnitType victory")
        {
            Fixture f;
            MissionInfo info;
            info.localPlayerId = f.playerId;
            info.objectives = {MissionObjective{"CaptureUnitType", {"DAMAGED"}}};
            f.sim.missionInfo = std::move(info);
            f.sim.initMissionObjectives();

            // the enemy captures one of our units: previousOwner is the
            // local slot, not the enemy slot -> no latch
            auto enemyCapturer = f.spawnTarget(f.enemyPlayerId, true, "COMMANDER", 20, 8);
            auto ownTarget = f.spawnTarget(f.playerId);

            f.sim.unitDefinitions.at("DAMAGED").buildCostEnergy = Energy(0);
            f.sim.unitDefinitions.at("DAMAGED").buildCostMetal = Metal(0);
            f.sim.getUnitState(enemyCapturer).addOrder(CaptureOrder(ownTarget));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getUnitState(ownTarget).isOwnedBy(f.enemyPlayerId));
            REQUIRE(!f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);
        }

        SECTION("recapture does not clear a latched objective")
        {
            Fixture f;
            MissionInfo info;
            info.localPlayerId = f.playerId;
            info.objectives = {MissionObjective{"CaptureUnitType", {"DAMAGED"}}};
            f.sim.missionInfo = std::move(info);
            f.sim.initMissionObjectives();

            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);

            f.sim.unitDefinitions.at("DAMAGED").buildCostEnergy = Energy(0);
            f.sim.unitDefinitions.at("DAMAGED").buildCostMetal = Metal(0);
            f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);

            // the enemy takes it back; the latch stays satisfied
            auto enemyCapturer = f.spawnTarget(f.enemyPlayerId, true, "COMMANDER", 14, 8);
            f.sim.getUnitState(enemyCapturer).addOrder(CaptureOrder(targetId));
            for (int i = 0; i < 950; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId));
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);
        }
    }

    TEST_CASE("nanolathe emission only ticks where work applies")
    {
        // Retail's emitter call sites sit inside the work handlers behind
        // their resource/range gates (TotalA.exe e.g. 0x403e0c: the
        // addScaledResourceDelta result jumps over the emit at 0x403ecc).
        // getActiveNanolatheTarget() therefore reports active only on ticks
        // where a handler actually applied work -- never while chasing,
        // waiting for stance, stalled on resources, or holding an orphaned
        // work state.

        SECTION("build: active while working, inactive on every stop path")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, false);
            auto& target = f.sim.getUnitState(targetId);
            target.buildTimeCompleted = 50;

            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;
            builder.addOrder(CompleteBuildOrder(targetId));

            // the first tick enters the building state; the work packet --
            // and with it the emitter -- runs from the second tick
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());
            REQUIRE(std::get<UnitId>(builder.getActiveNanolatheTarget()->first) == targetId);

            // target leaves working range: the beam stops even though the
            // building state persists while chasing
            target.position = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateBuilding>(builder.behaviourState));
            REQUIRE(!builder.getActiveNanolatheTarget());
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
                REQUIRE(!builder.getActiveNanolatheTarget());
            }

            // back in range: work and beam resume together
            target.position = f.sim.terrain.heightmapIndexToWorldCenter(12, 8);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            // resource stall: the desired-consumption check fails, no work
            // packet applies, and retail's emit sits behind that gate
            auto& player = f.sim.getPlayer(f.playerId);
            player.metalStalled = true;
            player.energyStalled = true;
            auto progressBefore = target.buildTimeCompleted;
            f.sim.tick();
            REQUIRE(target.buildTimeCompleted == progressBefore);
            REQUIRE(!builder.getActiveNanolatheTarget());
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
                REQUIRE(!builder.getActiveNanolatheTarget());
            }

            // stall cleared: emission resumes with work
            player.metalStalled = false;
            player.energyStalled = false;
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            // stance lost mid-work (StopBuilding/interrupt): no emission
            builder.inBuildStance = false;
            f.sim.tick();
            REQUIRE(!builder.getActiveNanolatheTarget());
            builder.inBuildStance = true;
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            // completion: the order retires, the beam is gone next tick
            target.buildTimeCompleted = f.sim.unitDefinitions.at("DAMAGED").buildTime - f.sim.unitDefinitions.at("REPAIRER").workerTimePerTick;
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(builder.behaviourState));
            REQUIRE(!builder.getActiveNanolatheTarget());
        }

        SECTION("build: STOP order drops the beam immediately")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, false);
            f.sim.getUnitState(targetId).buildTimeCompleted = 50;

            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;
            builder.addOrder(CompleteBuildOrder(targetId));
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            builder.clearOrders();
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(builder.behaviourState));
            REQUIRE(!builder.getActiveNanolatheTarget());
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
                REQUIRE(!builder.getActiveNanolatheTarget());
            }
        }

        SECTION("repair: active while working, inactive at full health, death, interruption")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);

            auto& target = f.sim.getUnitState(targetId);
            target.hitPoints = 300;

            auto& builder = f.sim.getUnitState(builderId);
            builder.addOrder(RepairOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(builder.behaviourState));
            // stance not raised yet: the handler waits without emitting
            REQUIRE(!builder.getActiveNanolatheTarget());

            builder.inBuildStance = true;
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            // target driven out of range: beam off while chasing
            target.position = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(!builder.getActiveNanolatheTarget());
            target.position = f.sim.terrain.heightmapIndexToWorldCenter(12, 8);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());

            // repaired to full: order completes, beam stops
            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(target.hitPoints == 500);
            REQUIRE(!builder.getActiveNanolatheTarget());

            // a fresh repair on a target that dies mid-work stops too
            target.hitPoints = 300;
            builder.addOrder(RepairOrder(targetId));
            f.sim.tick();
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());
            f.sim.killUnit(targetId);
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(builder.behaviourState));
            REQUIRE(!builder.getActiveNanolatheTarget());
        }

        SECTION("reclaim feature: active while working, inactive once removed")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto featureId = f.spawnFeature(f.wreckDefId);
            auto& builder = f.sim.getUnitState(builderId);
            builder.addOrder(ReclaimOrder(featureId));
            f.sim.tick();
            builder.inBuildStance = true;
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());
            REQUIRE(std::get<FeatureId>(builder.getActiveNanolatheTarget()->first) == featureId);

            // interrupt with a move: beam drops with the work state
            builder.replaceOrders({MoveOrder(f.sim.terrain.heightmapIndexToWorldCenter(30, 30))});
            f.sim.tick();
            REQUIRE(!builder.getActiveNanolatheTarget());

            // feature reclaimed to nothing mid-work: inactive afterwards
            builder.replaceOrders({ReclaimOrder(featureId)});
            f.sim.tick();
            REQUIRE(builder.getActiveNanolatheTarget());
            for (int i = 0; i < 40; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!builder.getActiveNanolatheTarget());
        }

        SECTION("capture: active while working, inactive on completion and death")
        {
            Fixture f;
            auto capturerId = f.spawnBuilder("COMMANDER");
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            auto& capturer = f.sim.getUnitState(capturerId);
            capturer.addOrder(CaptureOrder(targetId));
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitBehaviorStateCapturing>(capturer.behaviourState));
            f.sim.tick();
            REQUIRE(capturer.getActiveNanolatheTarget());

            // out-of-range chase: no beam
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(!capturer.getActiveNanolatheTarget());
            f.sim.getUnitState(targetId).position = f.sim.terrain.heightmapIndexToWorldCenter(12, 8);
            f.sim.tick();
            f.sim.tick();
            REQUIRE(capturer.getActiveNanolatheTarget());

            // target dies mid-capture: inactive
            f.sim.killUnit(targetId);
            f.sim.tick();
            REQUIRE(!capturer.getActiveNanolatheTarget());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(capturer.behaviourState));
        }

        SECTION("factory: active while producing, inactive on stall and completion")
        {
            Fixture f;
            f.sim.unitDefinitions.at("FACTORY").builder = true;
            f.sim.unitDefinitions.at("FACTORY").workerTimePerTick = 10;
            f.sim.unitDefinitions.at("FACTORY").yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Passable);
            auto factoryId = f.sim.trySpawnUnit("FACTORY", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(30, 30), std::nullopt);
            REQUIRE(factoryId);
            auto& factory = f.sim.getUnitState(*factoryId);
            factory.finishBuilding(f.sim.unitDefinitions.at("FACTORY"));
            factory.inBuildStance = true;
            factory.addOrder(MissionFactoryBuildOrder("DAMAGED", 1));

            // production starts once the product unit exists on the pad
            for (int i = 0; i < 30 && !factory.getActiveNanolatheTarget(); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(factory.getActiveNanolatheTarget());

            // resource stall: the factory stops emitting while it waits
            auto& player = f.sim.getPlayer(f.playerId);
            player.metalStalled = true;
            player.energyStalled = true;
            f.sim.tick();
            REQUIRE(!factory.getActiveNanolatheTarget());
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
                REQUIRE(!factory.getActiveNanolatheTarget());
            }

            // stall cleared: production and emission resume
            player.metalStalled = false;
            player.energyStalled = false;
            f.sim.tick();
            REQUIRE(factory.getActiveNanolatheTarget());

            // queue drains to completion: inactive from then on
            for (int i = 0; i < 300; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(factory.orders.empty());
            REQUIRE(!factory.getActiveNanolatheTarget());
        }

        SECTION("guard assist: orphaned work state emits nothing")
        {
            // regression: an assistant left in a work state by the guard
            // fallthrough used to keep its stale nanoParticleOrigin alive,
            // so the beam drew forever after the guarded unit stopped.
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto assistantId = f.sim.trySpawnUnit("REPAIRER", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(10, 6), std::nullopt);
            REQUIRE(assistantId);
            f.sim.getUnitState(*assistantId).finishBuilding(f.sim.unitDefinitions.at("REPAIRER"));
            auto targetId = f.spawnTarget(f.playerId, false);
            auto& target = f.sim.getUnitState(targetId);
            target.buildTimeCompleted = 40;

            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;
            builder.addOrder(CompleteBuildOrder(targetId));

            auto& assistant = f.sim.getUnitState(*assistantId);
            assistant.inBuildStance = true;
            assistant.addOrder(GuardOrder(builderId));

            // both nanolathe the same nanoframe once the assistant has
            // observed the builder's state and joined in
            for (int i = 0; i < 10 && !assistant.getActiveNanolatheTarget(); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(builder.getActiveNanolatheTarget());
            REQUIRE(assistant.getActiveNanolatheTarget());
            REQUIRE(std::holds_alternative<UnitBehaviorStateBuilding>(assistant.behaviourState));

            // target completes: the builder goes idle, the assistant's
            // building state is orphaned -- no beam may survive
            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!target.isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
                REQUIRE(!assistant.getActiveNanolatheTarget());
            }
        }
    }

    TEST_CASE("patrol order")
    {
        SECTION("a mobile unit navigates toward the current waypoint")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.nextWaypointIndex == 0);
            REQUIRE(unit.navigationState.desiredDestination);
            auto dest = std::get_if<SimVector>(&*unit.navigationState.desiredDestination);
            REQUIRE(dest);
            REQUIRE(dest->x == waypoint.x);
        }

        SECTION("reaching a waypoint advances to the next")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector near(unit.position.x + 4_ss, unit.position.y, unit.position.z);
            SimVector far(unit.position.x + 200_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({near, far}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.nextWaypointIndex == 1);
        }

        SECTION("the route wraps back to the first waypoint")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector near(unit.position.x + 4_ss, unit.position.y, unit.position.z);
            SimVector far(unit.position.x + 200_ss, unit.position.y, unit.position.z);
            PatrolOrder patrol({far, near});
            patrol.nextWaypointIndex = 1;
            unit.addOrder(patrol);

            f.sim.tick();

            const auto& current = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(current.nextWaypointIndex == 0);
        }

        SECTION("patrol is a standing order that never completes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector near(unit.position.x + 4_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({near}));

            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!unit.orders.empty());
            REQUIRE(std::holds_alternative<PatrolOrder>(unit.orders.front()));
        }

        SECTION("a non-mobile unit drops the order")
        {
            Fixture f;
            auto factoryId = f.spawnTarget(f.playerId, true, "FACTORY", 20, 20);
            auto& factory = f.sim.getUnitState(factoryId);
            factory.addOrder(PatrolOrder({SimVector(factory.position.x + 100_ss, factory.position.y, factory.position.z)}));

            f.sim.tick();

            REQUIRE(factory.orders.empty());
        }

        SECTION("an empty route completes immediately")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);
            unit.addOrder(PatrolOrder({}));

            f.sim.tick();

            REQUIRE(unit.orders.empty());
        }

        SECTION("a stale waypoint index clamps safely")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector far(unit.position.x + 200_ss, unit.position.y, unit.position.z);
            PatrolOrder patrol({far});
            patrol.nextWaypointIndex = 5;
            unit.addOrder(patrol);

            f.sim.tick();

            const auto& current = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(current.nextWaypointIndex == 0);
            REQUIRE(unit.navigationState.desiredDestination);
        }

        SECTION("identical patrol setups hash identically after the same ticks")
        {
            Fixture f1;
            Fixture f2;

            auto id1 = f1.spawnBuilder();
            auto id2 = f2.spawnBuilder();

            auto& u1 = f1.sim.getUnitState(id1);
            auto& u2 = f2.sim.getUnitState(id2);

            SimVector far1(u1.position.x + 200_ss, u1.position.y, u1.position.z);
            SimVector far2(u2.position.x + 200_ss, u2.position.y, u2.position.z);
            u1.addOrder(PatrolOrder({far1, u1.position}));
            u2.addOrder(PatrolOrder({far2, u2.position}));

            for (int i = 0; i < 30; ++i)
            {
                f1.sim.tick();
                f2.sim.tick();
            }

            REQUIRE(f1.sim.computeHash() == f2.sim.computeHash());
        }

        SECTION("patrol state affects the game hash")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            auto hashBefore = f.sim.computeHash();
            unit.addOrder(PatrolOrder({SimVector(-100_ss, 0_ss, -200_ss)}));
            auto hashWithPatrol = f.sim.computeHash();

            REQUIRE(hashBefore != hashWithPatrol);

            // a different route produces a different hash
            unit.orders.front() = PatrolOrder({SimVector(-150_ss, 0_ss, -200_ss)});
            auto hashOtherRoute = f.sim.computeHash();
            REQUIRE(hashWithPatrol != hashOtherRoute);

            // so does the current leg of the route
            unit.orders.front() = PatrolOrder({SimVector(-100_ss, 0_ss, -200_ss), SimVector(-300_ss, 0_ss, -300_ss)});
            auto hashRoute2 = f.sim.computeHash();
            auto patrol = std::get<PatrolOrder>(unit.orders.front());
            patrol.nextWaypointIndex = 1;
            unit.orders.front() = patrol;
            REQUIRE(hashRoute2 != f.sim.computeHash());
        }

        SECTION("routes differing only in negative waypoint coordinates hash differently")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(PatrolOrder({SimVector(-100_ss, 0_ss, -200_ss)}));
            auto hashRouteA = f.sim.computeHash();

            unit.orders.front() = PatrolOrder({SimVector(-101_ss, 0_ss, -200_ss)});
            auto hashRouteB = f.sim.computeHash();
            REQUIRE(hashRouteA != hashRouteB);

            // the order hash itself distinguishes negative coordinates
            PatrolOrder orderA({SimVector(-100_ss, 0_ss, -200_ss)});
            PatrolOrder orderB({SimVector(-100_ss, 0_ss, -201_ss)});
            REQUIRE(computeHashOf(orderA) != computeHashOf(orderB));
        }
    }

    TEST_CASE("wait order")
    {
        SECTION("waits the given number of ticks, then completes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitOrder(3));

            f.sim.tick();
            REQUIRE(std::get<WaitOrder>(unit.orders.front()).ticksRemaining == 2);
            f.sim.tick();
            REQUIRE(std::get<WaitOrder>(unit.orders.front()).ticksRemaining == 1);
            f.sim.tick();
            REQUIRE(unit.orders.empty());
        }

        SECTION("a zero-tick wait completes on the first update")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitOrder(0));
            unit.addOrder(WaitOrder(0));

            f.sim.tick();
            f.sim.tick();

            REQUIRE(unit.orders.empty());
        }

        SECTION("a wait blocks the orders behind it")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            SimVector far(unit.position.x + 200_ss, unit.position.y, unit.position.z);
            unit.addOrder(WaitOrder(5));
            unit.addOrder(MoveOrder(far));

            f.sim.tick();
            f.sim.tick();

            // the move has not started yet
            REQUIRE(unit.orders.size() == 2);
            REQUIRE(std::holds_alternative<WaitOrder>(unit.orders.front()));
        }
    }

    TEST_CASE("wait-for-attack order")
    {
        SECTION("bare wa waits until the ordered unit takes damage")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitForAttackOrder(std::nullopt));

            f.sim.tick();
            REQUIRE(!unit.orders.empty());

            f.sim.applyDamage(builderId, 10, std::nullopt);
            f.sim.tick();

            REQUIRE(unit.orders.empty());
        }

        SECTION("wa <ident> waits until the named unit takes damage")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitForAttackOrder(targetId));

            f.sim.tick();
            REQUIRE(!unit.orders.empty());

            // damage to the ordered unit does not release the wait
            f.sim.applyDamage(builderId, 10, std::nullopt);
            f.sim.tick();
            REQUIRE(!unit.orders.empty());

            f.sim.applyDamage(targetId, 10, std::nullopt);
            f.sim.tick();
            REQUIRE(unit.orders.empty());
        }

        SECTION("damage suffered before the wait armed does not release it")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            f.sim.applyDamage(builderId, 10, std::nullopt);
            f.sim.tick();

            unit.addOrder(WaitForAttackOrder(std::nullopt));
            f.sim.tick();

            REQUIRE(!unit.orders.empty());
        }

        SECTION("a destroyed watch target releases the wait")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.enemyPlayerId);
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitForAttackOrder(targetId));
            f.sim.tick();
            REQUIRE(!unit.orders.empty());

            f.sim.applyDamage(targetId, 10000, std::nullopt);
            f.sim.tick();
            REQUIRE(unit.orders.empty());
        }

        SECTION("a stale watch target releases the wait")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.addOrder(WaitForAttackOrder(UnitId(9999)));
            f.sim.tick();
            REQUIRE(unit.orders.empty());
        }
    }

    TEST_CASE("make-selectable order")
    {
        SECTION("marks a mission unit selectable by its owner")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);

            unit.missionState = MissionUnitState{};
            unit.missionState->selectable = false;
            REQUIRE(!unit.missionState->selectable);

            unit.addOrder(MakeSelectableOrder{});
            f.sim.tick();

            REQUIRE(unit.missionState->selectable);
            REQUIRE(unit.orders.empty());
        }
    }

    TEST_CASE("return fire")
    {
        SECTION("a return-fire unit acquires the unit that damaged it")
        {
            Fixture f;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto attackerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::ReturnFire;
            defender.lastDamagedBy = attackerId;
            defender.lastDamagedAt = f.sim.gameTime;

            f.sim.tick();

            const auto& weaponState = std::get<UnitWeaponStateAttacking>(defender.weapons[0]->state);
            REQUIRE(std::get<UnitId>(weaponState.target) == attackerId);
        }

        SECTION("a return-fire unit ignores enemies that did not damage it")
        {
            Fixture f;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::ReturnFire;

            f.sim.tick();

            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a hold-fire unit never acquires")
        {
            Fixture f;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto attackerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::HoldFire;
            defender.lastDamagedBy = attackerId;
            defender.lastDamagedAt = f.sim.gameTime;

            f.sim.tick();

            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a return-fire unit does not chase a dead attacker")
        {
            Fixture f;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto attackerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::ReturnFire;
            defender.lastDamagedBy = attackerId;
            defender.lastDamagedAt = f.sim.gameTime;

            f.sim.getUnitState(attackerId).markAsDead();
            f.sim.tick();

            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a return-fire unit does not acquire an out-of-range attacker")
        {
            Fixture f;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(50);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto attackerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 30, 30);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::ReturnFire;
            defender.lastDamagedBy = attackerId;
            defender.lastDamagedAt = f.sim.gameTime;

            f.sim.tick();

            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }
    }

    TEST_CASE("attack unit type order")
    {
        // test weapons stay permanently unready: they aim but never
        // fire, since firing needs script pieces the fixture
        // doesn't model
        auto makeAttackerReady = [](Fixture& f, UnitId id) {
            auto& unit = f.sim.getUnitState(id);
            unit.weapons[0] = UnitWeapon{"GUN"};
            unit.weapons[0]->readyTime = GameTime(1000000);
            f.sim.unitDefinitions.at(unit.unitType).canAttack = true;
        };

        auto makeWeapon = [](Fixture& f) {
            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);
        };

        SECTION("selects an enemy unit of the requested type")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            // the first scan runs after a random 1-90 tick delay
            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            const auto& order = std::get<AttackUnitTypeOrder>(attacker.orders.front());
            REQUIRE(order.currentTarget == enemyId);
            const auto& weaponState = std::get<UnitWeaponStateAttacking>(attacker.weapons[0]->state);
            REQUIRE(std::get<UnitId>(weaponState.target) == enemyId);
        }

        SECTION("ignores own-side units and non-matching enemy types")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // a matching type owned by the attacker's own player
            f.spawnTarget(f.playerId, true, "DAMAGED", 10, 8);
            // an enemy of a different type
            f.spawnTarget(f.enemyPlayerId, true, "FACTORY", 12, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            // no valid enemy of the type exists: the order completes
            REQUIRE(attacker.orders.empty());
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(attacker.weapons[0]->state));
        }

        SECTION("reacquires another matching enemy when the target dies")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            auto enemyId2 = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            const auto& order = std::get<AttackUnitTypeOrder>(attacker.orders.front());
            auto firstTarget = *order.currentTarget;
            REQUIRE((firstTarget == enemyId || firstTarget == enemyId2));

            auto survivor = firstTarget == enemyId ? enemyId2 : enemyId;
            f.sim.getUnitState(firstTarget).markAsDead();
            f.sim.tick();

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            const auto& order2 = std::get<AttackUnitTypeOrder>(attacker.orders.front());
            REQUIRE(order2.currentTarget == survivor);
        }

        SECTION("completes when the last matching enemy dies")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(std::get<AttackUnitTypeOrder>(attacker.orders.front()).currentTarget == enemyId);

            f.sim.getUnitState(enemyId).markAsDead();
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(attacker.orders.empty());
        }

        SECTION("survives killing its current target with live fire")
        {
            Fixture f;

            // flatten the terrain to sea level so projectiles
            // skimming just above the ground don't hit it
            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            // a real weapon: fires fast, kills through the normal
            // projectile -> damage -> kill -> erase lifecycle
            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(10);
            weaponDef.damage["DEFAULT"] = 10000;
            weaponDef.damageRadius = SimScalar(4);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            // firing needs a script piece and a matching model piece;
            // the firing piece sits above ground so the projectile
            // doesn't immediately collide with terrain, and the model
            // is tall enough for the shot to land inside target bounds
            for (const auto& name : {"DAMAGED", "FACTORY"})
            {
                f.sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {"flare1"}, {}, 0});
                f.sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
            }

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            auto enemyId2 = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.weapons[0] = UnitWeapon{"GUN"};
            f.sim.unitDefinitions.at("DAMAGED").canAttack = true;
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 600; ++i)
            {
                f.sim.tick();
            }

            // both matching enemies were killed by the attacker's
            // own fire; the order ran its lifecycle to completion
            // without crashing
            REQUIRE(attacker.orders.empty());
            REQUIRE(!f.sim.unitExists(enemyId));
            REQUIRE(!f.sim.unitExists(enemyId2));
        }

        SECTION("a tracking projectile loses its lock when the target dies")
        {
            Fixture f;

            // flat terrain so the projectile travels level
            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            // a slow homing weapon: the projectile is still in
            // flight when its target is killed
            WeaponDefinition weaponDef{};
            weaponDef.physicsType = ProjectilePhysicsTypeTracking{SimScalar(10)};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(2);
            weaponDef.damage["DEFAULT"] = 1;
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.weapons[0] = UnitWeapon{"GUN"};
            f.sim.unitDefinitions.at("DAMAGED").canAttack = true;
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            int ticks = 0;
            while (f.sim.projectiles.begin() == f.sim.projectiles.end() && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.projectiles.begin() != f.sim.projectiles.end());
            REQUIRE(std::any_of(
                f.sim.projectiles.begin(),
                f.sim.projectiles.end(),
                [](const auto& p) { return p.second.targetUnit.has_value(); }));

            // kill the target while the shot is still in the air;
            // the projectile must drop its lock and fly straight
            f.sim.killUnit(enemyId);
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            for (const auto& [projectileId, p] : f.sim.projectiles)
            {
                REQUIRE(!p.targetUnit);
                REQUIRE(std::isfinite(p.position.x.value));
                REQUIRE(std::isfinite(p.position.y.value));
                REQUIRE(std::isfinite(p.position.z.value));
                REQUIRE(std::isfinite(p.velocity.x.value));
                REQUIRE(std::isfinite(p.velocity.y.value));
                REQUIRE(std::isfinite(p.velocity.z.value));
            }

            // the scripted order saw its target die and ended cleanly
            REQUIRE(attacker.orders.empty());
        }

        SECTION("a tracking projectile survives a quietly deleted target")
        {
            Fixture f;

            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            WeaponDefinition weaponDef{};
            weaponDef.physicsType = ProjectilePhysicsTypeTracking{SimScalar(10)};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(2);
            weaponDef.damage["DEFAULT"] = 1;
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.weapons[0] = UnitWeapon{"GUN"};
            f.sim.unitDefinitions.at("DAMAGED").canAttack = true;
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            int ticks = 0;
            while (f.sim.projectiles.begin() == f.sim.projectiles.end() && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.projectiles.begin() != f.sim.projectiles.end());

            // the Deleted path: no explosion, same stale-lock hazard
            f.sim.quietlyKillUnit(enemyId);
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            for (const auto& [projectileId, p] : f.sim.projectiles)
            {
                REQUIRE(!p.targetUnit);
                REQUIRE(std::isfinite(p.position.x.value));
                REQUIRE(std::isfinite(p.position.z.value));
            }

            REQUIRE(attacker.orders.empty());
        }

        SECTION("survives the current target being killed and erased")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            auto enemyId2 = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            auto firstTarget = *std::get<AttackUnitTypeOrder>(attacker.orders.front()).currentTarget;
            REQUIRE((firstTarget == enemyId || firstTarget == enemyId2));
            auto survivor = firstTarget == enemyId ? enemyId2 : enemyId;

            // the real death path: death event, unit erased at end of tick
            f.sim.killUnit(firstTarget);
            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            const auto& order = std::get<AttackUnitTypeOrder>(attacker.orders.front());
            REQUIRE(order.currentTarget == survivor);
        }

        SECTION("completes safely when the last matching enemy is killed and erased")
        {
            Fixture f;

            makeWeapon(f);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            makeAttackerReady(f, attackerId);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(std::get<AttackUnitTypeOrder>(attacker.orders.front()).currentTarget == enemyId);

            f.sim.killUnit(enemyId);
            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(attacker.orders.empty());
        }

        SECTION("completes immediately when the unit cannot attack")
        {
            Fixture f;

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            // no canAttack, no weapon

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            f.sim.tick();
            f.sim.tick();

            REQUIRE(attacker.orders.empty());
        }

        SECTION("identical runs select identical targets")
        {
            auto runSelection = [] {
                Fixture f;

                WeaponDefinition weaponDef{};
                weaponDef.maxRange = SimScalar(500);
                weaponDef.reloadTime = SimScalar(100000);
                f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

                auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
                f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);
                f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 16, 10);
                f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 12);
                f.sim.unitDefinitions.at("DAMAGED").canAttack = true;
                f.sim.getUnitState(attackerId).weapons[0] = UnitWeapon{"GUN"};
                f.sim.getUnitState(attackerId).weapons[0]->readyTime = GameTime(1000000);
                f.sim.getUnitState(attackerId).addOrder(AttackUnitTypeOrder("DAMAGED"));

                for (int i = 0; i < 120; ++i)
                {
                    f.sim.tick();
                }

                const auto& unit = f.sim.getUnitState(attackerId);
                return std::get<AttackUnitTypeOrder>(unit.orders.front()).currentTarget;
            };

            REQUIRE(runSelection() == runSelection());
        }
    }

    TEST_CASE("mission build order")
    {
        SECTION("a mobile builder constructs the requested unit at the position")
        {
            Fixture f;

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            auto site = f.sim.terrain.heightmapIndexToWorldCenter(14, 14);
            builder.addOrder(MissionBuildOrder("DAMAGED", site));

            // the builder walks to the site, then nanolathes: the
            // target's buildTime (100) at workerTime 10 takes ~10+ ticks
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(builder.orders.empty());

            const auto* built = f.findUnitByTypeExcept("DAMAGED", builderId);
            REQUIRE(built != nullptr);
            REQUIRE(!built->isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));
            REQUIRE(built->isOwnedBy(f.playerId));
        }

        SECTION("consumes resources while building")
        {
            Fixture f;

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            auto metalBefore = f.sim.getPlayer(f.playerId).metal;
            builder.addOrder(MissionBuildOrder("DAMAGED", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getPlayer(f.playerId).metal < metalBefore);
        }

        SECTION("does not build instantly")
        {
            Fixture f;

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            builder.addOrder(MissionBuildOrder("DAMAGED", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            f.sim.tick();
            f.sim.tick();

            REQUIRE(!builder.orders.empty());
            const auto* built = f.findUnitByTypeExcept("DAMAGED", builderId);
            REQUIRE((built == nullptr || built->isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED"))));
        }

        SECTION("retries a blocked site then abandons the order")
        {
            Fixture f;

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            // a blocker sitting on the build site makes every
            // placement attempt fail while the approach stays open
            auto site = f.sim.terrain.heightmapIndexToWorldCenter(14, 14);
            f.spawnTarget(f.playerId, true, "DAMAGED", 14, 14);

            auto unitsBefore = f.countUnits();
            builder.addOrder(MissionBuildOrder("DAMAGED", site));

            // ~10 attempts at 30 tick intervals, plus travel time
            for (int i = 0; i < 600; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(builder.orders.empty());
            REQUIRE(f.countUnits() == unitsBefore);
        }

        SECTION("a scripted water structure abandons a dry-land site")
        {
            Fixture f;

            // a water-only yardmap can never be valid on this all-land
            // map; the scripted build must go through the same placement
            // rules the player cursor uses
            auto waterDef = makeTargetDefinition();
            waterDef.unitName = "waterbldg";
            waterDef.objectName = "WATERBLDG";
            waterDef.isMobile = false;
            waterDef.canMove = false;
            waterDef.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 10, 255, 10, 255};
            waterDef.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Water);
            f.sim.unitDefinitions.insert_or_assign("WATERBLDG", waterDef);
            f.sim.unitModelDefinitions.insert_or_assign("WATERBLDG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("WATERBLDG", CobScript{{}, {}, {}, 0});

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            auto unitsBefore = f.countUnits();
            builder.addOrder(MissionBuildOrder("WATERBLDG", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            for (int i = 0; i < 600; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(builder.orders.empty());
            REQUIRE(f.countUnits() == unitsBefore);
        }

        SECTION("a scripted structure builds and takes the terrain-derived height")
        {
            Fixture f;

            auto landDef = makeTargetDefinition();
            landDef.unitName = "landbldg";
            landDef.objectName = "LANDBLDG";
            landDef.isMobile = false;
            landDef.canMove = false;
            landDef.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Ground);
            f.sim.unitDefinitions.insert_or_assign("LANDBLDG", landDef);
            f.sim.unitModelDefinitions.insert_or_assign("LANDBLDG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("LANDBLDG", CobScript{{}, {}, {}, 0});

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            builder.addOrder(MissionBuildOrder("LANDBLDG", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            const auto* built = f.findUnitByTypeExcept("LANDBLDG", builderId);
            REQUIRE(built != nullptr);
            // flat test terrain is uniformly 64 high
            REQUIRE(built->position.y == 64_ss);
        }

        SECTION("completes when the in-progress unit is destroyed")
        {
            Fixture f;

            // a long build keeps the target under construction
            // when the builder finishes walking to the site
            auto slowDef = makeTargetDefinition();
            slowDef.unitName = "slowtarget";
            slowDef.objectName = "SLOWTARGET";
            slowDef.buildTime = 100000;
            f.sim.unitDefinitions.insert_or_assign("SLOWTARGET", slowDef);
            f.sim.unitModelDefinitions.insert_or_assign("SLOWTARGET", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("SLOWTARGET", CobScript{{}, {}, {}, 0});

            auto builderId = f.spawnBuilder();
            auto& builder = f.sim.getUnitState(builderId);
            builder.inBuildStance = true;

            builder.addOrder(MissionBuildOrder("SLOWTARGET", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            const auto* built = f.findUnitByTypeExcept("SLOWTARGET", builderId);
            REQUIRE(built != nullptr);
            REQUIRE(built->isBeingBuilt(f.sim.unitDefinitions.at("SLOWTARGET")));
            f.sim.getUnitState(f.findUnitIdByTypeExcept("SLOWTARGET", builderId)).markAsDead();

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(builder.orders.empty());
        }

        SECTION("a non-builder completes the order without building")
        {
            Fixture f;

            auto id = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& unit = f.sim.getUnitState(id);
            unit.addOrder(MissionBuildOrder("DAMAGED", f.sim.terrain.heightmapIndexToWorldCenter(14, 14)));

            f.sim.tick();
            f.sim.tick();

            REQUIRE(unit.orders.empty());
        }
    }

    TEST_CASE("mission factory build order")
    {
        auto makeFactory = [](Fixture& f, UnitId* out) {
            f.sim.unitDefinitions.at("FACTORY").builder = true;
            f.sim.unitDefinitions.at("FACTORY").workerTimePerTick = 10;
            // an open pad: produced units spawn inside the footprint
            f.sim.unitDefinitions.at("FACTORY").yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Passable);
            auto id = f.sim.trySpawnUnit("FACTORY", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(30, 30), std::nullopt);
            REQUIRE(id);
            f.sim.getUnitState(*id).finishBuilding(f.sim.unitDefinitions.at("FACTORY"));
            f.sim.getUnitState(*id).inBuildStance = true;
            *out = *id;
        };

        SECTION("produces the requested count through the normal build queue")
        {
            Fixture f;

            UnitId factoryId;
            makeFactory(f, &factoryId);
            auto& factory = f.sim.getUnitState(factoryId);
            factory.addOrder(MissionFactoryBuildOrder("DAMAGED", 2));

            // DAMAGED buildTime 100 at workerTime 10: at least ~10
            // ticks per unit plus activation overhead
            for (int i = 0; i < 5; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!factory.orders.empty());
            // nothing finished yet: any produced unit is still on the pad
            const auto* early = f.findUnitByTypeExcept("DAMAGED", factoryId);
            REQUIRE((early == nullptr || early->isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED"))));

            for (int i = 0; i < 300; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(factory.orders.empty());
            auto produced = f.countUnitsOfType("DAMAGED");
            REQUIRE(produced == 2);
        }

        SECTION("produced units receive no orders")
        {
            Fixture f;

            UnitId factoryId;
            makeFactory(f, &factoryId);
            auto& factory = f.sim.getUnitState(factoryId);
            factory.addOrder(MissionFactoryBuildOrder("DAMAGED", 1));

            for (int i = 0; i < 300; ++i)
            {
                f.sim.tick();
            }

            auto* produced = f.findUnitByTypeExcept("DAMAGED", factoryId);
            REQUIRE(produced != nullptr);
            // produced units only get a pad-clearing order, never the
            // factory's mission script
            for (const auto& order : produced->orders)
            {
                REQUIRE(std::holds_alternative<BuggerOffOrder>(order));
            }
        }

        SECTION("consumes resources during production")
        {
            Fixture f;

            UnitId factoryId;
            makeFactory(f, &factoryId);
            auto& factory = f.sim.getUnitState(factoryId);
            factory.addOrder(MissionFactoryBuildOrder("DAMAGED", 1));

            auto metalBefore = f.sim.getPlayer(f.playerId).metal;
            for (int i = 0; i < 300; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(f.sim.getPlayer(f.playerId).metal < metalBefore);
        }

        SECTION("a zero count completes immediately")
        {
            Fixture f;

            UnitId factoryId;
            makeFactory(f, &factoryId);
            auto& factory = f.sim.getUnitState(factoryId);
            factory.addOrder(MissionFactoryBuildOrder("DAMAGED", 0));

            f.sim.tick();
            f.sim.tick();

            REQUIRE(factory.orders.empty());
        }

        SECTION("a mobile or non-builder producer skips the order")
        {
            Fixture f;

            // mobile unit: wrong producer kind for the factory path
            auto mobileId = f.spawnTarget(f.playerId, true, "REPAIRER", 8, 8);
            auto& mobile = f.sim.getUnitState(mobileId);
            mobile.addOrder(MissionFactoryBuildOrder("DAMAGED", 1));
            f.sim.tick();
            f.sim.tick();
            REQUIRE(mobile.orders.empty());

            // static non-builder
            auto staticId = f.spawnTarget(f.playerId, true, "FACTORY", 30, 30);
            auto& staticUnit = f.sim.getUnitState(staticId);
            staticUnit.addOrder(MissionFactoryBuildOrder("DAMAGED", 1));
            f.sim.tick();
            f.sim.tick();
            REQUIRE(staticUnit.orders.empty());
        }
    }

    TEST_CASE("self-destruct order")
    {
        SECTION("detonates after the countdown and fuse")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            f.sim.getUnitState(id).addOrder(SelfDestructOrder{30});

            // the unit survives the whole countdown phase
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
                REQUIRE(f.sim.tryGetUnitState(id));
            }

            // retail then waits a fuse of RNG(15) ticks before detonating
            for (int i = 0; i < 20 && f.sim.tryGetUnitState(id); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetUnitState(id));

            bool diedEventSeen = false;
            for (const auto& event : f.sim.events)
            {
                if (std::holds_alternative<UnitDiedEvent>(event))
                {
                    diedEventSeen = true;
                }
            }
            REQUIRE(diedEventSeen);
        }
    }

    TEST_CASE("interactive self-destruct state")
    {
        SECTION("counts down and detonates in parallel with the order queue")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            auto& unit = f.sim.getUnitState(id);

            // retail SELFDESTRUCT is a unit state, not a queue order:
            // a queued order must keep executing while it counts down
            unit.addOrder(MoveOrder{unit.position});
            unit.selfDestruct = SelfDestructOrder{60};

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
                REQUIRE(f.sim.tryGetUnitState(id));
            }
            // the foreground order completed during the countdown
            REQUIRE(unit.orders.empty());

            // fuse phase: RNG(15) ticks after the countdown expires
            for (int i = 0; i < 20 && f.sim.tryGetUnitState(id); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetUnitState(id));

            // whole-second boundary announcements: 60 ticks = 2s,
            // so count1 (30 ticks left) and count0 (0 left) fire
            std::vector<int> counts;
            for (const auto& event : f.sim.events)
            {
                if (auto e = std::get_if<UnitSelfDestructCountdownEvent>(&event); e && e->unitId == id)
                {
                    counts.push_back(e->secondsRemaining);
                }
            }
            REQUIRE(counts == std::vector<int>{1, 0});
        }

        SECTION("survives a foreground order clear (retail STOP does not cancel it)")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            auto& unit = f.sim.getUnitState(id);
            unit.selfDestruct = SelfDestructOrder{30};

            unit.clearOrders();
            f.sim.tick();
            REQUIRE(unit.selfDestruct);
            REQUIRE(unit.selfDestruct->countdownTicks < 30u);
        }

        SECTION("cancellation clears the state before detonation")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            auto& unit = f.sim.getUnitState(id);
            unit.selfDestruct = SelfDestructOrder{30};

            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.selfDestruct);

            // retail reissues SELFDESTRUCT to toggle it off;
            // the cancel lands in the command path, the state clears
            unit.selfDestruct = std::nullopt;

            for (int i = 0; i < 100; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.tryGetUnitState(id));
            REQUIRE(unit.hitPoints > 0);
        }

        SECTION("mission Immunity does not block owner self-destruct")
        {
            // retail damage application (TotalA.exe 0x489bb0 -> 0x489ce0)
            // never consults the OTA Immunity bit (unit word +0x110 bit
            // 15); immunity is only consulted in target-selection and
            // area-damage sweep iterations (e.g. 0x40ab24). The
            // self-destruct countdown drives a direct DamageUnit call,
            // so an Immunity=1 mission unit (e.g. CC01's CORCOM) still
            // blows up.
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            auto& unit = f.sim.getUnitState(id);
            unit.missionState = MissionUnitState{};
            unit.missionState->immunity = true;
            unit.selfDestruct = SelfDestructOrder{30};

            // countdown phase
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
                REQUIRE(f.sim.tryGetUnitState(id));
            }

            // fuse phase, then detonation
            for (int i = 0; i < 20 && f.sim.tryGetUnitState(id); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetUnitState(id));
        }

        SECTION("scripted d order also bypasses mission Immunity")
        {
            // the InitialMission `d` command and Ctrl+D share the same
            // machinery; the same immunity exemption applies
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            auto& unit = f.sim.getUnitState(id);
            unit.missionState = MissionUnitState{};
            unit.missionState->immunity = true;
            unit.addOrder(SelfDestructOrder{30});

            for (int i = 0; i < 60 && f.sim.tryGetUnitState(id); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.tryGetUnitState(id));
        }
    }

    TEST_CASE("cross-mode player command parity")
    {
        // TotalA.exe routes every player gameplay command through one
        // shared dispatch: keyboard/game-layer commands (0x495e90) and
        // GUI order-panel commands produce the same command codes
        // regardless of mission type. A mission adds missionInfo (the
        // campaign/BT objectives envelope) and per-unit missionState, but
        // the command path must not fork on them. These scenarios assert
        // identical command acceptance and state transitions across the
        // three single-player shapes RWE can launch: canonical Campaign,
        // Battle Tactics (also a missionInfo sim), and Skirmish (no
        // missionInfo). The regression exists so a future feature wired
        // only in one mode fails here.
        enum class Mode
        {
            Campaign,
            BattleTactics,
            Skirmish
        };
        constexpr std::array<Mode, 3> allModes{Mode::Campaign, Mode::BattleTactics, Mode::Skirmish};

        // installs the mission envelope the way LoadingScene does for a
        // --campaign launch; Battle Tactics rides the same mechanism,
        // skirmish launches carry none
        auto installMission = [](Fixture& f, Mode mode) {
            if (mode == Mode::Skirmish)
            {
                return;
            }
            MissionInfo info;
            info.campaignName = mode == Mode::Campaign ? "ARM" : "Battle Tactics - ARM";
            info.campaignSide = "ARM";
            info.missionName = mode == Mode::Campaign ? "AC01" : "BTSHORT1";
            info.missionIndex = 0;
            info.missionFile = info.missionName + ".OTA";
            info.schemaType = "Medium";
            info.aiProfile = "MISSIONS";
            info.localPlayerId = f.playerId;
            f.sim.missionInfo = std::move(info);
            f.sim.initMissionObjectives();
        };

        // spawns a unit the way each mode does: mission units carry
        // authored missionState, skirmish units have none. selectable
        // defaults true; the caller adds Immunity where relevant.
        auto spawnModeUnit = [](Fixture& f, Mode mode, const std::string& type = "DAMAGED", int x = 12, int y = 8) {
            auto id = f.spawnTarget(f.playerId, true, type, x, y);
            if (mode != Mode::Skirmish)
            {
                f.sim.getUnitState(id).missionState = MissionUnitState{};
            }
            return id;
        };

        SECTION("ToggleSelfDestruct arm/cancel/re-arm/detonate")
        {
            // Ctrl/D's full retail contract, all three modes; the mission
            // units also carry Immunity=1, which retail's damage primitive
            // does not consult (only target sweeps do, e.g. 0x40ab24)
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto id = spawnModeUnit(f, mode);
                auto& unit = f.sim.getUnitState(id);
                if (mode != Mode::Skirmish)
                {
                    unit.missionState->immunity = true;
                }

                // arm -> countdown
                unit.selfDestruct = SelfDestructOrder{30};
                f.sim.tick();
                REQUIRE(unit.selfDestruct);
                REQUIRE(unit.selfDestruct->countdownTicks == 29u);

                // cancel mid-countdown
                unit.selfDestruct = std::nullopt;
                for (int i = 0; i < 40; ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(f.sim.tryGetUnitState(id));

                // re-arm -> detonate
                unit.selfDestruct = SelfDestructOrder{30};
                for (int i = 0; i < 60 && f.sim.tryGetUnitState(id); ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(!f.sim.tryGetUnitState(id));
            }
        }

        SECTION("Move order")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto id = spawnModeUnit(f, mode);
                auto& unit = f.sim.getUnitState(id);
                auto target = f.sim.terrain.heightmapIndexToWorldCenter(20, 20);

                unit.addOrder(MoveOrder{target});
                for (int i = 0; i < 600 && !unit.orders.empty(); ++i)
                {
                    f.sim.tick();
                }

                REQUIRE(unit.orders.empty());
                SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
                REQUIRE(xzPosition.distanceSquared(SimVector(target.x, 0_ss, target.z)) < 8_ss * 8_ss);
            }
        }

        SECTION("Patrol order")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto id = spawnModeUnit(f, mode);
                auto& unit = f.sim.getUnitState(id);

                // patrol never completes: the order stays queued while
                // the unit walks its loop, identically in every mode
                unit.addOrder(PatrolOrder({f.sim.terrain.heightmapIndexToWorldCenter(20, 20)}));
                for (int i = 0; i < 120; ++i)
                {
                    f.sim.tick();
                    REQUIRE(std::holds_alternative<PatrolOrder>(unit.orders.front()));
                }
            }
        }

        // registers one transport of each stock pickup class: a ground
        // loader (ship/hover share this mechanic) and a canFly VTOL
        auto registerTransportClasses = [](Fixture& f) {
            auto groundDef = makeTargetDefinition();
            groundDef.unitName = "loader";
            groundDef.objectName = "LOADER";
            groundDef.canLoad = true;
            groundDef.transportSize = 4;
            groundDef.transportCapacity = 2;
            f.sim.unitDefinitions.insert_or_assign("LOADER", groundDef);
            auto vtolDef = makeTargetDefinition();
            vtolDef.unitName = "vtolship";
            vtolDef.objectName = "VTOLSHIP";
            vtolDef.canFly = true;
            vtolDef.transportCapacity = 6;
            f.sim.unitDefinitions.insert_or_assign("VTOLSHIP", vtolDef);
            for (const auto& name : {"LOADER", "VTOLSHIP"})
            {
                f.sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                f.sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }
        };

        SECTION("Load order boards a friendly unit")
        {
            // the ground-class loader stands in for the ship/hover
            // pickup mechanic; the VTOL land-and-pickup flight phases
            // need real unit definitions (covered by the six-stock-
            // transport real-data test), so only the ground class runs
            // here -- the LoadOrder command path under test is
            // class-independent
            for (const auto mode : allModes)
            {
                Fixture f;
                registerTransportClasses(f);
                installMission(f, mode);
                auto shipId = spawnModeUnit(f, mode, "LOADER", 20, 20);
                auto cargoId = spawnModeUnit(f, mode, "DAMAGED", 26, 20);

                f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
                for (int i = 0; i < 200 && !f.sim.getUnitState(cargoId).carriedBy; ++i)
                {
                    f.sim.tick();
                }

                const auto& cargo = f.sim.getUnitState(cargoId);
                const auto& ship = f.sim.getUnitState(shipId);
                REQUIRE(cargo.carriedBy == std::optional<UnitId>(shipId));
                REQUIRE(ship.passengers.size() == 1);
                REQUIRE(ship.passengers.front() == cargoId);
            }
        }

        SECTION("Unload order releases cargo")
        {
            for (const auto mode : allModes)
            {
                for (const auto& carrierType : {"LOADER", "VTOLSHIP"})
                {
                    Fixture f;
                    registerTransportClasses(f);
                    installMission(f, mode);
                    auto shipId = spawnModeUnit(f, mode, carrierType, 20, 20);
                    auto cargoId = spawnModeUnit(f, mode, "DAMAGED", 26, 20);
                    REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

                    f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
                    for (int i = 0; i < 600 && f.sim.getUnitState(cargoId).carriedBy; ++i)
                    {
                        f.sim.tick();
                    }

                    REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
                    REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
                    REQUIRE(f.sim.getUnitState(cargoId).isAlive());
                }
            }
        }

        SECTION("Repair order restores a damaged unit")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto builderId = spawnModeUnit(f, mode, "REPAIRER", 8, 8);
                auto targetId = spawnModeUnit(f, mode, "DAMAGED", 12, 8);
                auto& target = f.sim.getUnitState(targetId);
                target.hitPoints = 300;

                f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
                f.sim.tick();
                REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(f.sim.getUnitState(builderId).behaviourState));
                f.sim.getUnitState(builderId).inBuildStance = true;
                f.sim.tick();
                REQUIRE(target.hitPoints == 350);
            }
        }

        SECTION("Reclaim order consumes a feature")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto builderId = spawnModeUnit(f, mode, "REPAIRER", 8, 8);
                auto featureId = f.spawnFeature(f.wreckDefId);
                f.sim.getPlayer(f.playerId).metal = Metal(0);

                f.sim.getUnitState(builderId).addOrder(ReclaimOrder(featureId));
                f.sim.tick();
                REQUIRE(std::holds_alternative<UnitBehaviorStateReclaiming>(f.sim.getUnitState(builderId).behaviourState));
                f.sim.getUnitState(builderId).inBuildStance = true;
                for (int i = 0; i < 12 && f.sim.tryGetFeature(featureId); ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(!f.sim.tryGetFeature(featureId));
                REQUIRE(f.sim.getPlayer(f.playerId).metalProductionBuffer == Metal(90));
            }
        }

        SECTION("Capture order transfers an enemy unit")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto capturerId = spawnModeUnit(f, mode, "COMMANDER", 8, 8);
                // capture targets are enemies; missionState ownership
                // gates live on the enemy side too
                auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);
                if (mode != Mode::Skirmish)
                {
                    f.sim.getUnitState(targetId).missionState = MissionUnitState{};
                }

                f.sim.getUnitState(capturerId).addOrder(CaptureOrder(targetId));
                f.sim.tick();
                REQUIRE(std::holds_alternative<UnitBehaviorStateCapturing>(f.sim.getUnitState(capturerId).behaviourState));
                for (int i = 0; i < 1700 && f.sim.getUnitState(targetId).isOwnedBy(f.enemyPlayerId); ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(f.sim.getUnitState(targetId).isOwnedBy(f.playerId));
            }
        }

        SECTION("CommandFire order drives the D-Gun weapon")
        {
            auto registerDGunDefs = [](Fixture& f) {
                f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);
                WeaponDefinition wd{};
                wd.commandFire = true;
                wd.noExplode = true;
                wd.maxRange = SimScalar(240);
                wd.reloadTime = SimScalar(1.2f);
                wd.burst = 1;
                wd.burstInterval = SimScalar(0);
                wd.velocity = SimScalar(8);
                wd.damage["DEFAULT"] = 30000;
                wd.damageRadius = SimScalar(48);
                wd.energyPerShot = Energy(400);
                wd.metalPerShot = Metal(0);
                wd.physicsType = ProjectilePhysicsTypeLineOfSight();
                f.sim.weaponDefinitions.insert_or_assign("DGUN", wd);

                auto def = makeTargetDefinition();
                def.unitName = "dgunner";
                def.objectName = "DGUNNER";
                def.canAttack = true;
                def.canDGun = true;
                def.weapon3 = "DGUN";
                def.energyStorage = Energy(1000000);
                f.sim.unitDefinitions.insert_or_assign("DGUNNER", def);
                f.sim.unitScriptDefinitions.insert_or_assign("DGUNNER", CobScript{{}, {"flare1"}, {}, 0});
                f.sim.unitModelDefinitions.insert_or_assign("DGUNNER", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
            };

            for (const auto mode : allModes)
            {
                Fixture f;
                registerDGunDefs(f);
                installMission(f, mode);
                auto id = spawnModeUnit(f, mode, "DGUNNER", 8, 8);
                auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

                f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));
                int ticks = 0;
                while (std::distance(f.sim.projectiles.begin(), f.sim.projectiles.end()) == 0 && ticks < 400)
                {
                    f.sim.tick();
                    ++ticks;
                }
                REQUIRE(std::distance(f.sim.projectiles.begin(), f.sim.projectiles.end()) == 1);
            }
        }

        SECTION("cloak request cloaks the unit")
        {
            for (const auto mode : allModes)
            {
                Fixture f;
                installMission(f, mode);
                auto id = spawnModeUnit(f, mode, "CLOAKER", 8, 8);
                f.sim.getUnitState(id).cloakRequested = true;
                // the cloak upkeep pass runs once per 30 ticks
                for (int i = 0; i < 30; ++i)
                {
                    f.sim.tick();
                }
                REQUIRE(f.sim.getUnitState(id).cloaked);
            }
        }
    }

    TEST_CASE("move order completion semantics")
    {
        SECTION("does not complete while the unit is still approaching")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);

            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(40, 8);
            unit.addOrder(MoveOrder{dest});

            // mid-flight the order must stay queued; only arrival or
            // best-effort path exhaustion may complete it
            for (int i = 0; i < 3; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(dest.x, 0_ss, dest.z)) >= 8_ss * 8_ss);

            // and it eventually arrives
            for (int i = 0; i < 400; ++i)
            {
                f.sim.tick();
                if (unit.orders.empty())
                {
                    break;
                }
            }
            REQUIRE(unit.orders.empty());
        }

        SECTION("temporary congestion does not cancel the order")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);
            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(40, 8);
            unit.addOrder(MoveOrder{dest});

            // let the unit commit to its path, then drop a blocker
            // directly on its route; the unit collides and repaths
            // around it -- the order must survive the congestion
            f.sim.tick();
            auto blockerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            for (int i = 0; i < 400; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(dest.x, 0_ss, dest.z)) < 8_ss * 8_ss);
            (void)blockerId;
        }

        SECTION("unreachable destination completes at the closest reachable point")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);

            // off-map destination: the path is partial, so the move
            // job ends when the unit consumes it -- the order must
            // complete instead of grinding on the unreachable goal
            unit.addOrder(MoveOrder{SimVector(100000_ss, 0_ss, 100000_ss)});
            for (int i = 0; i < 600; ++i)
            {
                f.sim.tick();
                if (unit.orders.empty())
                {
                    break;
                }
            }
            REQUIRE(unit.orders.empty());
            // the unit stopped at the closest reachable point, far
            // short of the requested destination
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(100000_ss, 0_ss, 100000_ss)) >= 8_ss * 8_ss);
        }
    }

    TEST_CASE("failed path request backoff")
    {
        SECTION("a unit sealed in by blockers re-requests sparingly")
        {
            Fixture f;

            // a 1x1 blocking feature so individual cells can be sealed
            auto wallId = f.sim.featureDefinitions.insert(makeFeatureDefinition("WALL", 0, 0));
            auto& wallDef = f.sim.featureDefinitions.get(wallId);
            wallDef.footprintX = 1;
            wallDef.footprintZ = 1;

            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& unit = f.sim.getUnitState(unitId);
            const auto& unitDef = f.sim.unitDefinitions.at("DAMAGED");
            auto anchor = f.sim.computeFootprintRegion(unit.position, unitDef.movementCollisionInfo);

            // seal every cell adjacent to the unit's footprint: all A*
            // successors are blocked, so every search drains instantly
            // into a trivial partial -- the AC05 "walled-in patroller"
            for (int x = anchor.x - 1; x < anchor.x + anchor.width + 1; ++x)
            {
                for (int y = anchor.y - 1; y < anchor.y + anchor.height + 1; ++y)
                {
                    if (x >= anchor.x && x < anchor.x + anchor.width && y >= anchor.y && y < anchor.y + anchor.height)
                    {
                        continue;
                    }
                    REQUIRE(f.sim.addFeature(wallId, x, y));
                }
            }

            // two unreachable waypoints: without backoff the unit
            // ping-pongs a fresh path request every few ticks forever
            unit.addOrder(PatrolOrder({f.sim.terrain.heightmapIndexToWorldCenter(40, 8),
                                       f.sim.terrain.heightmapIndexToWorldCenter(8, 40)}));

            unsigned int ticksWithRequests = 0;
            for (int i = 0; i < 90; ++i)
            {
                f.sim.tick();
                if (!f.sim.pathRequests.empty())
                {
                    ++ticksWithRequests;
                }
            }

            // retail sleeps the move job ~30 ticks on a failed path
            // request, so the sealed unit retries each leg at roughly
            // that cadence -- not once every few ticks
            REQUIRE(ticksWithRequests <= 6);

            // the patrol never completes; it is still cycling its
            // unreachable waypoints rather than being dropped
            REQUIRE(!unit.orders.empty());
        }

        SECTION("a reachable move order is not delayed by the backoff")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);

            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(30, 8);
            unit.addOrder(MoveOrder{dest});

            for (int i = 0; i < 400 && !unit.orders.empty(); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(dest.x, 0_ss, dest.z)) < 8_ss * 8_ss);
        }
    }

    TEST_CASE("queued order removal")
    {
        SECTION("removing a later queued order leaves the active order running")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);
            auto nearDest = f.sim.terrain.heightmapIndexToWorldCenter(16, 8);
            auto farDest = f.sim.terrain.heightmapIndexToWorldCenter(40, 8);
            unit.addOrder(MoveOrder{nearDest});
            unit.addOrder(MoveOrder{farDest});

            f.sim.tick();
            unit.removeOrderAt(1);
            REQUIRE(unit.orders.size() == 1);

            // the unit still walks the first order to completion and
            // never visits the removed second destination
            for (int i = 0; i < 400 && !unit.orders.empty(); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(nearDest.x, 0_ss, nearDest.z)) < 8_ss * 8_ss);
            REQUIRE(xzPosition.distanceSquared(SimVector(farDest.x, 0_ss, farDest.z)) >= 8_ss * 8_ss);
        }

        SECTION("removing the active order hands over to the next queued order")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);
            auto nearDest = f.sim.terrain.heightmapIndexToWorldCenter(16, 8);
            auto farDest = f.sim.terrain.heightmapIndexToWorldCenter(40, 8);
            unit.addOrder(MoveOrder{nearDest});
            unit.addOrder(MoveOrder{farDest});

            f.sim.tick();
            unit.removeOrderAt(0);
            REQUIRE(unit.orders.size() == 1);

            // the remaining order takes over immediately
            for (int i = 0; i < 400 && !unit.orders.empty(); ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(farDest.x, 0_ss, farDest.z)) < 8_ss * 8_ss);
        }

        SECTION("removing the only order idles the unit")
        {
            Fixture f;
            auto unitId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            auto& unit = f.sim.getUnitState(unitId);
            auto farDest = f.sim.terrain.heightmapIndexToWorldCenter(40, 8);
            unit.addOrder(MoveOrder{farDest});

            f.sim.tick();
            unit.removeOrderAt(0);
            REQUIRE(unit.orders.empty());

            // the unit stays idle instead of resuming travel
            for (int i = 0; i < 30; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(unit.orders.empty());
            SimVector xzPosition(unit.position.x, 0_ss, unit.position.z);
            REQUIRE(xzPosition.distanceSquared(SimVector(farDest.x, 0_ss, farDest.z)) >= 8_ss * 8_ss);
        }
    }

    TEST_CASE("transport board and unload")
    {
        auto registerTransportDefs = [](Fixture& f) {
            auto shipDef = makeTargetDefinition();
            shipDef.unitName = "ship";
            shipDef.objectName = "SHIP";
            shipDef.transportCapacity = 6;
            f.sim.unitDefinitions.insert_or_assign("SHIP", shipDef);
            auto cargoDef = makeTargetDefinition();
            cargoDef.unitName = "cargo";
            cargoDef.objectName = "CARGO";
            f.sim.unitDefinitions.insert_or_assign("CARGO", cargoDef);
            for (const auto& name : {"SHIP", "CARGO"})
            {
                f.sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                f.sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }
        };

        SECTION("boarding removes the unit from occupancy and pins it to the carrier")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargoOldPos = f.sim.getUnitState(cargoId).position;

            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            auto& cargo = f.sim.getUnitState(cargoId);
            auto& ship = f.sim.getUnitState(shipId);
            REQUIRE(cargo.carriedBy == std::optional<UnitId>(shipId));
            REQUIRE(ship.passengers.size() == 1);
            REQUIRE(ship.passengers.front() == cargoId);

            // the cargo's old footprint is free again
            auto rect = f.sim.computeFootprintRegion(cargoOldPos, f.sim.unitDefinitions.at("CARGO").movementCollisionInfo);
            REQUIRE(!f.sim.isCollisionAt(rect));

            // the cargo is no longer selectable or targetable
            REQUIRE(!cargo.isSelectableBy(f.sim.unitDefinitions.at("CARGO"), f.playerId));

            // and rides along with the carrier
            f.sim.tick();
            auto cargoPos = f.sim.getUnitState(cargoId).position;
            auto shipPos = f.sim.getUnitState(shipId).position;
            REQUIRE(cargoPos.x == shipPos.x);
            REQUIRE(cargoPos.z == shipPos.z);
        }

        SECTION("boarding rejects invalid pairs")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2Id = f.spawnTarget(f.playerId, true, "CARGO", 30, 30);

            // non-transport target
            REQUIRE(!f.sim.tryBoardTransport(cargoId, cargo2Id));
            // a unit cannot board itself
            REQUIRE(!f.sim.tryBoardTransport(cargoId, cargoId));

            // a unit carrying others cannot board
            REQUIRE(f.sim.tryBoardTransport(cargo2Id, shipId));
            REQUIRE(!f.sim.tryBoardTransport(shipId, cargo2Id));

            // a unit already aboard cannot board again
            auto cargo3Id = f.spawnTarget(f.playerId, true, "CARGO", 40, 40);
            REQUIRE(f.sim.tryBoardTransport(cargo3Id, shipId));
            REQUIRE(!f.sim.tryBoardTransport(cargo3Id, shipId));
        }

        SECTION("scripted boarding bypasses the interactive capacity limit")
        {
            // decoded retail semantics: the scripted InitialMission `i`
            // path checks only that the transport CAN carry (capacity
            // non-zero), never the current passenger count. This must
            // hold even under the TA-Transplant capacity overrides.
            Fixture f;
            registerTransportDefs(f);
            f.sim.unitDefinitions.at("SHIP").transportCapacity = 1;
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2Id = f.spawnTarget(f.playerId, true, "CARGO", 30, 30);

            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));
            REQUIRE(f.sim.tryBoardTransport(cargo2Id, shipId));
            REQUIRE(f.sim.getUnitState(shipId).passengers.size() == 2);
        }

        SECTION("carrier death kills the cargo")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            f.sim.applyDamage(shipId, 30000, std::nullopt);
            REQUIRE(f.sim.getUnitState(shipId).isDead());
            REQUIRE(f.sim.getUnitState(cargoId).isDead());
        }

        SECTION("passenger death unlinks it from the carrier")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            f.sim.killUnit(cargoId);
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
        }

        SECTION("carried units cannot be damaged")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            f.sim.applyDamage(cargoId, 100, std::nullopt);
            REQUIRE(f.sim.getUnitState(cargoId).hitPoints == 500);
        }

        SECTION("unload drains the cargo chain one passenger per cycle")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2Id = f.spawnTarget(f.playerId, true, "CARGO", 30, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));
            REQUIRE(f.sim.tryBoardTransport(cargo2Id, shipId));

            auto dropPos = f.sim.getUnitState(shipId).position;
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{dropPos});

            // retail sleeps 15 ticks after popping each member, so the
            // first drop cannot land within the first 15 ticks
            for (int i = 0; i < 15; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(shipId).passengers.size() == 2);

            f.sim.tick();
            // most recently boarded unit disembarks first (list head)
            REQUIRE(f.sim.getUnitState(shipId).passengers.size() == 1);
            REQUIRE(!f.sim.getUnitState(cargo2Id).carriedBy);
            // dropped near the destination, on the ground
            REQUIRE(f.sim.getUnitState(cargo2Id).position.distance(dropPos) < SimScalar(300));
            // and occupies terrain again
            auto rect = f.sim.computeFootprintRegion(f.sim.getUnitState(cargo2Id).position, f.sim.unitDefinitions.at("CARGO").movementCollisionInfo);
            REQUIRE(f.sim.isCollisionAt(rect));

            // the order stays queued and drops the remaining passenger
            // after the next 15-tick cycle
            REQUIRE(!f.sim.getUnitState(shipId).orders.empty());
            for (int i = 0; i < 20; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a single unload order drops at most two passengers")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            std::vector<UnitId> cargo;
            for (int i = 0; i < 4; ++i)
            {
                auto id = f.spawnTarget(f.playerId, true, "CARGO", 10 + i, 10);
                REQUIRE(f.sim.tryBoardTransport(id, shipId));
                cargo.push_back(id);
            }

            auto dropPos = f.sim.getUnitState(shipId).position;
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{dropPos});
            for (int i = 0; i < 80; ++i)
            {
                f.sim.tick();
            }

            // retail increments order+0x36 per member claim and aborts
            // the order when the third claim is reached, so one `u`
            // releases at most two passengers; the rest stay aboard
            // for the next `u` command
            const auto& ship = f.sim.getUnitState(shipId);
            REQUIRE(ship.orders.empty());
            REQUIRE(ship.passengers.size() == 2);
            REQUIRE(f.sim.getUnitState(cargo.front()).carriedBy == std::optional<UnitId>(shipId));
        }

        SECTION("the transport moves to the destination before releasing")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{dest});

            // while the transport is still short of the destination no
            // passenger is released
            for (int i = 0; i < 16; ++i)
            {
                f.sim.tick();
            }
            const auto& ship = f.sim.getUnitState(shipId);
            REQUIRE(ship.passengers.size() == 1);
            REQUIRE(f.sim.getUnitState(cargoId).carriedBy == std::optional<UnitId>(shipId));

            // once it arrives the passenger disembarks near the
            // destination, not at the spawn point
            for (int i = 0; i < 400; ++i)
            {
                f.sim.tick();
            }
            const auto& cargo = f.sim.getUnitState(cargoId);
            REQUIRE(!cargo.carriedBy);
            REQUIRE(cargo.position.distance(dest) < SimScalar(300));
        }

        SECTION("unload with no passengers completes immediately")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("unload on a non-transport completes immediately")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            f.sim.getUnitState(id).addOrder(UnloadOrder{f.sim.getUnitState(id).position});
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(id).orders.empty());
        }

        SECTION("far off-map destination abandons the unload after bounded retries")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            // a destination dozens of map-widths off the map can never
            // produce a valid cell: the transport stops at its closest
            // reachable point, each claim's jittered search exhausts
            // the retail job deadline, and the claim counter aborts
            // the order -- the passenger stays aboard for the next `u`
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{SimVector(100000_ss, 0_ss, 100000_ss)});
            for (int i = 0; i < 1000; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
            REQUIRE(f.sim.getUnitState(cargoId).carriedBy == std::optional<UnitId>(shipId));
        }

        SECTION("drop search exhausts when no cell can ever fit the passenger")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            // passenger footprint larger than the whole map: every
            // jittered candidate is out of bounds, so the search runs
            // the retail job deadline (~232t) per claim and the claim
            // counter aborts the order at three claims
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));
            f.sim.unitDefinitions.at("CARGO").movementCollisionInfo = UnitDefinition::AdHocMovementClass{80, 80, 0, 0, 0, 0};

            f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
            for (int i = 0; i < 600; ++i)
            {
                f.sim.tick();
            }
            // order abandoned after the bounded searches; the member
            // stays aboard for the next `u` command
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
            REQUIRE(f.sim.getUnitState(cargoId).carriedBy == std::optional<UnitId>(shipId));
        }

        SECTION("stale passenger entries are skipped")
        {
            Fixture f;
            registerTransportDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            // a passenger whose carriedBy was cleared elsewhere is
            // treated as a stale link, not a drop target
            f.sim.getUnitState(cargoId).carriedBy = std::nullopt;
            f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
        }

        SECTION("drop positions jitter deterministically from the sim RNG")
        {
            auto runDrop = [&registerTransportDefs] {
                Fixture f;
                registerTransportDefs(f);
                auto shipId = f.spawnTarget(f.playerId, true, "SHIP", 20, 20);
                auto cargo1 = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
                auto cargo2 = f.spawnTarget(f.playerId, true, "CARGO", 30, 10);
                REQUIRE(f.sim.tryBoardTransport(cargo1, shipId));
                REQUIRE(f.sim.tryBoardTransport(cargo2, shipId));

                f.sim.rng.seed(1337);
                f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
                for (int i = 0; i < 80; ++i)
                {
                    f.sim.tick();
                }
                return std::make_pair(
                    f.sim.getUnitState(cargo1).position,
                    f.sim.getUnitState(cargo2).position);
            };

            // identical seeds produce identical drops: the candidate
            // jitter is driven by the authoritative simulation RNG
            auto first = runDrop();
            auto second = runDrop();
            REQUIRE(first.first.distanceSquared(second.first) == 0_ss);
            REQUIRE(first.second.distanceSquared(second.second) == 0_ss);
            // and the two passengers land at distinct jittered cells,
            // not a deterministic fixed ring slot
            REQUIRE(first.first.distanceSquared(first.second) > 0_ss);
        }

        SECTION("a VTOL transport drops exactly one passenger per unload order")
        {
            Fixture f;
            registerTransportDefs(f);
            auto vtolDef = makeTargetDefinition();
            vtolDef.unitName = "vtolship";
            vtolDef.objectName = "VTOLSHIP";
            vtolDef.canFly = true;
            vtolDef.transportCapacity = 6;
            f.sim.unitDefinitions.insert_or_assign("VTOLSHIP", vtolDef);
            f.sim.unitModelDefinitions.insert_or_assign("VTOLSHIP", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("VTOLSHIP", CobScript{{}, {}, {}, 0});

            auto vtolId = f.spawnTarget(f.playerId, true, "VTOLSHIP", 20, 20);
            auto cargo1 = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2 = f.spawnTarget(f.playerId, true, "CARGO", 30, 10);
            REQUIRE(f.sim.tryBoardTransport(cargo1, vtolId));
            REQUIRE(f.sim.tryBoardTransport(cargo2, vtolId));

            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            f.sim.getUnitState(vtolId).addOrder(UnloadOrder{dest});
            for (int i = 0; i < 400; ++i)
            {
                f.sim.tick();
            }

            // VTOL_Unload releases a single member at the validated
            // destination cell and pops the order; the remaining
            // passenger waits for the next `u`
            const auto& vtol = f.sim.getUnitState(vtolId);
            REQUIRE(vtol.orders.empty());
            REQUIRE(vtol.passengers.size() == 1);

            const auto& dropped = f.sim.getUnitState(cargo2);
            REQUIRE(!dropped.carriedBy);
            SimVector droppedXZ(dropped.position.x, 0_ss, dropped.position.z);
            REQUIRE(droppedXZ.distance(SimVector(dest.x, 0_ss, dest.z)) < SimScalar(60));
            auto rect = f.sim.computeFootprintRegion(dropped.position, f.sim.unitDefinitions.at("CARGO").movementCollisionInfo);
            REQUIRE(f.sim.isCollisionAt(rect));
            REQUIRE(f.sim.getUnitState(cargo1).carriedBy == std::optional<UnitId>(vtolId));
        }

        SECTION("carried cargo rides at the carrier position (scripted i uses an invalid attach slot)")
        {
            Fixture f;
            registerTransportDefs(f);
            auto vtolDef = makeTargetDefinition();
            vtolDef.unitName = "vtolship";
            vtolDef.objectName = "VTOLSHIP";
            vtolDef.canFly = true;
            vtolDef.transportCapacity = 6;
            f.sim.unitDefinitions.insert_or_assign("VTOLSHIP", vtolDef);
            // a "link" piece offset 24wu along +x exists in the model but
            // must be ignored: scripted i attaches with slot -1, which
            // retail resolves to a zero offset (0x43def0 -> 0x43e01d)
            f.sim.unitModelDefinitions.insert_or_assign("VTOLSHIP",
                createUnitModelDefinition(0_ss, {UnitPieceDefinition{"link", SimVector(24_ss, 0_ss, 0_ss)}}));
            f.sim.unitScriptDefinitions.insert_or_assign("VTOLSHIP", CobScript{{}, {}, {}, 0});

            auto vtolId = f.spawnTarget(f.playerId, true, "VTOLSHIP", 20, 20);
            auto cargo = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2 = f.spawnTarget(f.playerId, true, "CARGO", 15, 15);
            REQUIRE(f.sim.tryBoardTransport(cargo, vtolId));
            REQUIRE(f.sim.tryBoardTransport(cargo2, vtolId));

            // members ride at the carrier position verbatim, not at the
            // model's link piece, and a second member shares the same
            // position (no slot fanning for scripted i cargo)
            f.sim.tick();
            auto carrierPos = f.sim.getUnitState(vtolId).position;
            REQUIRE(roundToInt(f.sim.getUnitState(cargo).position.distance(carrierPos)) == 0);
            REQUIRE(roundToInt(f.sim.getUnitState(cargo2).position.distance(carrierPos)) == 0);

            // carrier facing does not move invalid-slot cargo off the
            // carrier position
            f.sim.getUnitState(vtolId).rotation = QuarterTurn;
            f.sim.tick();
            carrierPos = f.sim.getUnitState(vtolId).position;
            REQUIRE(roundToInt(f.sim.getUnitState(cargo).position.distance(carrierPos)) == 0);
            REQUIRE(roundToInt(f.sim.getUnitState(cargo2).position.distance(carrierPos)) == 0);
        }

        SECTION("a VTOL transport drops cargo at its carried position, not an attach piece")
        {
            Fixture f;
            registerTransportDefs(f);
            auto vtolDef = makeTargetDefinition();
            vtolDef.unitName = "vtolship";
            vtolDef.objectName = "VTOLSHIP";
            vtolDef.canFly = true;
            vtolDef.transportCapacity = 6;
            f.sim.unitDefinitions.insert_or_assign("VTOLSHIP", vtolDef);
            // a nonzero link piece that retail would ignore for slot -1
            f.sim.unitModelDefinitions.insert_or_assign("VTOLSHIP",
                createUnitModelDefinition(0_ss, {UnitPieceDefinition{"link", SimVector(24_ss, 0_ss, 0_ss)}}));
            f.sim.unitScriptDefinitions.insert_or_assign("VTOLSHIP", CobScript{{}, {}, {}, 0});

            auto vtolId = f.spawnTarget(f.playerId, true, "VTOLSHIP", 20, 20);
            auto cargo = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargo, vtolId));

            // the member detaches at its carried position (the carrier
            // position verbatim): the scripted destination only steers
            // the carrier, so the drop lands wherever the carrier
            // stopped -- not on a link-piece offset
            auto dest = f.sim.terrain.heightmapIndexToWorldCenter(30, 20);
            f.sim.getUnitState(vtolId).addOrder(UnloadOrder{dest});
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            const auto& dropped = f.sim.getUnitState(cargo);
            const auto& carrier = f.sim.getUnitState(vtolId);
            REQUIRE(!dropped.carriedBy);
            SimVector droppedXZ(dropped.position.x, 0_ss, dropped.position.z);
            SimVector carrierXZ(carrier.position.x, 0_ss, carrier.position.z);
            // landed within a cell of the carrier's position (the
            // carrier drifts slightly after release); a 24wu link-piece
            // offset would put it over a full cell away
            REQUIRE(roundToInt(droppedXZ.distance(carrierXZ)) < 16);
            auto rect = f.sim.computeFootprintRegion(dropped.position, f.sim.unitDefinitions.at("CARGO").movementCollisionInfo);
            REQUIRE(f.sim.isCollisionAt(rect));
        }
    }

    TEST_CASE("player transport load order")
    {
        // interactive Load gates on the carrier's CanLoad bit plus its
        // TransportSize/TransportCapacity budget and the member's
        // transportable flags -- none of which the scripted `i` path
        // consults (TotalA.exe 0x406780/0x4111b0)
        auto registerLoadDefs = [](Fixture& f) {
            auto shipDef = makeTargetDefinition();
            shipDef.unitName = "loader";
            shipDef.objectName = "LOADER";
            shipDef.canLoad = true;
            shipDef.transportSize = 4;
            shipDef.transportCapacity = 2;
            f.sim.unitDefinitions.insert_or_assign("LOADER", shipDef);
            auto cargoDef = makeTargetDefinition();
            cargoDef.unitName = "cargo";
            cargoDef.objectName = "CARGO";
            f.sim.unitDefinitions.insert_or_assign("CARGO", cargoDef);
            auto bigDef = makeTargetDefinition();
            bigDef.unitName = "bigcargo";
            bigDef.objectName = "BIGCARGO";
            bigDef.movementCollisionInfo = UnitDefinition::AdHocMovementClass{8, 8, 0, 0, 0, 0};
            f.sim.unitDefinitions.insert_or_assign("BIGCARGO", bigDef);
            auto heavyDef = makeTargetDefinition();
            heavyDef.unitName = "heavy";
            heavyDef.objectName = "HEAVY";
            heavyDef.cantBeTransported = true;
            f.sim.unitDefinitions.insert_or_assign("HEAVY", heavyDef);
            auto bldDef = makeTargetDefinition();
            bldDef.unitName = "bldg";
            bldDef.objectName = "BLDG";
            bldDef.isMobile = false;
            bldDef.canMove = false;
            bldDef.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Ground);
            f.sim.unitDefinitions.insert_or_assign("BLDG", bldDef);
            auto vtolDef = makeTargetDefinition();
            vtolDef.unitName = "vtol";
            vtolDef.objectName = "VTOL";
            vtolDef.canFly = true;
            vtolDef.canLoad = true;
            vtolDef.transportSize = 4;
            vtolDef.transportCapacity = 2;
            f.sim.unitDefinitions.insert_or_assign("VTOL", vtolDef);
            for (const auto& name : {"LOADER", "CARGO", "BIGCARGO", "HEAVY", "BLDG", "VTOL"})
            {
                f.sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                f.sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }
        };

        SECTION("a ground transport approaches and boards a friendly unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 200 && !f.sim.getUnitState(cargoId).carriedBy; ++i)
            {
                f.sim.tick();
            }

            const auto& cargo = f.sim.getUnitState(cargoId);
            const auto& ship = f.sim.getUnitState(shipId);
            REQUIRE(cargo.carriedBy == std::optional<UnitId>(shipId));
            REQUIRE(ship.passengers.size() == 1);
            // a transport without a TransportPickup script boards the
            // member at the invalid slot: it rides at the carrier origin
            REQUIRE(cargo.attachSlot == -1);
            f.sim.tick();
            REQUIRE(roundToInt(f.sim.getUnitState(cargoId).position.distance(f.sim.getUnitState(shipId).position)) == 0);
            // the order completes once the member is aboard
            REQUIRE(ship.orders.empty());
        }

        SECTION("a transport cannot load an enemy unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.enemyPlayerId, true, "CARGO", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a transport cannot load a CantBeTransported unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "HEAVY", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a transport cannot load a building")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto bldgId = f.spawnTarget(f.playerId, true, "BLDG", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{bldgId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(bldgId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a transport cannot load an unfinished unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, false, "CARGO", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a transport cannot load an oversize unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            // BIGCARGO footprintX 8 exceeds the transport's size 4
            auto cargoId = f.spawnTarget(f.playerId, true, "BIGCARGO", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a full transport completes the load order without boarding")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargo1 = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            auto cargo2 = f.spawnTarget(f.playerId, true, "CARGO", 12, 10);
            auto cargo3 = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);
            // capacity 2 filled through the scripted path
            REQUIRE(f.sim.tryBoardTransport(cargo1, shipId));
            REQUIRE(f.sim.tryBoardTransport(cargo2, shipId));

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargo3});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargo3).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a non-transport completes a load order immediately")
        {
            Fixture f;
            registerLoadDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "CARGO", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);

            f.sim.getUnitState(id).addOrder(LoadOrder{cargoId});
            f.sim.tick();
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(id).orders.empty());
        }

        SECTION("a transport cannot load itself")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{shipId});
            f.sim.tick();
            REQUIRE(!f.sim.getUnitState(shipId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a transport cannot load an already-carried unit")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto otherId = f.spawnTarget(f.playerId, true, "LOADER", 40, 40);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);
            REQUIRE(f.sim.tryBoardTransport(cargoId, otherId));

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            // the member stays with its original carrier
            REQUIRE(f.sim.getUnitState(cargoId).carriedBy == std::optional<UnitId>(otherId));
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a load order abandons when the passenger dies mid-approach")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 10, 10);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 50, 50);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 5; ++i)
            {
                f.sim.tick();
            }
            f.sim.killUnit(cargoId);
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
        }

        SECTION("clearing orders mid-approach leaves no half-carried state")
        {
            Fixture f;
            registerLoadDefs(f);
            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 10, 10);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 50, 50);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 5; ++i)
            {
                f.sim.tick();
            }
            f.sim.getUnitState(shipId).clearOrders();
            for (int i = 0; i < 20; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
        }

        SECTION("a TransportPickup script attaches the member at its own slot")
        {
            Fixture f;
            registerLoadDefs(f);
            // stock ARMTSHIP pattern: attach the member at the pad piece
            // (script piece index 1) via ATTACH_UNIT(member, piece, arg)
            CobScript pickupScript;
            pickupScript.pieces = {"base", "pad"};
            pickupScript.functions = {{"TransportPickup", 0}};
            pickupScript.instructions = {
                0x10021002, 0, // PUSH_LOCAL_VAR 0 (member arg)
                0x10021001, 1, // PUSH_CONSTANT 1 (slot piece)
                0x10021001, 0, // PUSH_CONSTANT 0 (attach arg)
                0x10083000, // ATTACH_UNIT
                0x10065000, // RETURN
            };
            f.sim.unitScriptDefinitions.insert_or_assign("LOADER", pickupScript);
            f.sim.unitModelDefinitions.insert_or_assign("LOADER",
                createUnitModelDefinition(0_ss, {UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)}, UnitPieceDefinition{"pad", SimVector(5_ss, 0_ss, 0_ss)}}));

            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);
            f.sim.getUnitState(shipId).rotation = SimAngle(0);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 300 && !f.sim.getUnitState(cargoId).carriedBy; ++i)
            {
                f.sim.tick();
            }

            const auto& cargo = f.sim.getUnitState(cargoId);
            REQUIRE(cargo.carriedBy == std::optional<UnitId>(shipId));
            // the member rode onto the script's pad slot, not the
            // carrier origin: it hovers at the pad piece's world
            // position (5wu off the carrier, in whichever direction
            // the carrier faced on arrival)
            REQUIRE(cargo.attachSlot == 1);
            auto rideDist = cargo.position.distance(f.sim.getUnitState(shipId).position);
            REQUIRE(rideDist > 3_ss);
            REQUIRE(rideDist < 8_ss);
            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("a script can re-attach a member to a new slot")
        {
            Fixture f;
            registerLoadDefs(f);
            // ARMTSHIP TransportPickup: attach at the pad, then re-attach
            // at slot -1 so the member rides inside the hull
            CobScript pickupScript;
            pickupScript.pieces = {"base", "pad"};
            pickupScript.functions = {{"TransportPickup", 0}};
            pickupScript.instructions = {
                0x10021002, 0, // PUSH_LOCAL_VAR 0
                0x10021001, 1, // PUSH_CONSTANT 1
                0x10021001, 0, // PUSH_CONSTANT 0
                0x10083000, // ATTACH_UNIT -> slot 1
                0x10021002, 0, // PUSH_LOCAL_VAR 0
                0x10021001, 0xFFFFFFFF, // PUSH_CONSTANT -1
                0x10021001, 0, // PUSH_CONSTANT 0
                0x10083000, // ATTACH_UNIT -> slot -1
                0x10065000, // RETURN
            };
            f.sim.unitScriptDefinitions.insert_or_assign("LOADER", pickupScript);
            f.sim.unitModelDefinitions.insert_or_assign("LOADER",
                createUnitModelDefinition(0_ss, {UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)}, UnitPieceDefinition{"pad", SimVector(5_ss, 0_ss, 0_ss)}}));

            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 26, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 300 && !f.sim.getUnitState(cargoId).carriedBy; ++i)
            {
                f.sim.tick();
            }

            const auto& cargo = f.sim.getUnitState(cargoId);
            REQUIRE(cargo.carriedBy == std::optional<UnitId>(shipId));
            REQUIRE(cargo.attachSlot == -1);
            f.sim.tick();
            REQUIRE(roundToInt(f.sim.getUnitState(cargoId).position.distance(f.sim.getUnitState(shipId).position)) == 0);
        }

        SECTION("a pickup script that never attaches abandons after bounded retries")
        {
            Fixture f;
            registerLoadDefs(f);
            // TransportPickup exists but sleeps forever, so the member
            // never attaches: retail re-approaches twice then pops
            CobScript sleepyScript;
            sleepyScript.functions = {{"TransportPickup", 0}};
            sleepyScript.instructions = {
                0x10021001, 99999, // PUSH_CONSTANT 99999
                0x10013000, // SLEEP
            };
            f.sim.unitScriptDefinitions.insert_or_assign("LOADER", sleepyScript);

            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 24, 20);

            f.sim.getUnitState(shipId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 2000; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
        }

        SECTION("a VTOL transport descends, queries a slot and climbs")
        {
            Fixture f;
            registerLoadDefs(f);
            // stock VTOL pattern: QueryTransport returns the link piece
            // index for the member to ride at
            CobScript vtolScript;
            vtolScript.pieces = {"base", "link"};
            vtolScript.functions = {{"QueryTransport", 0}};
            vtolScript.instructions = {
                0x10022000, // CREATE_LOCAL_VAR (return slot)
                0x10021001, 1, // PUSH_CONSTANT 1
                0x10023002, 0, // POP_LOCAL_VAR 0
                0x10065000, // RETURN
            };
            f.sim.unitScriptDefinitions.insert_or_assign("VTOL", vtolScript);
            f.sim.unitModelDefinitions.insert_or_assign("VTOL",
                createUnitModelDefinition(10_ss, {UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)}, UnitPieceDefinition{"link", SimVector(6_ss, 0_ss, 0_ss)}}));

            auto vtolId = f.spawnTarget(f.playerId, true, "VTOL", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 24, 20);
            f.sim.getUnitState(vtolId).rotation = SimAngle(0);

            f.sim.getUnitState(vtolId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 1500 && !f.sim.getUnitState(cargoId).carriedBy; ++i)
            {
                f.sim.tick();
            }

            const auto& cargo = f.sim.getUnitState(cargoId);
            REQUIRE(cargo.carriedBy == std::optional<UnitId>(vtolId));
            REQUIRE(cargo.attachSlot == 1);
            for (int i = 0; i < 1500 && !f.sim.getUnitState(vtolId).orders.empty(); ++i)
            {
                f.sim.tick();
            }
            // the VTOL climbs back to cruise altitude once attached
            REQUIRE(f.sim.getUnitState(vtolId).orders.empty());
            REQUIRE(f.sim.getUnitState(vtolId).passengers.size() == 1);
        }

        SECTION("a VTOL load aborts when the script offers no slot")
        {
            Fixture f;
            registerLoadDefs(f);
            auto vtolId = f.spawnTarget(f.playerId, true, "VTOL", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 24, 20);

            f.sim.getUnitState(vtolId).addOrder(LoadOrder{cargoId});
            for (int i = 0; i < 1500; ++i)
            {
                f.sim.tick();
            }
            // no QueryTransport entry point: the pickup cannot proceed
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(vtolId).orders.empty());
        }

        SECTION("a TransportDrop re-seat does not re-board a released member")
        {
            Fixture f;
            registerLoadDefs(f);
            // stock ARMTSHIP TransportDrop: re-seat the member on the
            // pad (attach-unit) then drop-unit. The member is released
            // by the order before the script thread runs, so neither
            // op may resurrect the link.
            CobScript dropScript;
            dropScript.pieces = {"base", "pad"};
            dropScript.functions = {{"TransportDrop", 0}};
            dropScript.instructions = {
                0x10021002, 0, // PUSH_LOCAL_VAR 0
                0x10021001, 1, // PUSH_CONSTANT 1
                0x10021001, 0, // PUSH_CONSTANT 0
                0x10083000, // ATTACH_UNIT (re-seat; member already gone)
                0x10021002, 0, // PUSH_LOCAL_VAR 0
                0x10084000, // DROP_UNIT
                0x10065000, // RETURN
            };
            f.sim.unitScriptDefinitions.insert_or_assign("LOADER", dropScript);

            auto shipId = f.spawnTarget(f.playerId, true, "LOADER", 20, 20);
            auto cargoId = f.spawnTarget(f.playerId, true, "CARGO", 10, 10);
            REQUIRE(f.sim.tryBoardTransport(cargoId, shipId));

            f.sim.getUnitState(shipId).addOrder(UnloadOrder{f.sim.getUnitState(shipId).position});
            for (int i = 0; i < 100; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(!f.sim.getUnitState(cargoId).carriedBy);
            REQUIRE(f.sim.getUnitState(shipId).passengers.empty());
            REQUIRE(f.sim.getUnitState(shipId).orders.empty());
        }

        SECTION("scripted boarding ignores interactive eligibility gates")
        {
            Fixture f;
            registerLoadDefs(f);
            // Miniement regression: scripted `i` boards units the player
            // path would reject -- an enemy member, an oversize member,
            // a CantBeTransported member, and onto a carrier without
            // CanLoad, past its nominal capacity
            auto dumbShip = makeTargetDefinition();
            dumbShip.unitName = "dumbship";
            dumbShip.objectName = "DUMBSHIP";
            dumbShip.transportCapacity = 1;
            f.sim.unitDefinitions.insert_or_assign("DUMBSHIP", dumbShip);
            f.sim.unitModelDefinitions.insert_or_assign("DUMBSHIP", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("DUMBSHIP", CobScript{{}, {}, {}, 0});

            auto shipId = f.spawnTarget(f.playerId, true, "DUMBSHIP", 20, 30);
            auto enemyCargo = f.spawnTarget(f.enemyPlayerId, true, "CARGO", 8, 8);
            auto bigCargo = f.spawnTarget(f.playerId, true, "BIGCARGO", 20, 8);
            auto heavyCargo = f.spawnTarget(f.playerId, true, "HEAVY", 32, 8);

            REQUIRE(f.sim.tryBoardTransport(enemyCargo, shipId));
            // capacity 1 is nominal, not a scripted cap
            REQUIRE(f.sim.tryBoardTransport(bigCargo, shipId));
            REQUIRE(f.sim.tryBoardTransport(heavyCargo, shipId));
            REQUIRE(f.sim.getUnitState(shipId).passengers.size() == 3);
        }
    }

    TEST_CASE("unreachable working-range targets do no work")
    {
        // ad-hoc movers treat only occupancy as a barrier, so a sealed
        // ring of immobile units walls the actor in and every
        // destination is unreachable: its path exhausts while still out
        // of working range, which must never be mistaken for arrival
        auto registerWall = [](Fixture& f) {
            auto wallDef = makeTargetDefinition();
            wallDef.unitName = "wall";
            wallDef.objectName = "WALL";
            wallDef.isMobile = false;
            wallDef.canMove = false;
            wallDef.movementCollisionInfo = UnitDefinition::AdHocMovementClass{1, 1, 0, 0, 0, 0};
            wallDef.yardMap = Grid<YardMapCell>(1, 1, YardMapCell::Ground);
            f.sim.unitDefinitions.insert_or_assign("WALL", wallDef);
            f.sim.unitModelDefinitions.insert_or_assign("WALL", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("WALL", CobScript{{}, {}, {}, 0});
        };
        // seal a footprint region inside a ring of walls `ring` cells
        // thick, so it is reachable only over the wall and an actor's
        // path exhausts still out of working range
        auto ringRect = [](Fixture& f, const DiscreteRect& inner, int ring) {
            auto outer = inner;
            outer.x -= ring;
            outer.y -= ring;
            outer.width += 2 * ring;
            outer.height += 2 * ring;
            for (int x = outer.x; x < outer.x + outer.width; ++x)
            {
                for (int y = outer.y; y < outer.y + outer.height; ++y)
                {
                    if (x == outer.x || y == outer.y || x == outer.x + outer.width - 1 || y == outer.y + outer.height - 1)
                    {
                        f.spawnTarget(f.playerId, true, "WALL", x, y);
                    }
                }
            }
        };
        auto ringUnit = [&](Fixture& f, UnitId id, int ring) {
            const auto& u = f.sim.getUnitState(id);
            auto rect = f.sim.computeFootprintRegion(u.position, f.sim.unitDefinitions.at(u.unitType).movementCollisionInfo);
            ringRect(f, rect, ring);
        };

        SECTION("an unreachable repair target is never repaired")
        {
            Fixture f;
            registerWall(f);
            auto builderId = f.spawnBuilder("SHORTARM");
            // wall the target off five cells out -- beyond SHORTARM's 60wu
            // build distance -- so the builder's path exhausts out of range
            auto targetId = f.spawnTarget(f.playerId, true, "DAMAGED", 40, 40);
            ringUnit(f, targetId, 5);
            f.sim.getUnitState(targetId).hitPoints = 100;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(targetId));
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            // path exhausts at the wall still out of range: the repair
            // arm never deploys and the target is untouched
            REQUIRE(!f.hasRepairStartedEvent());
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 100);
        }

        SECTION("an unreachable reclaim target is never reclaimed")
        {
            Fixture f;
            registerWall(f);
            auto builderId = f.spawnBuilder("SHORTARM");
            auto targetId = f.spawnTarget(f.playerId, true, "DAMAGED", 40, 40);
            ringUnit(f, targetId, 5);

            f.sim.getUnitState(builderId).addOrder(ReclaimOrder(targetId));
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(!f.hasReclaimStartedEvent());
            REQUIRE(!f.sim.getUnitState(targetId).isDead());
        }

        SECTION("an unreachable build site never starts construction")
        {
            Fixture f;
            registerWall(f);
            auto builderId = f.spawnBuilder("SHORTARM");
            // ring an empty site so the builder can approach but cannot
            // close to within working range of the build cell
            auto siteCell = f.sim.terrain.heightmapIndexToWorldCenter(40, 40);
            auto siteProbe = f.spawnTarget(f.playerId, true, "DAMAGED", 40, 40);
            ringUnit(f, siteProbe, 5);
            f.sim.quietlyKillUnit(siteProbe);
            // let the dead probe get swept before taking the headcount
            f.sim.tick();
            auto unitsBefore = f.countUnits();

            f.sim.getUnitState(builderId).addOrder(MissionBuildOrder("DAMAGED", siteCell));
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            // the site is unreachable: construction never begins and no
            // new unit is spawned
            REQUIRE(f.countUnits() == unitsBefore);
        }

        SECTION("an unreachable attack target is never engaged")
        {
            Fixture f;
            registerWall(f);

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(60);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.sim.getUnitState(attackerId).weapons[0] = UnitWeapon{"GUN"};
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 40, 40);
            ringUnit(f, targetId, 5);
            auto targetHp = f.sim.getUnitState(targetId).hitPoints;

            f.sim.getUnitState(attackerId).addOrder(AttackOrder(targetId));
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            // path exhausts at the wall still out of weapon range: the
            // weapon never aims and the target is undamaged
            const auto& weapon = f.sim.getUnitState(attackerId).weapons[0];
            REQUIRE(weapon);
            REQUIRE(!std::holds_alternative<UnitWeaponStateAttacking>(weapon->state));
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == targetHp);
        }
    }

    TEST_CASE("build weapon order")
    {
        auto registerStockpileDefs = [](Fixture& f) {
            WeaponDefinition wd{};
            wd.stockpile = true;
            wd.commandFire = true;
            wd.reloadTime = SimScalar(2); // 60 ticks per round
            wd.energyPerShot = Energy(600);
            wd.metalPerShot = Metal(300);
            f.sim.weaponDefinitions.insert_or_assign("FMD_ROCKET", wd);

            auto fmdDef = makeTargetDefinition();
            fmdDef.unitName = "fmd";
            fmdDef.objectName = "FMD";
            fmdDef.weapon1 = "FMD_ROCKET";
            // player storage is recomputed from unit storage each
            // settlement; give the FMD enough room to hold the income
            fmdDef.energyStorage = Energy(1000000);
            fmdDef.metalStorage = Metal(1000000);
            f.sim.unitDefinitions.insert_or_assign("FMD", fmdDef);
            f.sim.unitModelDefinitions.insert_or_assign("FMD", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("FMD", CobScript{{}, {}, {}, 0});
        };

        SECTION("stockpiles rounds across the reload period")
        {
            Fixture f;
            registerStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD");
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{2});

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 1);
            REQUIRE(f.sim.getUnitState(id).backgroundOrders.size() == 1);

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 2);
            REQUIRE(f.sim.getUnitState(id).backgroundOrders.empty());
        }

        SECTION("drains resources while building")
        {
            Fixture f;
            registerStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD");
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{1});

            auto energyBefore = f.sim.getPlayer(f.playerId).energy;
            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 1);
            REQUIRE(f.sim.getPlayer(f.playerId).energy < energyBefore);
        }

        SECTION("a computer owner's stockpile drain is unscaled")
        {
            // negative control for C8: the per-round resource drain is
            // a weapon cost, which retail routes through the unscaled
            // consumption accumulators -- the campaign difficulty
            // coefficient must not touch it (TotalA.exe +0xc0/+0xc4)
            Fixture f;
            registerStockpileDefs(f);
            f.sim.computerResourceScale = 0.5f;
            auto computerInfo = makePlayer();
            computerInfo.type = GamePlayerType::Computer;
            computerInfo.color = PlayerColorIndex(9);
            auto computerId = f.sim.addPlayer(computerInfo);

            auto id = f.spawnTarget(computerId, true, "FMD");
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{1});

            // 600 energy / 300 metal spread over 60 ticks drains the
            // full 10/5 per tick, not half of it
            for (int i = 0; i < 20; ++i)
            {
                f.sim.tick();
            }
            const auto& player = f.sim.getPlayer(computerId);
            REQUIRE(player.desiredEnergyConsumptionBuffer == Energy(200));
            REQUIRE(player.desiredMetalConsumptionBuffer == Metal(100));
        }

        SECTION("stalls while resources are unavailable")
        {
            Fixture f;
            registerStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD");
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{1});

            f.sim.getPlayer(f.playerId).energy = Energy(0);
            f.sim.getPlayer(f.playerId).metal = Metal(0);
            // the engine's economy model lets consumption run on credit
            // until the first one-second settlement flags the stall;
            // progress then halts permanently while resources stay at 0
            for (int i = 0; i < 90; ++i)
            {
                f.sim.tick();
            }
            const auto& order = std::get<BuildWeaponOrder>(f.sim.getUnitState(id).backgroundOrders.front());
            REQUIRE(order.progressTicks <= SimTicksPerSecond);
            REQUIRE(order.remaining == 1);
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);
        }

        SECTION("a unit without a stockpile weapon completes immediately")
        {
            Fixture f;
            auto id = f.spawnTarget(f.playerId);
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{3});
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(id).backgroundOrders.empty());
        }

        SECTION("does not block the foreground order queue")
        {
            Fixture f;
            registerStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD");
            f.sim.getUnitState(id).addOrder(WaitOrder{5});
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{2});

            for (int i = 0; i < 10; ++i)
            {
                f.sim.tick();
            }
            // the wait finished while the background build kept running
            REQUIRE(f.sim.getUnitState(id).orders.empty());
            REQUIRE(f.sim.getUnitState(id).backgroundOrders.size() == 1);
        }
    }

    TEST_CASE("stockpile weapon firing")
    {
        auto registerArmedStockpileDefs = [](Fixture& f) {
            // flat terrain so the projectile travels level
            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            // a stockpile weapon that is allowed to auto-acquire so the
            // full aim -> fire -> projectile lifecycle runs in the sim
            WeaponDefinition wd{};
            wd.stockpile = true;
            wd.commandFire = false;
            wd.maxRange = SimScalar(10000);
            wd.reloadTime = SimScalar(1);
            wd.burst = 1;
            wd.burstInterval = SimScalar(0);
            wd.velocity = SimScalar(10);
            wd.damage["DEFAULT"] = 10;
            wd.damageRadius = SimScalar(4);
            wd.energyPerShot = Energy(60);
            wd.metalPerShot = Metal(30);
            f.sim.weaponDefinitions.insert_or_assign("STOCKGUN", wd);

            auto def = makeTargetDefinition();
            def.unitName = "fmd";
            def.objectName = "FMD";
            def.weapon1 = "STOCKGUN";
            def.canAttack = true;
            def.energyStorage = Energy(1000000);
            def.metalStorage = Metal(1000000);
            f.sim.unitDefinitions.insert_or_assign("FMD", def);
            f.sim.unitScriptDefinitions.insert_or_assign("FMD", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("FMD", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

            // a soft target that survives a few hits
            auto& targetDef = f.sim.unitDefinitions.at("DAMAGED");
            targetDef.maxHitPoints = 100000;
            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
        };

        auto countProjectiles = [](GameSimulation& sim) {
            return static_cast<int>(std::distance(sim.projectiles.begin(), sim.projectiles.end()));
        };

        SECTION("cannot fire with zero stockpile")
        {
            Fixture f;
            registerArmedStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(countProjectiles(f.sim) == 0);
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);
        }

        SECTION("a built round fires exactly once and decrements")
        {
            Fixture f;
            registerArmedStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(id).fireOrders = UnitFireOrders::FireAtWill;
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            // nothing fires while the magazine is empty
            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(countProjectiles(f.sim) == 0);

            // remove the target while the round is built so the
            // magazine increment is observable before it is consumed
            std::vector<UnitId> toKill;
            for (const auto& [unitId, u] : f.sim.units)
            {
                if (u.isOwnedBy(f.enemyPlayerId))
                {
                    toKill.push_back(unitId);
                }
            }
            for (auto unitId : toKill)
            {
                f.sim.killUnit(unitId);
            }
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{1});
            int ticks = 0;
            while (f.sim.getUnitState(id).weapons[0]->stockpileCount == 0 && ticks < 200)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 1);
            REQUIRE(f.sim.getUnitState(id).backgroundOrders.empty());

            // a new target triggers the real aim -> fire path; the
            // launch consumes the round
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);
            f.sim.getUnitState(id).fireOrders = UnitFireOrders::FireAtWill;
            ticks = 0;
            while (countProjectiles(f.sim) == 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(countProjectiles(f.sim) == 1);
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);

            // no second shot until another round is built
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);
        }

        SECTION("multiple built rounds fire one by one")
        {
            Fixture f;
            registerArmedStockpileDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(id).fireOrders = UnitFireOrders::FireAtWill;

            // keep the target out of the picture until both rounds are
            // built, otherwise the first shot fires before round two
            f.sim.getUnitState(id).backgroundOrders.push_back(BuildWeaponOrder{2});
            int ticks = 0;
            while (f.sim.getUnitState(id).weapons[0]->stockpileCount < 2 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 2);

            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            // first shot consumes one round
            ticks = 0;
            while (f.sim.getUnitState(id).weapons[0]->stockpileCount == 2 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 1);

            // second shot consumes the last round; magazine stays empty
            ticks = 0;
            while (f.sim.getUnitState(id).weapons[0]->stockpileCount == 1 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(f.sim.getUnitState(id).weapons[0]->stockpileCount == 0);
        }
    }

    TEST_CASE("command fire order")
    {
        auto registerDGunDefs = [](Fixture& f) {
            // flat terrain so the projectile travels level
            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            // the stock disintegrator shape: commandfire, noexplode,
            // high damage, energy-gated, small nominal AoE that
            // noexplode suppresses
            WeaponDefinition wd{};
            wd.commandFire = true;
            wd.noExplode = true;
            wd.maxRange = SimScalar(240);
            wd.reloadTime = SimScalar(1.2f);
            wd.burst = 1;
            wd.burstInterval = SimScalar(0);
            wd.velocity = SimScalar(8);
            wd.damage["DEFAULT"] = 30000;
            // the stock disintegrator's nominal AoE; noexplode keeps
            // it from ever reaching neighbouring units
            wd.damageRadius = SimScalar(48);
            wd.energyPerShot = Energy(400);
            wd.metalPerShot = Metal(0);
            wd.physicsType = ProjectilePhysicsTypeLineOfSight();
            f.sim.weaponDefinitions.insert_or_assign("DGUN", wd);

            // an ordinary weapon sharing the unit so tests can check
            // which slot an order drives
            WeaponDefinition gun{};
            gun.maxRange = SimScalar(240);
            gun.reloadTime = SimScalar(0.5f);
            gun.burst = 1;
            gun.burstInterval = SimScalar(0);
            gun.velocity = SimScalar(8);
            gun.damage["DEFAULT"] = 10;
            gun.damageRadius = SimScalar(4);
            gun.physicsType = ProjectilePhysicsTypeLineOfSight();
            f.sim.weaponDefinitions.insert_or_assign("LASER", gun);

            auto def = makeTargetDefinition();
            def.unitName = "dgunner";
            def.objectName = "DGUNNER";
            def.canAttack = true;
            def.canDGun = true;
            def.weapon1 = "LASER";
            def.weapon3 = "DGUN";
            def.energyStorage = Energy(1000000);
            def.metalStorage = Metal(1000000);
            f.sim.unitDefinitions.insert_or_assign("DGUNNER", def);
            f.sim.unitScriptDefinitions.insert_or_assign("DGUNNER", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DGUNNER", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
        };

        auto countProjectiles = [](GameSimulation& sim) {
            return static_cast<int>(std::distance(sim.projectiles.begin(), sim.projectiles.end()));
        };

        SECTION("the command-fire weapon is never auto-acquired")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            f.sim.getUnitState(id).fireOrders = UnitFireOrders::FireAtWill;
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }

            // the command-fire slot stayed idle even though the target
            // sat in range; only the ordinary weapon engaged
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(id).weapons[2]->state));
        }

        SECTION("an ordinary attack order does not use the command-fire weapon")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(AttackOrder(targetId));

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            REQUIRE(std::holds_alternative<UnitWeaponStateAttacking>(f.sim.getUnitState(id).weapons[0]->state));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(id).weapons[2]->state));
        }

        SECTION("command fire at an in-range unit fires the command-fire weapon")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            int ticks = 0;
            while (countProjectiles(f.sim) == 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            // only the command-fire slot was driven
            REQUIRE(countProjectiles(f.sim) == 1);
            auto& weapon = *f.sim.getUnitState(id).weapons[2];
            REQUIRE(weapon.readyTime > GameTime(0));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(id).weapons[0]->state));
        }

        SECTION("command fire kills the target and completes the order")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            // 30000 damage against 500 hit points always kills; the
            // order itself completes as soon as the shot launches
            int ticks = 0;
            while (!f.sim.getUnitState(id).orders.empty() && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            REQUIRE(f.sim.getUnitState(id).orders.empty());

            // the order ended at launch; keep ticking until the
            // projectile lands and resolves the hit
            ticks = 0;
            while (countProjectiles(f.sim) > 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            // dead units are removed from the sim entirely
            REQUIRE(!f.sim.tryGetUnitState(targetId));
        }

        SECTION("command fire does not re-fire when the target survives")
        {
            // retail command fire is strictly one-shot per order even
            // against unit targets that outlive the hit
            Fixture f;
            registerDGunDefs(f);
            auto toughDef = makeTargetDefinition();
            toughDef.unitName = "tough";
            toughDef.objectName = "TOUGH";
            toughDef.maxHitPoints = 100000;
            f.sim.unitDefinitions.insert_or_assign("TOUGH", toughDef);
            f.sim.unitScriptDefinitions.insert_or_assign("TOUGH", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("TOUGH", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "TOUGH", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            for (int i = 0; i < 400; ++i)
            {
                f.sim.tick();
            }

            // exactly one projectile across the whole run, order done,
            // weapon released even though the target still lives
            REQUIRE(f.sim.getUnitState(id).orders.empty());
            REQUIRE(f.sim.getUnitState(targetId).hitPoints > 0);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(id).weapons[2]->state));
        }

        SECTION("command fire at a position fires one shot and completes")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(f.sim.terrain.heightmapIndexToWorldCenter(12, 8)));

            int ticks = 0;
            while (!f.sim.getUnitState(id).orders.empty() && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            REQUIRE(f.sim.getUnitState(id).orders.empty());

            // the weapon released its target and never re-fired
            for (int i = 0; i < 200; ++i)
            {
                f.sim.tick();
            }
            REQUIRE(countProjectiles(f.sim) == 0);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(id).weapons[2]->state));
        }

        SECTION("command fire consumes the per-shot energy cost at launch")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(f.sim.terrain.heightmapIndexToWorldCenter(12, 8)));

            for (int i = 0; i < 60; ++i)
            {
                f.sim.tick();
            }

            // 10000 starting energy less the 400 per-shot cost
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(9600));
        }

        SECTION("command fire still launches when energy is short")
        {
            // retail ignores the per-shot spend result at launch; the
            // energy check is a cursor-level hint only
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getPlayer(f.playerId).energy = Energy(100);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            int ticks = 0;
            while (countProjectiles(f.sim) == 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            REQUIRE(countProjectiles(f.sim) == 1);
            // the unaffordable cost was never recorded against the stock
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(100));
        }

        SECTION("command fire chases an out-of-range target")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            // far past the weapon's range so the approach path runs
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 40, 40);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            int ticks = 0;
            while (countProjectiles(f.sim) == 0 && ticks < 1200)
            {
                f.sim.tick();
                ++ticks;
            }

            REQUIRE(countProjectiles(f.sim) == 1);
        }

        SECTION("command fire completes when the target dies before launch")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));
            f.sim.killUnit(targetId);

            // dead units linger briefly in the sim before the cleanup
            // pass erases them; the order drops once the target is gone
            int ticks = 0;
            while (f.sim.tryGetUnitState(targetId) && ticks < 60)
            {
                f.sim.tick();
                ++ticks;
            }
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(id).orders.empty());
        }

        SECTION("command fire completes immediately with no command-fire weapon")
        {
            Fixture f;
            registerDGunDefs(f);
            // a unit with the candgun flag but no commandfire weapon --
            // retail caches the weapon pointer per unit and an absent
            // weapon leaves the order nothing to drive
            auto id = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.sim.unitDefinitions.at("DAMAGED").canDGun = true;
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            f.sim.tick();

            REQUIRE(f.sim.getUnitState(id).orders.empty());
            REQUIRE(countProjectiles(f.sim) == 0);
        }

        SECTION("a noexplode weapon damages only the struck unit")
        {
            Fixture f;
            registerDGunDefs(f);
            auto id = f.spawnTarget(f.playerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            // a second target sits right beside the first, inside the
            // nominal damage radius (2x2 footprints pack tightly)
            auto bystanderId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            int ticks = 0;
            while (!f.sim.getUnitState(id).orders.empty() && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            // the order ended at launch; keep ticking until the
            // projectile lands and resolves the hit
            ticks = 0;
            while (countProjectiles(f.sim) > 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            // dead units are removed from the sim entirely
            REQUIRE(!f.sim.tryGetUnitState(targetId));
            REQUIRE(f.sim.getUnitState(bystanderId).isAlive());
            REQUIRE(f.sim.getUnitState(bystanderId).hitPoints == 500);
        }

        SECTION("a computer owner's per-shot weapon cost is unscaled")
        {
            // negative control for C8: the campaign difficulty
            // coefficient applies to production-accumulator
            // contributions only. Weapon per-shot costs route through
            // the unscaled consumption path (TotalA.exe +0xc0/+0xc4),
            // so a computer-owned unit still pays the full 400.
            Fixture f;
            registerDGunDefs(f);
            f.sim.computerResourceScale = 0.5f;
            auto computerInfo = makePlayer();
            computerInfo.type = GamePlayerType::Computer;
            computerInfo.color = PlayerColorIndex(9);
            auto computerId = f.sim.addPlayer(computerInfo);

            auto id = f.spawnTarget(computerId, true, "DGUNNER", 8, 8);
            auto targetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

            int ticks = 0;
            while (countProjectiles(f.sim) == 0 && ticks < 400)
            {
                f.sim.tick();
                ++ticks;
            }

            REQUIRE(countProjectiles(f.sim) == 1);
            const auto& player = f.sim.getPlayer(computerId);
            REQUIRE(player.desiredEnergyConsumptionBuffer == Energy(400));
            REQUIRE(player.actualEnergyConsumptionBuffer == Energy(400));
        }
    }

    TEST_CASE("fog of war")
    {
        // map is 64x64 cells of 16 world units centered on the origin:
        // cell (8,8) sits near the top-left, cell (32,32) near center
        Fixture f;
        f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
        f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);

        SECTION("everything is detected when lineofsight is off")
        {
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 40, 40);

            f.sim.tick();

            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isLosVisibleToPlayer(viewerId, f.enemyPlayerId));
        }

        SECTION("an enemy outside sight range is not detected")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.mappingEnabled = true;
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // cell 26x8 is 288wu away: outside sight=100, inside radar=400
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            f.sim.tick();

            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            // sight=100 < radar=400: detected on radar only
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("an enemy inside sight range becomes LOS-visible")
        {
            f.sim.lineOfSightEnabled = true;
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // cell 12x8 is ~64wu away, inside sight=100
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();

            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(!f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("leaving sight keeps terrain explored but drops the unit")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.mappingEnabled = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();
            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            auto& pv = f.sim.playerVisibility[f.playerId.value];
            REQUIRE(pv.exploredGrid.get(12, 8) != 0);

            // move the viewer far away: its old surroundings stay
            // explored while the enemy leaves LOS
            f.sim.getUnitState(viewerId).position = f.sim.terrain.heightmapIndexToWorldCenter(50, 50);
            f.sim.tick();

            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(pv.exploredGrid.get(12, 8) != 0);
            REQUIRE(pv.losGrid.get(12, 8) == 0);
        }

        SECTION("explored terrain does not persist when mapping is off")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.mappingEnabled = false;
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();
            auto& pv = f.sim.playerVisibility[f.playerId.value];
            REQUIRE(pv.losGrid.get(12, 8) != 0);
            REQUIRE(pv.exploredGrid.get(12, 8) == 0);
        }

        SECTION("own units are always visible to their owner")
        {
            f.sim.lineOfSightEnabled = true;
            auto ownId = f.spawnTarget(f.playerId, true, "DAMAGED", 40, 40);

            f.sim.tick();

            REQUIRE(f.sim.isLosVisibleToPlayer(ownId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(ownId, f.playerId));
        }

        SECTION("a fire-at-will weapon cannot acquire an undetected enemy")
        {
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 448wu: inside weapon maxRange 500 but outside sight 100 and radar 400
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 36, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            // enemy is inside weapon range but outside sight
            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a fire-at-will weapon acquires a detected enemy")
        {
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            const auto& weaponState = std::get<UnitWeaponStateAttacking>(defender.weapons[0]->state);
            REQUIRE(std::get<UnitId>(weaponState.target) == enemyId);
        }

        SECTION("a fire-at-will weapon cannot acquire a radar-only contact")
        {
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 288wu: radar-only contact, inside weapon maxRange 500
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            // radar contact is known but not weapon-targetable
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("an immune enemy is never auto-acquired")
        {
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            MissionUnitState missionState;
            missionState.immunity = true;
            f.sim.getUnitState(enemyId).missionState = missionState;

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            // in LOS and in range, but immune
            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("an immune enemy takes no damage")
        {
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            MissionUnitState missionState;
            missionState.immunity = true;
            f.sim.getUnitState(enemyId).missionState = missionState;

            auto hpBefore = f.sim.getUnitState(enemyId).hitPoints;
            f.sim.applyDamage(enemyId, 9999, std::nullopt);

            REQUIRE(f.sim.getUnitState(enemyId).hitPoints == hpBefore);
            REQUIRE(f.sim.getUnitState(enemyId).isAlive());
        }

        SECTION("an activated targeting facility makes radar contacts weapon targets")
        {
            f.sim.lineOfSightEnabled = true;

            auto targDef = makeTargetDefinition();
            targDef.unitName = "targ";
            targDef.objectName = "TARG";
            targDef.isTargetingUpgrade = true;
            targDef.sightDistance = SimScalar(0);
            targDef.radarDistance = SimScalar(0);
            f.sim.unitDefinitions.insert_or_assign("TARG", targDef);
            f.sim.unitModelDefinitions.insert_or_assign("TARG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("TARG", CobScript{{}, {}, {}, 0});

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto targId = f.spawnTarget(f.playerId, true, "TARG", 12, 12);
            // 288wu: radar-only contact, inside weapon maxRange 500
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            auto& targ = f.sim.getUnitState(targId);
            targ.activated = true;
            targ.isSufficientlyPowered = true;

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            REQUIRE(f.sim.playerVisibility[f.playerId.value].hasTargetingUpgrade);
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateAttacking>(defender.weapons[0]->state));
        }

        SECTION("a deactivated targeting facility does not enable radar targeting")
        {
            f.sim.lineOfSightEnabled = true;

            auto targDef = makeTargetDefinition();
            targDef.unitName = "targ";
            targDef.objectName = "TARG";
            targDef.isTargetingUpgrade = true;
            targDef.sightDistance = SimScalar(0);
            targDef.radarDistance = SimScalar(0);
            f.sim.unitDefinitions.insert_or_assign("TARG", targDef);
            f.sim.unitModelDefinitions.insert_or_assign("TARG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("TARG", CobScript{{}, {}, {}, 0});

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            f.spawnTarget(f.playerId, true, "TARG", 12, 12);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            // facility exists but is not activated
            REQUIRE(!f.sim.playerVisibility[f.playerId.value].hasTargetingUpgrade);
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("an unpowered targeting facility does not enable radar targeting")
        {
            f.sim.lineOfSightEnabled = true;

            auto targDef = makeTargetDefinition();
            targDef.unitName = "targ";
            targDef.objectName = "TARG";
            targDef.isTargetingUpgrade = true;
            targDef.sightDistance = SimScalar(0);
            targDef.radarDistance = SimScalar(0);
            // drains more than the fixture's starting 1000 energy,
            // so the activated unit stays energy-stalled
            targDef.energyUse = Energy(5000);
            f.sim.unitDefinitions.insert_or_assign("TARG", targDef);
            f.sim.unitModelDefinitions.insert_or_assign("TARG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("TARG", CobScript{{}, {}, {}, 0});

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto targId = f.spawnTarget(f.playerId, true, "TARG", 12, 12);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            auto& targ = f.sim.getUnitState(targId);
            targ.activated = true;
            // energy-stalled: isSufficientlyPowered stays false

            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();

            REQUIRE(!f.sim.playerVisibility[f.playerId.value].hasTargetingUpgrade);
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a scripted attack-unit-type order targets an undetected enemy")
        {
            // retail's scripted type scan is the global nearest-enemy
            // scan (0x4071f0), not the contact-list collector: scripted
            // orders know the map and do not require detection
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 448wu: outside sight 100 and radar 400 -- fully undetected
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 36, 8);

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.weapons[0] = UnitWeapon{"GUN"};
            attacker.weapons[0]->readyTime = GameTime(1000000);
            f.sim.unitDefinitions.at(attacker.unitType).canAttack = true;
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            f.sim.tick();
            REQUIRE(!f.sim.isDetectedByPlayer(enemyId, f.playerId));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            const auto& order = std::get<AttackUnitTypeOrder>(attacker.orders.front());
            REQUIRE(order.currentTarget == enemyId);
        }

        SECTION("a scripted attack-unit-type order skips an immune enemy")
        {
            f.sim.lineOfSightEnabled = true;

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            auto attackerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            MissionUnitState missionState;
            missionState.immunity = true;
            f.sim.getUnitState(enemyId).missionState = missionState;

            auto& attacker = f.sim.getUnitState(attackerId);
            attacker.weapons[0] = UnitWeapon{"GUN"};
            attacker.weapons[0]->readyTime = GameTime(1000000);
            f.sim.unitDefinitions.at(attacker.unitType).canAttack = true;
            attacker.addOrder(AttackUnitTypeOrder("DAMAGED"));

            for (int i = 0; i < 120; ++i)
            {
                f.sim.tick();
            }

            // the only matching enemy is immune: the order completes
            // without ever acquiring a target
            REQUIRE(attacker.orders.empty());
        }
    }

    TEST_CASE("fog of war covers non-square terrain")
    {
        // AC05's visibility grid is 351x225 cells: strongly non-square.
        // A width-for-height transposition in the marking loops leaves
        // whole far bands permanently unexplored; a tall map makes the
        // failure obvious. This grid is 32x128 cells.
        GameSimulation sim(MapTerrain(Grid<unsigned char>(33, 129, 64), 0_ss), 255, 0, 0);
        sim.lineOfSightEnabled = true;
        sim.mappingEnabled = true;
        auto def = makeTargetDefinition();
        def.sightDistance = SimScalar(100);
        sim.unitDefinitions.insert_or_assign("DAMAGED", def);
        sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(0_ss, {}));
        sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {}, {}, 0});
        auto playerId = sim.addPlayer(makePlayer());

        // spawn a sighted unit near the bottom edge
        auto unitId = sim.trySpawnUnit("DAMAGED", playerId,
            sim.terrain.heightmapIndexToWorldCenter(16, 120), std::nullopt);
        REQUIRE(unitId);
        sim.getUnitState(*unitId).finishBuilding(sim.unitDefinitions.at("DAMAGED"));

        sim.tick();

        const auto& pv = sim.playerVisibility[playerId.value];
        REQUIRE(pv.losGrid.getWidth() == 32);
        REQUIRE(pv.losGrid.getHeight() == 128);
        REQUIRE(pv.exploredGrid.getWidth() == 32);
        REQUIRE(pv.exploredGrid.getHeight() == 128);
        // the unit's surroundings near the bottom edge are marked
        REQUIRE(pv.losGrid.get(16, 120) != 0);
        REQUIRE(pv.exploredGrid.get(16, 120) != 0);
        // cells far from any viewer stay unexplored
        REQUIRE(pv.losGrid.get(0, 0) == 0);
        REQUIRE(pv.exploredGrid.get(0, 0) == 0);
    }

    TEST_CASE("sensor detection")
    {
        Fixture f;
        f.sim.lineOfSightEnabled = true;

        SECTION("sonar detects a submerged enemy that radar cannot see")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 192wu: inside sonar and radar; fully submerged so radar
            // cannot see it (retail 0x467840 rejects y+height<seaLevel)
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(enemyId).position.y = SimScalar(-50);
            f.sim.getUnitState(enemyId).previousPosition.y = SimScalar(-50);

            f.sim.tick();

            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(!f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isSonarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("sonar detects a surface unit at sea level")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(0);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // ships sit at sea level: sonar accepts y <= seaLevel
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(enemyId).position.y = SimScalar(0);
            f.sim.getUnitState(enemyId).previousPosition.y = SimScalar(0);

            f.sim.tick();

            REQUIRE(f.sim.isSonarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("sonar ignores a land unit above sea level")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(0);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // sonar accepts only y <= seaLevel; a unit standing on the
            // terrain above water is not a sonar target
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(enemyId).position.y = SimScalar(64);
            f.sim.getUnitState(enemyId).previousPosition.y = SimScalar(64);

            f.sim.tick();

            REQUIRE(!f.sim.isSonarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("a powered-off sonar emitter detects nothing")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // onoffable unit never activated: retail +0x10e bit0 clear
            REQUIRE(!f.sim.getUnitState(viewerId).activated);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(enemyId).position.y = SimScalar(-50);
            f.sim.getUnitState(enemyId).previousPosition.y = SimScalar(-50);

            f.sim.tick();

            REQUIRE(!f.sim.isSonarDetectedByPlayer(enemyId, f.playerId));
        }

        SECTION("a powered-off radar emitter keeps sight but loses radar")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            REQUIRE(!f.sim.getUnitState(viewerId).activated);
            auto radarOnlyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);
            auto losId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();

            // radar off: the 288wu contact is gone; sight is passive
            // and unaffected by the power state
            REQUIRE(!f.sim.isRadarDetectedByPlayer(radarOnlyId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(radarOnlyId, f.playerId));
            REQUIRE(f.sim.isLosVisibleToPlayer(losId, f.playerId));
        }

        SECTION("a stealth enemy is invisible to radar and sonar but not LOS")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").stealth = true;
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto radarRangeId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);
            auto submergedId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(submergedId).position.y = SimScalar(-50);
            f.sim.getUnitState(submergedId).previousPosition.y = SimScalar(-50);
            auto losId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();

            REQUIRE(!f.sim.isRadarDetectedByPlayer(radarRangeId, f.playerId));
            REQUIRE(!f.sim.isSonarDetectedByPlayer(submergedId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(radarRangeId, f.playerId));
            // stealth does not suppress LOS
            REQUIRE(f.sim.isLosVisibleToPlayer(losId, f.playerId));
        }

        SECTION("a radar jammer suppresses radar contacts but not LOS")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& viewer = f.sim.getUnitState(viewerId);
            viewer.activated = true;
            viewer.isSufficientlyPowered = true;
            auto jammerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 24, 8);
            f.sim.unitDefinitions.at("DAMAGED").radarDistanceJam = SimScalar(200);
            auto& jammer = f.sim.getUnitState(jammerId);
            jammer.activated = true;
            jammer.isSufficientlyPowered = true;
            // 288wu radar contact sits 32wu from the jammer: jammed
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);
            // 64wu: inside LOS, jam coverage irrelevant
            auto losId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);

            f.sim.tick();

            REQUIRE(!f.sim.isRadarDetectedByPlayer(contactId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(contactId, f.playerId));
            // the jammer is inside its own radius: also radar-invisible
            REQUIRE(!f.sim.isRadarDetectedByPlayer(jammerId, f.playerId));
            // LOS is never suppressed by jamming
            REQUIRE(f.sim.isLosVisibleToPlayer(losId, f.playerId));
        }

        SECTION("a powered-off radar jammer suppresses nothing")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& viewer = f.sim.getUnitState(viewerId);
            viewer.activated = true;
            viewer.isSufficientlyPowered = true;
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 24, 8);
            f.sim.unitDefinitions.at("DAMAGED").radarDistanceJam = SimScalar(200);
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            f.sim.tick();

            // jammer never activated: contacts survive
            REQUIRE(f.sim.isRadarDetectedByPlayer(contactId, f.playerId));
        }

        SECTION("a radar contact outside jam coverage survives")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& viewer = f.sim.getUnitState(viewerId);
            viewer.activated = true;
            viewer.isSufficientlyPowered = true;
            auto jammerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 40, 40);
            f.sim.unitDefinitions.at("DAMAGED").radarDistanceJam = SimScalar(200);
            auto& jammer = f.sim.getUnitState(jammerId);
            jammer.activated = true;
            jammer.isSufficientlyPowered = true;
            // far outside the jammer's 200wu radius
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            f.sim.tick();

            REQUIRE(f.sim.isRadarDetectedByPlayer(contactId, f.playerId));
        }

        SECTION("a sonar jammer suppresses sonar contacts")
        {
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(0);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& viewer = f.sim.getUnitState(viewerId);
            viewer.activated = true;
            viewer.isSufficientlyPowered = true;
            auto jammerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistanceJam = SimScalar(200);
            auto& jammer = f.sim.getUnitState(jammerId);
            jammer.activated = true;
            jammer.isSufficientlyPowered = true;
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 22, 8);
            f.sim.getUnitState(contactId).position.y = SimScalar(-50);
            f.sim.getUnitState(contactId).previousPosition.y = SimScalar(-50);

            f.sim.tick();

            REQUIRE(!f.sim.isSonarDetectedByPlayer(contactId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(contactId, f.playerId));
        }

        SECTION("a jammed radar contact is not weapon-targetable even with a targeting facility")
        {
            auto targDef = makeTargetDefinition();
            targDef.unitName = "targ";
            targDef.objectName = "TARG";
            targDef.isTargetingUpgrade = true;
            f.sim.unitDefinitions.insert_or_assign("TARG", targDef);
            f.sim.unitModelDefinitions.insert_or_assign("TARG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("TARG", CobScript{{}, {}, {}, 0});

            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.sim.unitDefinitions.at("DAMAGED").onOffable = true;
            auto viewerId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& viewer = f.sim.getUnitState(viewerId);
            viewer.activated = true;
            viewer.isSufficientlyPowered = true;
            auto targId = f.spawnTarget(f.playerId, true, "TARG", 12, 12);
            auto& targ = f.sim.getUnitState(targId);
            targ.activated = true;
            targ.isSufficientlyPowered = true;
            auto jammerId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 24, 8);
            f.sim.unitDefinitions.at("DAMAGED").radarDistanceJam = SimScalar(200);
            auto& jammer = f.sim.getUnitState(jammerId);
            jammer.activated = true;
            jammer.isSufficientlyPowered = true;
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            f.sim.tick();

            REQUIRE(f.sim.playerVisibility[f.playerId.value].hasTargetingUpgrade);
            REQUIRE(!f.sim.isRadarDetectedByPlayer(contactId, f.playerId));
            REQUIRE(!f.sim.isWeaponTargetableByPlayer(contactId, f.playerId));
        }

        SECTION("a sonar-only contact is never weapon-targetable")
        {
            auto targDef = makeTargetDefinition();
            targDef.unitName = "targ";
            targDef.objectName = "TARG";
            targDef.isTargetingUpgrade = true;
            f.sim.unitDefinitions.insert_or_assign("TARG", targDef);
            f.sim.unitModelDefinitions.insert_or_assign("TARG", createUnitModelDefinition(0_ss, {}));
            f.sim.unitScriptDefinitions.insert_or_assign("TARG", CobScript{{}, {}, {}, 0});

            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").sonarDistance = SimScalar(300);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(0);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto targId = f.spawnTarget(f.playerId, true, "TARG", 12, 12);
            auto& targ = f.sim.getUnitState(targId);
            targ.activated = true;
            targ.isSufficientlyPowered = true;
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);
            f.sim.getUnitState(contactId).position.y = SimScalar(-50);
            f.sim.getUnitState(contactId).previousPosition.y = SimScalar(-50);

            f.sim.tick();

            // the targeting facility admits radar contacts only;
            // retail collects no weapon list for sonar contacts
            REQUIRE(f.sim.playerVisibility[f.playerId.value].hasTargetingUpgrade);
            REQUIRE(f.sim.isSonarDetectedByPlayer(contactId, f.playerId));
            REQUIRE(!f.sim.isWeaponTargetableByPlayer(contactId, f.playerId));
        }

        SECTION("radar coverage reaches past raw distance by the emitter footprint edge")
        {
            // retail effective radius = radarDistance + 2*unit+0x70,
            // where +0x70 is the mean footprint half-extent: a 2x2
            // footprint adds 32wu to a 100wu radar
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(0);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(100);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 112wu: beyond raw radar=100 but inside 100+32=132
            auto edgeId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 15, 8);
            // 144wu: beyond the edge-extended radius
            auto farId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 17, 8);

            f.sim.tick();

            REQUIRE(f.sim.isRadarDetectedByPlayer(edgeId, f.playerId));
            REQUIRE(!f.sim.isRadarDetectedByPlayer(farId, f.playerId));
        }

        SECTION("a lost radar contact drops on the next update")
        {
            // retail recomputes the contact bits every sensor pass;
            // there is no flag-level persistence
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto contactId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 26, 8);

            f.sim.tick();
            REQUIRE(f.sim.isRadarDetectedByPlayer(contactId, f.playerId));

            f.sim.getUnitState(contactId).position.x += SimScalar(2000);
            f.sim.tick();

            REQUIRE(!f.sim.isRadarDetectedByPlayer(contactId, f.playerId));
            REQUIRE(!f.sim.isDetectedByPlayer(contactId, f.playerId));
        }
    }

    TEST_CASE("command fire with retail stock data")
    {
        // end-to-end probe against authentic TA data: parses the real
        // ARMCOM/CORCOM FBI blocks and ARM_DISINTEGRATOR weapon TDF,
        // then runs a real CommandFireOrder through the simulation.
        // Skips silently when stock data is not mounted (CI-safe);
        // set RWE_DATA_PATH to a TA data directory to enable it.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));

        auto fbiBytes = vfs.readFile("units/ARMCOM.FBI");
        REQUIRE(fbiBytes);
        MovementClassDatabase armcomMcd;
        auto armcom = parseUnitDefinition(parseUnitFbi(parseTdfFromBytes(*fbiBytes)), armcomMcd);
        REQUIRE(armcom.canDGun);
        REQUIRE(armcom.weapon3 == "ARM_DISINTEGRATOR");

        auto corcomBytes = vfs.readFile("units/CORCOM.FBI");
        REQUIRE(corcomBytes);
        MovementClassDatabase corcomMcd;
        auto corcom = parseUnitDefinition(parseUnitFbi(parseTdfFromBytes(*corcomBytes)), corcomMcd);
        REQUIRE(corcom.canDGun);
        REQUIRE(corcom.weapon3 == "CORE_DISINTEGRATOR");

        // the shot target is a stock non-commander unit: a commander's
        // ExplodeAs (COMMANDER_BLAST, areaOfEffect 950) far exceeds the
        // disintegrator's 240 range, so an in-range commander kill would
        // always destroy the attacker too
        auto corakBytes = vfs.readFile("units/CORAK.FBI");
        REQUIRE(corakBytes);
        MovementClassDatabase corakMcd;
        auto corak = parseUnitDefinition(parseUnitFbi(parseTdfFromBytes(*corakBytes)), corakMcd);

        // stock data spreads weapon definitions across every .tdf file
        // in the weapons directory (WEAPONS.TDF, COMMANDER_BLAST.TDF, ...)
        std::unordered_map<std::string, WeaponTdf> weapons;
        for (const auto& fileName : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + fileName);
            REQUIRE(bytes);
            for (auto& pair : parseWeaponTdf(parseTdfFromBytes(*bytes)))
            {
                weapons.insert_or_assign(std::move(pair.first), std::move(pair.second));
            }
        }
        REQUIRE(!weapons.empty());

        std::optional<WeaponTdf> dgunTdf;
        std::optional<WeaponTdf> laserTdf;
        std::optional<WeaponTdf> coreDgunTdf;
        std::optional<WeaponTdf> coreLaserTdf;
        std::optional<WeaponTdf> armExplodeAsTdf;
        std::optional<WeaponTdf> coreExplodeAsTdf;
        std::optional<WeaponTdf> corakExplodeAsTdf;
        for (const auto& [name, wtdf] : weapons)
        {
            const auto upperName = toUpper(name);
            if (upperName == "ARM_DISINTEGRATOR")
            {
                dgunTdf = wtdf;
            }
            if (upperName == "ARMCOMLASER")
            {
                laserTdf = wtdf;
            }
            if (upperName == "CORE_DISINTEGRATOR")
            {
                coreDgunTdf = wtdf;
            }
            if (upperName == "CORCOMLASER")
            {
                coreLaserTdf = wtdf;
            }
            if (upperName == toUpper(armcom.explodeAs))
            {
                armExplodeAsTdf = wtdf;
            }
            if (upperName == toUpper(corcom.explodeAs))
            {
                coreExplodeAsTdf = wtdf;
            }
            if (upperName == toUpper(corak.explodeAs))
            {
                corakExplodeAsTdf = wtdf;
            }
        }
        REQUIRE(dgunTdf);
        REQUIRE(laserTdf);
        REQUIRE(coreDgunTdf);
        REQUIRE(coreLaserTdf);
        // a unit's death spawns its ExplodeAs weapon projectile;
        // killUnit requires the definition to be registered
        if (!armcom.explodeAs.empty())
        {
            REQUIRE(armExplodeAsTdf);
        }
        if (!corcom.explodeAs.empty())
        {
            REQUIRE(coreExplodeAsTdf);
        }
        if (!corak.explodeAs.empty())
        {
            REQUIRE(corakExplodeAsTdf);
        }
        auto dgun = parseWeaponDefinition(*dgunTdf);
        auto laser = parseWeaponDefinition(*laserTdf);
        auto coreDgun = parseWeaponDefinition(*coreDgunTdf);
        auto coreLaser = parseWeaponDefinition(*coreLaserTdf);

        // stock disintegrator contract. Note: totala1.hpi (base TA)
        // ships default=5500 while ccdata.ccx (Core Contingency) ships
        // default=30000; expansion archives win VFS precedence, so the
        // resolved value on a CC-era install is 30000.
        REQUIRE(dgun.commandFire);
        REQUIRE(dgun.noExplode);
        REQUIRE(dgun.maxRange == SimScalar(240));
        REQUIRE(dgun.energyPerShot == Energy(400));
        REQUIRE(dgun.metalPerShot == Metal(0));
        REQUIRE(dgun.damage.at("DEFAULT") == 30000);

        // run a real command-fire order through the sim with the
        // authentic definitions; the target uses the real CORCOM def
        // (commander HP is far below the disintegrator's damage)
        Fixture f;
        f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);
        f.sim.weaponDefinitions.insert_or_assign("ARM_DISINTEGRATOR", dgun);
        f.sim.weaponDefinitions.insert_or_assign("ARMCOMLASER", laser);
        f.sim.weaponDefinitions.insert_or_assign("CORE_DISINTEGRATOR", coreDgun);
        f.sim.weaponDefinitions.insert_or_assign("CORCOMLASER", coreLaser);
        if (armExplodeAsTdf)
        {
            f.sim.weaponDefinitions.insert_or_assign(toUpper(armcom.explodeAs), parseWeaponDefinition(*armExplodeAsTdf));
        }
        if (coreExplodeAsTdf && toUpper(corcom.explodeAs) != toUpper(armcom.explodeAs))
        {
            f.sim.weaponDefinitions.insert_or_assign(toUpper(corcom.explodeAs), parseWeaponDefinition(*coreExplodeAsTdf));
        }
        if (corakExplodeAsTdf && toUpper(corak.explodeAs) != toUpper(armcom.explodeAs) && toUpper(corak.explodeAs) != toUpper(corcom.explodeAs))
        {
            f.sim.weaponDefinitions.insert_or_assign(toUpper(corak.explodeAs), parseWeaponDefinition(*corakExplodeAsTdf));
        }
        // register any other weapon the spawned units reference
        for (const auto& def : {armcom, corak})
        {
            for (const auto& weaponName : {def.weapon1, def.weapon2, def.weapon3})
            {
                const auto upperName = toUpper(weaponName);
                if (upperName.empty() || f.sim.weaponDefinitions.find(upperName) != f.sim.weaponDefinitions.end())
                {
                    continue;
                }
                for (const auto& [name, wtdf] : weapons)
                {
                    if (toUpper(name) == upperName)
                    {
                        f.sim.weaponDefinitions.insert_or_assign(upperName, parseWeaponDefinition(wtdf));
                    }
                }
            }
        }
        f.sim.unitDefinitions.insert_or_assign("ARMCOM", armcom);
        f.sim.unitScriptDefinitions.insert_or_assign("ARMCOM", CobScript{{}, {"flare1"}, {}, 0});
        f.sim.unitModelDefinitions.insert_or_assign("ARMCOM", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
        f.sim.unitDefinitions.insert_or_assign("CORCOM", corcom);
        f.sim.unitScriptDefinitions.insert_or_assign("CORCOM", CobScript{{}, {"flare1"}, {}, 0});
        f.sim.unitModelDefinitions.insert_or_assign("CORCOM", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
        f.sim.unitDefinitions.insert_or_assign("CORAK", corak);
        f.sim.unitScriptDefinitions.insert_or_assign("CORAK", CobScript{{}, {"flare1"}, {}, 0});
        f.sim.unitModelDefinitions.insert_or_assign("CORAK", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));

        // dead units spawn their Corpse feature; LoadingScene resolves
        // corpses (and their FeatureDead/FeatureReclamate/FeatureBurnt
        // chains) out of every features/*.tdf block -- mirror that here
        std::unordered_map<std::string, FeatureTdf> featureTdfs;
        for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
        {
            auto bytes = vfs.readFile("features/" + name);
            REQUIRE(bytes);
            for (const auto& e : parseTdfFromBytes(*bytes).blocks)
            {
                featureTdfs.insert_or_assign(toUpper(e.first), parseFeatureTdf(*e.second));
            }
        }

        std::deque<std::string> featureQueue;
        std::unordered_map<std::string, FeatureDefinitionId> featureOpenSet;
        auto featureNextId = f.sim.featureDefinitions.getNextId();
        auto enqueueFeature = [&](const std::string& name) -> std::optional<FeatureDefinitionId>
        {
            if (name.empty())
            {
                return std::nullopt;
            }
            const auto upper = toUpper(name);
            if (auto it = f.sim.featureNameIndex.find(upper); it != f.sim.featureNameIndex.end())
            {
                return it->second;
            }
            if (auto it = featureOpenSet.find(upper); it != featureOpenSet.end())
            {
                return it->second;
            }
            REQUIRE(featureTdfs.find(upper) != featureTdfs.end());
            auto id = featureNextId;
            featureNextId = FeatureDefinitionId(featureNextId.value + 1);
            featureOpenSet.emplace(upper, id);
            featureQueue.push_back(upper);
            return id;
        };
        for (const auto& def : {armcom, corcom, corak})
        {
            enqueueFeature(def.corpse);
        }
        while (!featureQueue.empty())
        {
            const auto name = featureQueue.front();
            featureQueue.pop_front();
            const auto& tdf = featureTdfs.at(name);

            FeatureDefinition fdef;
            fdef.name = name;
            fdef.footprintX = tdf.footprintX;
            fdef.footprintZ = tdf.footprintZ;
            fdef.height = SimScalar(tdf.height);
            fdef.reclaimable = tdf.reclaimable;
            fdef.autoreclaimable = tdf.autoreclaimable;
            fdef.featureReclamate = enqueueFeature(tdf.featureReclamate);
            fdef.metal = tdf.metal;
            fdef.energy = tdf.energy;
            fdef.flamable = tdf.flamable;
            fdef.featureBurnt = enqueueFeature(tdf.featureBurnt);
            fdef.burnMin = tdf.burnMin;
            fdef.burnMax = tdf.burnMax;
            fdef.sparkTime = tdf.sparkTime;
            fdef.spreadChance = tdf.spreadChance;
            fdef.burnWeapon = tdf.burnWeapon;
            fdef.geothermal = tdf.geothermal;
            fdef.hitDensity = tdf.hitDensity;
            fdef.reproduce = tdf.reproduce;
            fdef.reproduceArea = tdf.reproduceArea;
            fdef.noDisplayInfo = tdf.noDisplayInfo;
            fdef.permanent = tdf.permanent;
            fdef.blocking = tdf.blocking;
            fdef.indestructible = tdf.indestructible;
            fdef.damage = tdf.damage;
            fdef.featureDead = enqueueFeature(tdf.featureDead);

            auto fid = f.sim.featureDefinitions.insert(fdef);
            REQUIRE(fid == featureOpenSet.at(name));
            f.sim.featureNameIndex.emplace(name, fid);
        }

        auto id = f.spawnTarget(f.playerId, true, "ARMCOM", 8, 8);
        auto targetId = f.spawnTarget(f.enemyPlayerId, true, "CORAK", 10, 8);
        f.sim.getUnitState(id).addOrder(CommandFireOrder(targetId));

        int ticks = 0;
        while (f.sim.tryGetUnitState(targetId) && ticks < 600)
        {
            f.sim.tick();
            ++ticks;
        }

        // the authentic D-Gun killed the stock CORAK target
        // and the one-shot order completed
        REQUIRE(!f.sim.tryGetUnitState(targetId));
        REQUIRE(f.sim.getUnitState(id).orders.empty());
    }

    TEST_CASE("sensor fields with retail stock data")
    {
        // parses authentic FBI blocks for the sensor census units.
        // Skips silently when stock data is not mounted (CI-safe);
        // set RWE_DATA_PATH to a TA data directory to enable it.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        MovementClassDatabase mcd;
        auto parseDef = [&](const char* name) {
            auto bytes = vfs.readFile(std::string("units/") + name + ".FBI");
            REQUIRE(bytes);
            return parseUnitDefinition(parseUnitFbi(parseTdfFromBytes(*bytes)), mcd);
        };

        // radar emitters: fixed installations and the mobile Seer
        REQUIRE(parseDef("ARMRAD").radarDistance == 1440_ss);
        REQUIRE(parseDef("CORARAD").radarDistance == 2880_ss);
        REQUIRE(parseDef("ARMSEER").radarDistance == 1120_ss);
        REQUIRE(parseDef("CORVRAD").radarDistance == 1120_ss);

        // sonar emitters: fixed arrays, ships, submarines
        REQUIRE(parseDef("ARMASON").sonarDistance == 2250_ss);
        REQUIRE(parseDef("CORSONAR").sonarDistance == 1223_ss);
        REQUIRE(parseDef("ARMSUB").sonarDistance == 405_ss);
        REQUIRE(parseDef("CORSSUB").sonarDistance == 658_ss);
        REQUIRE(parseDef("CORHUNT").sonarDistance == 1445_ss);

        // radar jammers: field units and the shipboard variants
        REQUIRE(parseDef("ARMASER").radarDistanceJam == 500_ss);
        REQUIRE(parseDef("ARMJAM").radarDistanceJam == 400_ss);
        REQUIRE(parseDef("ARMSJAM").radarDistanceJam == 700_ss);
        REQUIRE(parseDef("CORETER").radarDistanceJam == 420_ss);
        REQUIRE(parseDef("CORSPEC").radarDistanceJam == 490_ss);
        REQUIRE(parseDef("CORSJAM").radarDistanceJam == 730_ss);

        // the only stock sonar jammer: the Fibber
        REQUIRE(parseDef("ARMSCRAM").sonarDistanceJam == 650_ss);

        // the two stock stealth units
        REQUIRE(parseDef("ARMHAWK").stealth);
        REQUIRE(parseDef("CORVAMP").stealth);

        // targeting facilities
        REQUIRE(parseDef("ARMTARG").isTargetingUpgrade);
        REQUIRE(parseDef("CORTARG").isTargetingUpgrade);

        // an ordinary unit carries no sensor fields
        auto armpw = parseDef("ARMPW");
        REQUIRE(armpw.radarDistance == 0_ss);
        REQUIRE(armpw.sonarDistance == 0_ss);
        REQUIRE(armpw.radarDistanceJam == 0_ss);
        REQUIRE(armpw.sonarDistanceJam == 0_ss);
        REQUIRE(!armpw.stealth);
        REQUIRE(!armpw.isTargetingUpgrade);
    }

    TEST_CASE("cloak")
    {
        // CLOAKER: cloakCost=100 / cloakCostMoving=400 /
        // mincloakdistance=40, sight=200. The cloak upkeep runs once
        // per second on gameTime % 30 == 0 (retail's per-unit upkeep
        // pass, TotalA.exe 0x401360), so each assertion waits a
        // whole-number of upkeep intervals.
        Fixture f;

        auto runTicks = [](GameSimulation& sim, int count) {
            for (int i = 0; i < count; ++i)
            {
                sim.tick();
            }
        };

        SECTION("init_cloaked spawns with the request set")
        {
            // stock mines ship init_cloaked=1: the request flag is on
            // from spawn, the effective bit follows at the first pass
            f.sim.unitDefinitions.at("CLOAKER").initCloaked = true;
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);

            REQUIRE(f.sim.getUnitState(id).cloakRequested);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
        }

        SECTION("a requested cloak becomes effective on the per-second upkeep and drains the standing cost")
        {
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 29);
            REQUIRE(!f.sim.getUnitState(id).cloaked);

            // gameTime=30: first upkeep pass
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getUnitState(id).cloakRequested);
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(9900));
        }

        SECTION("clearing the request decloaks on the next upkeep pass")
        {
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);

            f.sim.getUnitState(id).cloakRequested = false;

            // the effective bit only clears when the upkeep runs
            runTicks(f.sim, 29);
            REQUIRE(f.sim.getUnitState(id).cloaked);
            f.sim.tick();
            REQUIRE(!f.sim.getUnitState(id).cloaked);
        }

        SECTION("energy shortfall drops the cloak but the request survives")
        {
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);

            f.sim.getPlayer(f.playerId).energy = Energy(50);
            runTicks(f.sim, 30);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getUnitState(id).cloakRequested);

            // the unit recloaks automatically once energy recovers
            f.sim.getPlayer(f.playerId).energy = Energy(10000);
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);
        }

        SECTION("a unit below the truncated cost cannot cloak")
        {
            // retail truncates the float cost to an integer before
            // comparing (0x4e43a0): energy 99 < int(100)
            f.sim.getPlayer(f.playerId).energy = Energy(99);
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 30);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(99));
        }

        SECTION("a moving unit pays cloakCostMoving")
        {
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(9900));

            // retail selects the moving cost from the in-motion pair;
            // set it just before the upkeep tick since the idle
            // physics pass zeroes currentSpeed each tick
            runTicks(f.sim, 29);
            std::get<UnitPhysicsInfoGround>(f.sim.getUnitState(id).physics).currentSpeed = SimScalar(1);
            f.sim.tick();
            REQUIRE(f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(9500));
        }

        SECTION("a detected enemy inside mincloakdistance suppresses cloak")
        {
            f.sim.lineOfSightEnabled = true;
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            // 32wu away: inside mincloakdistance=40 and the cloaker's
            // own sight=200, so the owner detects it
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 30);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getUnitState(id).cloakRequested);
            // detection pushed the blocked-until tick 90 out
            REQUIRE(f.sim.getUnitState(id).cloakBlockedUntil == GameTime(120));
            // no upkeep was debited while suppressed
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(10000));
        }

        SECTION("suppression dwells ~3 seconds after contact breaks")
        {
            f.sim.lineOfSightEnabled = true;
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 30);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getUnitState(id).cloakBlockedUntil == GameTime(120));

            // the enemy leaves; the +0xb0 dwell still blocks recloak
            f.sim.getUnitState(enemyId).position = f.sim.terrain.heightmapIndexToWorldCenter(50, 50);
            runTicks(f.sim, 60);
            REQUIRE(!f.sim.getUnitState(id).cloaked);

            // gameTime=120 reaches the dwell boundary and recloaks
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getPlayer(f.playerId).energy == Energy(9900));
        }

        SECTION("an enemy beyond mincloakdistance does not suppress")
        {
            f.sim.lineOfSightEnabled = true;
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            // 64wu: detected through the cloaker's sight but outside 40
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 12, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);
        }

        SECTION("a cloaked enemy is not a detected contact and does not suppress")
        {
            f.sim.lineOfSightEnabled = true;
            // the enemy is effectively cloaked from the start, so it
            // never enters the owner's visible-enemy set -- matching
            // retail, where the ctx+9 list only holds units that pass
            // the central visibility predicate (0x465ac0)
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 10, 8);
            f.sim.getUnitState(enemyId).cloaked = true;
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;

            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);
        }

        SECTION("a cloaked enemy leaves LOS but stays on radar")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);
            f.sim.unitDefinitions.at("DAMAGED").radarDistance = SimScalar(400);
            f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 64wu: inside our sight=100 and radar=400 but outside the
            // enemy's own mincloakdistance=40, so it is not suppressed
            // by proximity to our detected unit
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 12, 8);

            f.sim.tick();
            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));

            f.sim.getUnitState(enemyId).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(enemyId).cloaked);
            f.sim.tick();

            // retail 0x465ac0: owner -> 1, cloaked -> 0, else the LOS
            // grid -- cloak is not stealth, radar contact survives
            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isRadarDetectedByPlayer(enemyId, f.playerId));
            REQUIRE(f.sim.isDetectedByPlayer(enemyId, f.playerId));
            // radar-only contacts still need a targeting facility
            REQUIRE(!f.sim.isWeaponTargetableByPlayer(enemyId, f.playerId));
            // the owner always sees its own unit
            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.enemyPlayerId));
        }

        SECTION("a cloaked enemy is hidden even when line of sight is off")
        {
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 10, 8);
            f.sim.getUnitState(enemyId).cloakRequested = true;

            f.sim.tick();
            REQUIRE(f.sim.isLosVisibleToPlayer(enemyId, f.playerId));

            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(enemyId).cloaked);
            REQUIRE(!f.sim.isLosVisibleToPlayer(enemyId, f.playerId));
        }

        SECTION("a weapon cannot auto-acquire a cloaked enemy")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(10);
            weaponDef.damage["DEFAULT"] = 1;
            weaponDef.damageRadius = SimScalar(4);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            // 64wu: inside sight=100 but outside the enemy's own
            // mincloakdistance=40 -- it cannot be proximity-suppressed
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 12, 8);
            f.sim.getUnitState(enemyId).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(enemyId).cloaked);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a weapon drops its lock when the target cloaks")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(10);
            weaponDef.damage["DEFAULT"] = 1;
            weaponDef.damageRadius = SimScalar(4);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            // firing resolves the piece name through the COB script
            // and the offset through the model -- both need "flare1"
            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 64wu: visible through sight=100, outside the enemy's own
            // suppression radius so it can actually finish cloaking
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 12, 8);
            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();
            REQUIRE(std::get<UnitId>(std::get<UnitWeaponStateAttacking>(defender.weapons[0]->state).target) == enemyId);

            f.sim.getUnitState(enemyId).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(enemyId).cloaked);

            // the next weapon pass drops the undetectable lock
            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
        }

        SECTION("a dropped lock reacquires when the target decloaks")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(10);
            weaponDef.damage["DEFAULT"] = 1;
            weaponDef.damageRadius = SimScalar(4);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            // firing resolves the piece name through the COB script
            // and the offset through the model -- both need "flare1"
            f.sim.unitScriptDefinitions.insert_or_assign("DAMAGED", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("DAMAGED", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            // 64wu: visible through sight=100, outside the enemy's own
            // suppression radius so it can actually finish cloaking
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 12, 8);
            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.fireOrders = UnitFireOrders::FireAtWill;

            f.sim.tick();
            REQUIRE(std::get<UnitId>(std::get<UnitWeaponStateAttacking>(defender.weapons[0]->state).target) == enemyId);

            f.sim.getUnitState(enemyId).cloakRequested = true;
            runTicks(f.sim, 31);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));

            f.sim.getUnitState(enemyId).cloakRequested = false;
            runTicks(f.sim, 30);
            REQUIRE(!f.sim.getUnitState(enemyId).cloaked);
            f.sim.tick();
            REQUIRE(std::get<UnitId>(std::get<UnitWeaponStateAttacking>(defender.weapons[0]->state).target) == enemyId);
        }

        SECTION("an explicit attack order cannot lock a cloaked target but survives")
        {
            f.sim.lineOfSightEnabled = true;
            f.sim.unitDefinitions.at("DAMAGED").sightDistance = SimScalar(100);

            WeaponDefinition weaponDef{};
            weaponDef.maxRange = SimScalar(500);
            weaponDef.reloadTime = SimScalar(1);
            weaponDef.burst = 1;
            weaponDef.burstInterval = SimScalar(0);
            weaponDef.velocity = SimScalar(10);
            weaponDef.damage["DEFAULT"] = 1;
            weaponDef.damageRadius = SimScalar(4);
            f.sim.weaponDefinitions.insert_or_assign("GUN", weaponDef);

            // 64wu: inside sight=100 but outside the enemy's own
            // mincloakdistance=40 -- it cannot be proximity-suppressed
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "CLOAKER", 12, 8);
            f.sim.getUnitState(enemyId).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(enemyId).cloaked);

            auto defenderId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto& defender = f.sim.getUnitState(defenderId);
            defender.weapons[0] = UnitWeapon{"GUN"};
            defender.addOrder(AttackOrder(enemyId));

            f.sim.tick();
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(defender.weapons[0]->state));
            // the order stays and re-arms if the target decloaks
            REQUIRE(!defender.orders.empty());
        }

        SECTION("capture resets cloak state")
        {
            auto id = f.spawnTarget(f.playerId, true, "CLOAKER", 8, 8);
            f.sim.getUnitState(id).cloakRequested = true;
            runTicks(f.sim, 30);
            REQUIRE(f.sim.getUnitState(id).cloaked);

            REQUIRE(f.sim.transferUnitOwnership(id, f.enemyPlayerId, std::nullopt));

            REQUIRE(!f.sim.getUnitState(id).cloakRequested);
            REQUIRE(!f.sim.getUnitState(id).cloaked);
            REQUIRE(f.sim.getUnitState(id).cloakBlockedUntil == GameTime(0));
        }
    }

    TEST_CASE("alliance-aware orders (F2C)")
    {
        auto runTicks = [](GameSimulation& sim, int count) {
            for (int i = 0; i < count; ++i)
            {
                sim.tick();
            }
        };

        auto addAllyPlayer = [](Fixture& f) {
            // a third player sharing playerId's ally group
            f.sim.getPlayer(f.playerId).allyGroup = 1;
            auto info = makePlayer();
            info.color = PlayerColorIndex(2);
            info.allyGroup = 1;
            return f.sim.addPlayer(info);
        };

        SECTION("guard accepts an allied different-owner target")
        {
            Fixture f;
            auto allyId = addAllyPlayer(f);
            auto builderId = f.spawnBuilder();
            auto allyTargetId = f.spawnTarget(allyId, true, "DAMAGED", 20, 8);

            f.sim.getUnitState(builderId).addOrder(GuardOrder(allyTargetId));
            runTicks(f.sim, 3);

            // the order stands -- the builder follows the ally
            REQUIRE(!f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<GuardOrder>(f.sim.getUnitState(builderId).orders.front()));
        }

        SECTION("guard rejects an enemy target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto enemyTargetId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 20, 8);

            f.sim.getUnitState(builderId).addOrder(GuardOrder(enemyTargetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
        }

        SECTION("repair accepts an allied target")
        {
            Fixture f;
            auto allyId = addAllyPlayer(f);
            auto builderId = f.spawnBuilder();
            auto allyTargetId = f.spawnTarget(allyId);
            f.sim.getUnitState(allyTargetId).hitPoints = 300;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(allyTargetId));
            f.sim.tick();

            REQUIRE(std::holds_alternative<UnitBehaviorStateRepairing>(f.sim.getUnitState(builderId).behaviourState));
        }

        SECTION("repair rejects an enemy target")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto enemyTargetId = f.spawnTarget(f.enemyPlayerId);
            f.sim.getUnitState(enemyTargetId).hitPoints = 300;

            f.sim.getUnitState(builderId).addOrder(RepairOrder(enemyTargetId));
            f.sim.tick();

            REQUIRE(f.sim.getUnitState(builderId).orders.empty());
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(f.sim.getUnitState(builderId).behaviourState));
        }
    }

    TEST_CASE("builder patrol auto-activity (F2A)")
    {
        auto runTicks = [](GameSimulation& sim, int count) {
            for (int i = 0; i < count; ++i)
            {
                sim.tick();
            }
        };

        SECTION("diverts to repair a damaged friendly unit")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.getUnitState(targetId).hitPoints = 300;

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.divertTarget);
            REQUIRE(std::holds_alternative<PatrolRepairTarget>(*patrol.divertTarget));
            REQUIRE(std::get<PatrolRepairTarget>(*patrol.divertTarget).target == targetId);
        }

        SECTION("diverts to assist unfinished construction")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId, false); // unfinished
            REQUIRE(f.sim.getUnitState(targetId).isBeingBuilt(f.sim.unitDefinitions.at("DAMAGED")));

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.divertTarget);
            REQUIRE(std::holds_alternative<PatrolAssistTarget>(*patrol.divertTarget));
        }

        SECTION("diverts to reclaim while metal-starved")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            f.spawnFeature(f.wreckDefId, 12, 8);
            f.sim.getPlayer(f.playerId).metal = Metal(100); // < 20% of 10000

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.divertTarget);
            REQUIRE(std::holds_alternative<ReclaimTarget>(*patrol.divertTarget));
            REQUIRE(std::holds_alternative<FeatureId>(std::get<ReclaimTarget>(*patrol.divertTarget)));
        }

        SECTION("skips reclaim candidates of the wrong resource class")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            f.spawnFeature(f.wreckDefId, 12, 8); // WRECK: metal only
            // energy-starved, metal fine: a metal-only wreck is not a
            // candidate when the player is not metal-low
            f.sim.getPlayer(f.playerId).energy = Energy(100);

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            // WRECK holds metal but no energy; an energy-starved
            // (not metal-starved) player finds no eligible candidate
            REQUIRE(!patrol.divertTarget);
            REQUIRE(unit.navigationState.desiredDestination);
        }

        SECTION("resumes the route after the repair completes")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.getUnitState(targetId).hitPoints = 480; // one tick of work

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint, unit.position}));

            f.sim.tick();
            REQUIRE(std::get<PatrolOrder>(unit.orders.front()).divertTarget);

            f.sim.getUnitState(builderId).inBuildStance = true;
            runTicks(f.sim, 30);

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(!patrol.divertTarget);
            // navigating again toward the route
            REQUIRE(unit.navigationState.desiredDestination);
        }

        SECTION("keeps navigating with no eligible work")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint}));

            runTicks(f.sim, 70);

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(!patrol.divertTarget);
            REQUIRE(unit.navigationState.desiredDestination);
        }

        SECTION("a non-builder patrol never diverts")
        {
            Fixture f;
            auto tankId = f.spawnTarget(f.playerId, true, "DAMAGED", 8, 8);
            auto damagedId = f.spawnTarget(f.playerId, true, "DAMAGED", 12, 8);
            f.sim.getUnitState(damagedId).hitPoints = 300;

            auto& unit = f.sim.getUnitState(tankId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint}));

            runTicks(f.sim, 70);

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(!patrol.divertTarget);
            REQUIRE(std::holds_alternative<UnitBehaviorStateIdle>(unit.behaviourState));
        }

        SECTION("damaged enemies are not repair candidates")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto enemyId = f.spawnTarget(f.enemyPlayerId);
            f.sim.getUnitState(enemyId).hitPoints = 300;

            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint}));

            runTicks(f.sim, 70);

            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(!patrol.divertTarget);
        }

        SECTION("the scan runs on the retail 60-tick cadence")
        {
            Fixture f;
            auto builderId = f.spawnBuilder();
            auto& unit = f.sim.getUnitState(builderId);
            SimVector waypoint(unit.position.x + 100_ss, unit.position.y, unit.position.z);
            unit.addOrder(PatrolOrder({waypoint}));

            f.sim.tick(); // first scan fires immediately (scanTicks starts 0)
            const auto& patrol = std::get<PatrolOrder>(unit.orders.front());
            REQUIRE(patrol.scanTicks == 60);
        }
    }

    TEST_CASE("interceptor weapons (F2B)")
    {
        auto runTicks = [](GameSimulation& sim, int count) {
            for (int i = 0; i < count; ++i)
            {
                sim.tick();
            }
        };

        auto registerInterceptorDefs = [](Fixture& f) {
            // flat terrain so projectiles travel level
            f.sim.terrain = MapTerrain(Grid<unsigned char>(65, 65, 0), 0_ss);

            // the defender weapon: interceptor, generous coverage box
            WeaponDefinition amd{};
            amd.interceptor = true;
            amd.coverage = SimScalar(2000);
            amd.maxRange = SimScalar(32000);
            amd.reloadTime = SimScalar(1);
            amd.burst = 1;
            amd.burstInterval = SimScalar(0);
            amd.velocity = SimScalar(80);
            amd.damage["DEFAULT"] = 500;
            amd.damageRadius = SimScalar(96);
            amd.physicsType = ProjectilePhysicsTypeTracking{SimScalar(10)};
            f.sim.weaponDefinitions.insert_or_assign("AMD", amd);

            // the strategic missile: targetable, slow, ballistic
            WeaponDefinition nuke{};
            nuke.targetable = true;
            nuke.velocity = SimScalar(2);
            nuke.damage["DEFAULT"] = 1000;
            nuke.damageRadius = SimScalar(50);
            nuke.physicsType = ProjectilePhysicsTypeBallistic{};
            f.sim.weaponDefinitions.insert_or_assign("NUKE", nuke);

            auto def = makeTargetDefinition();
            def.unitName = "fmd";
            def.objectName = "FMD";
            def.weapon1 = "AMD";
            def.canAttack = true;
            f.sim.unitDefinitions.insert_or_assign("FMD", def);
            f.sim.unitScriptDefinitions.insert_or_assign("FMD", CobScript{{}, {"flare1"}, {}, 0});
            f.sim.unitModelDefinitions.insert_or_assign("FMD", createUnitModelDefinition(30_ss, {UnitPieceDefinition{"flare1", SimVector(0_ss, 5_ss, 0_ss)}}));
        };

        auto dropMissile = [](GameSimulation& sim, PlayerId owner, const SimVector& pos, const SimVector& dir) {
            auto p = sim.createProjectileFromWeapon(owner, "NUKE", pos, dir, 0_ss, std::nullopt);
            return sim.projectiles.emplace(std::move(p));
        };

        auto countProjectiles = [](GameSimulation& sim) {
            return static_cast<int>(std::distance(sim.projectiles.begin(), sim.projectiles.end()));
        };

        SECTION("ignores enemy units entirely")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;
            f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);

            runTicks(f.sim, 100);

            REQUIRE(countProjectiles(f.sim) == 0);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(fmdId).weapons[0]->state));
        }

        SECTION("acquires and kills an enemy targetable missile")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;

            auto silo = f.sim.getUnitState(fmdId).position;
            auto missileId = dropMissile(f.sim, f.enemyPlayerId, SimVector(silo.x + 500_ss, silo.y + 100_ss, silo.z), SimVector(0_ss, 0_ss, -1_ss));

            // first the weapon locks the missile
            f.sim.tick();
            auto& weapon = f.sim.getUnitState(fmdId).weapons[0];
            if (auto attacking = std::get_if<UnitWeaponStateAttacking>(&weapon->state))
            {
                REQUIRE(std::holds_alternative<ProjectileId>(attacking->target));
            }

            // then it fires a tracking interceptor that kills the missile
            bool missileDied = false;
            for (int i = 0; i < 300 && !missileDied; ++i)
            {
                f.sim.tick();
                auto missile = f.sim.projectiles.tryGet(missileId);
                missileDied = !missile || missile->get().isDead;
            }
            REQUIRE(missileDied);
        }

        SECTION("same-owner missiles are never engaged")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;

            auto silo = f.sim.getUnitState(fmdId).position;
            dropMissile(f.sim, f.playerId, SimVector(silo.x + 500_ss, silo.y + 100_ss, silo.z), SimVector(0_ss, 0_ss, -1_ss));

            runTicks(f.sim, 30);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(fmdId).weapons[0]->state));
        }

        SECTION("coverage is a per-axis box, not a radius")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;

            auto silo = f.sim.getUnitState(fmdId).position;
            // inside on Z but beyond coverage on X -> not engaged
            dropMissile(f.sim, f.enemyPlayerId, SimVector(silo.x + 2100_ss, silo.y + 100_ss, silo.z), SimVector(0_ss, 0_ss, -1_ss));

            runTicks(f.sim, 30);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(fmdId).weapons[0]->state));
        }

        SECTION("a claimed missile is not double-engaged")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;

            auto silo = f.sim.getUnitState(fmdId).position;
            auto missileId = dropMissile(f.sim, f.enemyPlayerId, SimVector(silo.x + 500_ss, silo.y + 100_ss, silo.z), SimVector(0_ss, 0_ss, -1_ss));

            // a phantom in-flight interceptor already claims the missile
            auto claim = f.sim.createProjectileFromWeapon(f.playerId, "AMD", silo, SimVector(0_ss, 1_ss, 0_ss), 0_ss, std::nullopt);
            claim.targetProjectile = missileId;
            f.sim.projectiles.emplace(std::move(claim));

            runTicks(f.sim, 30);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(fmdId).weapons[0]->state));
        }

        SECTION("manual unit locks are dropped, never fired at")
        {
            Fixture f;
            registerInterceptorDefs(f);
            auto fmdId = f.spawnTarget(f.playerId, true, "FMD", 8, 8);
            f.sim.getUnitState(fmdId).fireOrders = UnitFireOrders::FireAtWill;
            auto enemyId = f.spawnTarget(f.enemyPlayerId, true, "DAMAGED", 10, 8);
            f.sim.getUnitState(fmdId).addOrder(AttackOrder(enemyId));

            runTicks(f.sim, 60);
            REQUIRE(countProjectiles(f.sim) == 0);
            REQUIRE(std::holds_alternative<UnitWeaponStateIdle>(f.sim.getUnitState(fmdId).weapons[0]->state));
        }
    }

    TEST_CASE("armored damage scaling (F2D)")
    {
        SECTION("damage below 30000 is scaled by DamageModifier")
        {
            Fixture f;
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.unitDefinitions.at("DAMAGED").damageModifier = 0x8000; // 0.5 fixed-point
            f.sim.getUnitState(targetId).armored = true;

            f.sim.applyDamage(targetId, 100);
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 500 - 50);
        }

        SECTION("damage at 30000 bypasses the modifier")
        {
            Fixture f;
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.unitDefinitions.at("DAMAGED").damageModifier = 0x8000;
            f.sim.getUnitState(targetId).armored = true;

            f.sim.applyDamage(targetId, 30000);
            REQUIRE(f.sim.getUnitState(targetId).isDead());
        }

        SECTION("unarmored units take full damage")
        {
            Fixture f;
            auto targetId = f.spawnTarget(f.playerId);
            f.sim.unitDefinitions.at("DAMAGED").damageModifier = 0x8000;
            REQUIRE(!f.sim.getUnitState(targetId).armored);

            f.sim.applyDamage(targetId, 100);
            REQUIRE(f.sim.getUnitState(targetId).hitPoints == 500 - 100);
        }

        SECTION("setBuggerOff persists the flag")
        {
            Fixture f;
            auto factoryId = f.spawnTarget(f.playerId, true, "FACTORY", 20, 20);
            f.sim.setBuggerOff(factoryId, true);
            REQUIRE(f.sim.getUnitState(factoryId).buggerOff);
            f.sim.setBuggerOff(factoryId, false);
            REQUIRE(!f.sim.getUnitState(factoryId).buggerOff);
        }
    }

    TEST_CASE("maxunits per-player enforcement (F2E)")
    {
        SECTION("the cap blocks normal creation at the limit")
        {
            Fixture f;
            f.sim.maxUnitsPerPlayer = 2;

            auto a = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
            auto b = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
            REQUIRE(a);
            REQUIRE(b);

            auto c = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(14, 8), std::nullopt);
            REQUIRE(!c);
        }

        SECTION("the cap is per-player")
        {
            Fixture f;
            f.sim.maxUnitsPerPlayer = 2;

            f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
            f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);

            // the enemy's own count is independent
            auto e = f.sim.trySpawnUnit("DAMAGED", f.enemyPlayerId, f.sim.terrain.heightmapIndexToWorldCenter(14, 8), std::nullopt);
            REQUIRE(e);
        }

        SECTION("bypassUnitLimit skips the cap")
        {
            Fixture f;
            f.sim.maxUnitsPerPlayer = 1;

            f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
            auto authored = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt, false, std::nullopt, true);
            REQUIRE(authored);
        }

        SECTION("dead units free their slot")
        {
            Fixture f;
            f.sim.maxUnitsPerPlayer = 1;

            auto a = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
            REQUIRE(a);
            REQUIRE(!f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt));

            f.sim.killUnit(*a);
            auto c = f.sim.trySpawnUnit("DAMAGED", f.playerId, f.sim.terrain.heightmapIndexToWorldCenter(12, 8), std::nullopt);
            REQUIRE(c);
        }
    }
}
