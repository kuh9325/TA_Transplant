#include <catch2/catch_test_macros.hpp>
#include <rwe/io/cob/Cob.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0);
        }

        GamePlayerInfo makePlayer(PlayerColorIndex color)
        {
            return GamePlayerInfo{
                std::nullopt,
                GamePlayerType::Human,
                color,
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

        UnitDefinition makeMobileDef(const std::string& name, bool commander = false)
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
            def.commander = commander;
            def.maxHitPoints = 500;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }

        UnitDefinition makeBuildingDef(const std::string& name)
        {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.isMobile = false;
            def.canMove = false;
            def.yardMap = Grid<YardMapCell>(2, 2, YardMapCell::Ground);
            def.maxHitPoints = 500;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        }

        MissionObjective objective(const std::string& type, std::vector<std::string> params = {})
        {
            return MissionObjective{type, std::move(params)};
        }

        struct Fixture
        {
            GameSimulation sim;
            PlayerId localId;
            PlayerId enemyId;

            Fixture()
                : sim(makeSim())
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

                sim.unitDefinitions.insert_or_assign("ARMCOM", makeMobileDef("ARMCOM", true));
                sim.unitDefinitions.insert_or_assign("CORCOM", makeMobileDef("CORCOM", true));
                sim.unitDefinitions.insert_or_assign("ARMTANK", makeMobileDef("ARMTANK"));
                sim.unitDefinitions.insert_or_assign("CORTANK", makeMobileDef("CORTANK"));
                sim.unitDefinitions.insert_or_assign("ARMFUS", makeBuildingDef("ARMFUS"));
                for (const auto& name : {"ARMCOM", "CORCOM", "ARMTANK", "CORTANK", "ARMFUS"})
                {
                    sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                    sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
                }

                localId = sim.addPlayer(makePlayer(PlayerColorIndex(0)));
                enemyId = sim.addPlayer(makePlayer(PlayerColorIndex(1)));
            }

            void setObjectives(std::vector<MissionObjective> objectives)
            {
                MissionInfo info;
                info.localPlayerId = localId;
                info.objectives = std::move(objectives);
                sim.missionInfo = std::move(info);
                sim.initMissionObjectives();
            }

            /**
             * Spawn at centered world coordinates. Note OTA objective
             * parameters are top-left-origin: param = world + 520 on
             * this 65x65-cell map (1040 world units wide).
             */
            UnitId spawn(const std::string& type, PlayerId owner, float x, float z, bool complete = true)
            {
                auto id = sim.trySpawnUnit(type, owner, SimVector(SimScalar(x), SimScalar(50), SimScalar(z)), std::nullopt);
                REQUIRE(id);
                if (complete)
                {
                    sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(type));
                }
                return *id;
            }

            void eval()
            {
                sim.missionObjectives->tick(sim);
            }

            /** Run n eval ticks and return the final result. */
            MissionResult evals(int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    eval();
                }
                return sim.getMissionResult();
            }
        };
    }

    TEST_CASE("MissionObjectiveEvaluator defaults and debounce")
    {
        Fixture f;

        SECTION("empty objective lists gain retail defaults")
        {
            f.setObjectives({});
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives().size() == 1);
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].key == "DestroyAllUnits");
            REQUIRE(f.sim.missionObjectives->getDefeatObjectives().size() == 1);
            REQUIRE(f.sim.missionObjectives->getDefeatObjectives()[0].key == "AllUnitsKilled");
        }

        SECTION("unknown keys are ignored")
        {
            f.setObjectives({objective("NotARealObjective", {"X"})});
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives().size() == 1);
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].key == "DestroyAllUnits");
        }

        SECTION("result commits only after six passing evals")
        {
            f.spawn("ARMTANK", f.localId, 100, 100);
            // no enemies: default DestroyAllUnits is immediately true
            f.setObjectives({});
            for (int i = 0; i < 5; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::Victory);
        }

        SECTION("countdown progress persists across false evals")
        {
            // a live local unit holds off the default AllUnitsKilled defeat
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.setObjectives({objective("VictoryTimerRunsOut", {"10"})});
            // condition true once the clock reaches 300 ticks
            f.sim.gameTime = GameTime(300);
            f.eval(); // arm
            f.eval();
            f.eval();
            f.eval();
            f.eval(); // counter now 0 after 5 passing evals
            // a false eval leaves the counter armed rather than reset
            f.sim.gameTime = GameTime(0);
            f.eval();
            f.sim.gameTime = GameTime(300);
            f.eval(); // sixth cumulative passing eval commits
            REQUIRE(f.sim.getMissionResult() == MissionResult::Victory);
        }

        SECTION("committed result stops further evaluation")
        {
            f.setObjectives({objective("VictoryTimerRunsOut", {"0"})});
            REQUIRE(f.evals(10) == MissionResult::Victory);
            // evaluation is finished; result stays latched
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::Victory);
        }
    }

    TEST_CASE("MissionObjectiveEvaluator kill objectives")
    {
        Fixture f;
        f.spawn("ARMTANK", f.localId, 100, 100);

        SECTION("KillEnemyCommander: enemy commander death wins")
        {
            f.setObjectives({objective("KillEnemyCommander")});
            auto enemyCom = f.spawn("CORCOM", f.enemyId, -400, -400);
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(enemyCom);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillEnemyCommander: local commander death does not satisfy victory")
        {
            f.setObjectives({objective("KillEnemyCommander")});
            auto localCom = f.spawn("ARMCOM", f.localId, 200, 200);
            f.sim.killUnit(localCom);
            // defeat default is AllUnitsKilled; the ARMTANK keeps it from firing
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("DestroyAllUnits: last enemy death wins")
        {
            f.setObjectives({objective("DestroyAllUnits")});
            auto e1 = f.spawn("CORTANK", f.enemyId, -400, -400);
            auto e2 = f.spawn("CORTANK", f.enemyId, -350, -350);
            f.sim.killUnit(e1);
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(e2);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("DestroyAllUnits: counts enemy buildings too")
        {
            f.setObjectives({objective("DestroyAllUnits")});
            auto building = f.spawn("ARMFUS", f.enemyId, -400, -400);
            f.sim.killUnit(building);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillAllMobileUnits: enemy buildings do not block victory")
        {
            f.setObjectives({objective("KillAllMobileUnits")});
            f.spawn("ARMFUS", f.enemyId, -450, -450);
            auto tank = f.spawn("CORTANK", f.enemyId, -400, -400);
            f.sim.killUnit(tank);
            // latch requires a death event; the building remains but is not mobile
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillAllOfType: all enemy units of the type must die")
        {
            f.setObjectives({objective("KillAllOfType", {"CORTANK"})});
            auto t1 = f.spawn("CORTANK", f.enemyId, -400, -400);
            auto t2 = f.spawn("CORTANK", f.enemyId, -350, -350);
            f.spawn("ARMTANK", f.enemyId, -300, -300); // different type, irrelevant
            f.sim.killUnit(t1);
            f.evals(10);
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(t2);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillAllOfType: ignores local units of the type")
        {
            f.setObjectives({objective("KillAllOfType", {"ARMTANK"})});
            // the fixture's local ARMTANK must not count; killing the
            // only enemy ARMTANK satisfies the objective
            auto enemyTank = f.spawn("ARMTANK", f.enemyId, -400, -400);
            f.sim.killUnit(enemyTank);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillUnitType: decrements on matching enemy deaths")
        {
            f.setObjectives({objective("KillUnitType", {"CORTANK", "2"})});
            auto t1 = f.spawn("CORTANK", f.enemyId, -400, -400);
            auto t2 = f.spawn("CORTANK", f.enemyId, -350, -350);
            f.sim.killUnit(t1);
            f.evals(10);
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(t2);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("KillUnitType: local deaths of the type do not count")
        {
            f.setObjectives({objective("KillUnitType", {"ARMTANK", "1"})});
            auto localTank = f.spawn("ARMTANK", f.localId, 300, 300);
            f.sim.killUnit(localTank);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }
    }

    TEST_CASE("MissionObjectiveEvaluator defeat objectives")
    {
        Fixture f;
        f.spawn("CORTANK", f.enemyId, -400, -400); // keeps default win from firing

        SECTION("CommanderKilled: local commander death loses")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("CommanderKilled")});
            auto localCom = f.spawn("ARMCOM", f.localId, 100, 100);
            f.sim.killUnit(localCom);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("CommanderKilled: enemy commander death is not a defeat")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("CommanderKilled")});
            f.spawn("ARMCOM", f.localId, 100, 100);
            auto enemyCom = f.spawn("CORCOM", f.enemyId, -350, -350);
            f.sim.killUnit(enemyCom);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                // still in progress: another enemy CORTANK blocks victory
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("AllUnitsKilled: last local unit death loses")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("AllUnitsKilled")});
            auto localTank = f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.killUnit(localTank);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("AllUnitsKilled: under-construction units do not count")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("AllUnitsKilled")});
            // a unit still under construction is not a live unit: with no
            // completed local units the defeat condition already holds
            f.spawn("ARMTANK", f.localId, 100, 100, false);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("AllUnitsKilledOfType: counts units of any owner")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("AllUnitsKilledOfType", {"ARMTANK"})});
            auto mine = f.spawn("ARMTANK", f.localId, 100, 100);
            auto theirs = f.spawn("ARMTANK", f.enemyId, -350, -350);
            f.sim.killUnit(mine);
            f.evals(10);
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(theirs);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("UnitTypeKilled: decrements on matching deaths of any owner")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("UnitTypeKilled", {"ARMTANK", "2"})});
            auto mine = f.spawn("ARMTANK", f.localId, 100, 100);
            auto theirs = f.spawn("ARMTANK", f.enemyId, -350, -350);
            f.sim.killUnit(mine);
            f.evals(10);
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.killUnit(theirs);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }
    }

    TEST_CASE("MissionObjectiveEvaluator position and timer objectives")
    {
        Fixture f;
        f.spawn("CORTANK", f.enemyId, -500, -500); // live enemy blocks default DestroyAllUnits

        SECTION("MoveUnitToRadius: local unit inside radius wins")
        {
            // target = top-left (1000,1000) -> centered world (480,480), r=100
            f.setObjectives({objective("MoveUnitToRadius", {"ARMTANK", "1000", "1000", "100"})});
            auto tank = f.spawn("ARMTANK", f.localId, 470, 470);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("MoveUnitToRadius: outside radius does not trigger")
        {
            f.setObjectives({objective("MoveUnitToRadius", {"ARMTANK", "1000", "1000", "100"})});
            // dist = 180 > r on x
            auto tank = f.spawn("ARMTANK", f.localId, 300, 480);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
            // move inside later
            f.sim.getUnitState(tank).position.x = SimScalar(480);
            f.sim.getUnitState(tank).position.z = SimScalar(480);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("MoveUnitToRadius: boundary is inclusive")
        {
            // target centered at (0,0); unit at dist = 100 = r exactly
            f.setObjectives({objective("MoveUnitToRadius", {"ARMTANK", "520", "520", "100"})});
            f.spawn("ARMTANK", f.localId, 100, 0);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("MoveUnitToRadius: enemy unit inside does not count")
        {
            f.setObjectives({objective("MoveUnitToRadius", {"ANYTYPE", "1000", "1000", "100"})});
            f.spawn("ARMTANK", f.localId, 0, 0); // local exists but far away
            f.spawn("CORTANK", f.enemyId, 480, 480);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("MoveUnitToRadius: non-selectable scripted unit inside does not count")
        {
            // AC01's ARMGATE sits at its MoveUnitToRadius target but is
            // non-selectable (scripted `w 3600`); retail excludes it
            f.setObjectives({objective("MoveUnitToRadius", {"ANYTYPE", "1000", "1000", "100"})});
            auto gate = f.spawn("ARMFUS", f.localId, 480, 480);
            f.sim.getUnitState(gate).missionState = MissionUnitState{"", std::nullopt, false};
            f.spawn("ARMTANK", f.localId, 0, 0); // selectable local stays outside
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("AllUnitsKilled: non-selectable scripted unit does not prevent defeat")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("AllUnitsKilled")});
            auto gate = f.spawn("ARMFUS", f.localId, 100, 100);
            f.sim.getUnitState(gate).missionState = MissionUnitState{"", std::nullopt, false};
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("UnitTypePassesX: local unit of type crossing wins")
        {
            // param 520 -> cell 32; unit at centered x=0 -> top-left 520 -> cell 32
            f.setObjectives({objective("UnitTypePassesX", {"ARMTANK", "520"})});
            f.spawn("ARMTANK", f.localId, 0, 0);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("UnitTypePassesX: wrong side of the line does not trigger")
        {
            // param 560 -> cell 35; unit cell 32 -> |35-32| = 3 > 2
            f.setObjectives({objective("UnitTypePassesX", {"ARMTANK", "560"})});
            f.spawn("ARMTANK", f.localId, 0, 0);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("AnyUnitPassesX: enemy crossing loses")
        {
            // param 300 -> cell 18; enemy at centered x=-220 -> top-left 300 -> cell 18
            f.setObjectives({objective("DestroyAllUnits"), objective("AnyUnitPassesX", {"300"})});
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.spawn("CORTANK", f.enemyId, -220, 0);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }

        SECTION("AnyUnitPassesX: local crossing does not defeat")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("AnyUnitPassesX", {"300"})});
            f.spawn("ARMTANK", f.localId, -220, 0);
            for (int i = 0; i < 10; ++i)
            {
                f.eval();
                REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            }
        }

        SECTION("VictoryTimerRunsOut: boundary at seconds*30 ticks")
        {
            f.setObjectives({objective("VictoryTimerRunsOut", {"60"})});
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.gameTime = GameTime(60 * SimTicksPerSecond - 1);
            f.eval();
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.gameTime = GameTime(60 * SimTicksPerSecond);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("DeathTimerRunsOut: expiry loses")
        {
            f.setObjectives({objective("DestroyAllUnits"), objective("DeathTimerRunsOut", {"30"})});
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.gameTime = GameTime(30 * SimTicksPerSecond);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }
    }

    TEST_CASE("MissionObjectiveEvaluator combination semantics")
    {
        Fixture f;

        SECTION("multiple victory objectives require ALL")
        {
            f.setObjectives({
                objective("KillAllOfType", {"CORTANK"}),
                objective("MoveUnitToRadius", {"ARMCOM", "1000", "1000", "100"}),
            });
            auto tank = f.spawn("CORTANK", f.enemyId, -400, -400);
            auto com = f.spawn("ARMCOM", f.localId, 0, 0);
            f.sim.killUnit(tank);
            // one of two victory objectives satisfied -> no win
            f.evals(10);
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            // satisfy the second -> win (target centered world is 480,480)
            f.sim.getUnitState(com).position.x = SimScalar(480);
            f.sim.getUnitState(com).position.z = SimScalar(480);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("victory beats defeat when both pass on the same eval")
        {
            // both timers expire immediately; victory is evaluated first
            // and the defeat side is skipped entirely
            f.setObjectives({
                objective("VictoryTimerRunsOut", {"0"}),
                objective("DeathTimerRunsOut", {"0"}),
            });
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("defeat OR: any satisfied defeat objective loses")
        {
            f.setObjectives({
                objective("DestroyAllUnits"),
                objective("DeathTimerRunsOut", {"9999"}),
                objective("CommanderKilled"),
            });
            auto com = f.spawn("ARMCOM", f.localId, 100, 100);
            f.spawn("CORTANK", f.enemyId, -400, -400);
            f.sim.killUnit(com);
            REQUIRE(f.evals(6) == MissionResult::Defeat);
        }
    }

    TEST_CASE("MissionObjectiveEvaluator CaptureUnitType ownership-change semantics")
    {
        // Retail (TotalA.exe 0x48eeb0): the owner-change event fires
        // before the transfer is applied and requires the unit's
        // previous owner index == 1 (the enemy slot). No new-owner
        // check, no live/construction check, latch never clears.
        Fixture f;

        SECTION("enemy unit of matching type changing owner satisfies")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto fus = f.spawn("ARMFUS", f.enemyId, -200, -200);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.eval();
            REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
            f.sim.getUnitState(fus).owner = f.localId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), f.enemyId);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("wrong type does not satisfy")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto tank = f.spawn("CORTANK", f.enemyId, -200, -200);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.getUnitState(tank).owner = f.localId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(tank), f.enemyId);
            REQUIRE(f.evals(10) != MissionResult::Victory);
        }

        SECTION("previously local unit does not satisfy")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto fus = f.spawn("ARMFUS", f.localId, -200, -200);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.getUnitState(fus).owner = f.enemyId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), f.localId);
            REQUIRE(f.evals(10) != MissionResult::Victory);
        }

        SECTION("previous owner other than enemy index does not satisfy")
        {
            // Retail tests owner index == 1 literally; a third-party
            // owner would not count even though it is non-local.
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto fus = f.spawn("ARMFUS", f.enemyId, -200, -200);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), PlayerId(2));
            REQUIRE(f.evals(10) != MissionResult::Victory);
        }

        SECTION("already locally owned at start does not satisfy (event-latched, not polled)")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            f.spawn("ARMFUS", f.localId, 100, 100);
            f.spawn("ARMTANK", f.localId, 200, 200);
            REQUIRE(f.evals(10) != MissionResult::Victory);
        }

        SECTION("under-construction matching unit still satisfies (retail does not inspect it)")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto fus = f.spawn("ARMFUS", f.enemyId, -200, -200, false);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.getUnitState(fus).owner = f.localId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), f.enemyId);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }

        SECTION("latch survives a later owner change of the same unit")
        {
            f.setObjectives({objective("CaptureUnitType", {"ARMFUS"})});
            auto fus = f.spawn("ARMFUS", f.enemyId, -200, -200);
            f.spawn("ARMTANK", f.localId, 100, 100);
            f.sim.getUnitState(fus).owner = f.localId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), f.enemyId);
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);
            // A subsequent event cannot clear the retail latch.
            f.sim.getUnitState(fus).owner = f.enemyId;
            f.sim.missionObjectives->onUnitOwnerChanged(f.sim, f.sim.getUnitState(fus), f.localId);
            REQUIRE(f.sim.missionObjectives->getVictoryObjectives()[0].satisfied);
            REQUIRE(f.evals(6) == MissionResult::Victory);
        }
    }

    TEST_CASE("MissionObjectiveEvaluator skirmish isolation")
    {
        Fixture f;
        // no missionInfo -> no evaluator, skirmish path untouched
        REQUIRE(!f.sim.missionObjectives);
        REQUIRE(f.sim.getMissionResult() == MissionResult::InProgress);
        auto tank = f.spawn("ARMTANK", f.localId, 100, 100);
        f.sim.killUnit(tank);
        auto enemyTank = f.spawn("CORTANK", f.enemyId, -400, -400);
        f.sim.killUnit(enemyTank);
        // skirmish win logic still works: with both players' units dead
        // the win status is decided by player elimination, not objectives
    }
}
