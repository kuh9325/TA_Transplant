// Included by SkirmishClosure.test.cpp to share its installed-definition loader.
namespace
{
    GameSimulation recoveryWorld(const SkirmishDefinitionPack& pack)
    {
        GameSimulation s(MapTerrain(Grid<unsigned char>(129, 129, 64), 0_ss), 255, 0, 0);
        s.unitDefinitions = pack.unitDefinitions;
        s.weaponDefinitions = pack.weaponDefinitions;
        s.movementClassDatabase = pack.movementClassDatabase;
        s.unitModelDefinitions = pack.unitModelDefinitions;
        s.unitScriptDefinitions = pack.unitScriptDefinitions;
        s.movementClassCollisionService = createMovementClassCollisionService(s.terrain, s.movementClassDatabase);
        for (const auto& [name, d] : pack.unitDefinitions)
        {
            if (!d.corpse.empty() && pack.featureTdfs.count(toUpper(d.corpse)))
            { loadFeatureDefinitions(pack.featureTdfs, s.featureDefinitions, s.featureNameIndex, toUpper(d.corpse)); }
        }
        for (unsigned int i = 0; i < 2; ++i)
        {
            s.addPlayer(GamePlayerInfo{std::nullopt, i ? GamePlayerType::Computer : GamePlayerType::Human,
                PlayerColorIndex(i), GamePlayerStatus::Alive, i ? "CORE" : "ARM", std::nullopt,
                Metal(100000), Energy(100000), Metal(100000), Energy(100000), Metal(100000), Energy(100000)});
        }
        s.commanderDeathContinues = true;
        return s;
    }

    GameParameters recoveryParams()
    {
        GameParameters p("RECOVERY_FIXTURE", 0);
        for (unsigned int i = 0; i < 2; ++i)
        {
            p.players[i] = PlayerInfo{std::nullopt,
                i ? PlayerControllerType(PlayerControllerTypeComputer()) : PlayerControllerType(PlayerControllerTypeHuman()),
                i ? "CORE" : "ARM", PlayerColorIndex(i), Metal(100000), Energy(100000)};
        }
        return p;
    }

    std::string recoveryHistory(const GameSimulation& s)
    {
        std::ostringstream out;
        for (const auto& ai : s.campaignAi)
        {
            out << ai.player.value << ':' << ai.nextDeadlockScan.value << ';';
            for (const auto& [id, d] : ai.deadlockedFactories)
            {
                auto opt = [&](const auto& v) { out << (v ? std::to_string(v->value) : "-") << ','; };
                out << id.value << ':';
                opt(d.blockedSince); opt(d.reclaimTarget); opt(d.reclaimBuilder); opt(d.reclaimIssuedAt);
                out << d.reclaimAttempts << ',';
                opt(d.reclaimStalledAt); opt(d.reclaimBestDistance); opt(d.reclaimDeadline);
                for (auto x : std::set<unsigned int>(d.reclaimAvoid.begin(), d.reclaimAvoid.end())) { out << 'a' << x; }
                for (auto [x, n] : std::map<unsigned int, unsigned int>(d.evictAttempts.begin(), d.evictAttempts.end())) { out << 'e' << x << ':' << n; }
                out << ';';
            }
        }
        return out.str();
    }

    struct RecoveryFixtureIds { UnitId factory; UnitId blocker; UnitId builder; };

