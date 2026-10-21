// Included only by CampaignClosure.test.cpp, inside namespace rwe. Reuses its
// media-free retail loader. All diagnostics are opt-in; no runtime tick changes.
namespace
{
    using AuditJson = nlohmann::json;

    // Explicit VM oracle: the world hash does not cover every COB stack/queue.
    // Used only by opt-in save investigation, never normal simulation ticks.
    AuditJson auditCobState(const GameSimulation& sim)
    {
        AuditJson result = AuditJson::array();
        for (const auto& [id, unit] : sim.units)
        {
            const auto& env = *unit.cobEnvironment;
            AuditJson j = {{"id", id.value}, {"statics", env._statics}, {"threads", AuditJson::array()},
                {"ready", AuditJson::array()}, {"blocked", AuditJson::array()},
                {"sleeping", AuditJson::array()}, {"finished", AuditJson::array()}};
            auto index = [&](const CobThread* t) {
                auto it = std::find_if(env.threads.begin(), env.threads.end(), [&](const auto& p) { return p.get() == t; });
                return std::distance(env.threads.begin(), it);
            };
            for (const auto& t : env.threads)
            {
                AuditJson stack = AuditJson::array(), frames = AuditJson::array();
                auto values = t->stack;
                while (!values.empty()) { stack.push_back(values.top()); values.pop(); }
                auto calls = t->callStack;
                while (!calls.empty())
                {
                    const auto& f = calls.top();
                    frames.push_back({{"pc", f.instructionIndex}, {"count", f.localCount}, {"locals", f.locals}});
                    calls.pop();
                }
                j["threads"].push_back({{"name", t->name}, {"signal", t->signalMask}, {"stack", stack},
                    {"frames", frames}, {"return", t->returnValue}, {"return_locals", t->returnLocals}});
            }
            for (auto* t : env.readyQueue) { j["ready"].push_back(index(t)); }
            for (auto* t : env.finishedQueue) { j["finished"].push_back(index(t)); }
            for (const auto& [wake, t] : env.sleepingQueue) { j["sleeping"].push_back({wake.value, index(t)}); }
            for (const auto& [status, t] : env.blockedQueue)
            {
                std::visit([&](const auto& c) { j["blocked"].push_back(
                    {index(t), status.condition.index(), c.object, static_cast<unsigned int>(c.axis)}); }, status.condition);
            }
            result.push_back(std::move(j));
        }
        return result;
    }

    AuditJson auditTdf(const TdfBlock& block)
    {
        AuditJson j = AuditJson::object();
        for (const auto& [k, v] : block.properties) { j[k] = v; }
        for (const auto& [k, v] : block.blocks) { j[k] = auditTdf(*v); }
        return j;
    }

    AuditJson auditObjectives(const GameSimulation& sim)
    {
        AuditJson j;
        j["result"] = static_cast<int>(sim.getMissionResult());
        j["countdown"] = sim.missionObjectives->getResultCountdown();
        for (const auto& side : {"victory", "defeat"})
        {
            j[side] = AuditJson::array();
            const auto& objectives = std::string(side) == "victory"
                ? sim.missionObjectives->getVictoryObjectives() : sim.missionObjectives->getDefeatObjectives();
            for (const auto& o : objectives)
            {
                j[side].push_back({{"key", o.key}, {"params", o.params},
                    {"satisfied", o.satisfied}, {"remaining_count", o.remainingCount}});
            }
        }
        return j;
    }

    GameParameters auditParams(const GameSimulation& sim, unsigned int schema)
    {
        const auto& m = *sim.missionInfo;
        GameParameters p(missionFileToMapName(m.missionFile), schema);
        p.campaign = CampaignMissionParameters{m.campaignName, m.campaignSide, m.missionIndex,
            m.missionFile, m.missionName, m.schemaType};
        return p;
    }

    std::unique_ptr<GameSimulation> auditRestore(const GameSimulation& source, const DecodedSaveGame& saved)
    {
        auto terrain = source.terrain;
        auto sim = std::make_unique<GameSimulation>(std::move(terrain), 0,
            source.minWindSpeed, source.maxWindSpeed, source.tidalStrength);
        sim->unitDefinitions = source.unitDefinitions;
        sim->weaponDefinitions = source.weaponDefinitions;
        sim->unitModelDefinitions = source.unitModelDefinitions;
        sim->unitScriptDefinitions = source.unitScriptDefinitions;
        sim->movementClassDatabase = source.movementClassDatabase;
        sim->featureDefinitions = source.featureDefinitions;
        sim->featureNameIndex = source.featureNameIndex;
        sim->computerResourceScale = source.computerResourceScale;
        sim->mapGravity = source.mapGravity;
        sim->noSeaLevelTrigger = source.noSeaLevelTrigger;
        sim->waterDoesDamage = source.waterDoesDamage;
        sim->waterDamage = source.waterDamage;
        // Static hazard parameters come from the map, as in LoadingScene;
        // RWESAVE stores the scheduler, not the weapon/config catalog.
        sim->meteorShower = source.meteorShower;
        sim->movementClassCollisionService = createMovementClassCollisionService(sim->terrain, sim->movementClassDatabase);
        applySavedGame(*sim, saved, nullptr);
        return sim;
    }
}

