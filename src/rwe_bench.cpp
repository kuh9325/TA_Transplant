/**
 * Headless simulation benchmark harness.
 *
 * Measures pure sim throughput (ticks/sec, ms/tick) for the scenarios
 * that matter to late-game performance: a restored real save, a fresh
 * skirmish, dense projectile combat and high unit counts. Designed to
 * be run under `sample`/Instruments for stack attribution.
 *
 * Usage:
 *   rwe_bench <scenario> [args] --data-path <ta-data-dir>
 *
 *   save <file.sav> [ticks]        restore a real save and tick forward
 *   skirmish <map.ota> [ticks]     fresh 4-player skirmish on a retail map
 *   combat <units-per-side> [map.ota] [ticks]   opposing missile armies
 *   units <count> [map.ota] [ticks]             N idle mobiles scattered
 *   kamikaze / kamikaze-burst <count> <map> [ticks]  bomb armies / simultaneous fuses
 *   bomber / bomber-combat <count> <map> [ticks]  repeated dropped runs / mixed combat
 *   veteran-none / veteran-small / veteran-large / veteran-combat / veteran-aoe / veteran-ai <count> <map> [ticks]
 *   emp <count> [map.ota] [ticks]               same army under 1800-tick EMP
 *   hash <file.sav> [iters]      time GameSimulation::computeHash()
 *
 * Every scenario prints one summary line:
 *   scenario units projectiles ticks wall_s tps ms_per_tick
 *
 * Build: RelWithDebInfo for representative numbers with symbols.
 */

