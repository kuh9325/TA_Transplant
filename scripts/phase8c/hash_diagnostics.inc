// Offline only: appended to a copy of GameHash_util.cpp by build_observer.py.
// No timers, allocation hooks, or diagnostic branches enter the application.
#include <chrono>
#include <functional>
#include <iostream>
extern bool phase8cCountAllocations;
extern std::uint64_t phase8cAllocations, phase8cAllocatedBytes;
namespace rwe {
void phase8cHashDiagnostics(const GameSimulation& s, unsigned iterations)
{
    auto measure = [&](const char* name, std::size_t count, const std::function<GameHash()>& fn) {
        std::uint64_t sink = 0;
        phase8cAllocations = phase8cAllocatedBytes = 0;
        phase8cCountAllocations = true;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i) {
            asm volatile("" ::: "memory"); // prevent invariant-loop elimination
            sink += fn().value;
        }
        const auto end = std::chrono::steady_clock::now();
        phase8cCountAllocations = false;
        std::cout << "hash_category=" << name << " elements=" << count
            << " us=" << std::chrono::duration<double, std::micro>(end-start).count()/iterations
            << " allocations=" << double(phase8cAllocations)/iterations
            << " allocated_bytes=" << double(phase8cAllocatedBytes)/iterations
            << " checksum=" << sink << '\n';
    };
    std::size_t units=0, pieces=0, threads=0, orders=0, weapons=0, recovery=0, activePieces=0, moving=0, maxThreads=0;
    for (const auto& [id,u] : s.units) {
        ++units; pieces+=u.pieces.size(); orders+=u.orders.size()+u.backgroundOrders.size();
        if (u.cobEnvironment) { threads+=u.cobEnvironment->threads.size(); maxThreads=std::max(maxThreads,u.cobEnvironment->threads.size()); }
        for (const auto& w : u.weapons) weapons+=bool(w);
        for (const auto& p:u.pieces) activePieces+=bool(p.xMoveOperation||p.yMoveOperation||p.zMoveOperation||p.xTurnOperation||p.yTurnOperation||p.zTurnOperation);
        if (auto ground=std::get_if<UnitPhysicsInfoGround>(&u.physics)) moving+=ground->currentSpeed>0_ss;
    }
    std::cout << "hash_inventory units=" << units << " pieces=" << pieces << " active_pieces=" << activePieces << " threads=" << threads << " max_threads_per_unit=" << maxThreads << " moving_ground=" << moving << '\n';
    for (const auto& ai : s.campaignAi) recovery+=ai.deadlockedFactories.size();
    auto unitCategory = [&](const char* name, std::size_t count, auto fn) {
        measure(name,count,[&]{ std::uint32_t h=0; for (const auto& [id,u]:s.units) h+=fn(u).value; return GameHash(h); });
    };
    measure("whole",units,[&]{return computeHashOf(s);});
    measure("units_inclusive",units,[&]{return computeHashOf(s.units);});
    unitCategory("traversal",units,[](const auto& u){return GameHash(u.hitPoints);});
    unitCategory("pieces",pieces,[](const auto& u){return computeHashOf(u.pieces);});
    unitCategory("cob",threads,[](const auto& u){return u.cobEnvironment ? hashCob(*u.cobEnvironment):GameHash(0);});
    unitCategory("weapon_references",weapons,[](const auto& u){return u.cobEnvironment ? hashWeaponReferences(u):GameHash(0);});
    unitCategory("orders_including_emp",orders,[](const auto& u){return combineHashes(u.orders,u.backgroundOrders);});
    unitCategory("behavior_navigation",units,[](const auto& u){return combineHashes(u.physics,u.navigationState,u.behaviourState,u.factoryState,u.buildQueue,u.buildOrderUnitId);});
    unitCategory("weapons",weapons,[](const auto& u){return computeHashOf(u.weapons);});
    unitCategory("veterancy",units,[](const auto& u){return computeHashOf(static_cast<uint32_t>(u.kills));});
    unitCategory("kamikaze",units,[](const auto& u){return combineHashes(u.selfDestruct,u.nextKamikazeScan.has_value(),u.nextKamikazeScan);});
    unitCategory("bomber",units,[](const auto& u){return combineHashes(bool(u.bomberState),u.bomberState?computeHashOf(*u.bomberState):GameHash(0));});
    unitCategory("service_transport",units,[](const auto& u){return combineHashes(u.nextServiceScan.has_value(),u.nextServiceScan,bool(u.aircraftService),u.aircraftService?computeHashOf(*u.aircraftService):GameHash(0),u.carriedBy,u.attachSlot,u.passengers);});
    unitCategory("unit_other",units,[](const auto& u){return combineHashes(u.unitType,u.position,u.owner,u.rotation,u.hitPoints,u.lifeState,u.inBuildStance,u.yardOpen,u.inCollision,u.armored,u.busy,u.buggerOff,u.fireOrders,u.moveOrders,u.lastDamagedAt,u.lastDamagedBy,u.lastDamagedByPlayer,u.missionState,u.buildTimeCompleted,u.activated,u.isSufficientlyPowered,u.energyProductionBuffer,u.metalProductionBuffer,u.previousEnergyConsumptionBuffer,u.previousMetalConsumptionBuffer,u.energyConsumptionBuffer,u.metalConsumptionBuffer,u.cloakRequested,u.cloaked,u.cloakBlockedUntil);});
    measure("projectiles",std::distance(s.projectiles.begin(),s.projectiles.end()),[&]{return computeHashOf(s.projectiles);});
    measure("features",std::distance(s.features.begin(),s.features.end()),[&]{return computeHashOf(s.features);});
    measure("feature_names_active_rng",std::distance(s.features.begin(),s.features.end()),[&]{return hashFeatureIdentities(s);});
    measure("occupancy_resources",s.occupiedGrid.getVector().size(),[&]{return combineHashes(s.occupiedGrid.stateHash(),s.metalGrid.stateHash(),s.geoGrid.stateHash());});
    measure("ai_including_recovery",recovery,[&]{return computeHashOf(s.campaignAi);});
    measure("recovery_only",recovery,[&]{std::uint32_t h=0;for(const auto& ai:s.campaignAi)h+=computeHashOf(ai.deadlockedFactories).value;return GameHash(h);});
    measure("mission",s.missionInfo.has_value(),[&]{return combineHashes(s.missionInfo,s.missionObjectives,s.gameStatus);});
    measure("rng",1,[&]{auto rng=s.rng;return computeHashOf(static_cast<uint32_t>(rng()));});
    measure("environment_players_queues",s.players.size(),[&]{return combineHashes(s.gameTime,SimulationHashContractVersion,s.pathRequests,s.unitCreationRequests,s.currentWindGenerationFactor,s.noSeaLevelTrigger,s.waterDoesDamage,s.waterDamage,s.minWindSpeed,s.maxWindSpeed,s.tidalStrength,s.players,s.meteorShower,s.commanderDeathContinues,s.computerResourceScale,s.lineOfSightEnabled,s.maxUnitsPerPlayer,s.mapGravity,s.projectileWind,s.nextWindSpeedChange);});
}
}
