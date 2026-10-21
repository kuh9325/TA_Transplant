// Included by SavedGame.test.cpp, sharing its minimal deterministic world.
TEST_CASE("Phase8B hash covers future unit execution and ignores presentation", "[phase8b][hash]")
{
    PlayerId a{0}, b{0}; auto sim = makeWorld(a, b);
    auto id = sim.trySpawnUnit("ARMTANK", a, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
    REQUIRE(id); auto& u = sim.getUnitState(*id);
    u.pieces.emplace_back(); u.missionState = MissionUnitState{};
    u.cobEnvironment->_statics = {1, 2};
    for (int i = 0; i < 2; ++i)
    {
        auto thread = std::make_unique<CobThread>("probe");
        thread->stack.push(7); thread->callStack.push(CobFunction(0, {1, 2}));
        u.cobEnvironment->readyQueue.push_back(thread.get()); u.cobEnvironment->threads.push_back(std::move(thread));
    }
    u.weapons[0] = UnitWeapon{};
    UnitWeaponStateAttacking attack{UnitId(0)};
    attack.attackInfo = UnitWeaponStateAttacking::AimInfo{u.cobEnvironment->threads[0].get(), SimAngle(0), SimAngle(0)};
    u.weapons[0]->state = attack;
    const auto before = sim.computeHash();
    bool presentation = false;
    SECTION("factory queue") { u.buildQueue.emplace_back("ARMTANK", 1); }
    SECTION("factory creating") { u.factoryState = FactoryBehaviorStateCreatingUnit{"ARMTANK", a, u.position, SimAngle(0)}; }
    SECTION("factory reservation") { u.factoryState = FactoryBehaviorStateBuilding{std::make_pair(*id, std::optional<SimVector>())}; }
    SECTION("constructor reservation") { u.buildOrderUnitId = *id; }
    SECTION("build target") { u.behaviourState = UnitBehaviorStateBuilding{*id, std::nullopt}; }
    SECTION("reclaim beam affects watchdog") { u.behaviourState = UnitBehaviorStateReclaiming{*id, u.position, u.position}; }
    SECTION("air velocity") { u.physics = UnitPhysicsInfoAir{AirMovementStateFlying{std::nullopt, false, SimVector(1_ss, 0_ss, 0_ss)}}; }
    SECTION("takeoff intent") { std::get<UnitPhysicsInfoGround>(u.physics).steeringInfo.shouldTakeOff = true; }
    SECTION("landing abort") { u.physics = UnitPhysicsInfoAir{AirMovementStateLanding{false, true}}; }
    SECTION("path and cursor") { u.navigationState.state = NavigationStateMoving{u.position, u.position, PathFollowingInfo(UnitPath{{u.position, SimVector(1_ss, 2_ss, 3_ss)}, false}, GameTime(1)), false}; }
    SECTION("target position cache") { u.navigationState.unitPositionCache = UnitPositionCache{*id, u.position, GameTime(5)}; }
    SECTION("ballistic compensation") { u.weapons[0]->ballisticZOffset = 1_ss; }
    SECTION("aim thread identity") { std::get<UnitWeaponStateAttacking::AimInfo>(std::get<UnitWeaponStateAttacking>(u.weapons[0]->state).attackInfo).thread = u.cobEnvironment->threads[1].get(); }
    SECTION("burst progress") { std::get<UnitWeaponStateAttacking>(u.weapons[0]->state).attackInfo = UnitWeaponStateAttacking::FireInfo{SimAngle(0), SimAngle(0), u.position, 0, 1, GameTime(3)}; }
    SECTION("mission immunity") { u.missionState->immunity = true; }
    SECTION("AI ignore") { u.missionState->aiIgnore = true; }
    SECTION("corpse policy") { u.lifeState = UnitState::LifeStateDead{true}; }
    SECTION("COB static") { ++u.cobEnvironment->_statics[0]; }
    SECTION("COB operand") { ++u.cobEnvironment->threads[0]->stack.top(); }
    SECTION("COB instruction") { ++u.cobEnvironment->threads[0]->callStack.top().instructionIndex; }
    SECTION("COB local") { ++u.cobEnvironment->threads[0]->callStack.top().locals[0]; }
    SECTION("COB signal mask") { ++u.cobEnvironment->threads[0]->signalMask; }
    SECTION("COB queue order") { std::swap(u.cobEnvironment->readyQueue[0], u.cobEnvironment->readyQueue[1]); }
    SECTION("COB sleep") { auto* t = u.cobEnvironment->readyQueue.front(); u.cobEnvironment->readyQueue.pop_front(); u.cobEnvironment->sleepingQueue.emplace_back(CobTime(30), t); }
    SECTION("piece pose") { u.pieces[0].offset.x = 1_ss; }
    SECTION("piece move") { u.pieces[0].xMoveOperation = UnitMesh::MoveOperation(1_ss, 2_ss); }
    SECTION("presentation")
    {
        presentation = true; u.previousPosition = SimVector(10_ss, 20_ss, 30_ss); u.previousRotation = SimAngle(12);
        u.pieces[0].visible = false; u.pieces[0].shaded = false; u.pieces[0].previousOffset.x = 9_ss;
        u.cobEnvironment->threads[0]->name = "diagnostic-only";
        sim.pathFindingService.stats.requestsProcessed = 55; sim.tickPhaseTimings.cob = 999;
        sim.lineOfSightMode = 2; sim.mappingEnabled = true;
        MapFeature first{}, second{}; second.shadowFrame = 2; second.shadowDelay = 9;
        REQUIRE(computeHashOf(first) == computeHashOf(second));
    }
    REQUIRE((sim.computeHash() == before) == presentation);
}

TEST_CASE("Phase8B hash covers world queues objectives projectiles and grid ownership", "[phase8b][hash]")
{
    PlayerId a{0}, b{0}; auto sim = makeWorld(a, b);
    auto id = sim.trySpawnUnit("ARMTANK", a, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt); REQUIRE(id);
    sim.missionInfo = MissionInfo{}; sim.missionInfo->localPlayerId = a;
    sim.missionObjectives.emplace(std::vector<MissionObjective>{{"VictoryTimerRunsOut", {"100"}}}, a);
    Projectile p{}; p.damage = {{"DEFAULT", 1}, {"ARMTANK", 2}};
    auto shot = sim.projectiles.emplace(std::move(p));
    const auto before = sim.computeHash();
    SECTION("RNG") { sim.rng(); }
    SECTION("path queue") { sim.pathRequests.push_back(PathRequest{*id}); }
    SECTION("spawn queue") { sim.unitCreationRequests.push_back(*id); }
    SECTION("resource starting grant") { sim.players[0].startingMetal += Metal(1); }
    SECTION("wind income") { sim.currentWindGenerationFactor += 1_ss; }
    SECTION("water damage") { ++sim.waterDamage; }
    SECTION("sea projectile policy") { sim.noSeaLevelTrigger = true; }
    SECTION("whitelist") { sim.missionInfo->useOnlyUnits.insert("ARMTANK"); }
    SECTION("objective countdown") { sim.missionObjectives->restoreRuntimeState({}, {}, 2, MissionResult::InProgress); }
    SECTION("terminal outcome") { sim.gameStatus = WinStatusWon{a}; }
    SECTION("projectile bounce") { sim.projectiles.tryGet(shot)->get().groundBounce = true; }
    SECTION("projectile creation") { sim.projectiles.tryGet(shot)->get().createdAt = GameTime(1); }
    SECTION("damage keys") { auto& d = sim.projectiles.tryGet(shot)->get().damage; auto amount = d.at("ARMTANK"); d.erase("ARMTANK"); d["CORTANK"] = amount; }
    SECTION("mobile occupancy hole") { sim.occupiedGrid.mutate(GridCoordinates(8, 8), [](auto& cell) { cell.mobileUnitId = std::nullopt; }); }
    SECTION("building passability") { sim.occupiedGrid.mutate(GridCoordinates(4, 4), [&](auto& cell) { cell.buildingInfo = OccupiedCellBuildingInfo{*id, true}; }); }
    SECTION("feature occupancy") { sim.occupiedGrid.mutate(GridCoordinates(4, 4), [](auto& cell) { cell.featureId = FeatureId(0); }); }
    SECTION("metal grid") { sim.metalGrid.set(4, 4, 3); }
    SECTION("geothermal grid") { sim.geoGrid.set(4, 4, true); }
    REQUIRE(sim.computeHash() != before);
    REQUIRE(sim.occupiedGrid.digestIsValid()); REQUIRE(sim.metalGrid.digestIsValid()); REQUIRE(sim.geoGrid.digestIsValid());
}

TEST_CASE("Phase8B v16 recovery histories are canonical and legacy defaults are explicit", "[phase8b][save]")
{
    PlayerId a{0}, b{0}; auto sim = makeWorld(a, b);
    initCampaignAiPlayer(sim, b, CampaignAiProfile{});
    auto& d = sim.campaignAi.front().deadlockedFactories[UnitId(0)];
    d.blockedSince = GameTime(10); d.reclaimAttempts = 2;
    const auto legacy = decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "LEGACY", 0, 15));
    d.reclaimStalledAt = GameTime(8000); d.reclaimBestDistance = 48_ss; d.reclaimDeadline = GameTime(12000);
    d.reclaimAvoid = {512, 256}; d.evictAttempts = {{512, 2}, {256, 3}};
    auto bytes = encodeSavedGame(sim, makeParams(), nullptr, "RECOVERY", 0);
    auto decoded = decodeSavedGame(bytes); REQUIRE(decoded.version == 16);
    auto copy = makeWorld(a, b); applySavedGame(copy, decoded, nullptr);
    REQUIRE(copy.computeHash() == sim.computeHash());
    REQUIRE(encodeSavedGame(copy, makeParams(), nullptr, "RECOVERY", 0) == bytes);
    auto hash = sim.computeHash();
    d.reclaimAvoid.clear(); d.reclaimAvoid.insert(256); d.reclaimAvoid.insert(512);
    d.evictAttempts.clear(); d.evictAttempts.emplace(256, 3); d.evictAttempts.emplace(512, 2);
    REQUIRE(sim.computeHash() == hash);
    REQUIRE(encodeSavedGame(sim, makeParams(), nullptr, "RECOVERY", 0) == bytes);
    REQUIRE_THROWS_AS(encodeSavedGame(sim, makeParams(), nullptr, "LOSSY", 0, 15), SaveFormatException);
    REQUIRE_THROWS_AS(encodeSavedGame(sim, makeParams(), nullptr, "LOSSY", 0, 7), SaveFormatException);
    applySavedGame(copy, legacy, nullptr);
    const auto& old = copy.campaignAi.front().deadlockedFactories.at(UnitId(0));
    REQUIRE(old.reclaimAttempts == 2); REQUIRE(old.blockedSince == GameTime(10));
    REQUIRE_FALSE(old.reclaimStalledAt); REQUIRE_FALSE(old.reclaimBestDistance); REQUIRE_FALSE(old.reclaimDeadline);
    REQUIRE(old.reclaimAvoid.empty()); REQUIRE(old.evictAttempts.empty());
}