#include <algorithm>
#include <rwe/game/GameScene_util.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <numeric>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rwe/LoadingScene_util.h>
#include <rwe/SkirmishSettings.h>
#include <rwe/game/GameScene_util.h>
#include <rwe/io/_3do/_3do.h>
#include <rwe/io/cob/Cob.h>
#include <rwe/io/fbi/io.h>
#include <rwe/io/featuretdf/io.h>
#include <rwe/io/moveinfotdf/io.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/io/tnt/TntArchive.h>
#include <rwe/io/weapontdf/WeaponTdf.h>
#include <rwe/mesh_util.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/GameHash_util.h>
#include <rwe/sim/UnitOrder.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/SpanStream.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vertex_height.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    namespace
    {
        [[noreturn]] void die(const std::string& msg)
        {
            std::cerr << "rwe_bench: " << msg << std::endl;
            std::exit(1);
        }

        struct BenchDefinitionPack
        {
            MovementClassDatabase movementClassDatabase;
            std::unordered_map<std::string, UnitDefinition> unitDefinitions;
            std::unordered_map<std::string, UnitModelDefinition> unitModelDefinitions;
            std::unordered_map<std::string, CobScript> unitScriptDefinitions;
            std::unordered_map<std::string, WeaponDefinition> weaponDefinitions;
            std::unordered_map<std::string, FeatureTdf> featureTdfs;
        };

        /**
         * Media-free half of LoadingScene::loadDefinitions -- mirrors
         * the SkirmishClosure harness so bench numbers reflect the
         * real game sim.
         */
        BenchDefinitionPack loadBenchDefinitionPack(AbstractVirtualFileSystem& vfs)
        {
            BenchDefinitionPack pack;

            {
                auto bytes = vfs.readFile("gamedata/MOVEINFO.TDF");
                if (!bytes)
                {
                    die("missing gamedata/MOVEINFO.TDF");
                }
                for (const auto& c : parseMoveInfoTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
                {
                    pack.movementClassDatabase.registerMovementClass(parseMovementClassDefinition(c.second));
                }
            }
            {
                for (const auto& fileName : vfs.getFileNames("weapons", ".tdf"))
                {
                    auto bytes = vfs.readFile("weapons/" + fileName);
                    if (!bytes)
                    {
                        die("missing weapons/" + fileName);
                    }
                    for (const auto& pair : parseWeaponTdf(parseTdfFromString(std::string(bytes->data(), bytes->size()))))
                    {
                        pack.weaponDefinitions.insert({toUpper(pair.first), parseWeaponDefinition(pair.second)});
                    }
                }
            }
            {
                for (const auto& fbiName : vfs.getFileNames("units", ".fbi"))
                {
                    auto bytes = vfs.readFile("units/" + fbiName);
                    if (!bytes)
                    {
                        die("missing units/" + fbiName);
                    }
                    auto fbi = parseUnitFbi(parseTdfFromString(std::string(bytes->data(), bytes->size())));
                    auto unitDefinition = parseUnitDefinition(fbi, pack.movementClassDatabase);

                    auto objBytes = vfs.readFile("objects3d/" + fbi.objectName + ".3do");
                    if (!objBytes)
                    {
                        die("missing objects3d/" + fbi.objectName + ".3do");
                    }
                    SpanStream objStream(objBytes->data(), objBytes->size());
                    auto objects = parse3doObjects(objStream, objStream.tellg());
                    if (objects.size() != 1)
                    {
                        die("3do " + fbi.objectName + " gave " + std::to_string(objects.size()) + " roots");
                    }
                    pack.unitModelDefinitions.insert({toUpper(fbi.objectName),
                        createUnitModelDefinition(simScalarFromFixed(findHighestVertex(objects.front()).y), unitMeshFrom3do(objects.front()))});
                    pack.unitDefinitions.insert({toUpper(fbi.unitName), std::move(unitDefinition)});
                }
            }
            applyTransplantBalanceOverrides(pack.unitDefinitions);

            // GUI build pages flatten into each builder's build list
            // (mirrors LoadingScene: only BehaviorBuildButton gadgets
            // are products)
            for (auto& [name, def] : pack.unitDefinitions)
            {
                if (!def.builder)
                {
                    continue;
                }
                std::unordered_set<std::string> seen;
                for (int i = 1; auto rawGui = vfs.readFile("guis/" + name + std::to_string(i) + ".GUI"); ++i)
                {
                    auto parsedGui = parseGuiFromBytes(*rawGui);
                    if (!parsedGui)
                    {
                        die("bad gui " + name + std::to_string(i));
                    }
                    for (std::size_t j = 1; j < parsedGui->size(); ++j)
                    {
                        const auto& entry = (*parsedGui)[j];
                        if (!(entry.common.attribs & GuiButtonAttrib::BehaviorBuildButton))
                        {
                            continue;
                        }
                        auto n = toUpper(entry.common.name);
                        if (seen.insert(n).second
                            && pack.unitDefinitions.find(n) != pack.unitDefinitions.end())
                        {
                            def.buildOptions.push_back(n);
                        }
                    }
                }
            }

            pack.unitScriptDefinitions = loadCobScripts(vfs);

            {
                for (const auto& name : vfs.getFileNamesRecursive("features", ".tdf"))
                {
                    auto bytes = vfs.readFile("features/" + name);
                    if (!bytes)
                    {
                        continue;
                    }
                    auto tdfRoot = parseTdfFromString(std::string(bytes->data(), bytes->size()));
                    for (const auto& e : tdfRoot.blocks)
                    {
                        pack.featureTdfs.insert({toUpper(e.first), parseFeatureTdf(*e.second)});
                    }
                }
            }
            loadFeatureAnimations(vfs, pack.featureTdfs);
            return pack;
        }

        void wireDefinitions(GameSimulation& sim, const BenchDefinitionPack& pack)
        {
            sim.unitDefinitions = pack.unitDefinitions;
            sim.weaponDefinitions = pack.weaponDefinitions;
            sim.movementClassDatabase = pack.movementClassDatabase;
            sim.unitModelDefinitions = pack.unitModelDefinitions;
            sim.unitScriptDefinitions = pack.unitScriptDefinitions;
            sim.movementClassCollisionService = createMovementClassCollisionService(sim.terrain, sim.movementClassDatabase);
        }

        struct TntMap
        {
            Grid<TntTileAttributes> attrs;
            std::vector<std::string> featureNames;
            int seaLevel;
        };

        TntMap loadTnt(AbstractVirtualFileSystem& vfs, const std::string& mapName)
        {
            auto tntBytes = vfs.readFile("maps/" + mapName + ".tnt");
            if (!tntBytes)
            {
                die("missing maps/" + mapName + ".tnt");
            }
            SpanStream tntStream(tntBytes->data(), tntBytes->size());
            TntArchive tnt(&tntStream);
            TntMap m{Grid<TntTileAttributes>(tnt.getHeader().width, tnt.getHeader().height), getFeatureNames(tnt), static_cast<int>(tnt.getHeader().seaLevel)};
            tnt.readMapAttributes(m.attrs.getData());
            return m;
        }

        OtaRecord loadOta(AbstractVirtualFileSystem& vfs, const std::string& mapName)
        {
            auto bytes = vfs.readFile("maps/" + mapName + ".ota");
            if (!bytes)
            {
                die("missing maps/" + mapName + ".ota");
            }
            return parseOta(parseTdfFromString(std::string(bytes->data(), bytes->size())));
        }

        void loadMapFeatures(
            GameSimulation& sim,
            const BenchDefinitionPack& pack,
            const TntMap& map,
            const std::vector<std::pair<Point, std::string>>& extraFeatures)
        {
            std::unordered_set<std::string> requiredFeatures;
            std::vector<std::pair<Point, std::string>> features;
            map.attrs.forEachIndexed([&](auto c, const auto& e) {
                if (e.feature != TntTileAttributes::FeatureNone
                    && e.feature != TntTileAttributes::FeatureUnknown
                    && e.feature != TntTileAttributes::FeatureVoid)
                {
                    features.emplace_back(Point(c.x, c.y), map.featureNames.at(e.feature));
                }
            });
            for (const auto& f : extraFeatures)
            {
                features.push_back(f);
            }
            for (const auto& [pos, name] : features)
            {
                requiredFeatures.insert(toUpper(name));
            }
            for (const auto& [name, def] : pack.unitDefinitions)
            {
                if (!def.corpse.empty())
                {
                    requiredFeatures.insert(toUpper(def.corpse));
                }
            }
            for (const auto& name : requiredFeatures)
            {
                if (pack.featureTdfs.count(name) != 0)
                {
                    loadFeatureDefinitions(pack.featureTdfs, sim.featureDefinitions, sim.featureNameIndex, name);
                }
            }
            for (const auto& [pos, name] : features)
            {
                if (auto id = sim.tryGetFeatureDefinitionId(name); id)
                {
                    sim.addFeature(*id, pos.x, pos.y);
                }
            }
        }

        void attachAiForComputers(GameSimulation& sim, AbstractVirtualFileSystem& vfs, const std::string& aiProfileName, CampaignAiDifficulty difficulty)
        {
            if (auto text = loadCampaignAiProfileText(vfs, "ai", aiProfileName))
            {
                attachCampaignAiPlayers(sim, parseCampaignAiProfile(*text, difficulty));
            }
        }

        /**
         * Restore a real save end-to-end, mirroring the legacy-save
         * regression harness.
         */
        GameSimulation loadSaveIntoSim(
            const std::filesystem::path& savePath,
            AbstractVirtualFileSystem& vfs,
            const BenchDefinitionPack& pack)
        {
            std::ifstream f(savePath, std::ios::binary);
            if (!f)
            {
                die("cannot open " + savePath.string());
            }
            std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto decoded = decodeSavedGame(bytes);

            auto map = loadTnt(vfs, decoded.meta.mapName);
            auto ota = loadOta(vfs, decoded.meta.mapName);
            const auto& schema = ota.schemas.at(decoded.meta.schemaIndex);

            std::vector<std::pair<Point, std::string>> schemaFeatures;
            for (const auto& sf : schema.features)
            {
                schemaFeatures.emplace_back(Point(sf.xPos, sf.zPos), sf.featureName);
            }

            GameSimulation sim(
                MapTerrain(getHeightGrid(map.attrs), SimScalar(map.seaLevel)),
                static_cast<unsigned char>(schema.surfaceMetal),
                std::max(0, static_cast<int>(ota.minWindSpeed)),
                std::min(static_cast<int>(ota.maxWindSpeed), MaxUtilizableWindSpeed),
                static_cast<int>(ota.tidalStrength));
            sim.mapGravity = ota.gravity;
            wireDefinitions(sim, pack);
            loadMapFeatures(sim, pack, map, schemaFeatures);

            CampaignProgress progress;
            applySavedGame(sim, decoded, &progress);

            auto difficulty = decoded.meta.isCampaign
                ? campaignAiDifficultyIndex(decoded.meta.difficulty)
                : skirmishAiDifficultyIndex(decoded.meta.skirmishDifficulty
                        ? static_cast<SkirmishDifficulty>(*decoded.meta.skirmishDifficulty)
                        : SkirmishDifficulty::Medium);
            attachAiForComputers(sim, vfs, schema.aiProfile, difficulty);
            return sim;
        }

        /**
         * Fresh skirmish on a retail map: schema 0, N players spread
         * over the authored start positions, commanders + AI attached.
         */
        GameSimulation buildSkirmishSim(
            const std::string& mapName,
            AbstractVirtualFileSystem& vfs,
            const BenchDefinitionPack& pack,
            unsigned int playerCount)
        {
            auto map = loadTnt(vfs, mapName);
            auto ota = loadOta(vfs, mapName);
            const auto& schema = ota.schemas.at(0);

            std::vector<std::pair<Point, std::string>> schemaFeatures;
            for (const auto& sf : schema.features)
            {
                schemaFeatures.emplace_back(Point(sf.xPos, sf.zPos), sf.featureName);
            }

            GameSimulation sim(
                MapTerrain(getHeightGrid(map.attrs), SimScalar(map.seaLevel)),
                static_cast<unsigned char>(schema.surfaceMetal),
                std::max(0, static_cast<int>(ota.minWindSpeed)),
                std::min(static_cast<int>(ota.maxWindSpeed), MaxUtilizableWindSpeed),
                static_cast<int>(ota.tidalStrength));
            sim.lineOfSightEnabled = true;
            sim.lineOfSightMode = static_cast<int>(SkirmishLineOfSight::True);
            sim.mappingEnabled = true;
            sim.commanderDeathContinues = true;
            sim.mapGravity = ota.gravity;
            wireDefinitions(sim, pack);
            loadMapFeatures(sim, pack, map, schemaFeatures);

            auto startPositions = collectStartPositions(schema);
            playerCount = std::min<unsigned int>(playerCount, startPositions.size());
            if (playerCount < 2)
            {
                die("map " + mapName + " has <2 start positions");
            }
            for (unsigned int i = 0; i < playerCount; ++i)
            {
                bool isHuman = (i == 0);
                GamePlayerInfo gpi{
                    std::nullopt,
                    isHuman ? GamePlayerType::Human : GamePlayerType::Computer,
                    PlayerColorIndex(i),
                    GamePlayerStatus::Alive,
                    isHuman ? "ARM" : "CORE",
                    std::nullopt,
                    Metal(schema.humanMetal),
                    Energy(schema.humanEnergy),
                    Metal(schema.humanMetal),
                    Energy(schema.humanEnergy),
                    Metal(schema.humanMetal),
                    Energy(schema.humanEnergy)};
                addSkirmishPlayer(sim, gpi);
            }
            attachAiForComputers(sim, vfs, schema.aiProfile, skirmishAiDifficultyIndex(SkirmishDifficulty::Hard));

            for (unsigned int i = 0; i < playerCount; ++i)
            {
                const auto* sp = startPositions[i];
                auto pos = sim.terrain.topLeftCoordinateToWorld(
                    SimVector(SimScalar(sp->xPos), SimScalar(0), SimScalar(sp->zPos)));
                pos.y = sim.terrain.getHeightAt(pos.x, pos.z);
                const char* commander = (i == 0) ? "ARMCOM" : "CORCOM";
                auto spawned = sim.trySpawnUnit(commander, PlayerId(i), pos, std::nullopt, true, std::nullopt, true);
                if (!spawned)
                {
                    die("commander spawn failed on " + mapName);
                }
                auto& unit = sim.getUnitState(*spawned);
                unit.finishBuilding(sim.unitDefinitions.at(toUpper(unit.unitType)));
            }
            return sim;
        }

        /**
         * Scatter `count` mobiles of `unitType` owned by `owner` over
         * the map interior. Returns how many spawned.
         */
        unsigned int scatterSpawn(
            GameSimulation& sim,
            const std::string& unitType,
            PlayerId owner,
            unsigned int count,
            SimScalar x0,
            SimScalar z0,
            SimScalar x1,
            SimScalar z1,
            bool allowOverlap,
            bool topLeftCoordinates = false)
        {
            unsigned int spawned = 0;
            int cols = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(count))));
            SimScalar dx = (x1 - x0) / SimScalar(cols);
            // New veterancy cases use bounded map coordinates. Keep legacy
            // scenarios unchanged so their retained executable remains comparable.
            int rows = topLeftCoordinates ? (static_cast<int>(count) + cols - 1) / cols : cols;
            SimScalar dz = (z1 - z0) / SimScalar(std::max(1, rows));
            for (unsigned int i = 0; i < count; ++i)
            {
                SimScalar x = x0 + dx * SimScalar(i % cols) + dx / SimScalar(2);
                SimScalar z = z0 + dz * SimScalar(i / cols) + dz / SimScalar(2);
                SimVector pos(x, 0_ss, z);
                if (topLeftCoordinates)
                {
                    pos = sim.terrain.topLeftCoordinateToWorld(pos);
                    const auto& definition = sim.unitDefinitions.at(toUpper(unitType));
                    if (!sim.occupiedGrid.tryToRegion(sim.computeFootprintRegion(pos, definition.movementCollisionInfo)))
                    {
                        die("benchmark footprint outside map: " + unitType);
                    }
                }
                pos.y = sim.terrain.getHeightAt(pos.x, pos.z);
                if (auto id = sim.trySpawnUnit(unitType, owner, pos, std::nullopt, allowOverlap, std::nullopt, true))
                {
                    auto& u = sim.getUnitState(*id);
                    u.finishBuilding(sim.unitDefinitions.at(toUpper(u.unitType)));
                    ++spawned;
                }
            }
            return spawned;
        }

        SimVector mapCenter(const GameSimulation& sim)
        {
            auto w = sim.terrain.getWidthInWorldUnits();
            auto h = sim.terrain.getHeightInWorldUnits();
            return SimVector(w / SimScalar(2), SimScalar(0), h / SimScalar(2));
        }

        template <typename M>
        unsigned int liveCount(const M& m)
        {
            unsigned int n = 0;
            for (const auto& e : m)
            {
                (void)e;
                ++n;
            }
            return n;
        }

        /**
         * Per-unit animation/COB state dump for the `anim` scenario:
         * thread queue residency, piece ops, factory flags -- the
         * observable state that decides whether scripts still animate
         * after a save/load round trip.
         */
        void dumpUnitDiagnostics(const GameSimulation& sim, const char* label)
        {
            std::cout << "== " << label << " t=" << sim.gameTime.value << " ==" << std::endl;
            for (const auto& [id, u] : sim.units)
            {
                std::cout << "unit " << id.value << " " << u.unitType
                          << " pos=(" << u.position.x.value << "," << u.position.z.value << ")"
                          << " prev=(" << u.previousPosition.x.value << "," << u.previousPosition.z.value << ")"
                          << " yardOpen=" << u.yardOpen
                          << " inBuildStance=" << u.inBuildStance
                          << " activated=" << u.activated
                          << " fstate=" << u.factoryState.index()
                          << " queue=" << u.buildQueue.size();
                if (u.cobEnvironment)
                {
                    const auto& env = *u.cobEnvironment;
                    std::cout << " threads=" << env.threads.size()
                              << " rdy=" << env.readyQueue.size()
                              << " blk=" << env.blockedQueue.size()
                              << " slp=" << env.sleepingQueue.size()
                              << " fin=" << env.finishedQueue.size();
                    std::cout << " [";
                    const char* sep = "";
                    for (const auto& t : env.threads)
                    {
                        std::cout << sep << t->name;
                        sep = ",";
                    }
                    std::cout << "]";
                }
                else
                {
                    std::cout << " NO-COBENV";
                }
                // pieces with non-identity transform or in-flight ops,
                // plus a transform signature so animating pieces show
                // as a changing value between dumps
                unsigned int movingPieces = 0;
                unsigned int displacedPieces = 0;
                long long pieceSig = 0;
                for (const auto& p : u.pieces)
                {
                    if (p.xMoveOperation || p.yMoveOperation || p.zMoveOperation
                        || p.xTurnOperation || p.yTurnOperation || p.zTurnOperation)
                    {
                        ++movingPieces;
                    }
                    if (p.offset != SimVector(0_ss, 0_ss, 0_ss)
                        || p.rotationX != SimAngle(0) || p.rotationY != SimAngle(0) || p.rotationZ != SimAngle(0))
                    {
                        ++displacedPieces;
                    }
                    pieceSig = pieceSig * 31
                        + static_cast<long long>(p.offset.x.value + p.offset.y.value + p.offset.z.value)
                        + static_cast<long long>(p.rotationX.value + p.rotationY.value + p.rotationZ.value);
                }
                std::cout << " animPieces=" << movingPieces
                          << " displacedPieces=" << displacedPieces
                          << " pieceSig=" << pieceSig << std::endl;
            }
        }

        std::uint64_t residentBytes()
        {
#ifdef __APPLE__
            mach_task_basic_info_data_t info{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
            { return info.resident_size; }
#endif
            return 0;
        }

        struct RunResult
        {
            unsigned int ticks;
            double wallSeconds;
            std::vector<double> segmentSeconds;
            unsigned int peakProjectiles{0};
            unsigned int peakUnits{0};
            std::vector<double> tickMilliseconds;
            std::uint64_t rssBefore{0}, rssAfter{0};
            unsigned int serviceRequests{0}, serviceTransitions{0}, peakServiceWaiting{0}, peakServiceOccupied{0};
            unsigned int peakFeatureEvents{0}, peakPathRequests{0};
            std::vector<double> hashMicroseconds;
            std::uint64_t hashChecksum{0};
        };

        void reportHashTicks(const std::vector<double>& hashes, double tickMs, std::uint64_t checksum)
        {
            if (hashes.empty()) { return; }
            auto sorted=hashes; std::sort(sorted.begin(),sorted.end());
            auto at=[&](double q){return sorted[static_cast<std::size_t>(std::ceil(q*sorted.size()))-1];};
            const auto mean=std::accumulate(hashes.begin(),hashes.end(),0.0)/hashes.size();
            std::cout << "hash_samples=" << hashes.size() << " hash_mean_us=" << mean
                << " hash_p50_us=" << at(.5) << " hash_p95_us=" << at(.95) << " hash_p99_us=" << at(.99)
                << " hash_max_us=" << sorted.back() << " hash_work_percent=" << 100*mean/(tickMs*1000+mean)
                << " hash_tick_checksum=" << checksum << '\n';
        }

        /**
         * Tick `sim` `ticks` times with phase profiling enabled.
         * Wall time is also split into `segments` contiguous segments
         * so progressive slowdown within a run is visible.
         */
        RunResult runTicks(GameSimulation& sim, unsigned int ticks, unsigned int segments = 3, const std::function<void(unsigned int)>& onTick = {})
        {
            const char* warmupValue = std::getenv("RWE_BENCH_WARMUP");
            const unsigned int warmup = warmupValue ? std::stoul(warmupValue) : 0;
            sim.collectTickPhaseTimings = false;
            for (unsigned int i = 0; i < warmup; ++i) { sim.tick(); sim.events.clear(); }
            const char* phaseTiming = std::getenv("RWE_BENCH_PHASE_TIMING");
            sim.collectTickPhaseTimings = !phaseTiming || std::string(phaseTiming) != "0";
            sim.tickPhaseTimings.reset();
            std::cout << "warmup_ticks=" << warmup << " phase_timing=" << sim.collectTickPhaseTimings << '\n';
            RunResult r{ticks, 0.0, {}};
            // Opt-in production cadence: one full hash after every tick. These
            // measurements remain separate from sim.tick() and phase timers.
            const bool hashTicks=std::getenv("RWE_BENCH_HASH_TICKS") != nullptr;
            if (hashTicks) { r.hashMicroseconds.reserve(ticks); }
            auto segLen = std::max(1u, ticks / segments);
            const bool metrics = std::getenv("RWE_BENCH_TICK_METRICS") != nullptr;
            std::map<UnitId, unsigned int> priorServices;
            if (metrics) { r.tickMilliseconds.reserve(ticks); r.rssBefore = residentBytes(); }
            auto t0 = std::chrono::steady_clock::now();
            auto segStart = t0;
            for (unsigned int i = 0; i < ticks; ++i)
            {
                const auto tickStart = metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                sim.tick();
                if (metrics)
                {
                    r.tickMilliseconds.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count());
                    unsigned int waiting = 0;
                    std::map<UnitId, unsigned int> current;
                    for (const auto& [id, unit] : sim.units)
                    {
                        if (!unit.aircraftService) { continue; }
                        const auto phase = static_cast<unsigned int>(unit.aircraftService->phase);
                        current.emplace(id, phase);
                        auto old = priorServices.find(id);
                        if (old == priorServices.end()) { ++r.serviceRequests; ++r.serviceTransitions; }
                        else if (old->second != phase) { ++r.serviceTransitions; }
                        if (!unit.aircraftService->docked() && unit.aircraftService->phase != AircraftServiceState::Phase::Depart) { ++waiting; }
                    }
                    for (const auto& [id, phase] : priorServices) { if (!current.count(id)) { ++r.serviceTransitions; } }
                    priorServices = std::move(current);
                    r.peakFeatureEvents = std::max(r.peakFeatureEvents, static_cast<unsigned int>(sim.activeFeatures.size()));
                    r.peakPathRequests = std::max(r.peakPathRequests, static_cast<unsigned int>(sim.pathRequests.size()));
                    r.peakServiceWaiting = std::max(r.peakServiceWaiting, waiting);
                    r.peakServiceOccupied = std::max(r.peakServiceOccupied, static_cast<unsigned int>(sim.airServiceOccupants.size()));
                }
                if (hashTicks)
                {
                    const auto start=std::chrono::steady_clock::now();
                    const auto hash=sim.computeHash();
                    r.hashMicroseconds.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
                    r.hashChecksum+=hash.value;
                }
                r.peakProjectiles = std::max(r.peakProjectiles, liveCount(sim.projectiles));
                r.peakUnits = std::max(r.peakUnits, liveCount(sim.units));
                if ((i + 1) % segLen == 0)
                {
                    auto now = std::chrono::steady_clock::now();
                    r.segmentSeconds.push_back(std::chrono::duration<double>(now - segStart).count());
                    segStart = now;
                }
                if (onTick)
                {
                    onTick(i);
                }
            }
            auto t1 = std::chrono::steady_clock::now();
            r.wallSeconds = std::chrono::duration<double>(t1 - t0).count();
            sim.collectTickPhaseTimings = false;
            if (metrics) { r.rssAfter = residentBytes(); }
            return r;
        }

        void report(const std::string& scenario, const GameSimulation& sim, const RunResult& r)
        {
            std::cout << "scenario=" << scenario
                      << " units=" << liveCount(sim.units)
                      << " projectiles=" << liveCount(sim.projectiles)
                      << " features=" << liveCount(sim.features)
                      << " ticks=" << r.ticks
                      << " wall_s=" << r.wallSeconds
                      << " tps=" << (r.ticks / r.wallSeconds)
                      << " ms_per_tick=" << (r.wallSeconds * 1000.0 / r.ticks)
                      << " peakProj=" << r.peakProjectiles
                      << " peakUnits=" << r.peakUnits
                      << " state_hash=" << sim.computeHash().value
                      << std::endl;

            if (!r.segmentSeconds.empty())
            {
                std::cout << "segments_ms_per_tick=";
                const char* sep = "";
                for (double s : r.segmentSeconds)
                {
                    unsigned int segTicks = r.ticks / r.segmentSeconds.size();
                    std::cout << sep << (s * 1000.0 / segTicks);
                    sep = ",";
                }
                std::cout << std::endl;
            }

            if (!r.tickMilliseconds.empty())
            {
                auto samples = r.tickMilliseconds;
                std::sort(samples.begin(), samples.end());
                auto percentile = [&](double q) { return samples[static_cast<std::size_t>(std::ceil(q * samples.size())) - 1]; };
                std::cout << "tick_p50_ms=" << percentile(0.5) << " tick_p95_ms=" << percentile(0.95)
                    << " tick_p99_ms=" << percentile(0.99) << " tick_max_ms=" << samples.back() << " service_requests=" << r.serviceRequests
                    << " service_transitions=" << r.serviceTransitions << " peak_waiting=" << r.peakServiceWaiting
                    << " peak_feature_events=" << r.peakFeatureEvents << " peak_path_requests=" << r.peakPathRequests
                    << " peak_occupied=" << r.peakServiceOccupied << " rss_before=" << r.rssBefore << " rss_after=" << r.rssAfter << '\n';
                const auto tickMean=std::accumulate(samples.begin(),samples.end(),0.0)/samples.size();
                std::cout << "tick_mean_ms=" << tickMean << '\n';
                reportHashTicks(r.hashMicroseconds,tickMean,r.hashChecksum);
            }

            const auto& pt = sim.tickPhaseTimings;
            if (pt.tickCount == 0)
            {
                return;
            }
            auto per = [&pt](uint64_t ns) { return static_cast<double>(ns) / 1e6 / pt.tickCount; };
            double tracked = per(pt.visibility) + per(pt.wind) + per(pt.meteorShower)
                + per(pt.resources) + per(pt.cloak) + per(pt.campaignAi)
                + per(pt.pathfinding) + per(pt.unitBehavior) + per(pt.pieces)
                + per(pt.cob) + per(pt.projectiles) + per(pt.featureUpdates) + per(pt.corrosiveWater)
                + per(pt.victoryAndObjectives) + per(pt.deadUnitCleanup)
                + per(pt.deadProjectileCleanup) + per(pt.spawn);
            double total = r.wallSeconds * 1000.0 / r.ticks;
            std::cout << "phases_ms_per_tick"
                      << " visibility=" << per(pt.visibility)
                      << " wind=" << per(pt.wind)
                      << " meteor=" << per(pt.meteorShower)
                      << " resources=" << per(pt.resources)
                      << " cloak=" << per(pt.cloak)
                      << " ai=" << per(pt.campaignAi)
                      << " pathfinding=" << per(pt.pathfinding)
                      << " unitBehavior=" << per(pt.unitBehavior)
                      << " pieces=" << per(pt.pieces)
                      << " cob=" << per(pt.cob)
                      << " projectiles=" << per(pt.projectiles)
                      << " feature_updates=" << per(pt.featureUpdates)
                      << " feature_damage_nested=" << per(pt.featureDamage)
                      << " feature_collision_nested=" << per(pt.featureCollision)
                      << " veterancy_nested=" << per(pt.veterancy)
                      << " veteran_awards=" << pt.veterancyAwards
                      << " veteran_promotions=" << pt.veterancyPromotions
                      << " corrosive=" << per(pt.corrosiveWater)
                      << " objectives=" << per(pt.victoryAndObjectives)
                      << " deadUnits=" << per(pt.deadUnitCleanup)
                      << " deadProj=" << per(pt.deadProjectileCleanup)
                      << " spawn=" << per(pt.spawn)
                      << " tracked=" << tracked
                      << " other=" << (total - tracked)
                      << std::endl;
        }
    }
}

