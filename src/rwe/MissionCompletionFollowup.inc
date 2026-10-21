// Test-only command player. No gameplay state preparation is allowed here.
// A replay consumes the recorded commands, not a second policy decision.
namespace
{
    bool followupSelectable(const GameSimulation& sim, const UnitState& u)
    {
        return u.owner == sim.missionInfo->localPlayerId && u.isAlive() && !u.carriedBy
            && !u.isBeingBuilt(sim.unitDefinitions.at(u.unitType))
            && !(u.missionState && !u.missionState->selectable);
    }

    SimVector followupVector(const AuditJson& v)
    {
        return SimVector(SimScalar(v.at("x").get<float>()), SimScalar(v.at("y").get<float>()),
            SimScalar(v.at("z").get<float>()));
    }

    void followupApply(GameSimulation& sim, const AuditJson& c)
    {
        const UnitId id(c.at("unit").get<unsigned int>());
        auto unit = sim.tryGetUnitState(id);
        REQUIRE(unit);
        auto& u = unit->get();
        REQUIRE(followupSelectable(sim, u));
        const auto& def = sim.unitDefinitions.at(u.unitType);
        const auto command = c.at("command").get<std::string>();
        if (command == "HoldFire") { u.fireOrders = UnitFireOrders::HoldFire; return; }
        if (command == "Stop") { u.clearOrders(); return; }
        if (command == "Move")
        {
            REQUIRE(def.canMove);
            u.clearOrders(); u.addOrder(MoveOrder(followupVector(c.at("destination"))));
            return;
        }
        const UnitId target(c.at("target").get<unsigned int>());
        auto t = sim.tryGetUnitState(target);
        REQUIRE(t);
        REQUIRE(t->get().isAlive());
        REQUIRE_FALSE(sim.areAllied(u.owner, t->get().owner));
        // Unit identity/type is only used when actually visible, including
        // in playback. No hidden-unit targeting or fog edits.
        REQUIRE(sim.isLosVisibleToPlayer(target, u.owner));
        REQUIRE_FALSE(t->get().carriedBy);
        REQUIRE_FALSE((t->get().missionState && t->get().missionState->immunity));
        if (command == "Capture")
        {
            REQUIRE(def.canCapture);
            const auto& td = sim.unitDefinitions.at(t->get().unitType);
            REQUIRE_FALSE(td.canCapture);
            REQUIRE_FALSE(t->get().isBeingBuilt(td));
            u.clearOrders(); u.addOrder(CaptureOrder(target));
        }
        else
        {
            REQUIRE(command == "Attack"); REQUIRE(def.canAttack);
            u.clearOrders(); u.addOrder(AttackOrder(target));
        }
    }

    AuditJson followupCommands(GameSimulation& sim, const std::string& policy, unsigned int tick,
        const AuditJson& route)
    {
        AuditJson commands = AuditJson::array();
        if (tick % 90 != 0) { return commands; }
        std::set<std::string> captureTypes, killTypes;
        for (const auto& o : sim.missionObjectives->getVictoryObjectives())
        {
            if (o.params.empty() || o.satisfied) { continue; }
            if (o.key == "CaptureUnitType") { captureTypes.insert(toUpper(o.params[0])); }
            if (o.key == "KillUnitType" || o.key == "KillAllOfType") { killTypes.insert(toUpper(o.params[0])); }
        }
        for (const auto& [id, u] : sim.units)
        {
            if (!followupSelectable(sim, u)) { continue; }
            const auto& def = sim.unitDefinitions.at(u.unitType);
            auto command = [&](const std::string& name) {
                return AuditJson{{"tick", tick}, {"unit", id.value}, {"type", u.unitType}, {"command", name}};
            };
            if (policy == "defend")
            {
                // A player Stop command leaves fire-at-will, enemy scripts,
                // economy and survival clocks operating normally.
                if (tick == 0) { commands.push_back(command("Stop")); }
                continue;
            }
            if (policy == "capture" && tick == 0)
            { commands.push_back(command("HoldFire")); }
            // Do not reset an ongoing capture or attack animation every scan.
            if (!u.orders.empty() && (std::holds_alternative<CaptureOrder>(u.orders.front())
                || std::holds_alternative<AttackOrder>(u.orders.front()))) { continue; }
            std::optional<UnitId> target;
            float best = std::numeric_limits<float>::max();
            if (policy != "move")
            {
                for (const auto& [tid, t] : sim.units)
                {
                    if (sim.areAllied(u.owner, t.owner) || t.isDead() || t.carriedBy
                        || (t.missionState && t.missionState->immunity)
                        || !sim.isLosVisibleToPlayer(tid, u.owner)) { continue; }
                    const bool capture = captureTypes.count(t.unitType);
                    const auto& td = sim.unitDefinitions.at(t.unitType);
                    if (policy == "capture")
                    {
                        if (!def.canCapture || !capture || td.canCapture || t.isBeingBuilt(td)) { continue; }
                    }
                    else if (!def.canAttack || capture) { continue; }
                    float dx = simScalarToFloat(t.position.x - u.position.x);
                    float dz = simScalarToFloat(t.position.z - u.position.z);
                    float score = dx * dx + dz * dz;
                    if (killTypes.count(t.unitType)) { score *= 0.01f; }
                    if (score < best) { best = score; target = tid; }
                }
            }
            if (target)
            {
                auto c = command(policy == "capture" ? "Capture" : "Attack");
                c["target"] = target->value; commands.push_back(std::move(c)); continue;
            }
            if (!def.canMove) { continue; }
            if (policy == "capture" && !def.canCapture) { continue; }
            // Routes are ordinary world-coordinate Move clicks. A fixed route
            // is explicit input; the default is a map-size-only serpentine.
            if (tick % 1800 == 0 || u.orders.empty())
            {
                SimVector destination;
                if (!route.empty())
                {
                    const auto& p = route[std::min<std::size_t>(tick / 1800, route.size() - 1)];
                    destination = sim.terrain.topLeftCoordinateToWorld(
                        SimVector(SimScalar(p[0].get<float>()), 0_ss, SimScalar(p[1].get<float>())));
                }
                else
                {
                    unsigned int cell = (tick / 1800) % 16;
                    unsigned int row = cell / 4, col = row % 2 ? 3 - cell % 4 : cell % 4;
                    destination = sim.terrain.topLeftCoordinateToWorld(SimVector(
                        sim.terrain.getWidthInWorldUnits() * SimScalar((col + 0.5f) / 4), 0_ss,
                        sim.terrain.getHeightInWorldUnits() * SimScalar((row + 0.5f) / 4)));
                }
                auto c = command("Move"); c["destination"] = dumpJson(destination); commands.push_back(std::move(c));
            }
        }
        return commands;
    }
}

