#include <catch2/catch_test_macros.hpp>

#include <rwe/cob/CobOpCode.h>
#include <rwe/cob/CobValueId.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/cob.h>
#include <rwe/sim/util.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 0);
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
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0),
                Metal(0),
                Energy(0)};
        }
    }

    TEST_CASE("runUnitCobScripts bounds a thread that never suspends")
    {
        auto sim = makeSim();
        auto player = sim.addPlayer(makePlayer());

        UnitDefinition def{};
        def.unitName = "TESTUNIT";
        def.objectName = "TESTUNIT";
        def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
        def.turnRate = SimScalar(1);
        def.maxVelocity = SimScalar(10);
        def.acceleration = SimScalar(1);
        def.brakeRate = SimScalar(1);
        def.canMove = true;
        def.isMobile = true;
        def.maxHitPoints = 1000;
        def.buildTime = 100;
        sim.unitDefinitions.insert_or_assign("TESTUNIT", def);
        sim.unitModelDefinitions.insert_or_assign("TESTUNIT", createUnitModelDefinition(0_ss, {}));

        // a Create function that loops a query forever without
        // ever sleeping, blocking or finishing — a misbehaving
        // unit script must not starve the whole sim
        CobScript script{
            {
                static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                static_cast<uint32_t>(CobValueId::Health),
                static_cast<uint32_t>(OpCode::GET_VALUE),
                static_cast<uint32_t>(OpCode::JUMP),
                0,
            },
            {},
            {{"Create", 0}},
            0};
        sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", script);

        auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
        auto& env = *sim.getUnitState(id).cobEnvironment;
        REQUIRE(env.createThread("Create", {}).has_value());

        // must return instead of spinning on the interruption loop
        runUnitCobScripts(sim, id);

        // the thread remains scheduled for future ticks
        REQUIRE(!env.readyQueue.empty());
        REQUIRE(env.isNotCorrupt());
    }

    TEST_CASE("cob TURN speed semantics")
    {
        auto makeUnit = [](GameSimulation& sim) {
            UnitDefinition def{};
            def.unitName = "TESTUNIT";
            def.objectName = "TESTUNIT";
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 1000;
            def.buildTime = 100;
            sim.unitDefinitions.insert_or_assign("TESTUNIT", def);
            sim.unitModelDefinitions.insert_or_assign("TESTUNIT",
                createUnitModelDefinition(0_ss, {UnitPieceDefinition{"tongspivot", SimVector(0_ss, 0_ss, 0_ss)}}));
        };

        SECTION("non-positive speed snaps immediately like CORSPEC's Deactivate")
        {
            auto sim = makeSim();
            auto player = sim.addPlayer(makePlayer());
            makeUnit(sim);

            // CORSPEC.COB Deactivate(): `turn tongspivot to z-axis <0>
            // speed <-100>` compiles to a TURN whose speed operand is
            // negative — 0xFFFFB79C is <-100> in COB angular units.
            // Retail resolves it instantly; WAIT_FOR_TURN then returns
            // so close() runs. Here the piece snaps to the half-turn.
            CobScript script{
                {
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    4294949092u, // speed: -18204 as u32
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    32768, // angle: half turn
                    static_cast<uint32_t>(OpCode::TURN),
                    0, // piece index: tongspivot
                    2, // axis: z
                    static_cast<uint32_t>(OpCode::WAIT_FOR_TURN),
                    0,
                    2,
                    static_cast<uint32_t>(OpCode::RETURN),
                },
                {"tongspivot"},
                {{"Create", 0}},
                0};
            sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", script);

            auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            auto& env = *sim.getUnitState(id).cobEnvironment;
            REQUIRE(env.createThread("Create", {}).has_value());

            // must not assert on the negative speed; the piece lands
            // on the target with no pending operation
            runUnitCobScripts(sim, id);
            const auto& piece = sim.getUnitState(id).pieces[0];
            REQUIRE(piece.rotationZ == SimAngle(32768));
            REQUIRE(!piece.zTurnOperation);
        }

        SECTION("positive speed still installs an interpolated turn")
        {
            auto sim = makeSim();
            auto player = sim.addPlayer(makePlayer());
            makeUnit(sim);

            CobScript script{
                {
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    182, // speed: ~1 degree/tick
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    32768,
                    static_cast<uint32_t>(OpCode::TURN),
                    0,
                    2,
                    static_cast<uint32_t>(OpCode::RETURN),
                },
                {"tongspivot"},
                {{"Create", 0}},
                0};
            sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", script);

            auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            auto& env = *sim.getUnitState(id).cobEnvironment;
            REQUIRE(env.createThread("Create", {}).has_value());

            runUnitCobScripts(sim, id);
            const auto& piece = sim.getUnitState(id).pieces[0];
            REQUIRE(piece.rotationZ == SimAngle(0));
            REQUIRE(piece.zTurnOperation);
        }
    }

    TEST_CASE("cob compiled calls bind parameters in declaration order")
    {
        // Stock scripts push call-script/start-script arguments
        // left-to-right, so the last-declared parameter sits on top of
        // the stack. Regression: the VM previously popped them into
        // callee locals in stack order, reversing every multi-argument
        // call — e.g. BoomCalc(posxz, posy) in all four non-VTOL
        // transport scripts received its operands swapped.
        auto makeUnit = [](GameSimulation& sim) {
            UnitDefinition def{};
            def.unitName = "TESTUNIT";
            def.objectName = "TESTUNIT";
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.maxHitPoints = 1000;
            def.buildTime = 100;
            sim.unitDefinitions.insert_or_assign("TESTUNIT", def);
            sim.unitModelDefinitions.insert_or_assign("TESTUNIT", createUnitModelDefinition(0_ss, {}));
        };

        SECTION("call-script")
        {
            auto sim = makeSim();
            auto player = sim.addPlayer(makePlayer());
            makeUnit(sim);

            // Create: Sub(100, 7) writes (arg0 - arg1) to static 0.
            // Reversed binding would store 7 - 100 = -93 instead of 93.
            CobScript script{
                {
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    100,
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    7,
                    static_cast<uint32_t>(OpCode::CALL_SCRIPT),
                    1, // function index: Sub
                    2, // parameter count
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    0,
                    static_cast<uint32_t>(OpCode::RETURN),
                    // Sub @10
                    static_cast<uint32_t>(OpCode::CREATE_LOCAL_VAR),
                    static_cast<uint32_t>(OpCode::CREATE_LOCAL_VAR),
                    static_cast<uint32_t>(OpCode::PUSH_LOCAL_VAR),
                    0,
                    static_cast<uint32_t>(OpCode::PUSH_LOCAL_VAR),
                    1,
                    static_cast<uint32_t>(OpCode::SUB),
                    static_cast<uint32_t>(OpCode::POP_STATIC),
                    0,
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    0,
                    static_cast<uint32_t>(OpCode::RETURN),
                },
                {},
                {{"Create", 0}, {"Sub", 10}},
                1};
            sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", script);

            auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            auto& env = *sim.getUnitState(id).cobEnvironment;
            REQUIRE(env.createThread("Create", {}).has_value());

            runUnitCobScripts(sim, id);
            REQUIRE(env.getStatic(0) == 93);
        }

        SECTION("start-script")
        {
            auto sim = makeSim();
            auto player = sim.addPlayer(makePlayer());
            makeUnit(sim);

            // Create: starts Worker(100, 7); the new thread writes
            // (arg0 - arg1) to static 0.
            CobScript script{
                {
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    100,
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    7,
                    static_cast<uint32_t>(OpCode::START_SCRIPT),
                    1, // function index: Worker
                    2, // parameter count
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    0,
                    static_cast<uint32_t>(OpCode::RETURN),
                    // Worker @10
                    static_cast<uint32_t>(OpCode::CREATE_LOCAL_VAR),
                    static_cast<uint32_t>(OpCode::CREATE_LOCAL_VAR),
                    static_cast<uint32_t>(OpCode::PUSH_LOCAL_VAR),
                    0,
                    static_cast<uint32_t>(OpCode::PUSH_LOCAL_VAR),
                    1,
                    static_cast<uint32_t>(OpCode::SUB),
                    static_cast<uint32_t>(OpCode::POP_STATIC),
                    0,
                    static_cast<uint32_t>(OpCode::PUSH_CONSTANT),
                    0,
                    static_cast<uint32_t>(OpCode::RETURN),
                },
                {},
                {{"Create", 0}, {"Worker", 10}},
                1};
            sim.unitScriptDefinitions.insert_or_assign("TESTUNIT", script);

            auto id = sim.trySpawnUnit("TESTUNIT", player, SimVector(10_ss, 0_ss, 10_ss), SimAngle(0)).value();
            auto& env = *sim.getUnitState(id).cobEnvironment;
            REQUIRE(env.createThread("Create", {}).has_value());

            runUnitCobScripts(sim, id);
            REQUIRE(env.getStatic(0) == 93);
        }
    }
}