namespace rwe {
#include "rwe_bench_recovery.inc"
}

int main(int argc, char* argv[])
{
    using namespace rwe;

    std::filesystem::path dataPath;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--data-path" && i + 1 < argc)
        {
            dataPath = argv[++i];
        }
        else
        {
            args.push_back(a);
        }
    }
    if (dataPath.empty() || !std::filesystem::exists(dataPath))
    {
        std::cerr << "usage: rwe_bench <scenario> [args] --data-path <ta-data-dir>\n"
                     "  save <file.sav> [ticks]\n"
                     "  skirmish <mapName> [ticks]     (e.g. 'Coast To Coast')\n"
                     "  combat <n-per-side> <mapName> [ticks]\n"
                     "  units <count> <mapName> [ticks]\n"
                     "  bomber / bomber-combat <count> <mapName> [ticks]\n"
                     "  hash <file.sav> [iters]\n";
        return 1;
    }
    if (args.empty())
    {
        rwe::die("no scenario given");
    }
    const std::string scenario = args[0];

    setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_bench.log", false));

    auto vfs = constructVfs(dataPath);

    if (scenario == "feature-render")
    {
        const unsigned int count = args.size() > 1 ? std::stoul(args[1]) : 10000;
        const unsigned int frames = args.size() > 2 ? std::stoul(args[2]) : 1000;
        GameMediaDatabase media;
        FeatureSpriteInfo sprites{};
        auto series = [] {
            auto value = std::make_shared<SpriteSeries>();
            for (unsigned int i = 0; i < 3; ++i)
            {
                value->sprites.push_back(std::make_shared<Sprite>(Rectangle2f(0, -40, 20, 40), SharedTextureHandle{}, nullptr));
            }
            return value;
        };
        sprites.animation = series(); sprites.shadowAnimation = series();
        for (unsigned int i = 0; i < 3; ++i) { sprites.events[i] = series(); sprites.eventShadows[i] = series(); }
        sprites.eventCullRadii[2] = 594.0f;
        FeatureMediaInfo info; info.renderInfo = sprites; media.addFeature(std::move(info));
        FeatureDefinition definition{}; definition.height = 40_ss;
        MapFeature feature{}; feature.featureName = FeatureDefinitionId(0);
        SpriteBatch batch; batch.sprites.reserve(count * 2);
        auto matrix = Matrix4f::identity();
        for (const auto& [name, event] : std::vector<std::pair<std::string, FeatureEvent>>{
            {"intact", FeatureEvent::None}, {"burning", FeatureEvent::Burning}, {"dying", FeatureEvent::Dying}, {"reclaiming", FeatureEvent::Reclaiming}})
        {
            feature.event = event; feature.animationFrame = feature.shadowFrame = event == FeatureEvent::None ? 0 : 1;
            std::vector<double> times; times.reserve(frames);
            for (unsigned int frame = 0; frame < frames; ++frame)
            {
                batch.sprites.clear(); auto start = std::chrono::steady_clock::now();
                for (unsigned int n = 0; n < count; ++n)
                {
                    drawFeature(media, feature, definition, matrix, batch);
                    drawFeatureShadow(media, feature, definition, matrix, batch);
                }
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            const auto* expected = event == FeatureEvent::None ? sprites.animation->sprites[0].get()
                : sprites.events[static_cast<unsigned int>(event) - 1]->sprites[1].get();
            if (batch.sprites.size() != count * 2 || batch.sprites.front().sprite != expected) { die("feature render cursor mismatch"); }
            if (event == FeatureEvent::Burning && featureEventCullRadius(media, feature) != 594.0f) { die("feature event cull bound mismatch"); }
            std::sort(times.begin(), times.end());
            auto percentile = [&](double q) { return times[static_cast<unsigned int>(std::ceil(q * times.size())) - 1]; };
            std::cout << "feature_render_cpu phase=" << name << " features=" << count << " frames=" << frames
                << " p50_ms=" << percentile(0.5) << " p95_ms=" << percentile(0.95) << " p99_ms=" << percentile(0.99) << '\n';
        }
        // Finished shadow cursors disappear instead of retaining the rest shadow.
        batch.sprites.clear(); feature.shadowFrame = 3;
        drawFeatureShadow(media, feature, definition, matrix, batch);
        if (!batch.sprites.empty()) { die("stale feature shadow"); }
        std::cout << "feature_render_checks=PASS gpu_submission=NOT_MEASURED\n";
        return 0;
    }

    // Explicit audit mode keeps observations outside sim.tick(), consumes
    // transient events like GameScene, and never writes the input save.
    if (scenario == "audit-save")
    {
        if (args.size() < 2) { die("missing save file"); }
        const unsigned int ticks = args.size() > 2 ? std::stoul(args[2]) : 18000;
        auto pack = loadBenchDefinitionPack(vfs);
        auto sim = loadSaveIntoSim(args[1], vfs, pack);
        auto copy = loadSaveIntoSim(args[1], vfs, pack);
        std::ifstream input(args[1], std::ios::binary);
        auto original = decodeSavedGame(std::vector<char>((std::istreambuf_iterator<char>(input)), {}));
        auto params = gameParametersFromSave(original);
        unsigned int mismatches = 0, occupancyDifferences = 0, invalidCells = 0, cycles = 0;
        unsigned int savedStateDifferences=0, rngDifferences=0;
        auto observe = [&](const char* stage, unsigned int elapsed) {
            unsigned int threads = 0, orders = 0, recovery = 0, invalid = 0, differences = 0;
            std::unordered_set<UnitId> units;
            std::unordered_set<FeatureId> features;
            for (const auto& [id, unit] : sim.units)
            {
                units.insert(id);
                threads += unit.cobEnvironment ? unit.cobEnvironment->threads.size() : 0;
                orders += unit.orders.size();
            }
            for (const auto& [id, feature] : sim.features) { features.insert(id); }
            for (const auto& ai : sim.campaignAi) { recovery += ai.deadlockedFactories.size(); }
            const auto& a = sim.occupiedGrid.getVector();
            const auto& b = copy.occupiedGrid.getVector();
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                const auto& x = a[i]; const auto& y = b[i];
                if ((x.mobileUnitId && !units.count(*x.mobileUnitId))
                    || (x.buildingInfo && !units.count(x.buildingInfo->unit))
                    || (x.featureId && !features.count(*x.featureId))) { ++invalid; }
                if (x.mobileUnitId != y.mobileUnitId || x.featureId != y.featureId
                    || static_cast<bool>(x.buildingInfo) != static_cast<bool>(y.buildingInfo)
                    || (x.buildingInfo && y.buildingInfo && (x.buildingInfo->unit != y.buildingInfo->unit
                        || x.buildingInfo->passable != y.buildingInfo->passable))) { ++differences; }
            }
            invalidCells += invalid; occupancyDifferences += differences;
            std::cout << "audit stage=" << stage << " tick=" << elapsed << " rss=" << residentBytes()
                << " units=" << units.size() << " projectiles=" << liveCount(sim.projectiles)
                << " cob_threads=" << threads << " unit_orders=" << orders << " ai_players=" << sim.campaignAi.size()
                << " ai_recovery_tasks=" << recovery << " path_queue=" << sim.pathRequests.size()
                << " invalid_occupancy_refs=" << invalid << " occupancy_differences=" << differences << std::endl;
        };
        observe("initial", 0);
        for (unsigned int start = 0; start < ticks;)
        {
            const unsigned int window = std::min(3000u, ticks - start);
            auto result = runTicks(sim, window, 3, [&](unsigned int i) {
                copy.tick();
                if ((i + 1) % 30 == 0 && sim.computeHash() != copy.computeHash()) { ++mismatches; }
                sim.events.clear(); copy.events.clear();
            });
            start += window;
            report("audit-window-" + std::to_string(start), sim, result);
            observe("before-load", start);
            const auto controlBytes=encodeSavedGame(sim,params,nullptr,"IN-MEMORY-AUDIT",0);
            const auto copyBytes=encodeSavedGame(copy,params,nullptr,"IN-MEMORY-AUDIT",0);
            if(controlBytes!=copyBytes) { ++savedStateDifferences; }
            if(sim.rng!=copy.rng) { ++rngDifferences; }
            auto saved = decodeSavedGame(copyBytes);
            applySavedGame(copy, saved, nullptr);
            ++cycles;
            if (sim.computeHash() != copy.computeHash()) { ++mismatches; }
            if(controlBytes!=encodeSavedGame(copy,params,nullptr,"IN-MEMORY-AUDIT",0)) { ++savedStateDifferences; }
            if(sim.rng!=copy.rng) { ++rngDifferences; }
            observe("after-load", start);
        }
        std::cout << "audit_result cycles=" << cycles << " hash_mismatches=" << mismatches
            << " occupancy_difference_samples=" << occupancyDifferences << " invalid_reference_samples=" << invalidCells
            << " saved_state_differences=" << savedStateDifferences << " rng_differences=" << rngDifferences << std::endl;
        return mismatches || occupancyDifferences || invalidCells || savedStateDifferences || rngDifferences ? 2 : 0;
    }

    if (scenario == "save" || scenario == "hash")
    {
        if (args.size() < 2)
        {
            die("missing save file");
        }
        auto pack = loadBenchDefinitionPack(vfs);
        auto sim = loadSaveIntoSim(args[1], vfs, pack);
        if (scenario == "hash")
        {
            unsigned int iters = args.size() > 2 ? std::stoul(args[2]) : 1000;
            auto t0 = std::chrono::steady_clock::now();
            GameHash acc(0);
            for (unsigned int i = 0; i < iters; ++i)
            {
                acc += sim.computeHash();
            }
            auto t1 = std::chrono::steady_clock::now();
            double wall = std::chrono::duration<double>(t1 - t0).count();
            std::cout << "scenario=hash units=" << liveCount(sim.units)
                      << " iters=" << iters << " wall_s=" << wall
                      << " us_per_hash=" << (wall * 1e6 / iters)
                      << " acc=" << acc.value << std::endl;
            return 0;
        }
        unsigned int ticks = args.size() > 2 ? std::stoul(args[2]) : 3000;
        auto r = runTicks(sim, ticks);
        report("save:" + args[1], sim, r);
        return 0;
    }

    if (scenario == "recovery")
    {
        const unsigned ticks=args.size()>1?std::stoul(args[1]):18000;
        auto pack=loadBenchDefinitionPack(vfs); auto sim=recoveryWorld(pack);
        auto ids=installRecoveryFixture(sim,32,32,false);
        for(unsigned i=0;i<2101;++i) { sim.tick(); sim.events.clear(); }
        sim.unitDefinitions.at("CORCV").maxVelocity=0_ss;
        std::get<UnitPhysicsInfoGround>(sim.getUnitState(ids.builder).physics).currentSpeed=0_ss;
        unsigned peakRecovery=0, peakAvoid=0, peakEvictions=0;
        const auto result=runTicks(sim,ticks,3,[&](unsigned){
            const auto& tasks=sim.campaignAi.front().deadlockedFactories;
            peakRecovery=std::max(peakRecovery,static_cast<unsigned>(tasks.size()));
            for(const auto& [id,d]:tasks) {
                peakAvoid=std::max(peakAvoid,static_cast<unsigned>(d.reclaimAvoid.size()));
                peakEvictions=std::max(peakEvictions,static_cast<unsigned>(d.evictAttempts.size()));
            }
            sim.events.clear();
        });
        report("recovery",sim,result);
        std::cout << "peak_recovery=" << peakRecovery << " peak_avoid=" << peakAvoid << " peak_evictions=" << peakEvictions << '\n';
        return 0;
    }

    if (scenario == "skirmish")
    {
        if (args.size() < 2)
        {
            die("missing map name");
        }
        auto pack = loadBenchDefinitionPack(vfs);
        unsigned int ticks = args.size() > 2 ? std::stoul(args[2]) : 3000;
        auto sim = buildSkirmishSim(args[1], vfs, pack, 4);
        std::cerr << "campaignAi=" << sim.campaignAi.size() << std::endl;
        auto r = runTicks(sim, ticks, 3, [&](unsigned int i) {
            if ((i + 1) % 3000 == 0)
            {
                for (const auto& st : sim.campaignAi)
                {
                    std::cerr << "  t+" << (i + 1) << " ai p" << st.player.value
                              << " fac=" << st.groups[AiGroupFactory].size()
                              << " bld=" << st.groups[AiGroupBuilder].size()
                              << " rA=" << st.groups[AiGroupReserveA].size()
                              << " rB=" << st.groups[AiGroupReserveB].size();
                }
                std::cerr << " units=" << liveCount(sim.units) << std::endl;
            }
        });
        report("skirmish:" + args[1], sim, r);
        return 0;
    }

    if (scenario == "anim")
    {
        if (args.size() < 2)
        {
            die("missing map name");
        }
        auto pack = loadBenchDefinitionPack(vfs);
        const std::string& mapName = args[1];
        auto sim = buildSkirmishSim(mapName, vfs, pack, 2);

        auto w = sim.terrain.getWidthInWorldUnits();
        auto h = sim.terrain.getHeightInWorldUnits();
        auto px = [&](double fx, double fz) {
            SimScalar x = w * SimScalar(static_cast<float>(fx));
            SimScalar z = h * SimScalar(static_cast<float>(fz));
            return SimVector(x, sim.terrain.getHeightAt(x, z), z);
        };
        auto spawnDone = [&](const std::string& type, SimVector pos) -> UnitId {
            auto id = sim.trySpawnUnit(type, PlayerId(0), pos, std::nullopt, true, std::nullopt, true);
            if (!id)
            {
                die("spawn failed: " + type);
            }
            sim.getUnitState(*id).finishBuilding(sim.unitDefinitions.at(toUpper(type)));
            return *id;
        };

        // diagnostic set: moving kbot + moving vehicle, an idle unit,
        // an open producing factory and a closed idle factory
        auto kbot = spawnDone("CORCK", px(0.40, 0.40));
        auto veh = spawnDone("ARMCV", px(0.42, 0.40));
        auto idle = spawnDone("ARMCK", px(0.44, 0.40));
        auto lab = spawnDone("ARMLAB", px(0.46, 0.44));
        auto vp = spawnDone("ARMVP", px(0.50, 0.44));

        // distant move targets so the movers are still walking at save
        sim.getUnitState(kbot).orders.push_back(UnitOrder(MoveOrder(px(0.90, 0.90))));
        sim.getUnitState(veh).orders.push_back(UnitOrder(MoveOrder(px(0.90, 0.85))));
        // queue production on the lab only; vp stays closed/idle
        sim.getUnitState(lab).buildQueue.emplace_back("ARMPW", 1);

        // save mid-move / mid-production
        for (unsigned int i = 0; i < 400; ++i)
        {
            sim.tick();
        }
        dumpUnitDiagnostics(sim, "PRE-SAVE t=400");

        GameParameters params(mapName, 0);
        params.players[0] = PlayerInfo{std::nullopt, PlayerControllerType(PlayerControllerTypeHuman()), "ARM", PlayerColorIndex(0), Metal(10000), Energy(10000)};
        params.players[1] = PlayerInfo{std::nullopt, PlayerControllerType(PlayerControllerTypeComputer()), "CORE", PlayerColorIndex(1), Metal(10000), Energy(10000)};

        auto bytes = encodeSavedGame(sim, params, nullptr, "animtest", 1700000000);
        auto decoded = decodeSavedGame(bytes);

        auto sim2 = buildSkirmishSim(mapName, vfs, pack, 2);
        applySavedGame(sim2, decoded, nullptr);

        dumpUnitDiagnostics(sim2, "POST-LOAD t=400");
        for (unsigned int i = 0; i < 30; ++i) { sim2.tick(); }
        dumpUnitDiagnostics(sim2, "POST-LOAD +30t");
        for (unsigned int i = 0; i < 270; ++i) { sim2.tick(); }
        dumpUnitDiagnostics(sim2, "POST-LOAD +300t");

        // case C: idle unit ordered to move after load -> StartMoving?
        sim2.getUnitState(idle).orders.push_back(UnitOrder(MoveOrder(px(0.60, 0.10))));
        // case B: previously-closed factory ordered to produce after load
        sim2.getUnitState(vp).buildQueue.emplace_back("ARMCV", 1);
        for (unsigned int i = 0; i < 600; ++i) { sim2.tick(); }
        dumpUnitDiagnostics(sim2, "POST-LOAD +900t (+orders)");

        // baseline control: same sim, never saved, same extra orders
        sim.getUnitState(idle).orders.push_back(UnitOrder(MoveOrder(px(0.60, 0.10))));
        sim.getUnitState(vp).buildQueue.emplace_back("ARMCV", 1);
        for (unsigned int i = 0; i < 900; ++i) { sim.tick(); }
        dumpUnitDiagnostics(sim, "NO-SAVE +900t (+orders)");
        return 0;
    }

    if (scenario == "move" || scenario == "movesave")
    {
        // Mass-movement benchmark: issue a simultaneous MOVE order to
        // N clustered units and record per-tick cost, command-to-motion
        // latency and pathfinding-queue behaviour.
        //
        //   move <count> <mapName> [ticks] [--type U|vehicle|mixed]
        //        [--dest short|long] [--repeat K]
        //   movesave <file.sav> [ticks]     (orders all human mobiles)
        if (scenario == "move" && args.size() < 3)
        {
            die("missing count/map");
        }
        if (scenario == "movesave" && args.size() < 2)
        {
            die("missing save file");
        }
        auto pack = loadBenchDefinitionPack(vfs);

        unsigned int count = 0;
        std::string mapName;
        unsigned int ticks;
        std::string unitType = "CORCK";
        std::string destMode = "long";
        unsigned int repeats = 1;
        std::vector<std::string> rest;
        if (scenario == "move")
        {
            count = std::stoul(args[1]);
            mapName = args[2];
            ticks = args.size() > 3 ? std::stoul(args[3]) : 1500;
            for (std::size_t i = 4; i < args.size(); ++i)
            {
                rest.push_back(args[i]);
            }
        }
        else
        {
            ticks = args.size() > 2 ? std::stoul(args[2]) : 1500;
            for (std::size_t i = 3; i < args.size(); ++i)
            {
                rest.push_back(args[i]);
            }
        }
        for (std::size_t i = 0; i + 1 < rest.size(); i += 2)
        {
            if (rest[i] == "--type") { unitType = rest[i + 1]; }
            else if (rest[i] == "--dest") { destMode = rest[i + 1]; }
            else if (rest[i] == "--repeat") { repeats = std::stoul(rest[i + 1]); }
            else { die("unknown flag " + rest[i]); }
        }

        GameSimulation sim = scenario == "move"
            ? buildSkirmishSim(mapName, vfs, pack, 2)
            : loadSaveIntoSim(args[1], vfs, pack);
        sim.collectTickPhaseTimings = true;

        const char* movePhaseTiming=std::getenv("RWE_BENCH_PHASE_TIMING");
        sim.collectTickPhaseTimings=!movePhaseTiming || std::string(movePhaseTiming)!="0";

        PlayerId owner(0);

        // spawn cluster near player 0's commander (guaranteed ground)
        std::vector<UnitId> group;
        if (scenario == "move")
        {
            SimVector anchor(0_ss, 0_ss, 0_ss);
            bool found = false;
            for (auto& [id, u] : sim.units)
            {
                if (u.owner == owner)
                {
                    anchor = u.position;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                die("no player-0 unit to anchor on");
            }
            auto cell = sim.terrain.worldToHeightmapCoordinateNearest(anchor);
            unsigned int spawned = 0;
            int cols = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(count))));
            for (unsigned int i = 0; spawned < count && i < count * 4; ++i)
            {
                int dx = 8 + static_cast<int>(i % cols) * 3;
                int dz = -4 + static_cast<int>(i / cols) * 3;
                auto pos = sim.terrain.heightmapIndexToWorldCenter(cell.x + dx, cell.y + dz);
                std::string t = unitType == "mixed" ? (i % 2 ? "CORCK" : "ARMFLASH")
                    : unitType == "vehicle" ? "ARMFLASH"
                    : unitType;
                // skip candidates whose footprint pokes off the
                // occupied grid -- an off-grid unit trips the
                // moveUnitOccupiedArea assert as soon as it moves
                auto rect = sim.computeFootprintRegion(pos, sim.unitDefinitions.at(t).movementCollisionInfo);
                if (!sim.occupiedGrid.tryToRegion(rect))
                {
                    continue;
                }
                if (auto id = sim.trySpawnUnit(t, owner, pos, std::nullopt, true, std::nullopt, true))
                {
                    auto& u = sim.getUnitState(*id);
                    u.finishBuilding(sim.unitDefinitions.at(toUpper(u.unitType)));
                    group.push_back(*id);
                    ++spawned;
                }
            }
            std::cerr << "move spawned=" << spawned << "/" << count << std::endl;
        }
        else
        {
            for (auto& [id, u] : sim.units)
            {
                if (u.owner != owner)
                {
                    continue;
                }
                const auto& def = sim.unitDefinitions.at(u.unitType);
                if (def.canMove && std::holds_alternative<UnitDefinition::NamedMovementClass>(def.movementCollisionInfo))
                {
                    group.push_back(id);
                }
            }
            std::cerr << "movesave group=" << group.size() << " human mobiles" << std::endl;
        }

        // destination: "long" = opposite player's start area, "short"
        // = a modest offset from the anchor
        SimVector destA;
        SimVector destB;
        {
            SimVector anchor(0_ss, 0_ss, 0_ss);
            bool first = true;
            SimVector other;
            bool foundOther = false;
            for (auto& [id, u] : sim.units)
            {
                if (u.owner != owner && u.unitType == "CORCOM")
                {
                    other = u.position;
                    foundOther = true;
                }
                if (first)
                {
                    anchor = u.position;
                    first = false;
                }
            }
            if (!foundOther)
            {
                auto w = sim.terrain.getWidthInWorldUnits();
                auto h = sim.terrain.getHeightInWorldUnits();
                other = SimVector(w - anchor.x, anchor.y, h - anchor.z);
            }
            destA = destMode == "short"
                ? anchor + SimVector((other.x - anchor.x) / SimScalar(4), SimScalar(0), (other.z - anchor.z) / SimScalar(4))
                : other;
            destB = anchor;
        }
        destA.y = sim.terrain.getHeightAt(destA.x, destA.z);
        destB.y = sim.terrain.getHeightAt(destB.x, destB.z);

        // settle
        for (unsigned int i = 0; i < 30; ++i)
        {
            sim.tick();
        }

        auto dest = destA;
        bool flip = false;
        double wallTotal = 0.0;
        for (unsigned int rep = 0; rep < repeats; ++rep)
        {
            std::unordered_map<UnitId, SimVector> issuePos;
            for (auto id : group)
            {
                if (auto u = sim.tryGetUnitState(id))
                {
                    issuePos[id] = u->get().position;
                    u->get().orders.clear();
                    u->get().orders.push_back(UnitOrder(MoveOrder(dest)));
                }
            }
            dest = flip ? destA : destB;
            flip = !flip;

            std::vector<double> tickNs;
            tickNs.reserve(ticks);
            std::vector<double> hashUs;
            const bool hashTicks=std::getenv("RWE_BENCH_HASH_TICKS") != nullptr;
            if(hashTicks) { hashUs.reserve(ticks); }
            std::uint64_t hashChecksum=0;
            const auto rssBefore=residentBytes();
            unsigned int firstMoved = 0, halfMoved = 0, allMoved = 0;
            unsigned int halfTarget = (group.size() + 1) / 2;
            unsigned int waited30 = 0;
            std::unordered_map<UnitId, unsigned int> waitTicks;
            for (unsigned int i = 0; i < ticks; ++i)
            {
                auto t0 = std::chrono::steady_clock::now();
                sim.tick();
                auto t1 = std::chrono::steady_clock::now();
                tickNs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
                if(hashTicks) {
                    const auto start=std::chrono::steady_clock::now(); const auto hash=sim.computeHash();
                    hashUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
                    hashChecksum+=hash.value;
                }

                unsigned int moved = 0;
                for (auto id : group)
                {
                    auto u = sim.tryGetUnitState(id);
                    if (!u)
                    {
                        continue;
                    }
                    const auto& unit = u->get();
                    auto dx = unit.position.x - issuePos[id].x;
                    auto dz = unit.position.z - issuePos[id].z;
                    if (dx * dx + dz * dz > SimScalar(64))
                    {
                        ++moved;
                    }
                    if (auto ms = std::get_if<NavigationStateMoving>(&unit.navigationState.state);
                        ms && ms->pathRequested)
                    {
                        if (++waitTicks[id] == 30)
                        {
                            ++waited30;
                        }
                    }
                }
                if (!firstMoved && moved >= 1) { firstMoved = i + 1; }
                if (!halfMoved && moved >= halfTarget) { halfMoved = i + 1; }
                if (!allMoved && moved >= group.size()) { allMoved = i + 1; }
            }

            unsigned int arrived = 0;
            for (auto id : group)
            {
                if (auto u = sim.tryGetUnitState(id))
                {
                    const auto& unit = u->get();
                    if (unit.orders.empty()
                        && !std::holds_alternative<NavigationStateMoving>(unit.navigationState.state))
                    {
                        ++arrived;
                    }
                }
            }

            std::sort(tickNs.begin(), tickNs.end());
            auto pct = [&](double p) {
                return tickNs.empty() ? 0.0 : tickNs[static_cast<std::size_t>(p * (tickNs.size() - 1))] / 1e6;
            };
            const auto& st = sim.pathFindingService.stats;
            const double wallNs = std::accumulate(tickNs.begin(), tickNs.end(), 0.0);
            std::cout << "scenario=" << scenario << " rep=" << rep
                      << " group=" << group.size()
                      << " ticks=" << ticks
                      << " p50=" << pct(0.50) << " p95=" << pct(0.95)
                      << " p99=" << pct(0.99) << " max=" << (tickNs.empty() ? 0.0 : tickNs.back() / 1e6)
                      << " firstMoveTick=" << firstMoved
                      << " halfMoveTick=" << halfMoved
                      << " allMoveTick=" << allMoved
                      << " waitedOver30t=" << waited30
                      << " arrived=" << arrived << "/" << group.size()
                      << " maxQDepth=" << st.maxQueueDepth
                      << " reqProcessed=" << st.requestsProcessed
                      << " expansions=" << st.totalExpansions
                      << " maxExpansionsPerReq=" << st.maxExpansionsPerRequest
                      << std::endl;
            wallTotal += wallNs;
            std::cout << "tick_mean_ms=" << wallNs/ticks/1e6 << " rss_before=" << rssBefore << " rss_after=" << residentBytes() << '\n';
            reportHashTicks(hashUs,wallNs/ticks/1e6,hashChecksum);
        }
        report(scenario == "move"
                ? "move:" + std::to_string(count) + ":" + unitType + ":" + destMode + "@" + mapName
                : "movesave:" + args[1],
            sim, RunResult{ticks * repeats, wallTotal / 1e9, {}, 0, 0});
        return 0;
    }

    const bool veteranScenario = scenario.rfind("veteran-", 0) == 0;
    const bool featureScenario = scenario.rfind("feature-", 0) == 0;
    const bool serviceScenario = scenario.rfind("service-", 0) == 0;
    if (veteranScenario || featureScenario || serviceScenario || scenario == "combat" || scenario == "units" || scenario == "emp" || scenario == "kamikaze" || scenario == "kamikaze-burst" || scenario == "bomber" || scenario == "bomber-combat")
    {
        if (args.size() < 3)
        {
            die("missing count/map");
        }
        unsigned int count = std::stoul(args[1]);
        const std::string& mapName = args[2];
        unsigned int ticks = args.size() > 3 ? std::stoul(args[3]) : 3000;
        auto pack = loadBenchDefinitionPack(vfs);
        auto sim = buildSkirmishSim(mapName, vfs, pack, 2);

        auto w = sim.terrain.getWidthInWorldUnits();
        auto h = sim.terrain.getHeightInWorldUnits();
        std::vector<SimVector> featureCenters;
        if (featureScenario)
        {
            // Pure environmental cases: no commanders, aircraft or AI production
            // may quietly change the requested feature-only workload.
            sim.campaignAi.clear();
            for (const auto& [id, unit] : sim.units) { sim.quietlyKillUnit(id); }
            sim.deleteDeadUnits();
            std::vector<FeatureId> old;
            for (const auto& [id, feature] : sim.features) { old.push_back(id); }
            for (auto id : old) { sim.removeFeature(id); }
            if (scenario != "feature-empty")
            {
                if (!sim.featureNameIndex.count("TREE1"))
                {
                    loadFeatureDefinitions(pack.featureTdfs, sim.featureDefinitions, sim.featureNameIndex, "TREE1");
                }
                const auto type = sim.featureNameIndex.at("TREE1");
                for (int z = 8; z < sim.occupiedGrid.getHeight() - 8 && featureCenters.size() < count; z += 2)
                {
                    for (int x = 8; x < sim.occupiedGrid.getWidth() - 8 && featureCenters.size() < count; x += 2)
                    {
                        if (auto id = sim.addFeature(type, x, z))
                        {
                            featureCenters.push_back(sim.getFeature(*id).position);
                            if (scenario == "feature-fire" && featureCenters.size() % 32 == 1) { sim.igniteFeature(*id); }
                        }
                    }
                }
            }
            sim.events.clear();
        }
        else if (serviceScenario)
        {
            std::vector<UnitId> pads, aircraft;
            const bool serviceAi = scenario == "service-ai";
            const auto serviceOwner = PlayerId(serviceAi ? 1 : 0);
            const std::string serviceAircraft = serviceAi ? "CORHURC" : "ARMTHUND";
            if (scenario != "service-air")
            {
                const auto padCount = scenario == "service-contention" ? 2u : (scenario == "service-idle" || scenario == "service-active" ? count : std::max(1u, count / 4));
                scatterSpawn(sim, serviceAi ? "CORASP" : "ARMASP", serviceOwner, padCount, w / 8_ss, h / 8_ss, w * 3_ss / 8_ss, h * 7_ss / 8_ss, true);
                for (auto id : sim.airServiceFacilities) { pads.push_back(id); }
            }
            if (scenario != "service-idle")
            {
                scatterSpawn(sim, serviceAircraft, serviceOwner, count, w / 8_ss, h / 8_ss, w * 3_ss / 8_ss, h * 7_ss / 8_ss, true);
                for (auto& [id, unit] : sim.units)
                {
                    if (unit.unitType != serviceAircraft) { continue; }
                    aircraft.push_back(id);
                    unit.hitPoints = sim.unitDefinitions.at(unit.unitType).maxHitPoints / 2;
                    if (!pads.empty() && !serviceAi) { unit.addOrder(LandOrder(pads[(aircraft.size() - 1) % pads.size()])); }
                    else { unit.addOrder(PatrolOrder({unit.position, unit.position + SimVector(600_ss, 0_ss, 0_ss)})); }
                }
            }
            if (scenario == "service-dense")
            {
                scatterSpawn(sim, "CORSTORM", PlayerId(1), count, w / 2_ss, h / 4_ss, w * 7_ss / 8_ss, h * 3_ss / 4_ss, true);
                scatterSpawn(sim, "ARMROCK", PlayerId(0), count, w * 3_ss / 8_ss, h / 4_ss, w / 2_ss, h * 3_ss / 4_ss, true);
            }
            if (scenario == "service-active")
            {
                unsigned int setup = 0;
                while (sim.airServiceOccupants.size() < aircraft.size() && setup++ < 6000) { sim.tick(); }
                for (auto id : aircraft)
                {
                    auto& unit = sim.getUnitState(id);
                    if (unit.aircraftService && unit.aircraftService->docked())
                    { unit.hitPoints = 1; unit.aircraftService->phase = AircraftServiceState::Phase::Repair; }
                }
                std::cerr << "service_setup_ticks=" << setup << " docked=" << sim.airServiceOccupants.size() << '\n';
            }
        }
        else if (scenario == "combat" || scenario == "veteran-combat" || scenario == "veteran-aoe" || scenario == "veteran-ai")
        {
            // two missile-army blocks interleaved in the map center
            // so both sides sit inside weapon range -- a sustained
            // projectile storm rather than an approach march
            unsigned int a = scatterSpawn(sim, scenario == "veteran-aoe" ? "CORHRK" : "CORSTORM", PlayerId(0), count,
                w * SimScalar(3) / SimScalar(8), h * SimScalar(3) / SimScalar(8),
                w * SimScalar(5) / SimScalar(8), h * SimScalar(1) / SimScalar(2), true, veteranScenario);
            unsigned int b = scatterSpawn(sim, scenario == "veteran-aoe" ? "ARMMART" : "ARMROCK", PlayerId(1), count,
                w * SimScalar(3) / SimScalar(8), h * SimScalar(1) / SimScalar(2),
                w * SimScalar(5) / SimScalar(8), h * SimScalar(5) / SimScalar(8), true, veteranScenario);
            auto centerA = SimVector(w / SimScalar(2), SimScalar(0), h * SimScalar(3) / SimScalar(8));
            auto centerB = SimVector(w / SimScalar(2), SimScalar(0), h * SimScalar(5) / SimScalar(8));
            if (veteranScenario)
            {
                centerA = sim.terrain.topLeftCoordinateToWorld(centerA);
                centerB = sim.terrain.topLeftCoordinateToWorld(centerB);
            }
            for (auto& [id, u] : sim.units)
            {
                auto t = toUpper(u.unitType);
                if (t == "CORSTORM" || t == "CORAK" || t == "CORHRK")
                {
                    u.addOrder(AttackOrder(centerB));
                }
                else if (t == "ARMROCK" || t == "ARMMART")
                {
                    u.addOrder(AttackOrder(centerA));
                }
            }
            std::cerr << "combat spawned arm=" << a << " core=" << b << " orders-issued" << std::endl;
        }
        else if (scenario == "bomber" || scenario == "bomber-combat")
        {
            auto n = scatterSpawn(sim, "ARMTHUND", PlayerId(0), count,
                w / 8_ss, h / 8_ss, w * 3_ss / 8_ss, h * 7_ss / 8_ss, true);
            if (scenario == "bomber-combat")
            {
                scatterSpawn(sim, "CORSTORM", PlayerId(1), count,
                    w / 2_ss, h / 4_ss, w * 7_ss / 8_ss, h * 3_ss / 4_ss, true);
                scatterSpawn(sim, "ARMROCK", PlayerId(0), count,
                    w * 3_ss / 8_ss, h / 4_ss, w / 2_ss, h * 3_ss / 4_ss, true);
            }
            auto target = SimVector(w * 3_ss / 4_ss, 0_ss, h / 2_ss);
            target.y = sim.terrain.getHeightAt(target.x, target.z);
            for (auto& [id, unit] : sim.units)
            {
                if (unit.unitType == "ARMTHUND") { unit.addOrder(AttackOrder(target)); }
            }
            std::cerr << "bombers spawned=" << n << std::endl;
        }
        else if (scenario == "kamikaze" || scenario == "kamikaze-burst")
        {
            auto n = scatterSpawn(sim, "CORROACH", PlayerId(0), count,
                w / 16_ss, h / 16_ss, w * 15_ss / 16_ss, h * 15_ss / 16_ss, true);
            auto targets = scatterSpawn(sim, "ARMFLEA", PlayerId(1), count,
                w / 16_ss, h / 16_ss, w * 15_ss / 16_ss, h * 15_ss / 16_ss, true);
            for (auto& [id, unit] : sim.units)
            {
                unit.fireOrders = UnitFireOrders::HoldFire;
                if (unit.unitType == "CORROACH")
                {
                    unit.fireOrders = UnitFireOrders::FireAtWill;
                    if (scenario == "kamikaze-burst")
                    {
                        unit.selfDestruct = SelfDestructOrder{0};
                        unit.selfDestruct->fuseTicks = 0;
                    }
                }
            }
            std::cerr << "kamikaze spawned=" << n << " targets=" << targets << std::endl;
        }
        else
        {
            unsigned int n = scatterSpawn(sim, "ARMFLEA", PlayerId(0), count,
                w / SimScalar(16), h / SimScalar(16), w * SimScalar(15) / SimScalar(16), h * SimScalar(15) / SimScalar(16), true, veteranScenario);
            std::cerr << "units spawned=" << n << std::endl;
        }
        if (veteranScenario)
        {
            const bool idleComparison = scenario == "veteran-none" || scenario == "veteran-small" || scenario == "veteran-large";
            if (idleComparison) { sim.campaignAi.clear(); }
            unsigned int experienced = 0;
            for (auto& [id, unit] : sim.units)
            {
                if (idleComparison) { unit.fireOrders = UnitFireOrders::HoldFire; }
                if (scenario != "veteran-none" && (scenario != "veteran-small" || experienced < 8))
                {
                    unit.kills = scenario == "veteran-combat" || scenario == "veteran-aoe" ? 4 : 25;
                    ++experienced;
                }
            }
            std::cout << "initial_experienced=" << experienced << " unit_state_bytes=" << sizeof(UnitState) << '\n';
            if (const char* path = std::getenv("RWE_BENCH_ZERO_VETERAN_SAVE"))
            {
                if (scenario != "veteran-none") { die("baseline fixture requires veteran-none"); }
                // A fresh, on-map v13 fixture lets the retained pre-veterancy
                // binary benchmark exactly the same idle population and hash.
                // Never overwrite an existing save, including a diagnostic one.
                for (auto& player : sim.players) { player.type = GamePlayerType::Human; }
                GameParameters params(mapName, 0);
                auto bytes = encodeSavedGame(sim, params, nullptr, "zero-veteran-benchmark", 1700000000, 13);
                auto* file = std::fopen(path, "wbx");
                if (!file) { die("cannot exclusively create baseline fixture"); }
                const auto written = std::fwrite(bytes.data(), 1, bytes.size(), file);
                const auto closed = std::fclose(file);
                if (written != bytes.size() || closed != 0) { die("baseline fixture write failed"); }
                std::cout << "baseline_fixture_version=13 units=" << liveCount(sim.units) << '\n';
                return 0;
            }
        }
        if (scenario == "emp")
        {
            for (const auto& [id, unit] : sim.units)
            {
                sim.applyDamage(id, 1800, std::nullopt, false, true);
            }
        }
        const bool bombScenario = scenario == "kamikaze" || scenario == "kamikaze-burst";
        double firstTickMs = 0;
        auto start = std::chrono::steady_clock::now();
        auto r = bombScenario ? runTicks(sim, ticks, 3, [&](unsigned int i) {
            if (i == 0) { firstTickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
        }) : featureScenario ? runTicks(sim, ticks, 3, [&](unsigned int tick) {
            // Consume presentation events like GameScene; otherwise an unattended
            // headless event vector would measure a harness leak, not feature state.
            sim.events.clear();
            if (!featureCenters.empty() && tick % 30 == 0
                && (scenario == "feature-explosions" || scenario == "feature-large"))
            {
                const auto& weapon = sim.weaponDefinitions.at("NUCLEAR_MISSILE");
                const unsigned int shots = scenario == "feature-large" ? 16u : 1u;
                for (unsigned int shot = 0; shot < shots; ++shot)
                {
                    Projectile p{}; p.weaponType = "NUCLEAR_MISSILE"; p.owner = PlayerId(0);
                    p.position = featureCenters[(tick / 30 * 17 + shot * 64) % featureCenters.size()];
                    p.position.y -= 1_ss; p.previousPosition = p.origin = p.position;
                    p.damage = weapon.damage; p.damageRadius = weapon.damageRadius;
                    p.dieOnFrame = sim.gameTime + GameTime(100); sim.projectiles.emplace(std::move(p));
                }
            }
        }) : veteranScenario ? runTicks(sim, ticks, 3, [&](unsigned int) {
            sim.events.clear(); // Match the app's presentation event consumer in long runs.
        }) : runTicks(sim, ticks);
        report(scenario + ":" + std::to_string(count) + "@" + mapName, sim, r);
        if (veteranScenario || scenario == "units" || scenario == "combat")
        {
            unsigned int veterans = 0, experienced = 0;
            uint64_t kills = 0;
            for (const auto& [id, unit] : sim.units)
            {
                veterans += unit.kills >= 5;
                experienced += unit.kills != 0;
                kills += unit.kills;
            }
            std::cout << "final_veterans=" << veterans << " final_experienced=" << experienced << " final_kills=" << kills << '\n';
            uint64_t checksum = 0;
            auto hashStart = std::chrono::steady_clock::now();
            for (unsigned int i = 0; i < 1000; ++i) { checksum += sim.computeHash().value; }
            auto hashNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - hashStart).count() / 1000;
            const auto& pt = sim.tickPhaseTimings;
            std::cout << "hash_ns=" << hashNs << " hash_checksum=" << checksum
                      << " award_ns=" << (pt.veterancyAwards ? static_cast<double>(pt.veterancy) / pt.veterancyAwards : 0)
                      << " unit_state_bytes=" << sizeof(UnitState) << '\n';
        }
        if (bombScenario)
        {
            unsigned int blasts = 0;
            for (const auto& event : sim.events)
            {
                if (auto death = std::get_if<UnitDiedEvent>(&event); death && !death->explosionWeapon.empty()) { ++blasts; }
            }
            std::cout << "explosions=" << blasts << " first_tick_ms=" << firstTickMs << " hash=" << sim.computeHash().value << '\n';
        }
        {
            unsigned int drops = 0;
            for (const auto& event : sim.events)
            {
                if (auto fire = std::get_if<FireWeaponEvent>(&event);
                    fire && std::holds_alternative<ProjectilePhysicsTypeDropped>(sim.weaponDefinitions.at(fire->weaponType).physicsType)) { ++drops; }
            }
            unsigned int bombers = 0;
            for (const auto& [id, unit] : sim.units)
            {
                const auto& def = sim.unitDefinitions.at(unit.unitType);
                if (def.canFly && unit.weapons[0]
                    && std::holds_alternative<ProjectilePhysicsTypeDropped>(sim.weaponDefinitions.at(unit.weapons[0]->weaponType).physicsType)) { ++bombers; }
            }
            std::cout << "bomb_releases=" << drops << " bombers_remaining=" << bombers << " hash=" << sim.computeHash().value << '\n';
        }
        if (scenario == "emp")
        {
            unsigned int paralyzed = 0;
            for (const auto& [id, unit] : sim.units) { paralyzed += unit.isParalyzed(); }
            std::cout << "emp_paralyzed=" << paralyzed << " hash=" << sim.computeHash().value << '\n';
        }
        return 0;
    }

    die("unknown scenario " + scenario);
}
