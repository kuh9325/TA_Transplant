// Included by SavedGame.test.cpp. Each probe restores the exact original field.
GameHash hashFeatureIdentities(const GameSimulation& sim);

TEST_CASE("Phase8C feature name scratch is local collision safe and byte exact", "[phase8c][hash]")
{
    PlayerId a{0}, b{0}; auto sim=makeWorld(a,b);
    for (unsigned i=0;i<129;++i) {
        FeatureDefinition f{}; f.name="Mixed_case_feature_"+std::to_string(i);
        sim.featureDefinitions.insert(f);
    }
    std::string bytes; for(unsigned c=0;c<256;++c) bytes.push_back(static_cast<char>(c));
    sim.featureDefinitions.get(FeatureDefinitionId(0)).name=bytes;
    // Repeated IDs and collisions in the bounded direct-mapped scratch table.
    for (auto n:{0u,0u,64u,128u,64u,0u,1u,2u,1u,128u}) {
        MapFeature f{}; f.featureName=FeatureDefinitionId(n); sim.features.emplace(std::move(f));
    }
    auto reference=[&] {
        uint32_t h=2166136261u;
        for(const auto& [id,f]:sim.features) {
            h=(h^id.value)*16777619u;
            h=(h^computeHashOf(toUpper(sim.getFeatureDefinition(f.featureName).name)).value)*16777619u;
        }
        for(auto id:sim.activeFeatures) h=(h^id.value)*16777619u;
        auto rng=sim.rng; return combineHashes(GameHash(h),static_cast<uint32_t>(rng()));
    };
    REQUIRE(hashFeatureIdentities(sim)==reference());
    const auto initial=sim.computeHash();
    auto& first=sim.featureDefinitions.get(FeatureDefinitionId(0)).name;
    auto& second=sim.featureDefinitions.get(FeatureDefinitionId(64)).name;
    const auto oldFirst=first,oldSecond=second;
    first="new first"; second="new second";
    REQUIRE(hashFeatureIdentities(sim)==reference()); REQUIRE(sim.computeHash()!=initial);
    first=oldFirst; second=oldSecond;
    REQUIRE(hashFeatureIdentities(sim)==reference()); REQUIRE(sim.computeHash()==initial);
    sim.featureDefinitions.get(FeatureDefinitionId(0)).name.clear();
    REQUIRE(hashFeatureIdentities(sim)==reference());
}

TEST_CASE("Phase8C authoritative mutations restore the original hash", "[phase8c][hash]")
{
    PlayerId a{0}, b{0}; auto sim = makeWorld(a, b);
    auto id=sim.trySpawnUnit("ARMTANK",a,sim.terrain.heightmapIndexToWorldCenter(8,8),std::nullopt);
    REQUIRE(id); auto& u=sim.getUnitState(*id);
    u.weapons[0]=UnitWeapon{};
    u.bomberState=std::make_unique<BomberState>();
    u.aircraftService=std::make_unique<AircraftServiceState>();
    u.pieces.emplace_back();
    u.cobEnvironment->_statics={1,2};
    initCampaignAiPlayer(sim,b,CampaignAiProfile{});
    auto& recovery=sim.campaignAi.front().deadlockedFactories[*id];
    Projectile projectile{}; projectile.damage={{"DEFAULT",1},{"ARMTANK",2}};
    auto shot=sim.projectiles.emplace(std::move(projectile));
    auto& p=sim.projectiles.tryGet(shot)->get();
    FeatureDefinition feature{}; feature.name="HASH_PROBE";
    MapFeature f{}; f.featureName=sim.featureDefinitions.insert(feature);
    auto fid=sim.features.emplace(std::move(f));
    auto& liveFeature=sim.features.tryGet(fid)->get();
    sim.missionInfo=MissionInfo{};
    auto probe=[&](const char* name,auto& field,auto change) {
        CAPTURE(name);
        const auto original=field;
        const auto before=sim.computeHash();
        change(field);
        REQUIRE(sim.computeHash()!=before);
        field=original;
        REQUIRE(sim.computeHash()==before);
    };
    probe("position",u.position.x,[](auto& x){x+=1_ss;});
    probe("health",u.hitPoints,[](auto& x){++x;});
    probe("orders",u.orders,[](auto& x){x.push_back(WaitOrder{7});});
    probe("weapon",u.weapons[0]->readyTime,[](auto& x){x+=GameTime(1);});
    probe("projectile",p.velocity.x,[](auto& x){x+=1_ss;});
    probe("projectile damage",p.damage,[](auto& x){++x.at("ARMTANK");});
    probe("EMP",u.orders,[](auto& x){x.push_front(ParalyzeOrder{3,GameTime(20)});});
    probe("veterancy",u.kills,[](auto& x){++x;});
    probe("kamikaze",u.nextKamikazeScan,[](auto& x){x=GameTime(12);});
    probe("bomber",u.bomberState->phase,[](auto& x){x=BomberState::Phase::Approach;});
    probe("service",u.aircraftService->phase,[](auto& x){x=AircraftServiceState::Phase::Repair;});
    probe("feature",liveFeature.damageAccumulated,[](auto& x){++x;});
    const auto cell=sim.occupiedGrid.get(8,8); const auto before=sim.computeHash();
    sim.occupiedGrid.mutate(GridCoordinates(8,8),[](auto& x){x.mobileUnitId=std::nullopt;});
    REQUIRE(sim.computeHash()!=before);
    sim.occupiedGrid.set(8,8,cell); REQUIRE(sim.computeHash()==before); REQUIRE(sim.occupiedGrid.digestIsValid());
    probe("COB",u.cobEnvironment->_statics[0],[](auto& x){++x;});
    probe("piece",u.pieces[0].offset.x,[](auto& x){x+=1_ss;});
    probe("recovery cooldown",recovery.reclaimStalledAt,[](auto& x){x=GameTime(4);});
    probe("recovery best",recovery.reclaimBestDistance,[](auto& x){x=3_ss;});
    probe("recovery deadline",recovery.reclaimDeadline,[](auto& x){x=GameTime(5);});
    probe("recovery avoid",recovery.reclaimAvoid,[](auto& x){x.insert(256);});
    probe("recovery evictions",recovery.evictAttempts,[](auto& x){x.emplace(512,2);});
    probe("RNG",sim.rng,[](auto& x){x();});
    probe("mission",sim.missionInfo->useOnlyUnits,[](auto& x){x.insert("ARMTANK");});

    // Simultaneous changes and irrelevant unordered insertion order must not
    // invalidate canonical traversal or lose any payloads.
    recovery.reclaimAvoid={512,256,768}; recovery.evictAttempts={{512,2},{256,1},{768,3}};
    sim.missionInfo->useOnlyUnits={"ARMTANK","CORTANK"};
    const auto canonical=sim.computeHash();
    recovery.reclaimAvoid.clear(); for(auto x:{768u,256u,512u}) recovery.reclaimAvoid.insert(x);
    recovery.evictAttempts.clear(); for(auto x:{768u,256u,512u}) recovery.evictAttempts.emplace(x,x/256);
    p.damage.clear(); p.damage.emplace("ARMTANK",2); p.damage.emplace("DEFAULT",1);
    sim.missionInfo->useOnlyUnits.clear(); sim.missionInfo->useOnlyUnits.insert("CORTANK"); sim.missionInfo->useOnlyUnits.insert("ARMTANK");
    REQUIRE(sim.computeHash()==canonical);
}
