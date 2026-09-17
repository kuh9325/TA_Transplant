#include <catch2/catch_test_macros.hpp>
#include <rwe/io/cob/Cob.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/sim/util.h>

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
            def.turnRate = SimScalar(1);
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
            return def;
        }

        UnitDefinition makeTargetDefinition()
        {
            UnitDefinition def{};
            def.unitName = "damaged";
            def.objectName = "DAMAGED";
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1);
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

            Fixture()
                : sim(makeSim())
            {
                // an ordinary construction unit whose own build time is far
                // shorter than the target's, and a commander whose build time
                // is far longer -- construction continuation must not depend
                // on the builder's buildTime
                sim.unitDefinitions.insert_or_assign("REPAIRER", makeBuilderDefinition("REPAIRER", 10));
                sim.unitDefinitions.insert_or_assign("COMMANDER", makeBuilderDefinition("COMMANDER", 5000));
                auto shortArm = makeBuilderDefinition("SHORTARM", 10);
                shortArm.buildDistance = SimScalar(60);
                sim.unitDefinitions.insert_or_assign("SHORTARM", shortArm);
                sim.unitDefinitions.insert_or_assign("DAMAGED", makeTargetDefinition());
                sim.unitDefinitions.insert_or_assign("BIGTARGET", makeBigTargetDefinition());
                for (const auto& name : {"REPAIRER", "COMMANDER", "SHORTARM", "DAMAGED", "BIGTARGET"})
                {
                    sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                    sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
                }

                playerId = sim.addPlayer(makePlayer());
                auto enemyInfo = makePlayer();
                enemyInfo.color = PlayerColorIndex(1);
                enemyPlayerId = sim.addPlayer(enemyInfo);
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
}