TEST_CASE("Phase8B v16 rejects malformed recovery history", "[phase8b][save]")
{
    PlayerId a{0}, b{0}; auto sim = makeWorld(a, b);
    initCampaignAiPlayer(sim, b, CampaignAiProfile{});
    auto& d = sim.campaignAi.front().deadlockedFactories[UnitId(0)];
    d.reclaimStalledAt = GameTime(0x12345678); d.reclaimBestDistance = 48_ss;
    d.reclaimDeadline = GameTime(0x12345679); d.reclaimAvoid = {256, 512};
    d.evictAttempts = {{256, 2}, {512, 3}};
    auto saved = decodeSavedGame(encodeSavedGame(sim, makeParams(), nullptr, "BAD_HISTORY", 0));
    SaveWriter pattern;
    pattern.boolean(true); pattern.u32(0x12345678); pattern.boolean(true); pattern.f32bits(48.0f);
    pattern.boolean(true); pattern.u32(0x12345679); pattern.u32(2); pattern.u32(256); pattern.u32(512);
    pattern.u32(2); pattern.u32(256); pattern.u32(2); pattern.u32(512); pattern.u32(3);
    const auto& bytes = pattern.getData();
    auto found = std::search(saved.simulationBytes.begin(), saved.simulationBytes.end(), bytes.begin(), bytes.end());
    REQUIRE(found != saved.simulationBytes.end());
    const auto at = std::distance(saved.simulationBytes.begin(), found);
    auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned int i = 0; i < 4; ++i) { saved.simulationBytes[at + offset + i] = static_cast<char>(value >> (i * 8)); }
    };
    SECTION("NaN distance") { put(6, 0x7fc00000); }
    SECTION("negative distance") { put(6, 0xbf800000); }
    SECTION("unbounded avoid count") { put(15, 0xffffffff); }
    SECTION("duplicate avoid id") { put(23, 256); }
    SECTION("unbounded eviction count") { put(27, 0xffffffff); }
    SECTION("unordered eviction id") { put(39, 128); }
    auto copy = makeWorld(a, b);
    REQUIRE_THROWS_AS(applySavedGame(copy, saved, nullptr), SaveFormatException);
}