    RecoveryFixtureIds installRecoveryFixture(GameSimulation& s, int x, int z, bool mobile)
    {
        auto spawn = [&](const std::string& name, int cx, int cz) {
            auto pos = s.terrain.heightmapIndexToWorldCenter(cx, cz);
            pos.y = s.terrain.getHeightAt(pos.x, pos.z);
            auto id = s.trySpawnUnit(name, PlayerId(1), pos, std::nullopt, true);
            REQUIRE(id);
            s.getUnitState(*id).finishBuilding(s.unitDefinitions.at(name));
            return *id;
        };
        auto factory = spawn("CORLAB", x, z);
        auto rect = s.computeFootprintRegion(s.getUnitState(factory).position, s.unitDefinitions.at("CORLAB").movementCollisionInfo);
        if (mobile) { REQUIRE(s.trySetYardOpen(factory, true)); }
        auto blocker = mobile ? spawn("CORAK", rect.x + rect.width / 2, rect.y + rect.height - 1)
                              : spawn("CORSOLAR", rect.x + 1, rect.y + 1);
        auto builder = spawn("CORCV", x + 36, z + 5);
        s.getUnitState(builder).addOrder(WaitOrder{200000});
        s.getUnitState(factory).buildQueue.emplace_back("CORAK", 5);
        if (s.campaignAi.empty())
        {
            initCampaignAiPlayer(s, PlayerId(1), CampaignAiProfile{});
            auto& ai = s.campaignAi.front();
            // Isolate the real deadlock task; ordinary builder/attack tasks would
            // introduce unrelated orders. Factory production and all sim phases run.
            ai.nextRepartition = ai.nextFactoryScan = ai.nextBuilderScan = ai.nextAttackScan = GameTime(1000000);
        }
        auto& ai = s.campaignAi.front();
        ai.groups[AiGroupFactory].push_back(factory);
        ai.groups[AiGroupBuilder].push_back(builder);
        return {factory, blocker, builder};
    }
}

TEST_CASE("AI recovery native save continuation matrix", "[phase8b][recovery-save]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    if (!data || !std::filesystem::exists(data)) { SKIP("requires installed retail definitions"); }
    setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_phase8b_recovery.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto pack = loadSkirmishDefinitionPack(vfs);
    const auto params = recoveryParams();
    for (unsigned int scenario = 0; scenario < 4; ++scenario)
    {
        CAPTURE(scenario);
        auto s = recoveryWorld(pack);
        auto ids = installRecoveryFixture(s, scenario == 3 ? 4 : 32, 32, scenario == 1);
        if (scenario == 2) { installRecoveryFixture(s, 32, 90, false); }
        auto restored = recoveryWorld(pack);
        bool haveCopy = false;
        unsigned int recoverySeen = 0, progressSeen = 0, evictionSeen = 0, concurrent = 0;
        bool savedBeam = false;
        for (unsigned int t = 0; t <= 18000; ++t)
        {
            bool beaming = false;
            for (const auto& [id, u] : s.units)
            { if (const auto* r = std::get_if<UnitBehaviorStateReclaiming>(&u.behaviourState)) { beaming |= r->nanoParticleOrigin.has_value(); } }
            if ((beaming && !savedBeam) || t == 1 || t == 901 || t == 1801 || t == 2101 || t == 2401 || t == 7000 || t == 12000)
            {
                INFO("save tick " << t << " history " << recoveryHistory(s));
                applySavedGame(restored, decodeSavedGame(encodeSavedGame(s, params, nullptr, "RECOVERY", 0)), nullptr);
                haveCopy = true; savedBeam |= beaming;
                REQUIRE(recoveryHistory(restored) == recoveryHistory(s));
            }
            if (haveCopy)
            {
                INFO("continuation tick " << t);
                REQUIRE(restored.computeHash() == s.computeHash());
                REQUIRE(recoveryHistory(restored) == recoveryHistory(s));
                if (t % 300 == 0)
                {
                    // Byte comparison independently covers positions, orders,
                    // production, resources, projectiles, paths and COB execution.
                    REQUIRE(encodeSavedGame(restored, params, nullptr, "STATE", 0)
                        == encodeSavedGame(s, params, nullptr, "STATE", 0));
                    const auto& a = s.occupiedGrid.getVector();
                    const auto& b = restored.occupiedGrid.getVector();
                    bool same = a.size() == b.size();
                    for (std::size_t i = 0; same && i < a.size(); ++i)
                    {
                        same = a[i].mobileUnitId == b[i].mobileUnitId && a[i].featureId == b[i].featureId
                            && a[i].buildingInfo.has_value() == b[i].buildingInfo.has_value();
                        if (same && a[i].buildingInfo) { same = a[i].buildingInfo->unit == b[i].buildingInfo->unit
                            && a[i].buildingInfo->passable == b[i].buildingInfo->passable; }
                    }
                    REQUIRE(same);
                }
            }
            if (t % 300 == 1)
            {
                unsigned int active = 0;
                for (const auto& [id, d] : s.campaignAi.front().deadlockedFactories)
                {
                    active += d.reclaimTarget.has_value();
                    recoverySeen += d.reclaimTarget.has_value();
                    progressSeen += d.reclaimBestDistance.has_value();
                    evictionSeen += !d.evictAttempts.empty();
                }
                concurrent = std::max(concurrent, active);
                if (std::getenv("RWE_RECOVERY_DIAGNOSTICS"))
                { std::cout << "recovery scenario=" << scenario << " tick=" << t << " " << recoveryHistory(s) << '\n'; }
            }
            if (t == 18000) { break; }
            auto advance = [&](GameSimulation& world) {
                if (scenario == 1 && t < 1801)
                {
                    // Isolate scan timing before enabling ordinary factory COB:
                    // otherwise its own BuggerOff clears this mobile before AI's
                    // confirmation interval. Actual recovery still issues the shove.
                    world.gameTime += GameTime(1); updateCampaignAi(world);
                }
                else { world.tick(); }
                world.events.clear();
            };
            advance(s);
            if (haveCopy) { advance(restored); }
        }
        CAPTURE(recoverySeen, progressSeen, evictionSeen);
        if (scenario == 1) { REQUIRE(evictionSeen > 0); }
        else { REQUIRE(recoverySeen > 0); REQUIRE(progressSeen > 0); REQUIRE(savedBeam); }
        if (scenario == 2) { REQUIRE(concurrent == 2); }
        REQUIRE(s.campaignAi.front().deadlockedFactories.empty());
        REQUIRE(s.getUnitState(ids.factory).buildQueue.empty());
    }
}