TEST_CASE("mission followup legitimate command batch", "[.mission-followup]")
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
                        else { commands = followupCommands(sim, plan.at("policy"), tick, plan.value("route", AuditJson::array())); }
                        for (const auto& command : commands)
                        {
                            followupApply(sim, command);
                            if (restored) { followupApply(*restored, command); }
                            report["commands"].push_back(command);
                        }
                        sim.tick();
                        if (restored)
                        {
                            restored->tick();
                            if (sim.computeHash() != restored->computeHash()
                                || auditObjectives(sim) != auditObjectives(*restored))
                            {
                                if (deterministic) { report["first_divergence_tick"] = sim.gameTime.value; }
                                deterministic = false;
                            }
                            restored->events.clear();
                        }
                        if (tick == 149)
                        {
                            auto save = encodeSavedGame(sim, auditParams(sim, schema), nullptr, "PHASE7B", 0);
                            std::ofstream f(std::filesystem::path(output) / "mid.rwesave", std::ios::binary);
                            f.write(save.data(), save.size());
                            restored = auditRestore(sim, decodeSavedGame(save));
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
                    report["remaining_units"] = AuditJson::array();
                    for (const auto& [id, u] : sim.units)
                    { report["remaining_units"].push_back({{"id", id.value}, {"type", u.unitType}, {"owner", u.owner.value}, {"hp", u.hitPoints}, {"position", dumpJson(u.position)}, {"orders", u.orders.size()}}); }
                    if (sim.getMissionResult() == MissionResult::Victory)
                    {
                        auto save = encodeSavedGame(sim, auditParams(sim, schema), nullptr, "PHASE7B-VICTORY", 0);
                        std::ofstream f(std::filesystem::path(output) / "victory.rwesave", std::ios::binary); f.write(save.data(), save.size());
                        auto win = auditRestore(sim, decodeSavedGame(save));
                        CHECK(win->getMissionResult() == MissionResult::Victory);
                        CHECK(auditObjectives(*win) == auditObjectives(sim));
                    }
                    if (replayPath)
                    {
                        report["replay_equal"] = replay["checkpoints"] == report["checkpoints"]
                            && replay["transitions"] == report["transitions"] && replay["final_hash"] == report["final_hash"]
                            && replay["ticks"] == report["ticks"] && replay["outcome"] == report["outcome"]
                            && replay["commands"] == report["commands"];
                        CHECK(report["replay_equal"].get<bool>());
                    }
                    CHECK(deterministic);
                }, true);
            report["spawn_failures"] = result.spawnFailures;
            report["script_errors"] = result.errors;
            report["unsupported_commands"] = result.unsupportedCommandStrings;
            report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
            std::ofstream(std::filesystem::path(output) / "result.json") << report.dump(2) << '\n';
        }
    }
    REQUIRE(found);
}