TEST_CASE("Phase8B incremental grid hash remains equal under equivalent mutation histories", "[phase8b][hash]")
{
    OccupiedGrid a(32, 32), b(32, 32);
    std::minstd_rand rng(17);
    for (unsigned int n = 0; n < 2000; ++n)
    {
        auto c = GridCoordinates(rng() % 32, rng() % 32);
        OccupiedCell value; value.mobileUnitId = UnitId(rng() % 256);
        if (n % 3 == 0) { value.buildingInfo = OccupiedCellBuildingInfo{UnitId(n), n % 2 == 0}; }
        if (n % 5 == 0) { value.featureId = FeatureId(n); }
        a.set(c, value); b.mutate(c, [&](auto& v) { v = OccupiedCell{}; v = value; });
        REQUIRE(a.stateHash() == b.stateHash());
        if (n % 100 == 0) { REQUIRE(a.digestIsValid()); REQUIRE(b.digestIsValid()); }
    }
    const auto before = a.stateHash();
    CHECK_THROWS(a.mutate(GridCoordinates(1, 1), [](auto& v) { v.mobileUnitId = UnitId(999); throw std::runtime_error("rollback"); }));
    REQUIRE(a.stateHash() == before); REQUIRE(a.digestIsValid());
    auto moved = std::move(a); REQUIRE(moved.digestIsValid()); REQUIRE(moved.stateHash() == b.stateHash());
    StateHashGrid<bool> bits(2, 2); bits.set(1, 1, true);
    REQUIRE(bits.tryGetValue(Point(1, 1)) == true); REQUIRE_FALSE(bits.tryGetValue(Point(-1, 0)));
    REQUIRE(bits.any([](bool value) { return value; }));
    REQUIRE(bits.accumulate(bits.getRegion(), 0, [](int total, bool value) { return total + value; }) == 1);
    unsigned int count = 0; bits.forEachIndexed([&](auto, bool value) { count += value; }); REQUIRE(count == 1);
}