TEST_CASE("AI recovery watchdog continuation retains the first stall decision", "[phase8b][recovery-save]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    if (!data) { SKIP("requires installed definitions"); }
    setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_phase8b_legacy.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto pack = loadSkirmishDefinitionPack(vfs);
    auto s = recoveryWorld(pack);
    const auto ids = installRecoveryFixture(s, 32, 32, false);
    for (int i = 0; i < 2101; ++i) { s.tick(); s.events.clear(); }
    REQUIRE(s.campaignAi.front().deadlockedFactories.at(ids.factory).reclaimBestDistance);
    // Controlled obstruction: a zero-speed fixture holds the already-issued
    // builder approach stationary. Both branches have the same definitions.
    s.unitDefinitions.at("CORCV").maxVelocity = 0_ss;
    std::get<UnitPhysicsInfoGround>(s.getUnitState(ids.builder).physics).currentSpeed = 0_ss;
    auto params = recoveryParams();
    auto bytes = encodeSavedGame(s, params, nullptr, "WATCHDOG", 0);
    if (const auto* path = std::getenv("RWE_RECOVERY_REPRO_SAVE"))
    { std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); }
    auto restored = recoveryWorld(pack);
    restored.unitDefinitions.at("CORCV").maxVelocity = 0_ss;
    applySavedGame(restored, decodeSavedGame(bytes), nullptr);
    unsigned int firstOrderDifference = 0;
    for (unsigned int t = 2102; t <= 12000; ++t)
    {
        s.tick(); restored.tick(); s.events.clear(); restored.events.clear();
        if (computeHashOf(s.getUnitState(ids.builder).orders) != computeHashOf(restored.getUnitState(ids.builder).orders))
        {
            firstOrderDifference = t;
            std::cout << "first-order-divergence tick=" << t << " control=" << recoveryHistory(s)
                << " restored=" << recoveryHistory(restored) << '\n';
            break;
        }
    }
    REQUIRE(firstOrderDifference == 0);
}

