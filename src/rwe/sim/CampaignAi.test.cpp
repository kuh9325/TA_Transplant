#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <unordered_set>
#include <rwe/LoadingScene_util.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/sim/CampaignAi.h>
#include <rwe/sim/CampaignAiProfile.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/sim/UnitOrder.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    namespace
    {
        GameSimulation makeSim()
        {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);
        }

        GamePlayerInfo makePlayer(PlayerColorIndex color, const std::string& side, GamePlayerType type)
        {
            return GamePlayerInfo{
                std::nullopt,
                type,
                color,
                GamePlayerStatus::Alive,
                side,
                std::nullopt,
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000)};
        }

        UnitDefinition makeMobileDef(const std::string& name)
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

        void registerUnit(GameSimulation& sim, const UnitDefinition& def)
        {
            auto name = def.unitName;
            sim.unitDefinitions.insert_or_assign(name, def);
            sim.unitModelDefinitions.insert_or_assign(def.objectName, createUnitModelDefinition(0_ss, {}));
            sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
        }

        /**
         * A world with a human player A and computer player B and
         * these defs: ARMCOM (mobile builder, builds ARMFUS),
         * ARMFUS (building), ARMFAC (factory, builds ARMTANK),
         * ARMTANK/CORTANK (mobile combat), ARMTUR (armed building).
         */
        GameSimulation makeWorld(PlayerId& playerA, PlayerId& playerB)
        {
            auto sim = makeSim();

            auto armcom = makeMobileDef("ARMCOM");
            armcom.builder = true;
            armcom.categories = {"ARM", "LEVEL3", "CONSTR"};
            armcom.buildOptions = {"ARMFUS", "ARMTUR"};
            registerUnit(sim, armcom);

            auto armfus = makeBuildingDef("ARMFUS");
            armfus.categories = {"ARM", "LEVEL2"};
            registerUnit(sim, armfus);

            auto armtur = makeBuildingDef("ARMTUR");
            armtur.categories = {"ARM", "LEVEL1", "WEAPON"};
            registerUnit(sim, armtur);

            auto armfac = makeBuildingDef("ARMFAC");
            armfac.builder = true;
            armfac.categories = {"ARM", "LEVEL1", "PLANT"};
            armfac.buildOptions = {"ARMTANK"};
            registerUnit(sim, armfac);

            auto armtank = makeMobileDef("ARMTANK");
            armtank.canAttack = true;
            armtank.categories = {"ARM", "LEVEL1", "TANK", "WEAPON"};
            registerUnit(sim, armtank);

            auto cortank = makeMobileDef("CORTANK");
            cortank.canAttack = true;
            cortank.categories = {"CORE", "LEVEL1", "TANK", "WEAPON"};
            registerUnit(sim, cortank);

            playerA = sim.addPlayer(makePlayer(PlayerColorIndex(0), "ARM", GamePlayerType::Human));
            playerB = sim.addPlayer(makePlayer(PlayerColorIndex(1), "CORE", GamePlayerType::Computer));
            return sim;
        }

        UnitId spawnCompleted(GameSimulation& sim, const std::string& type, PlayerId owner, int cellX, int cellZ)
        {
            auto id = sim.trySpawnUnit(type, owner, sim.terrain.heightmapIndexToWorldCenter(cellX, cellZ), std::nullopt);
            REQUIRE(id);
            auto& unit = sim.getUnitState(*id);
            unit.finishBuilding(sim.unitDefinitions.at(unit.unitType));
            return *id;
        }

        const CampaignAiPlayerState& aiStateOf(const GameSimulation& sim, PlayerId player)
        {
            auto it = std::find_if(sim.campaignAi.begin(), sim.campaignAi.end(), [&](const auto& st) { return st.player == player; });
            REQUIRE(it != sim.campaignAi.end());
            return *it;
        }

        int indexOf(const CampaignAiPlayerState& st, const std::string& name)
        {
            auto it = std::find(st.unitIndex.begin(), st.unitIndex.end(), name);
            REQUIRE(it != st.unitIndex.end());
            return static_cast<int>(it - st.unitIndex.begin());
        }
    }

    TEST_CASE("initCampaignAiPlayer builds per-player tables")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);

        SECTION("defaults: weight 100, limit -1, sorted index")
        {
            initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});
            const auto& st = aiStateOf(sim, playerB);
            REQUIRE(std::is_sorted(st.unitIndex.begin(), st.unitIndex.end()));
            REQUIRE(st.unitIndex.size() == sim.unitDefinitions.size());
            for (auto w : st.unitWeights)
            {
                REQUIRE(w == 100);
            }
            for (auto l : st.unitLimits)
            {
                REQUIRE(l == -1);
            }
        }

        SECTION("non-computer players get no state")
        {
            initCampaignAiPlayer(sim, playerA, CampaignAiProfile{});
            REQUIRE(sim.campaignAi.empty());
        }

        SECTION("weight multiplies, truncates and clamps to 0..100")
        {
            CampaignAiProfile p;
            p.weightDirectives = {
                {"ARMTANK", 0.5f},   // 100*0.5 -> 50
                {"CORTANK", 1.8f},   // clamped to 100
                {"ARMTUR", 0.0f},    // prohibited
            };
            initCampaignAiPlayer(sim, playerB, p);
            const auto& st = aiStateOf(sim, playerB);
            REQUIRE(st.unitWeights[indexOf(st, "ARMTANK")] == 50);
            REQUIRE(st.unitWeights[indexOf(st, "CORTANK")] == 100);
            REQUIRE(st.unitWeights[indexOf(st, "ARMTUR")] == 0);
        }

        SECTION("first matching directive wins")
        {
            CampaignAiProfile p;
            p.weightDirectives = {
                {"TANK", 0.25f},     // category hits ARMTANK + CORTANK
                {"ARMTANK", 0.9f},   // ignored: already locked
            };
            p.limitDirectives = {
                {"ARMTANK", 3},
                {"ARMTANK", 9},      // ignored
            };
            initCampaignAiPlayer(sim, playerB, p);
            const auto& st = aiStateOf(sim, playerB);
            REQUIRE(st.unitWeights[indexOf(st, "ARMTANK")] == 25);
            REQUIRE(st.unitWeights[indexOf(st, "CORTANK")] == 25);
            REQUIRE(st.unitLimits[indexOf(st, "ARMTANK")] == 3);
        }

        SECTION("category tokens match via FBI Category tags")
        {
            CampaignAiProfile p;
            p.weightDirectives = {{"CONSTR", 0.5f}};
            p.limitDirectives = {{"TANK", 2}};
            initCampaignAiPlayer(sim, playerB, p);
            const auto& st = aiStateOf(sim, playerB);
            REQUIRE(st.unitWeights[indexOf(st, "ARMCOM")] == 50);
            REQUIRE(st.unitLimits[indexOf(st, "ARMTANK")] == 2);
            REQUIRE(st.unitLimits[indexOf(st, "CORTANK")] == 2);
        }

        SECTION("unknown selectors are ignored")
        {
            CampaignAiProfile p;
            p.weightDirectives = {{"NOTAUNIT", 0.0f}, {"CIRUWES", 0.0f}};
            initCampaignAiPlayer(sim, playerB, p);
            const auto& st = aiStateOf(sim, playerB);
            for (auto w : st.unitWeights)
            {
                REQUIRE(w == 100);
            }
        }

        SECTION("unit self-directive applies when the profile left the kind unlocked")
        {
            // CORDECOM-style: the unit's own FBI ai_limit line caps it
            auto sim2 = makeSim();
            auto corcom = makeMobileDef("CORDECOM");
            corcom.aiSelfDirective = "limit CORDECOM 2;";
            registerUnit(sim2, corcom);
            auto tank = makeMobileDef("CORTANK");
            tank.categories = {"TANK"};
            registerUnit(sim2, tank);
            auto p2 = sim2.addPlayer(makePlayer(PlayerColorIndex(0), "CORE", GamePlayerType::Computer));

            initCampaignAiPlayer(sim2, p2, CampaignAiProfile{});
            const auto& st = aiStateOf(sim2, p2);
            REQUIRE(st.unitLimits[indexOf(st, "CORDECOM")] == 2);
            REQUIRE(st.unitLimits[indexOf(st, "CORTANK")] == -1);
        }

        SECTION("self-directive does not override a locked profile limit")
        {
            auto sim2 = makeSim();
            auto corcom = makeMobileDef("CORDECOM");
            corcom.aiSelfDirective = "limit CORDECOM 2;";
            registerUnit(sim2, corcom);
            auto p2 = sim2.addPlayer(makePlayer(PlayerColorIndex(0), "CORE", GamePlayerType::Computer));

            CampaignAiProfile p;
            p.limitDirectives = {{"CORDECOM", 5}};
            initCampaignAiPlayer(sim2, p2, p);
            const auto& st = aiStateOf(sim2, p2);
            REQUIRE(st.unitLimits[indexOf(st, "CORDECOM")] == 5);
        }
    }

    TEST_CASE("campaign AI repartition groups units")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        auto fac = spawnCompleted(sim, "ARMFAC", playerB, 10, 10);
        auto builder = spawnCompleted(sim, "ARMCOM", playerB, 14, 10);
        auto tank1 = spawnCompleted(sim, "ARMTANK", playerB, 18, 10);
        auto tank2 = spawnCompleted(sim, "ARMTANK", playerB, 20, 10);
        auto tank3 = spawnCompleted(sim, "ARMTANK", playerB, 22, 10);
        auto ignored = spawnCompleted(sim, "ARMTANK", playerB, 24, 10);
        auto enemy = spawnCompleted(sim, "ARMTANK", playerA, 30, 10);

        sim.getUnitState(ignored).missionState = MissionUnitState{};
        sim.getUnitState(ignored).missionState->aiIgnore = true;

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(st.groups[AiGroupFactory].size() == 1);
        REQUIRE(st.groups[AiGroupBuilder].size() == 1);
        REQUIRE(st.groups[AiGroupBuilder][0] == builder);
        // combat reserves alternate A/B; the aiIgnore and enemy tanks are excluded
        auto reserveCount = st.groups[AiGroupReserveA].size() + st.groups[AiGroupReserveB].size();
        REQUIRE(reserveCount == 3);
    }

    TEST_CASE("campaign AI factory produces from its build list")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});
        auto fac = spawnCompleted(sim, "ARMFAC", playerB, 10, 10);

        updateCampaignAi(sim);

        const auto& unit = sim.getUnitState(fac);
        REQUIRE(unit.buildQueue.size() == 1);
        REQUIRE(unit.buildQueue.front().first == "ARMTANK");
    }

    TEST_CASE("campaign AI factory obeys weight 0 and limits")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);

        SECTION("weight 0 prohibits the product")
        {
            CampaignAiProfile p;
            p.weightDirectives = {{"ARMTANK", 0.0f}};
            initCampaignAiPlayer(sim, playerB, p);
            auto fac = spawnCompleted(sim, "ARMFAC", playerB, 10, 10);
            sim.tick();
            REQUIRE(sim.getUnitState(fac).buildQueue.empty());
        }

        SECTION("limit caps production")
        {
            auto sim2 = makeWorld(playerA, playerB);
            CampaignAiProfile p;
            p.limitDirectives = {{"ARMTANK", 1}};
            initCampaignAiPlayer(sim2, playerB, p);
            auto fac = spawnCompleted(sim2, "ARMFAC", playerB, 10, 10);
            spawnCompleted(sim2, "ARMTANK", playerB, 30, 30); // already at the cap
            updateCampaignAi(sim2);
            REQUIRE(sim2.getUnitState(fac).buildQueue.empty());
        }

        SECTION("missing limit argument prohibits the product")
        {
            auto sim2 = makeWorld(playerA, playerB);
            auto profile = parseCampaignAiProfile("plan any\nlimit ARMTANK\n", CampaignAiDifficulty::Easy);
            initCampaignAiPlayer(sim2, playerB, profile);
            auto fac = spawnCompleted(sim2, "ARMFAC", playerB, 10, 10);
            updateCampaignAi(sim2);
            REQUIRE(sim2.getUnitState(fac).buildQueue.empty());
        }
    }

    TEST_CASE("campaign AI builders get a build order")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});
        auto builder = spawnCompleted(sim, "ARMCOM", playerB, 10, 10);

        updateCampaignAi(sim);

        const auto& unit = sim.getUnitState(builder);
        REQUIRE(unit.orders.size() == 1);
        const auto& order = unit.orders.front();
        auto kind = std::get_if<MissionBuildOrder>(&order) != nullptr || std::get_if<MoveOrder>(&order) != nullptr;
        REQUIRE(kind);
        // on flat free terrain a site for a 2x2 ground yardmap is found
        if (auto* mb = std::get_if<MissionBuildOrder>(&order))
        {
            REQUIRE((mb->unitType == "ARMFUS" || mb->unitType == "ARMTUR"));
        }
    }

    TEST_CASE("campaign AI leaves busy builders alone")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});
        auto builder = spawnCompleted(sim, "ARMCOM", playerB, 10, 10);

        // a queued scripted order is not disturbed
        sim.getUnitState(builder).orders.push_back(UnitOrder(MoveOrder(SimVector(500_ss, 0_ss, 500_ss))));
        auto before = sim.getUnitState(builder).orders.size();
        updateCampaignAi(sim);
        // the scripted queue still leads; AI may not push anything ahead of it
        REQUIRE(sim.getUnitState(builder).orders.size() <= before);
        REQUIRE(std::get_if<MoveOrder>(&sim.getUnitState(builder).orders.front()));
    }

    TEST_CASE("campaign AI musters an attack squad")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10);
        }
        auto enemy = spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(st.attackTarget == enemy);
        for (auto g : {AiGroupReserveA, AiGroupReserveB})
        {
            for (auto id : st.groups[g])
            {
                const auto& u = sim.getUnitState(id);
                REQUIRE(!u.orders.empty());
                const auto* attack = std::get_if<AttackOrder>(&u.orders.front());
                REQUIRE(attack);
                REQUIRE(std::holds_alternative<UnitId>(attack->target));
                REQUIRE(std::get<UnitId>(attack->target) == enemy);
            }
        }
    }

    TEST_CASE("campaign AI attack scan respects aiIgnore")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        for (int i = 0; i < 4; ++i)
        {
            auto id = spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10);
            sim.getUnitState(id).missionState = MissionUnitState{};
            sim.getUnitState(id).missionState->aiIgnore = true;
        }
        spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(!st.attackTarget);
        REQUIRE(st.groups[AiGroupReserveA].empty());
        REQUIRE(st.groups[AiGroupReserveB].empty());
    }

    TEST_CASE("campaign AI is deterministic across identical sims")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId a1{0}, b1{0}, a2{0}, b2{0};
        auto simA = makeWorld(a1, b1);
        auto simB = makeWorld(a2, b2);

        auto profile = parseCampaignAiProfile(
            "plan any\nweight WEAPON 1.0\nlimit ARMTANK 6\n",
            CampaignAiDifficulty::Medium);
        initCampaignAiPlayer(simA, b1, profile);
        initCampaignAiPlayer(simB, b2, profile);

        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(simA, "CORTANK", b1, 10 + 2 * i, 10);
            spawnCompleted(simB, "CORTANK", b2, 10 + 2 * i, 10);
        }
        spawnCompleted(simA, "ARMFAC", b1, 20, 20);
        spawnCompleted(simB, "ARMFAC", b2, 20, 20);
        spawnCompleted(simA, "ARMCOM", b1, 24, 20);
        spawnCompleted(simB, "ARMCOM", b2, 24, 20);
        spawnCompleted(simA, "ARMTANK", a1, 40, 10);
        spawnCompleted(simB, "ARMTANK", a2, 40, 10);

        for (int i = 0; i < 400; ++i)
        {
            simA.tick();
            simB.tick();
            REQUIRE(simA.computeHash() == simB.computeHash());
        }
    }

    TEST_CASE("campaign AI task timers tick at retail cadence")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        sim.tick();

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(st.nextRepartition == GameTime(31));
        REQUIRE(st.nextFactoryScan == GameTime(31));
        REQUIRE(st.nextBuilderScan == GameTime(91));
        REQUIRE(st.nextAttackScan == GameTime(301));
    }

    TEST_CASE("campaign AI drops captured units and never commands them")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        // five AI combat units fill a reserve group; one is scripted
        // like the CC13 Atlas (`p` patrol suppresses the implicit
        // make-selectable, so the selectability gate is still down
        // when the player captures it)
        std::vector<UnitId> tanks;
        for (int i = 0; i < 5; ++i)
        {
            tanks.push_back(spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10));
        }
        auto& scripted = sim.getUnitState(tanks[0]);
        scripted.missionState = MissionUnitState{};
        scripted.missionState->selectable = false;
        auto enemy = spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);
        REQUIRE(sim.getUnitState(tanks[0]).orders.empty() == false);
        const auto& st0 = aiStateOf(sim, playerB);
        REQUIRE(st0.attackTarget == enemy);

        // capture it mid-squad through the normal ownership path
        REQUIRE(sim.transferUnitOwnership(tanks[0], playerA, std::nullopt));
        auto& captured = sim.getUnitState(tanks[0]);
        REQUIRE(captured.owner == playerA);
        // retail respawn semantics: controllable by the new owner
        REQUIRE(captured.isSelectableBy(sim.unitDefinitions.at(captured.unitType), playerA));

        // the player commands it
        captured.addOrder(MoveOrder(SimVector(200_ss, 0_ss, 200_ss)));

        // run past several repartitions and an attack scan
        for (int i = 0; i < 400; ++i)
        {
            sim.tick();
        }

        const auto& st = aiStateOf(sim, playerB);
        // absent from every actionable group after repartition
        for (const auto& g : st.groups)
        {
            REQUIRE(std::find(g.begin(), g.end(), tanks[0]) == g.end());
        }
        // the former AI issued no orders to it; the human's move was
        // either still queued or consumed by navigation -- never
        // replaced by an AI attack order
        for (const auto& o : captured.orders)
        {
            REQUIRE(!std::holds_alternative<AttackOrder>(o));
        }
        REQUIRE(captured.owner == playerA);
    }

    TEST_CASE("campaign AI groups stay bounded and stale members are purged")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        spawnCompleted(sim, "ARMFAC", playerB, 10, 10);
        spawnCompleted(sim, "ARMCOM", playerB, 12, 10);
        std::vector<UnitId> tanks;
        for (int i = 0; i < 4; ++i)
        {
            tanks.push_back(spawnCompleted(sim, "CORTANK", playerB, 16 + 2 * i, 10));
        }
        spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        auto groupInvariant = [&] {
            const auto& st = aiStateOf(sim, playerB);
            std::unordered_set<unsigned int> seen;
            std::size_t total = 0;
            for (const auto& g : st.groups)
            {
                for (auto id : g)
                {
                    REQUIRE(seen.insert(id.value).second);
                    ++total;
                }
            }
            // live AI-owned units currently grouped: factory + builder + tanks
            return total;
        };

        // several repartitions: membership stays unique and bounded
        std::size_t before = 0;
        for (int i = 0; i < 100; ++i)
        {
            sim.tick();
            if (i == 0)
            {
                before = groupInvariant();
            }
        }
        REQUIRE(groupInvariant() <= before);

        // death and capture are purged by the next repartition
        sim.applyDamage(tanks[1], 999999, std::nullopt);
        sim.transferUnitOwnership(tanks[2], playerA, std::nullopt);
        for (int i = 0; i < 40; ++i)
        {
            sim.tick();
        }
        const auto& st = aiStateOf(sim, playerB);
        for (const auto& g : st.groups)
        {
            REQUIRE(std::find(g.begin(), g.end(), tanks[1]) == g.end());
            REQUIRE(std::find(g.begin(), g.end(), tanks[2]) == g.end());
        }
    }

    TEST_CASE("campaign AI does not re-order units that already hold orders")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        auto fac = spawnCompleted(sim, "ARMFAC", playerB, 10, 10);
        auto builder = spawnCompleted(sim, "ARMCOM", playerB, 12, 10);
        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(sim, "CORTANK", playerB, 16 + 2 * i, 10);
        }
        spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        // factory queue holds exactly one AI entry across later scans
        REQUIRE(sim.getUnitState(fac).buildQueue.size() == 1);
        for (int i = 0; i < 100; ++i)
        {
            sim.tick();
        }
        REQUIRE(sim.getUnitState(fac).buildQueue.size() <= 1);

        // combat reserves that attacked already carry an order; a second
        // scan must not append a redundant copy while it executes
        const auto& st = aiStateOf(sim, playerB);
        for (auto gi : {AiGroupReserveA, AiGroupReserveB})
        {
            for (auto id : st.groups[gi])
            {
                const auto& u = sim.getUnitState(id);
                if (!u.orders.empty() && std::holds_alternative<AttackOrder>(u.orders.front()))
                {
                    REQUIRE(u.orders.size() == 1);
                }
            }
        }
    }

    /**
     * Retail target scan 0x4071f0 consults the AI object's allied
     * player table (aiObj+0x108): allied players' units are skipped
     * even when they are the nearest candidates. C13A ally groups
     * drive the same exclusion for both campaign and skirmish.
     */
    TEST_CASE("campaign AI attack never targets an allied player")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        // a third player shares the AI's ally group; its units must be
        // invisible to the AI's target scan
        auto allyC = sim.addPlayer(GamePlayerInfo{
            std::nullopt,
            GamePlayerType::Computer,
            PlayerColorIndex(2),
            GamePlayerStatus::Alive,
            "CORE",
            std::optional<unsigned int>(0),
            Metal(10000),
            Energy(10000),
            Metal(10000),
            Energy(10000),
            Metal(10000),
            Energy(10000)});
        sim.players[playerB.value].allyGroup = 0;
        REQUIRE(sim.areAllied(playerB, allyC));
        REQUIRE(!sim.areAllied(playerB, playerA));

        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10);
        }
        // allied unit sits closest to the squad centroid; the real
        // enemy is farther away and is the only legal target
        auto allyUnit = spawnCompleted(sim, "CORTANK", allyC, 12, 12);
        auto enemy = spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(st.attackTarget == enemy);
        REQUIRE(st.attackTarget != allyUnit);
        for (auto g : {AiGroupReserveA, AiGroupReserveB})
        {
            for (auto id : st.groups[g])
            {
                const auto& u = sim.getUnitState(id);
                REQUIRE(!u.orders.empty());
                const auto* attack = std::get_if<AttackOrder>(&u.orders.front());
                REQUIRE(attack);
                REQUIRE(std::get<UnitId>(attack->target) == enemy);
            }
        }
    }

    /**
     * A squad whose only candidates are allies musters but receives
     * no attack order -- the retail target scan returns "none" and
     * the task waits for a legal target.
     */
    TEST_CASE("campaign AI finds no target when only allies exist")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        sim.players[playerA.value].allyGroup = 0;
        sim.players[playerB.value].allyGroup = 0;
        REQUIRE(sim.areAllied(playerA, playerB));

        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10);
        }
        spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(!st.attackTarget);
        for (auto g : {AiGroupReserveA, AiGroupReserveB})
        {
            for (auto id : st.groups[g])
            {
                REQUIRE(sim.getUnitState(id).orders.empty());
            }
        }
    }

    /**
     * Retail 0x4071f0 also filters dead (state+0x110&3==2), immune
     * (+0x110&0x8000) and cloaked (+0x10e&4) candidates. RWE's dead
     * check covers the first; mission immunity and the effective
     * cloak bit cover the other two.
     */
    TEST_CASE("campaign AI attack scan skips cloaked and immune targets")
    {
        setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_test.log", false));

        PlayerId playerA{0}, playerB{0};
        auto sim = makeWorld(playerA, playerB);
        initCampaignAiPlayer(sim, playerB, CampaignAiProfile{});

        for (int i = 0; i < 4; ++i)
        {
            spawnCompleted(sim, "CORTANK", playerB, 10 + 2 * i, 10);
        }

        auto cloaked = spawnCompleted(sim, "ARMTANK", playerA, 12, 12);
        sim.getUnitState(cloaked).cloaked = true;
        auto immune = spawnCompleted(sim, "ARMTANK", playerA, 14, 12);
        sim.getUnitState(immune).missionState = MissionUnitState{};
        sim.getUnitState(immune).missionState->immunity = true;
        auto legal = spawnCompleted(sim, "ARMTANK", playerA, 40, 10);

        updateCampaignAi(sim);

        const auto& st = aiStateOf(sim, playerB);
        REQUIRE(st.attackTarget == legal);
        REQUIRE(st.attackTarget != cloaked);
        REQUIRE(st.attackTarget != immune);
    }

    /**
     * Retail smoke test: every schema of every canonical campaign
     * mission names an aiprofile that resolves through the VFS
     * fallback chain (ai/<name> -> ai/<name>.TXT -> ai/DEFAULT ->
     * ai/DEFAULT.TXT) and parses at all three difficulties.
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("every campaign mission aiprofile resolves and parses")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto campaigns = loadCampaignDescriptors(vfs, "camps");
        REQUIRE(!campaigns.empty());

        unsigned int schemasChecked = 0;
        unsigned int missionsChecked = 0;
        for (const auto& c : campaigns)
        {
            for (const auto& m : c.descriptor.missions)
            {
                auto mapName = missionFileToMapName(m.missionFile);
                auto otaRaw = vfs.readFile("maps/" + mapName + ".ota");
                if (!otaRaw)
                {
                    continue;
                }
                ++missionsChecked;
                auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
                for (const auto& schema : ota.schemas)
                {
                    INFO("mission " + m.missionFile + " schema " + schema.type + " aiprofile '" + schema.aiProfile + "'");
                    auto text = loadCampaignAiProfileText(vfs, "ai", schema.aiProfile);
                    REQUIRE(text);
                    for (auto difficulty : {CampaignAiDifficulty::Easy, CampaignAiDifficulty::Medium, CampaignAiDifficulty::Hard})
                    {
                        REQUIRE_NOTHROW(parseCampaignAiProfile(*text, difficulty));
                    }
                    ++schemasChecked;
                }
            }
        }
        REQUIRE(missionsChecked >= 75);
        REQUIRE(schemasChecked >= 200);
    }

    /**
     * Stock Krogoth Encounter is the one mission whose AI profile
     * comes from a name shared by no other schema; guard the
     * profile-name fallback on a couple of shapes stock data uses:
     * mixed-case file names and missing profiles falling to DEFAULT.
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("campaign AI profile fallback chain")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));

        SECTION("each canonical profile resolves")
        {
            for (const auto& name : {"MISSIONS", "DEFAULT", "Hover", "Waterwrld", "SeaBattle", "AirBattle", "Krogoth"})
            {
                INFO("profile " + std::string(name));
                auto text = loadCampaignAiProfileText(vfs, "ai", name);
                REQUIRE(text);
                REQUIRE(!text->empty());
            }
        }

        SECTION("unknown names fall back to DEFAULT")
        {
            auto fallback = loadCampaignAiProfileText(vfs, "ai", "NOSUCHPROFILE");
            auto def = loadCampaignAiProfileText(vfs, "ai", "DEFAULT");
            REQUIRE(fallback);
            REQUIRE(def);
            REQUIRE(*fallback == *def);
        }
    }
}
