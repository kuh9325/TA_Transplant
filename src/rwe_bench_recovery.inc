// Benchmark counterpart of the native Phase 8B recovery fixture.
namespace
{
    GameSimulation recoveryWorld(const BenchDefinitionPack& pack)
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

    struct RecoveryFixtureIds { UnitId factory; UnitId blocker; UnitId builder; };

    RecoveryFixtureIds installRecoveryFixture(GameSimulation& s, int x, int z, bool mobile)
    {
        auto spawn = [&](const std::string& name, int cx, int cz) {
            auto pos = s.terrain.heightmapIndexToWorldCenter(cx, cz);
            pos.y = s.terrain.getHeightAt(pos.x, pos.z);
            auto id = s.trySpawnUnit(name, PlayerId(1), pos, std::nullopt, true);
            if (!id) { die("recovery fixture spawn failed"); }
            s.getUnitState(*id).finishBuilding(s.unitDefinitions.at(name));
            return *id;
        };
        auto factory = spawn("CORLAB", x, z);
        auto rect = s.computeFootprintRegion(s.getUnitState(factory).position, s.unitDefinitions.at("CORLAB").movementCollisionInfo);
        if (mobile) { if (!s.trySetYardOpen(factory, true)) { die("recovery fixture yard failed"); } }
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
