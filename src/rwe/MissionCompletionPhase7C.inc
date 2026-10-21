// Opt-in retail completion experiments. Commands enter the same typed command
// queue and authoritative effects used by GameScene. No fixture state edits.
namespace
{
    PlayerUnitCommand phase7cDecode(GameSimulation& sim, const AuditJson& c)
    {
        const UnitId id(c.at("unit").get<unsigned int>());
        auto unit = sim.tryGetUnitState(id); REQUIRE(unit);
        const auto& u = unit->get(); REQUIRE(followupSelectable(sim, u));
        const auto& def = sim.unitDefinitions.at(u.unitType);
        const std::string name = c.at("command");
        if (name == "Stop") { return {id, PlayerUnitCommand::Stop{}}; }
        if (name == "HoldFire") { return {id, PlayerUnitCommand::SetFireOrders{UnitFireOrders::HoldFire}}; }
        if (name == "FireAtWill") { return {id, PlayerUnitCommand::SetFireOrders{UnitFireOrders::FireAtWill}}; }
        auto order = [&](const UnitOrder& o) {
            return PlayerUnitCommand(id, PlayerUnitCommand::IssueOrder(o,
                c.value("queued", false) ? PlayerUnitCommand::IssueOrder::Queued : PlayerUnitCommand::IssueOrder::Immediate));
        };
        if (name == "Move") { REQUIRE(def.canMove); return order(MoveOrder(followupVector(c.at("destination")))); }
        if (name == "Build" || name == "Produce")
        {
            const std::string type = c.at("build_type");
            REQUIRE(def.builder);
            REQUIRE(std::find(def.buildOptions.begin(), def.buildOptions.end(), type) != def.buildOptions.end());
            if (name == "Produce") { return {id, PlayerUnitCommand::ModifyBuildQueue{c.value("count", 1), type}}; }
            const auto& bd = sim.unitDefinitions.at(type);
            const auto pos = followupVector(c.at("destination"));
            auto rect = sim.computeFootprintRegion(pos, bd.movementCollisionInfo);
            auto placement = sim.computeBuildingPlacement(sim.getAdHocMovementClass(bd.movementCollisionInfo),
                bd.yardMap, bd.yardMapContainsGeo, bd.waterline, rect.x, rect.y);
            REQUIRE(placement); REQUIRE(placement->valid);
            return order(BuildOrder(type, pos));
        }
        const UnitId target(c.at("target").get<unsigned int>());
        auto t = sim.tryGetUnitState(target); REQUIRE(t); REQUIRE(t->get().isAlive());
        REQUIRE_FALSE(t->get().carriedBy);
        if (name == "Guard") { REQUIRE(sim.areAllied(u.owner, t->get().owner)); return order(GuardOrder(target)); }
        REQUIRE_FALSE(sim.areAllied(u.owner, t->get().owner));
        REQUIRE(sim.isLosVisibleToPlayer(target, u.owner));
        REQUIRE_FALSE((t->get().missionState && t->get().missionState->immunity));
        if (name == "Capture")
        {
            REQUIRE(def.canCapture);
            const auto& td = sim.unitDefinitions.at(t->get().unitType);
            REQUIRE_FALSE(td.canCapture); REQUIRE_FALSE(t->get().isBeingBuilt(td));
            return order(CaptureOrder(target));
        }
        REQUIRE(name == "Attack"); REQUIRE(def.canAttack); return order(AttackOrder(target));
    }

    void phase7cApply(GameSimulation& sim, const AuditJson& commands)
    {
        if (commands.empty()) { return; }
        PlayerCommandService service;
        const auto local = sim.missionInfo->localPlayerId;
        service.registerPlayer(local);
        std::vector<PlayerCommand> batch;
        for (const auto& c : commands) { batch.emplace_back(phase7cDecode(sim, c)); }
        service.pushCommands(local, batch);
        auto popped = service.tryPopCommands(); REQUIRE(popped.has_value());
        for (const auto& [owner, list] : *popped)
        {
            REQUIRE(owner == local);
            for (const auto& c : list)
            {
                const auto& unitCommand = std::get<PlayerUnitCommand>(c);
                auto& u = sim.getUnitState(unitCommand.unit);
                REQUIRE(u.owner == owner);
                REQUIRE(applyBasicPlayerUnitCommand(u, unitCommand.command));
            }
        }
    }