TEST_CASE("AI recovery cooldown and exclusions survive repeated stalled save cycles", "[phase8b][recovery-save]")
{
    const char* data = std::getenv("RWE_DATA_PATH"); if (!data) { SKIP("requires installed definitions"); }
    setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_phase8b_cooldown.log", false));
    auto vfs = constructVfs(std::filesystem::path(data)); auto pack = loadSkirmishDefinitionPack(vfs);
    auto s = recoveryWorld(pack); const auto ids = installRecoveryFixture(s, 32, 32, false);
    auto second = s.trySpawnUnit("CORCV", PlayerId(1), s.terrain.heightmapIndexToWorldCenter(72, 42), std::nullopt, true);
    REQUIRE(second); s.getUnitState(*second).finishBuilding(s.unitDefinitions.at("CORCV"));
    s.getUnitState(*second).addOrder(WaitOrder{200000});
    s.campaignAi.front().groups[AiGroupBuilder].push_back(*second);
    for (unsigned int t = 0; t < 2101; ++t) { s.tick(); s.events.clear(); }
    s.unitDefinitions.at("CORCV").maxVelocity = 0_ss;
    for (auto id : {ids.builder, *second}) { std::get<UnitPhysicsInfoGround>(s.getUnitState(id).physics).currentSpeed = 0_ss; }
    const auto params = recoveryParams();
    auto copy = recoveryWorld(pack); copy.unitDefinitions.at("CORCV").maxVelocity = 0_ss;
    applySavedGame(copy, decodeSavedGame(encodeSavedGame(s, params, nullptr, "COOLDOWN", 0)), nullptr);
    unsigned int refreshes = 0, previousAttempts = 0, peakAvoid = 0, cycles = 0;
    std::vector<double> scanUs, hashUs, saveUs, loadUs, tickUs;
    const bool metrics = std::getenv("RWE_RECOVERY_METRICS") != nullptr;
    s.collectTickPhaseTimings = metrics;
    auto micros = [](auto start) { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count(); };
    for (unsigned int tick = 2102; tick <= 60000; ++tick)
    {
        const bool scan = s.gameTime + GameTime(1) >= s.campaignAi.front().nextDeadlockScan;
        auto start = std::chrono::steady_clock::now(); auto aiBefore = s.tickPhaseTimings.campaignAi;
        s.tick(); if (metrics) { tickUs.push_back(micros(start)); if (scan) { scanUs.push_back((s.tickPhaseTimings.campaignAi - aiBefore) / 1000.0); } }
        copy.tick(); s.events.clear(); copy.events.clear();
        start = std::chrono::steady_clock::now(); const auto h = s.computeHash(); if (metrics) { hashUs.push_back(micros(start)); }
        REQUIRE(copy.computeHash() == h); REQUIRE(recoveryHistory(s) == recoveryHistory(copy));
        const auto& d = s.campaignAi.front().deadlockedFactories.at(ids.factory);
        if (d.reclaimAttempts < previousAttempts) { ++refreshes; }
        previousAttempts = d.reclaimAttempts; peakAvoid = std::max(peakAvoid, static_cast<unsigned int>(d.reclaimAvoid.size()));
        REQUIRE(d.reclaimAvoid.size() <= 2); REQUIRE(d.evictAttempts.empty());
        if (tick % 300 == 1)
        {
            start = std::chrono::steady_clock::now(); auto bytes = encodeSavedGame(copy, params, nullptr, "COOLDOWN", 0);
            if (metrics) { saveUs.push_back(micros(start)); }
            REQUIRE(bytes == encodeSavedGame(s, params, nullptr, "COOLDOWN", 0));
            start = std::chrono::steady_clock::now(); applySavedGame(copy, decodeSavedGame(bytes), nullptr);
            if (metrics) { loadUs.push_back(micros(start)); }
            REQUIRE(copy.computeHash() == s.computeHash()); ++cycles;
        }
    }
    REQUIRE(peakAvoid == 2); REQUIRE(refreshes >= 2); REQUIRE(cycles == 192);
    REQUIRE(s.campaignAi.front().deadlockedFactories.size() == 1);
    if (metrics)
    {
        auto print = [](const char* name, std::vector<double>& samples) {
            std::sort(samples.begin(), samples.end());
            auto at = [&](double p) { return samples.at(static_cast<std::size_t>((samples.size() - 1) * p)); };
            std::cout << "recovery_metric=" << name << " n=" << samples.size() << " p50_us=" << at(.50)
                << " p95_us=" << at(.95) << " p99_us=" << at(.99) << '\n';
        };
        print("tick", tickUs); print("scan", scanUs); print("hash", hashUs); print("save", saveUs); print("load", loadUs);
        std::cout << "recovery_cycles=" << cycles << " cooldown_refreshes=" << refreshes << " peak_avoid=" << peakAvoid << '\n';
    }
}
