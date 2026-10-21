#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rwe/LoadingScene_util.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <cstdlib>
#include <numeric>

namespace rwe
{
    namespace
    {
        struct FeatureFixture
        {
            GameSimulation sim{MapTerrain(Grid<unsigned char>(129, 129, 64), 0_ss), 0, 0, 0};
            FeatureDefinitionId rock, tree, dead, smudge;
            FeatureFixture()
            {
                setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_features.log", false));
                sim.lineOfSightEnabled = false;
                for (unsigned int i = 0; i < 2; ++i)
                {
                    sim.addPlayer(GamePlayerInfo{std::nullopt, i == 0 ? GamePlayerType::Human : GamePlayerType::Computer,
                        PlayerColorIndex(i), GamePlayerStatus::Alive, i == 0 ? "ARM" : "CORE", std::nullopt,
                        Metal(10000), Energy(10000), Metal(10000), Energy(10000), Metal(10000), Energy(10000)});
                }
                FeatureDefinition d{};
                d.name = "SMUDGE"; d.footprintX = d.footprintZ = 1; d.indestructible = true;
                smudge = define(d);
                d.name = "DEAD"; d.indestructible = false; d.blocking = d.reclaimable = true;
                d.height = 40_ss; d.damage = 100; d.energy = 50; d.featureReclamate = d.featureDead = smudge;
                dead = define(d);
                d.name = "TREE"; d.energy = 250; d.flamable = true; d.featureBurnt = dead;
                d.sparkTime = 5; d.spreadChance = 90;
                // Tiny authored cursor fixture exercises exact GAF delay rules;
                // actual retail sequences are checked separately below.
                d.animations[0].frames = {2, 3, 1};
                d.animations[1].frames = {2, 2};
                d.animations[2].frames = {2, 3, 1};
                d.animations[2].shadows = {1, 1};
                tree = define(d);
                d.name = "ROCK"; d.object = true; d.energy = 0; d.metal = 100; d.flamable = false;
                d.animations = {}; d.featureDead.reset(); d.featureReclamate.reset();
                rock = define(d);
                WeaponDefinition w{}; w.damage = {{"DEFAULT", 40}, {"ROCK", 9999}}; w.damageRadius = 64_ss;
                w.physicsType = ProjectilePhysicsTypeLineOfSight{}; w.velocity = 1_ss; w.maxRange = 1000_ss;
                sim.weaponDefinitions.emplace("HIT", w);
                w.damage = {{"DEFAULT", 100}}; w.fireStarter = true;
                sim.weaponDefinitions.emplace("FIRE", w);
                UnitDefinition u{}; u.unitName = u.objectName = "UNIT";
                u.movementCollisionInfo = UnitDefinition::AdHocMovementClass{1, 1, 255, 255, 0, 255};
                u.maxHitPoints = 1000; u.buildTime = 100; u.isMobile = u.canMove = true;
                u.maxVelocity = 2_ss; u.acceleration = 1_ss; u.brakeRate = 1_ss; u.turnRate = 2048_ss;
                u.builder = u.canReclaim = true; u.buildDistance = 100_ss; u.workerTimePerTick = 10;
                u.selfDestructAs = u.explodeAs = "HIT";
                sim.unitDefinitions.emplace("UNIT", u);
                sim.unitModelDefinitions.emplace("UNIT", createUnitModelDefinition(16_ss, {UnitPieceDefinition{"base", SimVector(0_ss, 0_ss, 0_ss)}}));
                sim.unitScriptDefinitions.emplace("UNIT", CobScript{{}, {"base"}, {}, 0});
            }
            FeatureDefinitionId define(const FeatureDefinition& d)
            {
                auto id = sim.featureDefinitions.insert(d); sim.featureNameIndex.emplace(d.name, id); return id;
            }
            FeatureId place(FeatureDefinitionId type, int x = 50, int z = 50)
            {
                auto id = sim.addFeature(type, x, z); REQUIRE(id); return *id;
            }
            UnitId unit(int x = 45, int z = 50, PlayerId owner = PlayerId(0))
            {
                auto p = sim.terrain.heightmapIndexToWorldCenter(x, z); p.y = 64_ss;
                auto id = sim.trySpawnUnit("UNIT", owner, p, std::nullopt); REQUIRE(id);
                sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at("UNIT")); return *id;
            }
            Projectile packet(const SimVector& p, const std::string& name = "HIT")
            {
                Projectile result{}; result.position = result.previousPosition = result.origin = p;
                result.weaponType = name; result.owner = PlayerId(0); result.damage = sim.weaponDefinitions.at(name).damage;
                result.damageRadius = sim.weaponDefinitions.at(name).damageRadius; return result;
            }
            void hit(FeatureId id, const std::string& weapon = "HIT") { sim.damageFeature(id, sim.weaponDefinitions.at(weapon)); }
            void frames(unsigned int n) { for (unsigned int i = 0; i < n; ++i) { sim.updateFeatures(); } }
            GameParameters params() const
            {
                GameParameters p("FEATURE_TEST", 0);
                p.players[0] = PlayerInfo{std::nullopt, PlayerControllerTypeHuman(), "ARM", PlayerColorIndex(0), Metal(10000), Energy(10000)};
                p.players[1] = PlayerInfo{std::nullopt, PlayerControllerTypeComputer(), "CORE", PlayerColorIndex(1), Metal(10000), Energy(10000)};
                return p;
            }
        };
    }

    TEST_CASE("feature data keeps independent destruction reclaim and burn selectors", "[features]")
    {
        auto block = parseTdfFromString("[X]{damage=150;reclaimable=1;blocking=1;flamable=1;featuredead=A;featureburnt=B;featurereclamate=C;sparktime=0.5;}");
        auto f = parseFeatureTdf(block.findBlock("X")->get());
        REQUIRE(f.damage == 150); REQUIRE(f.reclaimable); REQUIRE(f.autoreclaimable); REQUIRE(f.blocking);
        REQUIRE(f.flamable); REQUIRE_FALSE(f.indestructible); REQUIRE(f.featureDead == "A");
        REQUIRE(f.featureBurnt == "B"); REQUIRE(f.featureReclamate == "C"); REQUIRE(f.sparkTime == 0.5f);
        REQUIRE(parseFeatureTdf(TdfBlock{}).damage == 0);
    }
    TEST_CASE("indestructible features ignore damage and ignition", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.smudge); auto h = computeHashOf(f.sim.getFeature(id));
        f.hit(id); f.hit(id, "FIRE"); REQUIRE_FALSE(f.sim.igniteFeature(id));
        REQUIRE(computeHashOf(f.sim.getFeature(id)) == h); REQUIRE(f.sim.activeFeatures.empty());
    }
    TEST_CASE("feature damage accumulates full default independently of class falloff and EMP", "[features][explosion]")
    {
        FeatureFixture f; auto id = f.place(f.rock); auto pos = f.sim.getFeature(id).position;
        f.sim.weaponDefinitions.at("HIT").paralyzer = true;
        auto p = f.packet(pos + SimVector(63_ss, 0_ss, 0_ss));
        f.sim.doProjectileImpact(p, ImpactType::Normal); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
        f.sim.doProjectileImpact(p, ImpactType::Normal); REQUIRE(f.sim.getFeature(id).damageAccumulated == 80);
        f.sim.doProjectileImpact(p, ImpactType::Normal); REQUIRE_FALSE(f.sim.tryGetFeature(id));
    }
    TEST_CASE("feature radius uses three dimensional truncated distance and strict boundary", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.rock); auto pos = f.sim.getFeature(id).position;
        f.sim.doProjectileImpact(f.packet(pos + SimVector(64_ss, 0_ss, 0_ss)), ImpactType::Normal);
        REQUIRE(f.sim.getFeature(id).damageAccumulated == 0);
        f.sim.doProjectileImpact(f.packet(pos + SimVector(0_ss, 64_ss, 0_ss)), ImpactType::Normal);
        REQUIRE(f.sim.getFeature(id).damageAccumulated == 0);
        f.sim.doProjectileImpact(f.packet(pos + SimVector(63.9_ssf, 0_ss, 0_ss)), ImpactType::Normal);
        REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
    }
    TEST_CASE("units only weapons leave feature damage and collisions unchanged", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.rock); f.sim.weaponDefinitions.at("HIT").unitsOnly = true;
        auto p = f.packet(f.sim.getFeature(id).position + SimVector(0_ss, 2_ss, 0_ss));
        f.sim.doProjectileImpact(p, ImpactType::Normal); REQUIRE(f.sim.getFeature(id).damageAccumulated == 0);
        auto pid = f.sim.projectiles.emplace(p); f.sim.updateProjectiles();
        REQUIRE_FALSE(f.sim.projectiles.find(pid)->second.isDead);
    }
    TEST_CASE("direct noexplode feature impacts damage only the struck feature once", "[features][projectile]")
    {
        FeatureFixture f; auto id = f.place(f.rock); auto other = f.place(f.rock, 51);
        f.sim.weaponDefinitions.at("HIT").noExplode = true;
        auto p = f.packet(f.sim.getFeature(id).position + SimVector(0_ss, 2_ss, 0_ss));
        auto pid = f.sim.projectiles.emplace(p); f.sim.updateProjectiles();
        REQUIRE(f.sim.projectiles.find(pid)->second.isDead); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
        REQUIRE(f.sim.getFeature(other).damageAccumulated == 0);
        f.sim.updateProjectiles(); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
    }
    TEST_CASE("one large explosion deduplicates every multi cell feature beyond 64 anchors", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "BIG"; d.footprintX = d.footprintZ = 2;
        auto type = f.define(d); std::vector<FeatureId> ids;
        for (int x = 20; x < 40; x += 2) { for (int z = 20; z < 40; z += 2) { ids.push_back(f.place(type, x, z)); } }
        auto p = f.packet(f.sim.terrain.heightmapIndexToWorldCenter(30, 30)); p.position.y = 64_ss; p.damageRadius = 300_ss;
        f.sim.doProjectileImpact(p, ImpactType::Normal);
        for (auto id : ids) { REQUIRE(f.sim.getFeature(id).damageAccumulated == 40); }
    }
    TEST_CASE("sprite death animation retains occupancy and blocks repeated damage until completion", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.tree); f.hit(id); f.hit(id); f.hit(id);
        REQUIRE(f.sim.getFeature(id).event == FeatureEvent::Dying); REQUIRE(f.sim.activeFeatures.size() == 1);
        f.hit(id, "FIRE"); f.hit(id); REQUIRE(f.sim.activeFeatures.size() == 1);
        f.frames(5); REQUIRE(f.sim.tryGetFeature(id)); REQUIRE(f.sim.occupiedGrid.get(50, 50).featureId == id);
        f.frames(1); REQUIRE_FALSE(f.sim.tryGetFeature(id)); REQUIRE(f.sim.activeFeatures.empty());
        auto next = f.sim.occupiedGrid.get(50, 50).featureId; REQUIRE(next); REQUIRE(*next != id);
        REQUIRE(f.sim.getFeature(*next).featureName == f.smudge);
        f.hit(id); REQUIRE(f.sim.tryGetFeature(*next));
    }
    TEST_CASE("3D death transforms immediately with a new identity and fresh damage", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "WRECK"; d.featureDead = f.dead;
        auto id = f.place(f.define(d)); auto pos = f.sim.getFeature(id).position;
        f.hit(id); f.hit(id); f.hit(id); REQUIRE_FALSE(f.sim.tryGetFeature(id));
        auto next = f.sim.occupiedGrid.get(50, 50).featureId; REQUIRE(next); REQUIRE(*next != id);
        REQUIRE(f.sim.getFeature(*next).featureName == f.dead); REQUIRE((f.sim.getFeature(*next).position == pos));
        REQUIRE(f.sim.getFeature(*next).damageAccumulated == 0);
    }
    TEST_CASE("sprite accumulators do not wrap but retail 3D accumulators do", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "TOUGH"; d.damage = 65535;
        auto id = f.place(f.define(d)); f.sim.getFeature(id).damageAccumulated = 65530; f.hit(id);
        REQUIRE(f.sim.getFeature(id).damageAccumulated == 34);
        d.name = "SPRITE"; d.object = false; auto sprite = f.place(f.define(d), 52);
        f.sim.getFeature(sprite).damageAccumulated = 65530; f.hit(sprite); REQUIRE_FALSE(f.sim.tryGetFeature(sprite));
    }
    TEST_CASE("zero capacity feature dies from zero default damage", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "ZERO"; d.damage = 0;
        auto id = f.place(f.define(d)); f.sim.weaponDefinitions.at("HIT").damage["DEFAULT"] = 0; f.hit(id);
        REQUIRE_FALSE(f.sim.tryGetFeature(id));
    }
    TEST_CASE("feature removal releases only its own footprint and permits movement", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.rock); auto other = f.place(f.rock, 51);
        f.sim.removeFeature(id); f.sim.removeFeature(id);
        REQUIRE_FALSE(f.sim.occupiedGrid.get(50, 50).featureId); REQUIRE(f.sim.occupiedGrid.get(51, 50).featureId == other);
        auto unit = f.unit(50); REQUIRE(f.sim.getUnitState(unit).isAlive());
    }
    TEST_CASE("blocking successors cannot entomb existing ground units", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "PASSABLE"; d.blocking = false; d.featureDead = f.dead;
        auto id = f.place(f.define(d)); auto unit = f.unit(50); f.hit(id); f.hit(id); f.hit(id);
        REQUIRE_FALSE(f.sim.tryGetFeature(id)); REQUIRE_FALSE(f.sim.occupiedGrid.get(50, 50).featureId);
        REQUIRE(f.sim.getUnitState(unit).isAlive());
    }
    TEST_CASE("damaged features retain reclaim pools and multiple workers cannot duplicate credits", "[features][reclaim]")
    {
        FeatureFixture f; auto id = f.place(f.rock); f.hit(id);
        float paid = 0;
        for (int i = 0; i < 12; ++i) { auto credit = f.sim.applyReclaimProgress(id, 10); REQUIRE(credit); paid += credit->second.value; }
        REQUIRE(paid == 100); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
        REQUIRE(f.sim.reclaimComplete(f.sim.getFeature(id)));
        f.sim.transitionFeature(id, FeatureEvent::Reclaiming); REQUIRE_FALSE(f.sim.applyReclaimProgress(id, 10));
    }
    TEST_CASE("damage and burn successors deduct previously awarded reclaim resources", "[features][reclaim]")
    {
        FeatureFixture f; auto id = f.place(f.tree); auto paid = f.sim.applyReclaimProgress(id, 30); REQUIRE(paid->first == Energy(30));
        REQUIRE(f.sim.igniteFeature(id)); REQUIRE_FALSE(f.sim.applyReclaimProgress(id, 20)); f.frames(6);
        auto next = *f.sim.occupiedGrid.get(50, 50).featureId; REQUIRE(f.sim.getFeature(next).featureName == f.dead);
        auto remaining = f.sim.applyReclaimProgress(next, 50); REQUIRE(remaining->first == Energy(20));
        REQUIRE_FALSE(f.sim.applyReclaimProgress(id, 50));
    }
    TEST_CASE("reclaim animation selects reclamate and pays no extra resources", "[features][reclaim]")
    {
        FeatureFixture f; auto id = f.place(f.tree); auto credit = f.sim.applyReclaimProgress(id, 250); REQUIRE(credit->first == Energy(250));
        f.sim.transitionFeature(id, FeatureEvent::Reclaiming); REQUIRE_FALSE(f.sim.applyReclaimProgress(id, 250));
        f.frames(3); REQUIRE(f.sim.tryGetFeature(id)); f.frames(1); REQUIRE_FALSE(f.sim.tryGetFeature(id));
        REQUIRE(f.sim.getFeature(*f.sim.occupiedGrid.get(50, 50).featureId).featureName == f.smudge);
    }
    TEST_CASE("firestarter is an ignition gate and missing sequences suppress damage", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.tree); d.name = "MISSING"; d.animations = {};
        auto id = f.place(f.define(d)); f.hit(id, "FIRE"); REQUIRE(f.sim.getFeature(id).damageAccumulated == 0);
        REQUIRE(f.sim.getFeature(id).event == FeatureEvent::None); REQUIRE(f.sim.activeFeatures.empty());
    }
    TEST_CASE("3D features never ignite even when authored flammable", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.rock); d.name = "FLAMMABLE3D"; d.flamable = true;
        auto id = f.place(f.define(d)); REQUIRE_FALSE(f.sim.igniteFeature(id)); f.hit(id, "FIRE"); REQUIRE_FALSE(f.sim.tryGetFeature(id));
    }
    TEST_CASE("burn completion follows GAF delay sum and selects burnt successor", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.tree); f.hit(id, "FIRE");
        REQUIRE(f.sim.getFeature(id).event == FeatureEvent::Burning); REQUIRE(f.sim.getFeature(id).sparkCountdown >= 75);
        REQUIRE(f.sim.getFeature(id).sparkCountdown <= 149); f.frames(2); REQUIRE(f.sim.getFeature(id).animationFrame == 1);
        REQUIRE(f.sim.getFeature(id).shadowFrame == 2); f.frames(3); REQUIRE(f.sim.tryGetFeature(id));
        f.frames(1); REQUIRE_FALSE(f.sim.tryGetFeature(id)); REQUIRE(f.sim.getFeature(*f.sim.occupiedGrid.get(50, 50).featureId).featureName == f.dead);
    }
    TEST_CASE("zero spark countdown burns without RNG draw or propagation", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.tree); d.name = "SHORT"; d.sparkTime = 0; d.spreadChance = 100;
        auto type = f.define(d); auto id = f.place(type); auto next = f.place(type, 51); auto rng = f.sim.rng;
        REQUIRE(f.sim.igniteFeature(id)); REQUIRE(f.sim.rng == rng); f.frames(6);
        REQUIRE(f.sim.getFeature(next).event == FeatureEvent::None); REQUIRE(f.sim.rng == rng);
    }
    TEST_CASE("one burn event spreads to anchors and routes its weapon through ordinary damage", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.tree); d.name = "SPREAD";
        d.sparkTime = 1.0f / 15.0f; d.spreadChance = 100; d.burnWeapon = "HIT";
        auto type = f.define(d); auto source = f.place(type); auto near = f.place(type, 51); auto far = f.place(type, 54);
        auto victim = f.place(f.rock, 50, 52); auto unit = f.unit(50, 49, PlayerId(1));
        auto hp = f.sim.getUnitState(unit).hitPoints;
        REQUIRE(f.sim.igniteFeature(source)); REQUIRE(f.sim.getFeature(source).sparkCountdown == 1);
        f.frames(1); REQUIRE(f.sim.getFeature(near).event == FeatureEvent::Burning);
        REQUIRE(f.sim.getFeature(near).animationFrame == 0); REQUIRE(f.sim.getFeature(near).animationDelay == 2);
        REQUIRE(f.sim.getFeature(far).event == FeatureEvent::None); REQUIRE(f.sim.getFeature(victim).damageAccumulated == 40);
        REQUIRE(f.sim.getUnitState(unit).hitPoints < hp); REQUIRE_FALSE(f.sim.getUnitState(unit).lastDamagedByPlayer);
        auto damage = f.sim.getFeature(victim).damageAccumulated;
        f.sim.removeFeature(near); f.frames(1); REQUIRE(f.sim.getFeature(victim).damageAccumulated == damage);
    }
    TEST_CASE("wind fire probes skip repeated tiles and reach authored downstream anchors", "[features]")
    {
        FeatureFixture f; auto d = f.sim.getFeatureDefinition(f.tree); d.name = "WIND"; d.sparkTime = 0; d.spreadChance = 100;
        auto type = f.define(d); auto source = f.place(type); auto target = f.place(type, 55);
        f.sim.projectileWind = SimVector(0.5_ssf, 0_ss, 0_ss); REQUIRE(f.sim.igniteFeature(source));
        f.sim.featureBurnEvent(source); REQUIRE(f.sim.getFeature(target).event == FeatureEvent::Burning);
    }
    TEST_CASE("simultaneous ignition order and sprite protection remain stable", "[features]")
    {
        FeatureFixture f; auto a = f.place(f.tree); auto b = f.place(f.tree, 52);
        f.hit(a, "FIRE"); f.hit(b, "FIRE"); f.hit(a, "FIRE");
        REQUIRE(f.sim.activeFeatures == std::vector<FeatureId>{a, b});
        f.sim.removeFeature(a); REQUIRE(f.sim.activeFeatures == std::vector<FeatureId>{b});
        f.frames(6); REQUIRE(f.sim.activeFeatures.empty());
    }
    TEST_CASE("self destruction and kamikaze death packets affect features without awarding feature kills", "[features]")
    {
        FeatureFixture f; auto feature = f.place(f.rock); auto unit = f.unit(49);
        f.sim.killUnit(unit, UnitDeathCause::SelfDestruct);
        REQUIRE(f.sim.getFeature(feature).damageAccumulated == 40);
        f.sim.killUnit(unit, UnitDeathCause::SelfDestruct); REQUIRE(f.sim.getFeature(feature).damageAccumulated == 40);
        REQUIRE(f.sim.players[0].unitsKilled == 0); REQUIRE(f.sim.players[1].unitsKilled == 0);
    }
    TEST_CASE("authoritative dropped bombs and strategic explosion packets damage features", "[features]")
    {
        FeatureFixture f; auto id = f.place(f.rock); f.sim.weaponDefinitions.at("HIT").physicsType = ProjectilePhysicsTypeDropped{};
        auto p = f.packet(f.sim.getFeature(id).position + SimVector(0_ss, 42_ss, 0_ss)); p.velocity = SimVector(0_ss, -1_ss, 0_ss);
        auto pid = f.sim.projectiles.emplace(p);
        for (int i = 0; i < 30 && !f.sim.projectiles.find(pid)->second.isDead; ++i) { f.sim.updateProjectiles(); }
        REQUIRE(f.sim.projectiles.find(pid)->second.isDead); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
        auto nuke = f.packet(f.sim.getFeature(id).position); nuke.damageRadius = 512_ss;
        f.sim.weaponDefinitions.at("HIT").damage["DEFAULT"] = 10000; f.sim.doProjectileImpact(nuke, ImpactType::Normal);
        REQUIRE_FALSE(f.sim.tryGetFeature(id));
    }
    TEST_CASE("feature state identity active order and RNG are included in lockstep hashes", "[features][save]")
    {
        FeatureFixture f; auto id = f.place(f.tree); auto hash = f.sim.computeHash(); f.hit(id); REQUIRE(f.sim.computeHash() != hash);
        hash = f.sim.computeHash(); f.sim.getFeature(id).featureName = f.dead; REQUIRE(f.sim.computeHash() != hash);
        f.sim.getFeature(id).featureName = f.tree; f.sim.igniteFeature(id); hash = f.sim.computeHash();
        --f.sim.getFeature(id).sparkCountdown; REQUIRE(f.sim.computeHash() != hash);
        hash = f.sim.computeHash(); ++f.sim.getFeature(id).animationFrame; REQUIRE(f.sim.computeHash() != hash);
        hash = f.sim.computeHash(); f.sim.rng(); REQUIRE(f.sim.computeHash() != hash);
        auto other = f.place(f.tree, 52); f.sim.igniteFeature(other); hash = f.sim.computeHash();
        std::reverse(f.sim.activeFeatures.begin(), f.sim.activeFeatures.end()); REQUIRE(f.sim.computeHash() != hash);
    }
    TEST_CASE("feature saves continue intact damaged burning dying reclaimed and removed states", "[features][save]")
    {
        const auto saveVersion = GENERATE(14u, 15u, SaveGameFormatVersion);
        for (unsigned int phase = 0; phase < 8; ++phase)
        {
            FeatureFixture a; auto id = a.place(a.tree); a.sim.applyReclaimProgress(id, 10);
            if (phase == 1) { a.hit(id); }
            if (phase == 2 || phase == 3) { a.sim.igniteFeature(id); a.frames(phase - 1); }
            if (phase == 4) { a.sim.transitionFeature(id, FeatureEvent::Dying); a.frames(1); }
            if (phase == 5) { a.sim.transitionFeature(id, FeatureEvent::Reclaiming); a.frames(1); }
            if (phase == 6) { a.sim.removeFeature(id); }
            if (phase == 7) { a.sim.igniteFeature(id); a.frames(6); }
            a.sim.projectiles.emplace(a.packet(a.sim.terrain.heightmapIndexToWorldCenter(52, 50) + SimVector(0_ss, 100_ss, 0_ss)));
            auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "FEATURE", 0, saveVersion)); REQUIRE(save.version == saveVersion);
            FeatureFixture b; applySavedGame(b.sim, save, nullptr); REQUIRE(a.sim.computeHash() == b.sim.computeHash());
            for (int tick = 0; tick < 200; ++tick)
            {
                a.sim.tick(); b.sim.tick(); REQUIRE(a.sim.computeHash() == b.sim.computeHash());
                REQUIRE(a.sim.activeFeatures == b.sim.activeFeatures);
            }
        }
    }
    TEST_CASE("older save layouts restore intact features and reject lossy damage writes", "[features][save]")
    {
        FeatureFixture a; auto id = a.place(a.rock);
        for (unsigned int version = 4; version <= 12; ++version)
        {
            auto save = decodeSavedGame(encodeSavedGame(a.sim, a.params(), nullptr, "LEGACY", 0, version));
            FeatureFixture b; applySavedGame(b.sim, save, nullptr); REQUIRE(b.sim.getFeature(id).damageAccumulated == 0);
            REQUIRE(b.sim.getFeature(id).event == FeatureEvent::None); REQUIRE(b.sim.activeFeatures.empty());
        }
        a.hit(id); REQUIRE_THROWS_AS(encodeSavedGame(a.sim, a.params(), nullptr, "LOSSY", 0, 12), SaveFormatException);
    }
    TEST_CASE("identical explosion and ignition commands replay deterministically", "[features]")
    {
        FeatureFixture a, b;
        for (int x = 30; x < 38; ++x) { a.place(a.tree, x); b.place(b.tree, x); }
        for (int tick = 0; tick < 300; ++tick)
        {
            if (tick % 7 == 0)
            {
                auto p = a.packet(a.sim.terrain.heightmapIndexToWorldCenter(30 + (tick / 7) % 8, 50) + SimVector(0_ss, 64_ss, 0_ss), tick % 2 ? "HIT" : "FIRE");
                a.sim.doProjectileImpact(p, ImpactType::Normal); b.sim.doProjectileImpact(p, ImpactType::Normal);
            }
            a.sim.tick(); b.sim.tick(); REQUIRE(a.sim.computeHash() == b.sim.computeHash());
        }
    }
    TEST_CASE("ground navigation and AI ownership recover after a feature wall opens", "[features][pathfinding]")
    {
        for (auto owner : {PlayerId(0), PlayerId(1)})
        {
            FeatureFixture f;
            FeatureId gate(0);
            for (int z = 0; z < 128; ++z)
            {
                auto id = f.place(f.rock, 50, z); if (z == 50) { gate = id; }
            }
            auto unit = f.unit(47, 50, owner);
            auto goal = f.sim.terrain.heightmapIndexToWorldCenter(53, 50); goal.y = 64_ss;
            f.sim.getUnitState(unit).replaceOrders({MoveOrder(goal)});
            for (int i = 0; i < 100; ++i) { f.sim.tick(); }
            REQUIRE(f.sim.getUnitState(unit).position.x < goal.x);
            f.hit(gate); f.hit(gate); f.hit(gate); REQUIRE_FALSE(f.sim.tryGetFeature(gate));
            // A failed move completes at the nearest reachable point. Issue a
            // fresh move after opening the gate; completed orders are not revived.
            f.sim.getUnitState(unit).replaceOrders({MoveOrder(goal)});
            for (int i = 0; i < 1500; ++i) { f.sim.tick(); }
            REQUIRE(f.sim.getUnitState(unit).position.x > f.sim.terrain.heightmapIndexToWorldCenter(50, 50).x);
            REQUIRE(f.sim.pathRequests.size() <= 1);
        }
    }

    TEST_CASE("builders safely abandon removed or burning reclaim targets", "[features][reclaim]")
    {
        for (bool burning : {false, true})
        {
            FeatureFixture f; auto target = f.place(f.tree); auto builder = f.unit(48);
            auto& unit = f.sim.getUnitState(builder); unit.inBuildStance = true;
            unit.replaceOrders({ReclaimOrder(target)});
            f.sim.tick();
            if (burning) { f.sim.igniteFeature(target); } else { f.sim.removeFeature(target); }
            for (int i = 0; i < 20; ++i) { f.sim.tick(); }
            REQUIRE(unit.orders.empty());
            REQUIRE_FALSE(f.sim.applyReclaimProgress(target, 1000));
        }
    }

    TEST_CASE("retail feature catalog resolves exact GAF event clocks and authored burn payloads", "[features][retail]")
    {
        const char* data = std::getenv("RWE_DATA_PATH"); if (!data) { SKIP("RWE_DATA_PATH required"); }
        auto vfs = constructVfs(data);
        std::unordered_map<std::string, FeatureTdf> tdfs;
        for (const auto& file : vfs.getFileNamesRecursive("features", ".tdf"))
        {
            auto bytes = vfs.readFile("features/" + file); REQUIRE(bytes);
            auto root = parseTdfFromString(std::string(bytes->begin(), bytes->end()));
            for (const auto& [name, block] : root.blocks) { tdfs.emplace(toUpper(name), parseFeatureTdf(*block)); }
        }
        loadFeatureAnimations(vfs, tdfs);
        REQUIRE(tdfs.size() == 1644);
        unsigned int burns = 0, minTicks = 99999, maxTicks = 0;
        std::string maxName;
        for (const auto& [name, tdf] : tdfs)
        {
            if (!tdf.flamable) { continue; }
            INFO(name); ++burns;
            REQUIRE(tdf.object.empty()); REQUIRE_FALSE(tdf.animations[2].frames.empty());
            unsigned int ticks = 0;
            for (auto delay : tdf.animations[2].frames) { ticks += std::max<unsigned int>(delay, 1); }
            if (ticks > maxTicks) { maxName = name + " bank=" + tdf.fileName + " seq=" + tdf.seqNameBurn; }
            minTicks = std::min(minTicks, ticks); maxTicks = std::max(maxTicks, ticks);
            REQUIRE_FALSE(tdf.burnWeapon.empty()); REQUIRE(tdf.sparkTime == 5);
        }
        INFO(maxName);
        REQUIRE(burns == 79); REQUIRE(minTicks == 46); REQUIRE(maxTicks == 282);
        REQUIRE(tdfs.at("TREE1").energy == 250); REQUIRE(tdfs.at("TREE1DEAD").energy == 50);
        REQUIRE(tdfs.at("TREE1").damage == 0); REQUIRE(tdfs.at("SHRUB1").featureBurnt.empty());
        REQUIRE(tdfs.at("TREE1").animations[0].frames.size() > 1);
    }

    TEST_CASE("underwater and elevated feature centres use terrain height for blast distance", "[features]")
    {
        FeatureFixture f;
        Grid<unsigned char> heights(129, 129, 64);
        heights.set(51, 51, 120); heights.set(52, 51, 120);
        heights.set(51, 52, 120); heights.set(52, 52, 120);
        f.sim.terrain = MapTerrain(std::move(heights), 128_ss);
        auto d = f.sim.getFeatureDefinition(f.rock); d.name = "WIDE"; d.footprintX = d.footprintZ = 2;
        auto id = f.place(f.define(d)); auto pos = f.sim.getFeature(id).position;
        REQUIRE(pos.y == f.sim.terrain.getHeightAt(pos.x, pos.z)); REQUIRE(pos.y < f.sim.terrain.getSeaLevel());
        f.sim.doProjectileImpact(f.packet(pos), ImpactType::Water); REQUIRE(f.sim.getFeature(id).damageAccumulated == 40);
    }

    TEST_CASE("retail nuclear and burn weapons preserve their distinct feature and friendly fire rules", "[features][retail]")
    {
        const char* data = std::getenv("RWE_DATA_PATH"); if (!data) { SKIP("RWE_DATA_PATH required"); }
        auto vfs = constructVfs(data); FeatureFixture f;
        for (const auto& file : vfs.getFileNames("weapons", ".tdf"))
        {
            auto bytes = vfs.readFile("weapons/" + file); REQUIRE(bytes);
            auto root = parseTdfFromString(std::string(bytes->begin(), bytes->end()));
            for (const auto& [name, weapon] : parseWeaponTdf(root))
            {
                if (toUpper(name) == "NUCLEAR_MISSILE" || toUpper(name) == "TREEBURN")
                {
                    f.sim.weaponDefinitions.emplace(toUpper(name), parseWeaponDefinition(weapon));
                }
            }
        }
        const auto& nuke = f.sim.weaponDefinitions.at("NUCLEAR_MISSILE");
        REQUIRE(nuke.fireStarter); REQUIRE(nuke.damage.at("DEFAULT") == 5500); REQUIRE(nuke.damageRadius == 256_ss);
        const auto& burn = f.sim.weaponDefinitions.at("TREEBURN"); REQUIRE(burn.unitsOnly); REQUIRE(burn.damage.at("DEFAULT") == 100);
        auto tree = f.place(f.tree); auto rock = f.place(f.rock, 51); auto friendly = f.unit(49);
        auto pos = f.sim.getFeature(tree).position;
        f.sim.doProjectileImpact(f.packet(pos, "TREEBURN"), ImpactType::Normal);
        REQUIRE(f.sim.getFeature(rock).damageAccumulated == 0); REQUIRE(f.sim.getFeature(tree).event == FeatureEvent::None);
        REQUIRE(f.sim.getUnitState(friendly).hitPoints < 1000);
        f.sim.doProjectileImpact(f.packet(pos, "NUCLEAR_MISSILE"), ImpactType::Normal);
        REQUIRE_FALSE(f.sim.tryGetFeature(rock)); REQUIRE(f.sim.getFeature(tree).event == FeatureEvent::Burning);
    }

    TEST_CASE("feature removal while a route is active allows continuation without a global path flush", "[features][pathfinding]")
    {
        FeatureFixture f; auto obstacle = f.place(f.rock); auto unit = f.unit(45);
        auto goal = f.sim.terrain.heightmapIndexToWorldCenter(55, 50); goal.y = 64_ss;
        f.sim.getUnitState(unit).replaceOrders({MoveOrder(goal)});
        f.sim.tick(); f.sim.tick();
        auto queued = f.sim.pathRequests.size(); f.sim.removeFeature(obstacle);
        REQUIRE(f.sim.pathRequests.size() == queued);
        for (int i = 0; i < 500; ++i) { f.sim.tick(); }
        REQUIRE(f.sim.getUnitState(unit).position.x > f.sim.terrain.heightmapIndexToWorldCenter(50, 50).x);
    }

}