    bool phase7cSelected(const UnitState& u, const AuditJson& group)
    {
        if (group.contains("types"))
        {
            auto types = group.at("types").get<std::vector<std::string>>();
            return std::find(types.begin(), types.end(), u.unitType) != types.end();
        }
        return true;
    }

    // Fixed plans contain map clicks, not hidden-state targets. Target identity
    // and type are read only inside current LOS. At most one scan per 90 ticks.
    AuditJson phase7cCommands(GameSimulation& sim, const AuditJson& plan, unsigned int tick)
    {
        AuditJson commands = AuditJson::array();
        if (tick % 90 != 0) { return commands; }
        auto groups = plan.value("groups", AuditJson::array({plan}));
        for (const auto& group : groups)
        {
            std::size_t selected = 0;
            for (const auto& [id, u] : sim.units)
            {
                if (!followupSelectable(sim, u) || !phase7cSelected(u, group)) { continue; }
                const auto index = selected++;
                if (index < group.value("offset", 0u) || index >= group.value("offset", 0u) + group.value("count", 10000u)) { continue; }
                const auto& def = sim.unitDefinitions.at(u.unitType);
                const std::string policy = group.value("policy", "defend");
                auto command = [&](const std::string& name) {
                    return AuditJson{{"tick", tick}, {"unit", id.value}, {"type", u.unitType}, {"command", name}};
                };
                auto move = [&](const AuditJson& point) {
                    auto c = command("Move");
                    c["destination"] = dumpJson(sim.terrain.topLeftCoordinateToWorld(
                        SimVector(SimScalar(point[0].get<float>()), 0_ss, SimScalar(point[1].get<float>()))));
                    commands.push_back(c);
                };
                if (policy == "build")
                {
                    if (tick != group.value("at", 0u)) { continue; }
                    const std::string type = group.at("build_type");
                    if (std::find(def.buildOptions.begin(), def.buildOptions.end(), type) == def.buildOptions.end()) { continue; }
                    auto pos = sim.terrain.topLeftCoordinateToWorld(SimVector(
                        SimScalar(group.at("position")[0].get<float>()), 0_ss, SimScalar(group.at("position")[1].get<float>())));
                    const auto& bd = sim.unitDefinitions.at(type);
                    bool issued = false;
                    const int radius = group.value("search_radius", 0);
                    REQUIRE(radius >= 0); REQUIRE(radius <= 512);
                    for (int ring = 0; ring <= radius && !issued; ring += 32)
                    {
                        for (int dz = -ring; dz <= ring && !issued; dz += 32)
                        {
                            for (int dx = -ring; dx <= ring && !issued; dx += 32)
                            {
                                if (std::max(std::abs(dx), std::abs(dz)) != ring) { continue; }
                                auto candidate = pos + SimVector(SimScalar(dx), 0_ss, SimScalar(dz));
                                auto rect = sim.computeFootprintRegion(candidate, bd.movementCollisionInfo);
                                auto placement = sim.computeBuildingPlacement(sim.getAdHocMovementClass(bd.movementCollisionInfo),
                                    bd.yardMap, bd.yardMapContainsGeo, bd.waterline, rect.x, rect.y);
                                if (!placement || !placement->valid) { continue; }
                                auto corner = sim.terrain.heightmapIndexToWorldCorner(rect.x, rect.y);
                                auto c = command("Build"); c["build_type"] = type;
                                c["destination"] = dumpJson(SimVector(corner.x + SimScalar(rect.width) * 8_ss,
                                    placement->baseY, corner.z + SimScalar(rect.height) * 8_ss));
                                commands.push_back(c); issued = true;
                            }
                        }
                    }
                    continue;
                }
                if (policy == "produce")
                {
                    if (tick != group.value("at", 0u)) { continue; }
                    const std::string type = group.at("build_type");
                    if (std::find(def.buildOptions.begin(), def.buildOptions.end(), type) == def.buildOptions.end()) { continue; }
                    auto c = command("Produce"); c["build_type"] = type; c["count"] = group.value("quantity", 1);
                    commands.push_back(c); continue;
                }
                if (policy == "hold") { if (tick == 0) { commands.push_back(command("Stop")); } continue; }
                const bool capturer = policy == "capture" && def.canCapture;
                if (capturer && tick == 0) { commands.push_back(command("HoldFire")); }
                if (!capturer && (!def.canAttack || def.commander) && policy != "move") { continue; }
                if (!u.orders.empty() && std::holds_alternative<CaptureOrder>(u.orders.front())) { continue; }
                std::set<std::string> captureTypes, killTypes;
                for (const auto& o : sim.missionObjectives->getVictoryObjectives())
                {
                    if (o.params.empty() || o.satisfied) { continue; }
                    if (o.key == "CaptureUnitType") { captureTypes.insert(toUpper(o.params[0])); }
                    if (o.key == "KillUnitType" || o.key == "KillAllOfType") { killTypes.insert(toUpper(o.params[0])); }
                }
                std::optional<UnitId> target;
                float best = std::numeric_limits<float>::max();
                if (policy != "move")
                {
                    for (const auto& [tid, t] : sim.units)
                    {
                        if (sim.areAllied(u.owner, t.owner) || t.isDead() || t.carriedBy
                            || (t.missionState && t.missionState->immunity) || !sim.isLosVisibleToPlayer(tid, u.owner)) { continue; }
                        if (capturer ? !captureTypes.count(t.unitType) : captureTypes.count(t.unitType)) { continue; }
                        if (policy == "strike" && !killTypes.count(t.unitType)) { continue; }
                        if (capturer && (sim.unitDefinitions.at(t.unitType).canCapture
                            || t.isBeingBuilt(sim.unitDefinitions.at(t.unitType)))) { continue; }
                        auto dist = simScalarToFloat(u.position.distanceSquared(t.position));
                        const float leash = group.value("leash", policy == "defend" ? 800.0f : 1200.0f);
                        if (!capturer && dist > leash * leash) { continue; }
                        if (dist < best) { best = dist; target = tid; }
                    }
                }
                if (target)
                {
                    // Retarget stale attacks, but do not continually restart a
                    // matching attack while aiming. Every 900 ticks permit a
                    // route retry for units stalled on an unreachable target.
                    if (!u.orders.empty() && tick % 900 != 0)
                    {
                        if (auto* a = std::get_if<AttackOrder>(&u.orders.front()); a && a->target == AttackTarget(*target)) { continue; }
                    }
                    if (tick % 900 != 0 || group.value("route", AuditJson::array()).empty())
                    { auto c = command(capturer ? "Capture" : "Attack"); c["target"] = target->value; commands.push_back(c); continue; }
                }
                const auto route = group.value("route", AuditJson::array());
                if (def.canMove && !route.empty() && tick % group.value("route_interval", 900u) == 0)
                {
                    auto i = (tick / group.value("route_interval", 900u));
                    i = group.value("cycle", false) ? i % route.size() : std::min<std::size_t>(i, route.size() - 1);
                    move(route[i]);
                }
            }
        }
        return commands;
    }
}
TEST_CASE("mission phase7c authoritative command batch", "[.mission-phase7c]")
{
    const char* data = std::getenv("RWE_DATA_PATH");
    const char* planPath = std::getenv("RWE_MISSION_PLAN");
    const char* output = std::getenv("RWE_MISSION_AUDIT_OUTPUT");
    REQUIRE(data); REQUIRE(planPath); REQUIRE(output);
    REQUIRE_FALSE(std::filesystem::exists(output));
    std::filesystem::create_directories(output);
    AuditJson plan; std::ifstream(planPath) >> plan;
    const char* replayPath = std::getenv("RWE_MISSION_REPLAY");
    AuditJson replay;
    if (replayPath) { std::ifstream(replayPath) >> replay; }
    const unsigned int limit = plan.at("tick_limit");
    REQUIRE(limit > 150); REQUIRE(limit <= 108300);
    const auto before = std::chrono::steady_clock::now();
    setGlobalLogger(std::make_shared<SimpleLogger>(std::string(output) + "/engine.log", false));
    auto vfs = constructVfs(data); auto pack = loadDefinitionPack(vfs);
    bool found = false;
    for (const auto& c : loadCampaignDescriptors(vfs, "camps"))
    {
        for (const auto& m : c.descriptor.missions)
        {
            if (m.missionFile != plan.at("id").get<std::string>()) { continue; }
            REQUIRE_FALSE(found); found = true;
            const std::string difficulty = plan.at("difficulty");
            auto bytes = vfs.readFile("maps/" + m.missionFile); REQUIRE(bytes);
            auto ota = parseOta(parseTdfFromString(std::string(bytes->begin(), bytes->end())));
            const auto schema = findCampaignSchemaIndex(ota, difficulty);
            AuditJson report = {{"id", m.missionFile}, {"difficulty", difficulty}, {"plan", plan},
                {"fixture_edits", false}, {"ai_enabled", true}, {"commands", AuditJson::array()},
                {"transitions", AuditJson::array()}, {"checkpoints", AuditJson::array()},
                {"fresh_process_replay", replayPath != nullptr}};
            auto result = runSchema(pack, c.displayName, c.descriptor.campaignSide, m, ota, schema, difficulty,
                vfs, 0, false, false, false, [&](GameSimulation& sim, unsigned int) {
                    report["initial_hash"] = sim.computeHash().value;
                    std::ostringstream rngState; rngState << sim.rng;
                    report["initial_rng_state"] = rngState.str();
                    report["initial_objectives"] = auditObjectives(sim);
                    report["command_interface"] = "PlayerCommandService / applyBasicPlayerUnitCommand (shared with GameScene)";
                    report["local_units"] = AuditJson::array();
                    for (const auto& [id, u] : sim.units)
                    {
                        if (u.owner != sim.missionInfo->localPlayerId) { continue; }
                        const auto& d = sim.unitDefinitions.at(u.unitType);
                        report["local_units"].push_back({{"id", id.value}, {"type", u.unitType}, {"selectable", followupSelectable(sim, u)},
                            {"build_options", d.buildOptions}, {"position", dumpJson(u.position)}});
                    }
                    std::unique_ptr<GameSimulation> restored;
                    bool deterministic = true;
                    AuditJson prior;
                    std::size_t replayCursor = 0;
                    unsigned int tick = 0;
                    for (; tick < limit && sim.getMissionResult() == MissionResult::InProgress; ++tick)
                    {
                        AuditJson commands = AuditJson::array();
                        if (replayPath)
                        {
                            while (replayCursor < replay["commands"].size()
                                && replay["commands"][replayCursor]["tick"] == tick)
                            { commands.push_back(replay["commands"][replayCursor++]); }
                        }
                        else { commands = phase7cCommands(sim, plan, tick); }
                        phase7cApply(sim, commands);
                        if (restored) { phase7cApply(*restored, commands); }
                        for (const auto& command : commands) { report["commands"].push_back(command); }
                        sim.tick();
                        if (restored)
                        {
                            restored->tick();
                            if (sim.computeHash() != restored->computeHash()
                                || auditObjectives(sim) != auditObjectives(*restored))
                            {
                                if (deterministic)
                                {
                                    report["first_divergence_tick"] = sim.gameTime.value;
                                    std::ofstream(std::filesystem::path(output) / "first-divergence.json")
                                        << AuditJson::diff(dumpJson(sim), dumpJson(*restored)).dump(2);
                                    if (std::getenv("RWE_MISSION_DIAG"))
                                    {
                                        std::ofstream(std::filesystem::path(output) / "first-before.json") << dumpJson(sim).dump(2);
                                        std::ofstream(std::filesystem::path(output) / "first-restored.json") << dumpJson(*restored).dump(2);
                                        std::ofstream(std::filesystem::path(output) / "first-cob-diff.json")
                                            << AuditJson::diff(auditCobState(sim), auditCobState(*restored)).dump(2);
                                    }
                                }
                                deterministic = false;
                            }
                            restored->events.clear();
                        }
                        if (tick == 149)
                        {
                            auto save = encodeSavedGame(sim, auditParams(sim, schema), nullptr, "PHASE7C", 0);
                            std::ofstream f(std::filesystem::path(output) / "mid.rwesave", std::ios::binary);
                            f.write(save.data(), save.size());
                            restored = auditRestore(sim, decodeSavedGame(save));
                            report["save_occupancy_differences"] = AuditJson::array();
                            const auto& a = sim.occupiedGrid.getVector();
                            const auto& b = restored->occupiedGrid.getVector();
                            for (std::size_t cell = 0; cell < a.size(); ++cell)
                            {
                                if (a[cell].mobileUnitId != b[cell].mobileUnitId)
                                { report["save_occupancy_differences"].push_back({{"cell", cell},
                                    {"before", a[cell].mobileUnitId ? int(a[cell].mobileUnitId->value) : -1},
                                    {"after", b[cell].mobileUnitId ? int(b[cell].mobileUnitId->value) : -1}}); }
                            }
                            if (std::getenv("RWE_MISSION_DIAG"))
                            {
                                std::ofstream(std::filesystem::path(output) / "save-state-diff.json")
                                    << AuditJson::diff(dumpJson(sim), dumpJson(*restored)).dump(2);
                                std::ofstream(std::filesystem::path(output) / "save-cob-diff.json")
                                    << AuditJson::diff(auditCobState(sim), auditCobState(*restored)).dump(2);
                            }
                            report["save_version"] = decodeSavedGame(save).version;
                            report["save_initial_hash_equal"] = sim.computeHash() == restored->computeHash();
                            deterministic &= sim.computeHash() == restored->computeHash()
                                && auditObjectives(sim) == auditObjectives(*restored);
                        }
                        auto state = auditObjectives(sim);
                        if (state != prior)
                        { report["transitions"].push_back({{"tick", sim.gameTime.value}, {"state", state}}); prior = state; }
                        if (sim.gameTime.value % 30 == 0)
                        { report["checkpoints"].push_back({{"tick", sim.gameTime.value}, {"hash", sim.computeHash().value}}); }
                        // Retain bounded progress even if the parent times this process out.
                        if (sim.gameTime.value % 300 == 0)
                        { std::ofstream(std::filesystem::path(output) / "progress.json") << AuditJson{{"tick", sim.gameTime.value}, {"commands", report["commands"].size()}, {"objectives", state}}.dump(2); }
                        sim.events.clear();
                    }
                    report["ticks"] = tick;
                    report["final_hash"] = sim.computeHash().value;
                    report["outcome"] = sim.getMissionResult() == MissionResult::Victory ? "VICTORY"
                        : sim.getMissionResult() == MissionResult::Defeat ? "DEFEAT" : "BOUND_REACHED";
                    report["commands_admitted"] = !report["commands"].empty();
                    report["save_continuation_tested"] = bool(restored);
                    report["save_continuation_equal"] = bool(restored) && deterministic;
                    report["final_players"] = AuditJson::array();
                    for (const auto& player : sim.players) { report["final_players"].push_back(dumpJson(player)); }
                    report["remaining_units"] = AuditJson::array();
                    for (const auto& [id, u] : sim.units)
                    { report["remaining_units"].push_back({{"id", id.value}, {"type", u.unitType}, {"owner", u.owner.value}, {"hp", u.hitPoints}, {"position", dumpJson(u.position)}, {"orders", u.orders.size()}, {"build_completed", u.buildTimeCompleted}, {"build_required", sim.unitDefinitions.at(u.unitType).buildTime}}); }
                    if (sim.getMissionResult() == MissionResult::Victory)
                    {
                        CampaignProgress progress;
                        REQUIRE(missionEndAction(sim.getMissionResult(), false) == MissionEndAction::DeclareVictory);
                        REQUIRE(missionEndAction(sim.getMissionResult(), true) == MissionEndAction::None);
                        progress.recordResult(c.displayName, c.descriptor.campaignSide, m.index, true, c.descriptor.missions.size());
                        auto save = encodeSavedGame(sim, auditParams(sim, schema), &progress, "PHASE7C-VICTORY", 0);
                        std::ofstream f(std::filesystem::path(output) / "victory.rwesave", std::ios::binary); f.write(save.data(), save.size());
                        auto win = auditRestore(sim, decodeSavedGame(save));
                        CHECK(win->getMissionResult() == MissionResult::Victory);
                        CHECK(auditObjectives(*win) == auditObjectives(sim));
                        CampaignProgress loadedProgress;
                        applySavedGame(*win, decodeSavedGame(save), &loadedProgress);
                        const bool retained = loadedProgress.missionStatus(c.displayName, c.descriptor.campaignSide, m.index) == CampaignMissionStatus::Won;
                        CHECK(retained);
                        const auto next = debriefSelectedMissionIndex(m.index, true, c.descriptor.missions.size());
                        const auto priorObjectives = auditObjectives(*win);
                        for (unsigned int n = 0; n < 180; ++n) { win->tick(); win->events.clear(); }
                        CHECK(auditObjectives(*win) == priorObjectives);
                        report["campaign_progression"] = {{"won_retained_after_load", retained}, {"mission_index", m.index},
                            {"debrief_selected_index", next}, {"duplicate_declaration_suppressed", true},
                            {"terminal_result_stable_180_ticks", auditObjectives(*win) == priorObjectives},
                            {"scope", "headless result/debrief/save APIs; UI transition remains Human QA"}};
                    }
                    if (replayPath)
                    {
                        report["replay_equal"] = replay["initial_hash"] == report["initial_hash"]
                            && replay["initial_rng_state"] == report["initial_rng_state"] && replay["checkpoints"] == report["checkpoints"]
                            && replay["transitions"] == report["transitions"] && replay["final_hash"] == report["final_hash"]
                            && replay["ticks"] == report["ticks"] && replay["outcome"] == report["outcome"]
                            && replay["commands"] == report["commands"];
                        CHECK(report["replay_equal"].get<bool>());
                    }
                    CHECK(deterministic);
                }, true);
            if (report["outcome"] == "VICTORY" && m.index + 1 < c.descriptor.missions.size())
            {
                const auto& next = c.descriptor.missions.at(m.index + 1);
                auto raw = vfs.readFile("maps/" + next.missionFile); REQUIRE(raw);
                auto nextOta = parseOta(parseTdfFromString(std::string(raw->begin(), raw->end())));
                auto nextSchema = findCampaignSchemaIndex(nextOta, difficulty);
                std::ifstream file(std::filesystem::path(output) / "victory.rwesave", std::ios::binary);
                auto saved = decodeSavedGame(std::vector<char>((std::istreambuf_iterator<char>(file)), {}));
                CampaignProgress progress;
                // Restore the earned history only; never fabricate previous wins.
                runSchema(pack, c.displayName, c.descriptor.campaignSide, m, ota, schema, difficulty,
                    vfs, 0, false, false, false, [&](GameSimulation& sim, unsigned int) {
                        applySavedGame(sim, saved, &progress);
                    }, true);
                auto nextResult = runSchema(pack, c.displayName, c.descriptor.campaignSide, next, nextOta, nextSchema, difficulty,
                    vfs, 120, false, false, false, [&](GameSimulation& sim, unsigned int n) {
                        if (n != 120) { return; }
                        auto bytes = encodeSavedGame(sim, auditParams(sim, nextSchema), &progress, "PHASE7C-NEXT", 0);
                        auto restored = auditRestore(sim, decodeSavedGame(bytes));
                        CampaignProgress reloaded;
                        applySavedGame(*restored, decodeSavedGame(bytes), &reloaded);
                        CHECK(reloaded.missionStatus(c.displayName, c.descriptor.campaignSide, m.index) == CampaignMissionStatus::Won);
                        CHECK(reloaded.missionStatus(c.displayName, c.descriptor.campaignSide, next.index) == CampaignMissionStatus::Unplayed);
                        CHECK(sim.computeHash() == restored->computeHash());
                        report["campaign_progression"]["next_save_hash_equal"] = sim.computeHash() == restored->computeHash();
                    }, true);
                report["campaign_progression"]["next_mission"] = next.missionFile;
                report["campaign_progression"]["next_ticks"] = 120;
                report["campaign_progression"]["next_spawn_failures"] = nextResult.spawnFailures;
            }
            report["spawn_failures"] = result.spawnFailures;
            report["script_errors"] = result.errors;
            report["unsupported_commands"] = result.unsupportedCommandStrings;
            report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
            std::ofstream(std::filesystem::path(output) / "result.json") << report.dump(2) << '\n';
        }
    }
    REQUIRE(found);
}