TEST_CASE("mission completion audit bounded batch", "[.mission-audit]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    const char* output = std::getenv("RWE_MISSION_AUDIT_OUTPUT");
    REQUIRE(data); REQUIRE(output);
    REQUIRE_FALSE(std::filesystem::exists(output));
    std::filesystem::create_directories(output);
    setGlobalLogger(std::make_shared<SimpleLogger>(std::string(output) + "/engine.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto campaigns = loadCampaignDescriptors(vfs, "camps");
    std::sort(campaigns.begin(), campaigns.end(), [](const auto& a, const auto& b) { return a.fileName < b.fileName; });
    const auto sharedStart = std::chrono::steady_clock::now();
    auto pack = loadDefinitionPack(vfs);
    const double sharedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sharedStart).count();
    AuditJson catalog = AuditJson::object();
    for (const auto& [name, def] : pack.unitDefinitions)
    {
        bool emp = false, bomber = false, nuke = false;
        for (const auto& weaponName : {def.weapon1, def.weapon2, def.weapon3})
        {
            auto weapon = pack.weaponDefinitions.find(toUpper(weaponName));
            if (weapon == pack.weaponDefinitions.end()) { continue; }
            emp |= weapon->second.paralyzer;
            bomber |= std::holds_alternative<ProjectilePhysicsTypeDropped>(weapon->second.physicsType);
            nuke |= weapon->second.stockpile && weapon->second.cruise;
        }
        catalog[name] = {{"emp", emp}, {"bomber", bomber}, {"nuclear_missile", nuke},
            {"kamikaze", def.kamikaze}, {"mine", def.standbyMine}, {"aircraft_servicing", def.isAirBase},
            {"transport", def.transportCapacity > 0}, {"reclaim", def.canReclaim}, {"capture", def.canCapture},
            {"cloak", def.cloakCost > 0}, {"veterancy", true}, {"can_move", def.canMove},
            {"builder", def.builder}, {"commander", def.commander}, {"build_options", def.buildOptions}};
    }
    std::ofstream(std::filesystem::path(output) / "unit-capabilities.json") << catalog.dump(2) << '\n';
    unsigned int start = std::getenv("RWE_MISSION_AUDIT_START") ? std::stoul(std::getenv("RWE_MISSION_AUDIT_START")) : 0;
    unsigned int count = std::getenv("RWE_MISSION_AUDIT_COUNT") ? std::stoul(std::getenv("RWE_MISSION_AUDIT_COUNT")) : 175;
    unsigned int ticks = std::getenv("RWE_MISSION_AUDIT_TICKS") ? std::stoul(std::getenv("RWE_MISSION_AUDIT_TICKS")) : 300;
    REQUIRE(ticks <= 18000);
    const bool saveCheck = std::getenv("RWE_MISSION_AUDIT_SAVE") != nullptr;
    const bool cobCheck = std::getenv("RWE_MISSION_AUDIT_COB") != nullptr;
    unsigned int ordinal = 0, visited = 0;
    for (const auto& c : campaigns)
    {
        for (const auto& m : c.descriptor.missions)
        {
            auto index = ordinal++;
            if (index < start || index >= start + count) { continue; }
            ++visited;
            INFO(m.missionFile);
            auto raw = vfs.readFile("maps/" + missionFileToMapName(m.missionFile) + ".ota");
            REQUIRE(raw);
            auto tdf = parseTdfFromString(std::string(raw->begin(), raw->end()));
            auto ota = parseOta(tdf);
            AuditJson record = {{"ordinal", index}, {"id", m.missionFile}, {"name", m.missionName},
                {"campaign", c.displayName}, {"descriptor", c.fileName}, {"mission_index", m.index},
                {"campaign_size", c.descriptor.missions.size()}, {"player_faction", c.descriptor.campaignSide},
                {"opposing_faction", oppositeSide(c.descriptor.campaignSide)},
                {"map", missionFileToMapName(m.missionFile)}, {"raw_ota", auditTdf(tdf)},
                {"shared_definition_load_ms", sharedMs}, {"runs", AuditJson::array()}};
            for (const auto& difficulty : {"Easy", "Medium", "Hard"})
            {
                INFO(difficulty);
                auto schema = findCampaignSchemaIndex(ota, difficulty);
                AuditJson run = {{"difficulty", difficulty}, {"schema_index", schema},
                    {"schema_type", ota.schemas[schema].type}, {"invalid_state", AuditJson::array()},
                    {"objective_transitions", AuditJson::array()}, {"ai_scheduler_changes", 0},
                    {"save_check_requested", saveCheck}};
                const auto before = std::chrono::steady_clock::now();
                auto tickStart = before;
                AuditJson lastObjective;
                std::vector<GameTime> aiSchedule;
                std::unique_ptr<GameSimulation> restored;
                std::size_t eventCount = 0;
                std::map<std::string, unsigned int> eventTypes;
                unsigned int aiChanges = 0;
                auto result = runSchema(pack, c.displayName, c.descriptor.campaignSide, m, ota,
                    schema, difficulty, vfs, ticks, false, false, false,
                    [&](GameSimulation& sim, unsigned int tick) {
                        if (tick == 0)
                        {
                            tickStart = std::chrono::steady_clock::now();
                            run["init_ms"] = std::chrono::duration<double, std::milli>(tickStart - before).count();
                            run["initial_units"] = std::distance(sim.units.begin(), sim.units.end());
                            run["initial_hash"] = sim.computeHash().value;
                            run["players"] = AuditJson::array();
                            for (const auto& p : sim.players) { run["players"].push_back(dumpJson(p)); }
                            unsigned int nonzero = 0;
                            for (const auto& [id, u] : sim.units) { nonzero += u.kills != 0; }
                            run["initial_live_experienced_units"] = nonzero;
                            REQUIRE(nonzero == 0);
                        }
                        if (restored)
                        {
                            restored->tick();
                            if (cobCheck && tick % 30 == 0)
                            {
                                bool equal = auditCobState(sim) == auditCobState(*restored);
                                run["cob_samples"] = run.value("cob_samples", 0) + 1;
                                if (!equal && !run.contains("cob_first_divergence_tick"))
                                { run["cob_first_divergence_tick"] = tick; }
                                CHECK(equal);
                            }
                            if (sim.computeHash() != restored->computeHash() && !run.contains("save_first_divergence_tick"))
                            {
                                run["save_first_divergence_tick"] = tick;
                                run["save_expected_hash"] = sim.computeHash().value;
                                run["save_actual_hash"] = restored->computeHash().value;
                                std::ofstream(std::filesystem::path(output) / (std::to_string(index) + "-" + difficulty + "-continuation-diff.json"))
                                    << AuditJson::diff(dumpJson(sim), dumpJson(*restored)).dump(2);
                            }
                        }
                        for (const auto& [id, u] : sim.units)
                        {
                            for (const auto& w : u.weapons)
                            {
                                if (!w) { continue; }
                                const auto* attack = std::get_if<UnitWeaponStateAttacking>(&w->state);
                                const auto* aim = attack ? std::get_if<UnitWeaponStateAttacking::AimInfo>(&attack->attackInfo) : nullptr;
                                if (aim && std::none_of(u.cobEnvironment->threads.begin(), u.cobEnvironment->threads.end(),
                                    [&](const auto& t) { return t.get() == aim->thread; }) && !run.contains("first_orphan_aim"))
                                { run["first_orphan_aim"] = {{"tick", tick}, {"unit", id.value}, {"type", u.unitType}, {"paralyzed", u.isParalyzed()}}; }
                            }
                        }
                        if (saveCheck && tick == ticks / 2)
                        {
                            try
                            {
                            const auto saved = decodeSavedGame(encodeSavedGame(sim, auditParams(sim, schema), nullptr, "MISSION-AUDIT", 0));
                            restored = auditRestore(sim, saved);
                            if (cobCheck)
                            {
                                run["save_cob_equal"] = auditCobState(sim) == auditCobState(*restored);
                                CHECK(run["save_cob_equal"].get<bool>());
                            }
                            run["save_reencoded_equal"] = encodeSavedGame(sim, auditParams(sim, schema), nullptr, "MISSION-AUDIT", 0)
                                == encodeSavedGame(*restored, auditParams(*restored, schema), nullptr, "MISSION-AUDIT", 0);
                            if (std::getenv("RWE_MISSION_AUDIT_DETAIL"))
                            {
                                std::ofstream(std::filesystem::path(output) / (std::to_string(index) + "-" + difficulty + "-before.json")) << dumpJson(sim).dump(2);
                                for (const auto& name : {"source", "restored"})
                                {
                                    auto* s = std::string(name) == "source" ? &sim : restored.get();
                                    auto b = encodeSavedGame(*s, auditParams(*s, schema), nullptr, "MISSION-AUDIT", 0);
                                    std::ofstream f(std::filesystem::path(output) / (std::to_string(index) + "-" + difficulty + "-" + name + ".rwesave"), std::ios::binary);
                                    f.write(b.data(), b.size());
                                }
                            }
                            unsigned int occupancyDiffs = 0;
                            run["save_occupancy_examples"] = AuditJson::array();
                            const auto& aGrid = sim.occupiedGrid.getVector();
                            const auto& bGrid = restored->occupiedGrid.getVector();
                            for (unsigned int i = 0; i < aGrid.size(); ++i)
                            {
                                const auto& a = aGrid[i]; const auto& b = bGrid[i];
                                const bool differs = a.mobileUnitId != b.mobileUnitId || a.featureId != b.featureId
                                    || a.buildingInfo.has_value() != b.buildingInfo.has_value()
                                    || (a.buildingInfo && b.buildingInfo && (a.buildingInfo->unit != b.buildingInfo->unit || a.buildingInfo->passable != b.buildingInfo->passable));
                                occupancyDiffs += differs;
                                if (differs && run["save_occupancy_examples"].size() < 32)
                                { run["save_occupancy_examples"].push_back({{"cell", i}, {"before_mobile", a.mobileUnitId ? int(a.mobileUnitId->value) : -1}, {"after_mobile", b.mobileUnitId ? int(b.mobileUnitId->value) : -1}, {"before_feature", a.featureId ? int(a.featureId->value) : -1}, {"after_feature", b.featureId ? int(b.featureId->value) : -1}}); }
                            }
                            run["save_occupancy_differences"] = occupancyDiffs;
                            run["save_version"] = saved.version;
                            run["save_tick"] = tick;
                            run["save_initial_hash_equal"] = sim.computeHash() == restored->computeHash();
                            run["save_objectives_equal"] = auditObjectives(sim) == auditObjectives(*restored);
                            if (sim.computeHash() != restored->computeHash())
                            {
                                run["hash_components"] = AuditJson::array();
                                for (auto* s : {&sim, restored.get()})
                                {
                                    run["hash_components"].push_back({
                                        {"units", computeHashOf(s->units).value}, {"features", computeHashOf(s->features).value},
                                        {"ai", computeHashOf(s->campaignAi).value}, {"meteor", computeHashOf(s->meteorShower).value},
                                        {"scale", s->computerResourceScale}, {"cap", s->maxUnitsPerPlayer},
                                        {"los", s->lineOfSightEnabled}, {"mode", s->lineOfSightMode}, {"mapping", s->mappingEnabled},
                                        {"wind_change", s->nextWindSpeedChange.value}});
                                }
                                std::ofstream(std::filesystem::path(output) / (std::to_string(index) + "-" + difficulty + "-save-diff.json"))
                                    << AuditJson::diff(dumpJson(sim), dumpJson(*restored)).dump(2);
                            }
                            }
                            catch (const std::exception& e)
                            {
                                run["save_exception"] = e.what();
                                run["save_initial_hash_equal"] = false;
                                run["save_objectives_equal"] = false;
                            }
                        }
                        auto objective = auditObjectives(sim);
                        if (objective != lastObjective)
                        {
                            if (run["objective_transitions"].size() < 64)
                            { run["objective_transitions"].push_back({{"tick", tick}, {"state", objective}}); }
                            lastObjective = objective;
                        }
                        std::vector<GameTime> current;
                        for (const auto& a : sim.campaignAi)
                        {
                            current.insert(current.end(), {a.nextRepartition, a.nextFactoryScan, a.nextBuilderScan, a.nextAttackScan});
                        }
                        if (tick && current != aiSchedule) { ++aiChanges; }
                        aiSchedule = std::move(current);
                        for (; eventCount < sim.events.size(); ++eventCount)
                        { ++eventTypes[std::to_string(sim.events[eventCount].index())]; }
                        if (tick % 30 == 0 || tick == ticks)
                        {
                            for (const auto& [id, u] : sim.units)
                            {
                                if (u.owner.value >= sim.players.size()
                                    || !std::isfinite(simScalarToFloat(u.position.x))
                                    || !std::isfinite(simScalarToFloat(u.position.y))
                                    || !std::isfinite(simScalarToFloat(u.position.z)))
                                {
                                    if (run["invalid_state"].size() < 16)
                                    { run["invalid_state"].push_back({{"tick", tick}, {"unit", id.value}}); }
                                }
                            }
                        }
                        if (tick == ticks)
                        {
                            run["ticks_completed"] = tick;
                            run["execution_ms_including_observer"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();
                            run["active_units"] = std::distance(sim.units.begin(), sim.units.end());
                            run["final_hash"] = sim.computeHash().value;
                            run["pathfinding_requests"] = sim.pathFindingService.stats.requestsProcessed;
                            run["pathfinding_expansions"] = sim.pathFindingService.stats.totalExpansions;
                            run["events_by_variant_index"] = eventTypes;
                            run["ai_scheduler_changes"] = aiChanges;
                            run["objective_state"] = objective;
                        }
                    }, true);
                run["spawned_units"] = result.unitsSpawned;
                run["spawn_failures"] = result.spawnFailures;
                run["scripted_units"] = result.scriptedUnits;
                run["parsed_commands"] = result.parsedCommands;
                run["installed_orders"] = result.installedOrders;
                run["script_errors"] = result.errors;
                run["unsupported_commands"] = result.unsupportedCommandStrings;
                run["unresolved_features"] = result.unresolvedFeatures;
                run["ai_players"] = result.campaignAiPlayers;
                run["result"] = static_cast<int>(result.result);
                record["runs"].push_back(run);
                std::ofstream(std::filesystem::path(output) / (std::to_string(index) + ".json")) << record.dump(2) << '\n';
                CHECK(run["invalid_state"].empty());
                if (saveCheck)
                {
                    CHECK(run["save_initial_hash_equal"].get<bool>());
                    CHECK(run["save_objectives_equal"].get<bool>());
                    CHECK_FALSE(run.contains("save_first_divergence_tick"));
                }
            }
        }
    }
    REQUIRE(ordinal == 175);
    REQUIRE(visited == std::min(count, ordinal - start));
}

TEST_CASE("mission completion audit legal movement attempt", "[.mission-completion]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    const char* output = std::getenv("RWE_MISSION_AUDIT_OUTPUT");
    const char* selected = std::getenv("RWE_MISSION_AUDIT_MISSION");
    REQUIRE(data); REQUIRE(output); REQUIRE(selected);
    REQUIRE_FALSE(std::filesystem::exists(output));
    std::filesystem::create_directories(output);
    setGlobalLogger(std::make_shared<SimpleLogger>(std::string(output) + "/engine.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto pack = loadDefinitionPack(vfs);
    bool found = false;
    for (const auto& c : loadCampaignDescriptors(vfs, "camps"))
    {
        for (const auto& m : c.descriptor.missions)
        {
            if (toUpper(m.missionFile) != toUpper(selected)) { continue; }
            found = true;
            auto bytes = vfs.readFile("maps/" + m.missionFile);
            REQUIRE(bytes);
            auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
            for (const auto& difficulty : {"Easy", "Medium", "Hard"})
            {
                const auto schema = findCampaignSchemaIndex(ota, difficulty);
                runSchema(pack, c.displayName, c.descriptor.campaignSide, m, ota, schema, difficulty,
                    vfs, 0, false, false, false, [&](GameSimulation& sim, unsigned int) {
                        AuditJson report = {{"id", m.missionFile}, {"difficulty", difficulty},
                            {"fixture_edits", false}, {"ai_enabled", true}, {"commands", AuditJson::array()},
                            {"transitions", AuditJson::array()}, {"strategy", "one group Move to authored destination"}};
                        auto goal = std::find_if(ota.objectives.begin(), ota.objectives.end(),
                            [](const auto& o) { return o.type == "MoveUnitToRadius"; });
                        REQUIRE(goal != ota.objectives.end());
                        const std::string type = toUpper(goal->params[0]);
                        const float x = std::stof(goal->params[1]), z = std::stof(goal->params[2]);
                        const auto destination = sim.terrain.topLeftCoordinateToWorld(SimVector(SimScalar(x), 0_ss, SimScalar(z)));
                        bool issued = false;
                        unsigned int elapsed = 0;
                        std::unique_ptr<GameSimulation> restored;
                        AuditJson prior;
                        bool deterministic = true;
                        for (; elapsed < 9000 && sim.getMissionResult() == MissionResult::InProgress; ++elapsed)
                        {
                            // One fixed group Move, admitted only for selectable,
                            // completed, live player-owned mobile unit. This is the same
                            // clearOrders/addOrder operation used by GameScene::issueUnitOrder.
                            // No HP, ownership, position, timer, AI or objective edits.
                            if (!issued)
                            {
                                for (auto& [id, u] : sim.units)
                                {
                                    const auto& def = sim.unitDefinitions.at(u.unitType);
                                    if (u.owner != sim.missionInfo->localPlayerId || u.isDead() || u.carriedBy
                                        || !def.canMove || u.isBeingBuilt(def)
                                        || (u.missionState && !u.missionState->selectable)
                                        || (type != "ANYTYPE" && u.unitType != type)) { continue; }
                                    u.clearOrders(); u.addOrder(MoveOrder(destination));
                                    report["commands"].push_back({{"tick", elapsed}, {"unit", id.value},
                                        {"type", u.unitType}, {"command", "Move"}, {"destination", dumpJson(destination)}});
                                    issued = true;
                                }
                            }
                            sim.tick();
                            if (restored)
                            {
                                restored->tick();
                                deterministic &= sim.computeHash() == restored->computeHash();
                                deterministic &= auditObjectives(sim) == auditObjectives(*restored);
                                restored->events.clear();
                            }
                            if (issued && elapsed == 149)
                            {
                                auto save = encodeSavedGame(sim, auditParams(sim, schema), nullptr, "MISSION-COMPLETION", 0);
                                std::ofstream file(std::filesystem::path(output) / (std::string(difficulty) + "-mid.rwesave"), std::ios::binary);
                                file.write(save.data(), save.size());
                                restored = auditRestore(sim, decodeSavedGame(save));
                                deterministic &= sim.computeHash() == restored->computeHash();
                            }
                            auto objective = auditObjectives(sim);
                            if (objective != prior)
                            { report["transitions"].push_back({{"tick", sim.gameTime.value}, {"state", objective}}); prior = objective; }
                            sim.events.clear();
                        }
                        report["ticks"] = elapsed;
                        report["result"] = static_cast<int>(sim.getMissionResult());
                        report["outcome"] = sim.getMissionResult() == MissionResult::Victory ? "VICTORY"
                            : sim.getMissionResult() == MissionResult::Defeat ? "DEFEAT" : "BOUND_REACHED";
                        report["commands_admitted"] = issued;
                        report["save_continuation_tested"] = static_cast<bool>(restored);
                        report["save_continuation_equal"] = restored && deterministic;
                        report["final_hash"] = sim.computeHash().value;
                        report["remaining_local_units"] = AuditJson::array();
                        for (const auto& [id, u] : sim.units)
                        { if (u.owner == sim.missionInfo->localPlayerId) { report["remaining_local_units"].push_back({{"id", id.value}, {"type", u.unitType}, {"position", dumpJson(u.position)}, {"orders", u.orders.size()}}); } }
                        if (sim.getMissionResult() == MissionResult::Victory)
                        {
                            CampaignProgress progress;
                            progress.recordResult(c.displayName, c.descriptor.campaignSide, m.index, true, c.descriptor.missions.size());
                            const auto save = encodeSavedGame(sim, auditParams(sim, schema), &progress, "COMPLETED", 0);
                            std::ofstream file(std::filesystem::path(output) / (std::string(difficulty) + "-victory.rwesave"), std::ios::binary);
                            file.write(save.data(), save.size());
                            auto restoredWin = auditRestore(sim, decodeSavedGame(save));
                            CampaignProgress newSession;
                            applySavedGame(*restoredWin, decodeSavedGame(save), &newSession);
                            CHECK(newSession.missionStatus(c.displayName, c.descriptor.campaignSide, m.index) == CampaignMissionStatus::Won);
                            CHECK(restoredWin->getMissionResult() == MissionResult::Victory);
                            CHECK(auditObjectives(sim) == auditObjectives(*restoredWin));
                            report["recorded_win_restored"] = true;
                            report["next_selected_index"] = debriefSelectedMissionIndex(m.index, true, c.descriptor.missions.size());
                        }
                        std::ofstream(std::filesystem::path(output) / (std::string(difficulty) + ".json")) << report.dump(2) << '\n';
                        CHECK(issued);
                        CHECK(deterministic);
                        // A failed strategy is evidence, not an engine assertion failure.
                    }, true);
            }
        }
    }
    REQUIRE(found);
}

namespace
{
    // Deliberately synthetic prerequisites. These tests establish outcome
    // transitions through normal tick/event APIs, never full playthroughs.
    std::unique_ptr<GameSimulation> auditOutcomeFixture(const GameSimulation& source, unsigned int schema)
    {
        auto saved = decodeSavedGame(encodeSavedGame(source, auditParams(source, schema), nullptr, "OUTCOME-FIXTURE", 0));
        auto sim = auditRestore(source, saved);
        sim->campaignAi.clear();
        sim->meteorShower.enabled = false;
        sim->waterDoesDamage = false;
        for (auto& [id, unit] : sim->units)
        {
            unit.clearOrders();
            unit.buildQueue.clear();
            for (auto& weapon : unit.weapons) { weapon.reset(); }
        }
        return sim;
    }

    UnitId auditCreateTarget(GameSimulation& sim, std::string type, PlayerId owner)
    {
        type = toUpper(type);
        const auto pos = sim.terrain.heightmapIndexToWorldCenter(8, 8);
        auto id = sim.trySpawnUnit(type, owner, pos, std::nullopt, true, std::nullopt, true);
        if (!id) { throw std::runtime_error("fixture could not spawn target " + type); }
        auto& u = sim.getUnitState(*id);
        u.finishBuilding(sim.unitDefinitions.at(type));
        for (auto& weapon : u.weapons) { weapon.reset(); }
        return *id;
    }

    void auditSatisfy(GameSimulation& sim, const MissionObjectiveRuntime& objective)
    {
        const auto& key = objective.key;
        const auto local = sim.missionInfo->localPlayerId;
        const PlayerId enemy(1);
        auto type = objective.params.empty() ? std::string() : toUpper(objective.params[0]);
        if (key == "KillEnemyCommander" || key == "CommanderKilled")
        {
            auto id = auditCreateTarget(sim, key == "CommanderKilled" ? "ARMCOM" : "CORCOM",
                key == "CommanderKilled" ? local : enemy);
            sim.quietlyKillUnit(id);
        }
        else if (key == "KillUnitType" || key == "UnitTypeKilled")
        {
            const int n = std::max(1, objective.remainingCount);
            for (int i = 0; i < n; ++i) { sim.quietlyKillUnit(auditCreateTarget(sim, type, enemy)); }
        }
        else if (key == "KillAllOfType" || key == "AllUnitsKilledOfType")
        {
            auditCreateTarget(sim, type, enemy);
            std::vector<UnitId> targets;
            for (const auto& [id, u] : sim.units)
            { if (u.unitType == type && (key == "AllUnitsKilledOfType" || u.owner != local)) { targets.push_back(id); } }
            for (auto id : targets) { sim.quietlyKillUnit(id); }
        }
        else if (key == "DestroyAllUnits" || key == "KillAllMobileUnits" || key == "AllUnitsKilled")
        {
            std::vector<UnitId> targets;
            for (const auto& [id, u] : sim.units)
            {
                if ((key == "AllUnitsKilled" ? u.owner == local : u.owner != local)
                    && (key != "KillAllMobileUnits" || sim.unitDefinitions.at(u.unitType).canMove))
                { targets.push_back(id); }
            }
            for (auto id : targets) { sim.quietlyKillUnit(id); }
        }
        else if (key == "CaptureUnitType")
        {
            auto id = auditCreateTarget(sim, type, enemy);
            REQUIRE(sim.transferUnitOwnership(id, local));
        }
        else if (key == "BuildUnitType") { auditCreateTarget(sim, type, local); }
        else if (key == "VictoryTimerRunsOut" || key == "DeathTimerRunsOut")
        { sim.gameTime = GameTime(std::stoul(objective.params[0]) * 30); }
        else if (key == "MoveUnitToRadius" || key == "UnitTypePassesX" || key == "UnitTypePassesZ"
            || key == "AnyUnitPassesX" || key == "AnyUnitPassesZ")
        {
            const bool anyEnemy = key == "AnyUnitPassesX" || key == "AnyUnitPassesZ";
            if (type == "ANYTYPE" || anyEnemy) { type = "ARMCOM"; }
            auto id = auditCreateTarget(sim, type, anyEnemy ? enemy : local);
            auto& u = sim.getUnitState(id);
            const auto width = simScalarToFloat(sim.terrain.getWidthInWorldUnits());
            const auto height = simScalarToFloat(sim.terrain.getHeightInWorldUnits());
            if (key == "MoveUnitToRadius")
            { u.position.x = SimScalar(std::stof(objective.params[1]) - width / 2); u.position.z = SimScalar(std::stof(objective.params[2]) - height / 2); }
            else if (key.back() == 'X') { u.position.x = SimScalar(std::stof(objective.params[anyEnemy ? 0 : 1]) - width / 2); }
            else { u.position.z = SimScalar(std::stof(objective.params[anyEnemy ? 0 : 1]) - height / 2); }
            u.position.y = sim.terrain.getHeightAt(u.position.x, u.position.z);
        }
        else { throw std::runtime_error("unhandled objective: " + key); }
    }
}

TEST_CASE("mission completion audit authored outcome conditions", "[.mission-outcomes]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    const char* output = std::getenv("RWE_MISSION_AUDIT_OUTPUT");
    REQUIRE(data); REQUIRE(output); REQUIRE_FALSE(std::filesystem::exists(output));
    std::filesystem::create_directories(output);
    setGlobalLogger(std::make_shared<SimpleLogger>(std::string(output) + "/engine.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto pack = loadDefinitionPack(vfs);
    unsigned int count = 0;
    for (const auto& c : loadCampaignDescriptors(vfs, "camps"))
    {
        for (const auto& m : c.descriptor.missions)
        {
            INFO(m.missionFile);
            auto raw = vfs.readFile("maps/" + m.missionFile); REQUIRE(raw);
            auto ota = parseOta(parseTdfFromString(std::string(raw->begin(), raw->end())));
            auto schema = findCampaignSchemaIndex(ota, "Medium");
            AuditJson record = {{"id", m.missionFile}, {"difficulty", "Medium"},
                {"synthetic_prerequisites", true}, {"probes", AuditJson::array()}};
            runSchema(pack, c.displayName, c.descriptor.campaignSide, m, ota, schema, "Medium", vfs,
                0, false, false, false, [&](GameSimulation& source, unsigned int) {
                    auto test = [&](const std::string& name, const std::vector<MissionObjectiveRuntime>& objectives, MissionResult expected) {
                        AuditJson result = {{"name", name}, {"expected", static_cast<int>(expected)}};
                        try
                        {
                            auto sim = auditOutcomeFixture(source, schema);
                            result["before"] = auditObjectives(*sim);
                            for (const auto& o : objectives) { auditSatisfy(*sim, o); }
                            sim->rebuildOccupancyAfterLoad();
                            // Snapshot before the first objective polling pass. Normal
                            // simulation ticks run the real debounce and game-over path.
                            auto save = decodeSavedGame(encodeSavedGame(*sim, auditParams(*sim, schema), nullptr, "BEFORE-OUTCOME", 0));
                            auto restored = auditRestore(*sim, save);
                            bool same = true;
                            unsigned int ticks = 0;
                            for (; ticks < 181 && sim->getMissionResult() == MissionResult::InProgress; ++ticks)
                            {
                                sim->tick(); restored->tick();
                                same &= sim->computeHash() == restored->computeHash();
                                same &= auditObjectives(*sim) == auditObjectives(*restored);
                                sim->events.clear(); restored->events.clear();
                            }
                            result["ticks"] = ticks;
                            result["after"] = auditObjectives(*sim);
                            result["save_continuation_equal"] = same;
                            result["passed"] = sim->getMissionResult() == expected && same;
                        }
                        catch (const std::exception& e) { result["exception"] = e.what(); result["passed"] = false; }
                        record["probes"].push_back(result);
                        CHECK(result["passed"].get<bool>());
                    };
                    test("all_victory_conditions", source.missionObjectives->getVictoryObjectives(), MissionResult::Victory);
                    for (const auto& defeat : source.missionObjectives->getDefeatObjectives())
                    { test(defeat.key, {defeat}, MissionResult::Defeat); }
                }, true);
            std::ofstream(std::filesystem::path(output) / (missionFileToMapName(m.missionFile) + ".json")) << record.dump(2) << '\n';
            ++count;
        }
    }
    REQUIRE(count == 175);
}

TEST_CASE("mission completion audit resumes actual completion in a fresh process", "[.mission-resume]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    const char* input = std::getenv("RWE_MISSION_AUDIT_INPUT");
    REQUIRE(data); REQUIRE(input);
    setGlobalLogger(std::make_shared<SimpleLogger>("/tmp/rwe_mission_resume.log", false));
    auto vfs = constructVfs(std::filesystem::path(data));
    auto pack = loadDefinitionPack(vfs);
    auto campaigns = loadCampaignDescriptors(vfs, "camps");
    for (const auto& difficulty : {"Easy", "Medium", "Hard"})
    {
        std::ifstream jsonFile(std::filesystem::path(input) / (std::string(difficulty) + ".json"));
        REQUIRE(jsonFile.good());
        AuditJson expected; jsonFile >> expected;
        REQUIRE(expected["outcome"] == "VICTORY");
        std::ifstream file(std::filesystem::path(input) / (std::string(difficulty) + "-mid.rwesave"), std::ios::binary);
        REQUIRE(file.good());
        auto saved = decodeSavedGame(std::vector<char>((std::istreambuf_iterator<char>(file)), {}));
        auto campaign = std::find_if(campaigns.begin(), campaigns.end(), [&](const auto& c) { return c.displayName == saved.meta.campaignName; });
        REQUIRE(campaign != campaigns.end());
        const auto& m = campaign->descriptor.missions.at(saved.meta.missionIndex);
        auto raw = vfs.readFile("maps/" + m.missionFile); REQUIRE(raw);
        auto ota = parseOta(parseTdfFromString(std::string(raw->begin(), raw->end())));
        auto schema = findCampaignSchemaIndex(ota, difficulty);
        CampaignProgress progress;
        runSchema(pack, campaign->displayName, campaign->descriptor.campaignSide, m, ota, schema, difficulty,
            vfs, 0, false, false, false, [&](GameSimulation& sim, unsigned int) {
                applySavedGame(sim, saved, &progress);
                for (unsigned int i = 0; i < 9000 && sim.getMissionResult() == MissionResult::InProgress; ++i)
                { sim.tick(); sim.events.clear(); }
                REQUIRE(sim.getMissionResult() == MissionResult::Victory);
                REQUIRE(sim.computeHash().value == expected["final_hash"].get<unsigned int>());
                REQUIRE(sim.gameTime.value == expected["ticks"].get<unsigned int>());
                progress.recordResult(campaign->displayName, campaign->descriptor.campaignSide, m.index, true, campaign->descriptor.missions.size());
                const auto winBytes = encodeSavedGame(sim, auditParams(sim, schema), &progress, "ACTUAL-WIN", 0);
                CampaignProgress newSession;
                applySavedGame(sim, decodeSavedGame(winBytes), &newSession);
                REQUIRE(newSession.missionStatus(campaign->displayName, campaign->descriptor.campaignSide, m.index) == CampaignMissionStatus::Won);
                progress = std::move(newSession);
            }, true);
        auto nextIndex = debriefSelectedMissionIndex(m.index, true, campaign->descriptor.missions.size());
        REQUIRE(nextIndex == m.index + 1);
        REQUIRE(endmsnHasContinuation(true, true, true));
        const auto& next = campaign->descriptor.missions.at(nextIndex);
        auto nextRaw = vfs.readFile("maps/" + next.missionFile); REQUIRE(nextRaw);
        auto nextOta = parseOta(parseTdfFromString(std::string(nextRaw->begin(), nextRaw->end())));
        auto nextSchema = findCampaignSchemaIndex(nextOta, difficulty);
        runSchema(pack, campaign->displayName, campaign->descriptor.campaignSide, next, nextOta, nextSchema,
            difficulty, vfs, 120, false, false, false, [&](GameSimulation& sim, unsigned int tick) {
                REQUIRE(progress.missionStatus(campaign->displayName, campaign->descriptor.campaignSide, m.index) == CampaignMissionStatus::Won);
                REQUIRE(progress.missionStatus(campaign->displayName, campaign->descriptor.campaignSide, nextIndex) == CampaignMissionStatus::Unplayed);
                if (tick == 120)
                {
                    auto nextSave = decodeSavedGame(encodeSavedGame(sim, auditParams(sim, nextSchema), &progress, "NEXT-MISSION", 0));
                    REQUIRE(nextSave.meta.missionIndex == nextIndex);
                    auto copy = auditRestore(sim, nextSave);
                    CampaignProgress restart;
                    applySavedGame(*copy, nextSave, &restart);
                    REQUIRE(restart.missionStatus(campaign->displayName, campaign->descriptor.campaignSide, m.index) == CampaignMissionStatus::Won);
                    REQUIRE(restart.missionStatus(campaign->displayName, campaign->descriptor.campaignSide, nextIndex) == CampaignMissionStatus::Unplayed);
                }
            }, true);
    }
}
