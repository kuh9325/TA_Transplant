#pragma once

#include <map>
#include <set>

#include <cstdint>
#include <random>
#include <rwe/cob/CobUnitId.h>
#include <rwe/collections/SimpleVectorMap.h>
#include <rwe/collections/VectorMap.h>
#include <rwe/game/PlayerColorIndex.h>
#include <rwe/geometry/BoundingBox3x.h>
#include <rwe/pathfinding/PathFindingService.h>
#include <rwe/sim/CampaignAi.h>
#include <rwe/sim/FeatureDefinition.h>
#include <rwe/sim/FeatureId.h>
#include <rwe/sim/GameHash.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/MapFeature.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/MeteorShower.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/MissionObjectives.h>
#include <rwe/sim/MovementClassCollisionService.h>
#include <rwe/sim/MovementClassDatabase.h>
#include <rwe/sim/MovementClassId.h>
#include <rwe/sim/OccupiedGrid.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/sim/Projectile.h>
#include <rwe/sim/ProjectileId.h>
#include <rwe/sim/SimAxis.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitId.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/sim/UnitState.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rwe
{
    constexpr int MaxUtilizableWindSpeed = 5000;

    enum class GamePlayerStatus
    {
        Alive,
        Dead
    };

    enum class GamePlayerType
    {
        Human,
        Computer
    };

    enum class UnitDeathCause { Normal, SelfDestruct };

    struct GamePlayerInfo
    {
        std::optional<std::string> name;
        GamePlayerType type;
        PlayerColorIndex color;
        GamePlayerStatus status;
        std::string side;

        /**
         * Retail ally group (skirmish Player%dAllyGroup, player record
         * +0x13f): players sharing a group value 0-4 are mutual allies.
         * Retail builds an alliedWith table from it at game start
         * (TotalA.exe 0x446e90 -> 0x452960) consumed by targeting, LOS
         * sharing and victory. An absent value means no alliance
         * (retail init value 5).
         */
        std::optional<unsigned int> allyGroup;

        Metal metal;
        Energy energy;

        Metal maxMetal;
        Energy maxEnergy;

        Metal startingMetal;
        Energy startingEnergy;

        bool metalStalled{false};
        bool energyStalled{false};

        Metal desiredMetalConsumptionBuffer{0};
        Energy desiredEnergyConsumptionBuffer{0};

        Metal previousDesiredMetalConsumptionBuffer{0};
        Energy previousDesiredEnergyConsumptionBuffer{0};

        Metal actualMetalConsumptionBuffer{0};
        Energy actualEnergyConsumptionBuffer{0};

        Metal metalProductionBuffer{0};
        Energy energyProductionBuffer{0};

        /**
         * Debrief statistics accumulated over the mission, matching the
         * retail player record fields the ENDMSN stats builder reads
         * (TotalA.exe 0x41dd70-0x41ddbb: kills +0xfc, losses +0xfe,
         * produced +0xac/+0xb4, wasted +0xcc/+0xd4).
         */
        unsigned int unitsKilled{0};
        unsigned int unitsLost{0};
        double metalProduced{0.0};
        double energyProduced{0.0};
        double metalExcess{0.0};
        double energyExcess{0.0};

        bool addResourceDelta(const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal);
        bool recordAndCheckDesire(const Energy& energy);
        bool recordAndCheckDesire(const Metal& metal);
        void acceptResource(const Energy& energy);
        void acceptResource(const Metal& metal);
    };

    struct PathRequest
    {
        UnitId unitId;

        bool operator==(const PathRequest& rhs) const;

        bool operator!=(const PathRequest& rhs) const;
    };

    struct WinStatusWon
    {
        PlayerId winner;
    };
    struct WinStatusDraw
    {
    };
    struct WinStatusUndecided
    {
    };
    using WinStatus = std::variant<WinStatusWon, WinStatusDraw, WinStatusUndecided>;

    /**
     * Per-player fog-of-war state, indexed identically to `players`.
     * Recomputed each tick by updateVisibility() before unit behavior
     * runs. The unit sets and losGrid are pure functions of unit
     * positions/definitions, so they are excluded from game hashing and
     * serialization. `exploredGrid` accumulates deterministically and
     * cannot be recomputed from positions, so it is serialized (but
     * still excluded from hashing).
     */
    struct PlayerVisibility
    {
        /** Enemy units currently inside this player's line of sight. */
        std::unordered_set<UnitId> losVisibleUnits;

        /**
         * Enemy units detected by radar but not inside LOS. Radar
         * contacts render on the minimap only and cannot be
         * auto-acquired by weapons unless `hasTargetingUpgrade`.
         * Units inside an enemy radar-jammer radius are excluded
         * (retail 0x467960 clears the contact bit after coverage).
         */
        std::unordered_set<UnitId> radarDetectedUnits;

        /**
         * Enemy units detected by sonar but not inside LOS
         * (retail unit+0x110 bit9). Sonar contacts render on the
         * minimap only; retail collects no weapon-target list for
         * them, so they are never weapon-targetable. Units inside an
         * enemy sonar-jammer radius are excluded (0x467980).
         */
        std::unordered_set<UnitId> sonarDetectedUnits;

        /**
         * The player owns an activated, powered `istargetingupgrade`
         * unit (retail Targeting Facility; the per-player flag at
         * TotalA.exe +0x79, recomputed by the visibility pass). While
         * set, radar-only contacts are valid weapon targets.
         *
         * Retail reads a single unit bit (`unit+0x10e & 1`) that the
         * per-tick energy gate maintains as the powered-and-active
         * composite: energy shortfall clears it (SetUnitFlagBits at
         * 0x48b090, gated on `onoffable`), surplus re-sets it.
         * RWE's `activated && isSufficientlyPowered` is the equivalent
         * conjunction.
         */
        bool hasTargetingUpgrade{false};

        /** Heightmap cells currently inside this player's LOS. */
        Grid<unsigned char> losGrid;

        /**
         * One viewing unit's LOS disc stamp: the position and sight the
         * disc was computed from plus the flat losGrid cell indices it
         * marked. While both are unchanged the stamp is carried across
         * ticks without recomputation; when the unit moves, dies or
         * leaves the viewer set its cells are released.
         */
        struct LosViewerStamp
        {
            SimVector position;
            SimScalar sightSquared;
            std::vector<int> cells;
        };

        /**
         * Per-viewer stamp refcounts backing losGrid: a cell is
         * LOS-visible iff its count is nonzero. Lets stationary
         * viewers keep their stamps across ticks — only moved/removed
         * viewers recompute — while shared cells survive one viewer's
         * departure. Not serialized — rebuilds alongside losGrid after
         * a load.
         */
        Grid<unsigned short> losStampCount;
        std::unordered_map<UnitId, LosViewerStamp> losStampCache;

        /**
         * Heightmap cells this player has ever seen. Accumulates while
         * OTA `mapping` is enabled.
         */
        Grid<unsigned char> exploredGrid;
    };

    struct FireWeaponEvent
    {
        std::string weaponType;
        /**
         * The number of this shot within the weapon's current burst.
         * If this is the first shot of the burst, it will be 0.
         */
        int shotNumber;
        SimVector firePoint;
    };

    struct UnitArrivedEvent
    {
        UnitId unitId;
    };

    struct UnitActivatedEvent
    {
        UnitId unitId;
    };

    struct UnitDeactivatedEvent
    {
        UnitId unitId;
    };

    struct UnitCompleteEvent
    {
        UnitId unitId;
    };

    struct UnitSpawnedEvent
    {
        UnitId unitId;
    };

    struct UnitDiedEvent
    {
        UnitId unitId;
        std::string unitType;
        SimVector position;
        enum class DeathType
        {
            NormalExploded,
            WaterExploded,
            Deleted,
        };
        DeathType deathType;
        /** Resolved payload used for this death, shared by simulation and presentation. */
        std::string explosionWeapon;
    };

    struct UnitStartedBuildingEvent
    {
        UnitId unitId;
    };

    struct UnitStartedRepairingEvent
    {
        UnitId unitId;
    };

    struct UnitStartedReclaimingEvent
    {
        UnitId unitId;
    };

    struct UnitStartedCapturingEvent
    {
        UnitId unitId;
    };

    /**
     * Emitted when a unit's interactive self-destruct countdown
     * crosses a whole-second boundary (retail announces count5..count0
     * from the unit's [soundcategory] sound list).
     */
    struct UnitSelfDestructCountdownEvent
    {
        UnitId unitId;
        // seconds remaining, 0..5
        int secondsRemaining;
    };

    /**
     * Emitted when a unit's interactive self-destruct countdown is
     * cancelled (retail's "canceldestruct"/"Self destruct terminated").
     */
    struct UnitSelfDestructCancelledEvent
    {
        UnitId unitId;
    };

    struct EmitParticleFromPieceEvent
    {
        enum class SfxType
        {
            LightSmoke,
            BlackSmoke,
            Wake1,
        };

        SfxType sfxType;
        UnitId unitId;
        std::string pieceName;
    };

    struct ProjectileSpawnedEvent
    {
        ProjectileId projectileId;
    };

    struct ProjectileDiedEvent
    {
        ProjectileId projectileId;
        std::string weaponType;
        SimVector position;

        enum class DeathType
        {
            OutOfBounds,
            NormalImpact,
            WaterImpact,
            EndOfLife,
        };
        DeathType deathType;
    };

    struct FeatureEffectEvent
    {
        SimVector position;
        bool ignition; // false: presentation-only smoke on the three-tick cadence
    };

    using GameEvent = std::variant<
        FireWeaponEvent,
        UnitArrivedEvent,
        UnitActivatedEvent,
        UnitDeactivatedEvent,
        UnitCompleteEvent,
        EmitParticleFromPieceEvent,
        UnitSpawnedEvent,
        UnitDiedEvent,
        UnitStartedBuildingEvent,
        UnitStartedRepairingEvent,
        UnitStartedReclaimingEvent,
        UnitStartedCapturingEvent,
        UnitSelfDestructCountdownEvent,
        UnitSelfDestructCancelledEvent,
        ProjectileSpawnedEvent,
        ProjectileDiedEvent,
        FeatureEffectEvent>;


    struct UnitInfo
    {
        const UnitId id;
        UnitState* const state;
        const UnitDefinition* const definition;

        UnitInfo(UnitId id, UnitState* state, const UnitDefinition* definition)
            : id(id), state(state), definition(definition) {}
    };

    struct ConstUnitInfo
    {
        const UnitId id;
        const UnitState* const state;
        const UnitDefinition* const definition;

        ConstUnitInfo(UnitId id, const UnitState* state, const UnitDefinition* definition)
            : id(id), state(state), definition(definition) {}

        ConstUnitInfo(UnitInfo unitInfo)
            : id(unitInfo.id), state(unitInfo.state), definition(unitInfo.definition) {}
    };

    enum class ImpactType
    {
        Normal,
        Water
    };

    /**
     * Per-phase wall-clock nanoseconds accumulated inside tick() while
     * collectTickPhaseTimings is enabled. Profiling only — never read
     * by game logic, never serialized, excluded from computeHash.
     */
    struct TickPhaseTimings
    {
        uint64_t visibility{0};
        uint64_t wind{0};
        uint64_t meteorShower{0};
        uint64_t resources{0};
        uint64_t cloak{0};
        uint64_t campaignAi{0};
        uint64_t pathfinding{0};
        uint64_t unitBehavior{0};
        uint64_t pieces{0};
        uint64_t cob{0};
        uint64_t projectiles{0};
        uint64_t featureUpdates{0};
        uint64_t featureDamage{0};
        uint64_t veterancy{0};
        uint64_t veterancyAwards{0};
        uint64_t veterancyPromotions{0};
        uint64_t featureCollision{0};
        uint64_t corrosiveWater{0};
        uint64_t victoryAndObjectives{0};
        uint64_t deadUnitCleanup{0};
        uint64_t deadProjectileCleanup{0};
        uint64_t spawn{0};
        uint64_t tickCount{0};

        void reset() { *this = TickPhaseTimings(); }
    };

    struct GameSimulation
    {
        std::minstd_rand rng;

        WinStatus gameStatus{WinStatusUndecided()};

        MapTerrain terrain;

        std::unordered_map<std::string, UnitDefinition> unitDefinitions;

        SimpleVectorMap<FeatureDefinition, FeatureDefinitionIdTag> featureDefinitions;
        std::unordered_map<std::string, FeatureDefinitionId> featureNameIndex;

        std::unordered_map<std::string, UnitModelDefinition> unitModelDefinitions;

        std::unordered_map<std::string, CobScript> unitScriptDefinitions;

        std::unordered_map<std::string, WeaponDefinition> weaponDefinitions;

        MovementClassDatabase movementClassDatabase;
        MovementClassCollisionService movementClassCollisionService;

        PathFindingService pathFindingService;

        OccupiedGrid occupiedGrid;
        std::set<UnitId> flyingUnitsSet;

        StateHashGrid<unsigned char> metalGrid;

        StateHashGrid<bool> geoGrid;

        std::vector<GamePlayerInfo> players;

        /**
         * OTA `lineofsight` toggle. When true, an enemy unit is only
         * detected by a player while inside that player's sight radius,
         * and weapon auto-acquisition, rendering and picking honor
         * detection. When false every unit counts as detected, matching
         * retail's `lineofsight=0` skirmish maps.
         */
        bool lineOfSightEnabled{false};

        /**
         * The raw OTA `lineofsight` value (0=Permanent, 1=True,
         * 2=Circular in retail's SKIRMISH select staging), retained so
         * the in-game GAMEOPTIONS screen can report the authored mode.
         */
        int lineOfSightMode{0};

        /**
         * OTA `mapping` toggle. When true, terrain a player has seen
         * stays explored (dimmed) after LOS leaves it; when false,
         * terrain outside current LOS renders dark.
         */
        bool mappingEnabled{false};

        /**
         * Per-player live-unit cap (retail `maxunits`/`UnitLimit`,
         * default 250, clamped [20,500] at OTA parse). Enforced in
         * trySpawnUnit against live (non-dead) units owned by the
         * spawning player; nanoframes under construction and cargo
         * aboard transports count.
         */
        unsigned int maxUnitsPerPlayer{250};

        /**
         * OTA `nosealeveltrigger` (retail missionstate+0xd48). When
         * set, projectiles are not destroyed on crossing below sea
         * level; retail gates the destroy path on it
         * (TotalA.exe 0x49b3c0). `waterWeapon` projectiles are exempt
         * regardless (0x49b3a1). Retail also uses the flag to suppress
         * water-splash presentation, which RWE does not model.
         */
        bool noSeaLevelTrigger{false};

        /** Raw OTA gravity, used by AirStrike's release lead. */
        int mapGravity{112};
        /** Published wind displacement per tick, consumed by dropped projectiles. */
        SimVector projectileWind{0_ss, 0_ss, 0_ss};
        SimScalar droppedProjectileGravity() const;

        /**
         * OTA `waterdoesdamage`/`waterdamage` (retail missionstate
         * +0xd4c/+0xd50). When armed, every 30th sim tick applies
         * `waterDamage` to each live unit whose footprint terrain
         * height is at or below sea level (retail unit+0x70);
         * hovercraft are exempt (TotalA.exe 0x48aed3..0x48af32,
         * damage type 0xb, no attacker). Derived from the OTA at load;
         * never serialized.
         */
        bool waterDoesDamage{false};
        int waterDamage{0};

        std::vector<PlayerVisibility> playerVisibility;

        /**
         * Present when this simulation is running a campaign mission
         * rather than a skirmish. Suppresses skirmish victory semantics
         * (commander-death kill, last-player-standing) and carries
         * mission metadata for later campaign phases.
         */
        std::optional<MissionInfo> missionInfo;

        /**
         * Campaign objective evaluator; present only when missionInfo
         * is set. Produces the mission's victory/defeat result with
         * retail TotalA.exe semantics. Skirmish games leave this empty
         * and use gameStatus instead.
         */
        std::optional<MissionObjectiveEvaluator> missionObjectives;

        /**
         * Per-computer-player campaign AI state (retail aiBlock at
         * player+0x1b63). Populated by LoadingScene only for campaign
         * missions; skirmish computer players have no states and stay
         * inert, matching retail's profile-driven campaign AI scope.
         * Driven by updateCampaignAi() inside tick().
         */
        std::vector<CampaignAiPlayerState> campaignAi;

        /**
         * Retail campaign difficulty economy coefficient for
         * computer-owned units (TotalA.exe ctx+0x37eee, gated by
         * owner type+0x73==2): Easy=0.5, Medium=0.7, Hard/other=1.0.
         * Retail multiplies every contribution into the per-unit
         * production accumulators (unit+0xbc energy, unit+0xd4 metal)
         * by this factor: resource income, nanolathe-class payments
         * and the starting grant all scale. Upkeep use costs, weapon
         * per-shot costs and storage capacity route through unscaled
         * accumulators (unit+0xc0/+0xc4) and are unaffected.
         * Everywhere 1.0 unless a difficulty economy scale applies
         * (campaign always; skirmish via the Difficulty toggle).
         */
        float computerResourceScale{1.0f};

        /**
         * Retail skirmish `CommanderDeath=Continues` (live copy of
         * missionstate+0x39219, TotalA.exe 0x14281-adjacent field
         * +0x37ef6). When false (Game ends), a dying commander
         * eliminates its owner. When true, commander loss does not
         * eliminate; the player dies only when their last live unit
         * is gone.
         */
        bool commanderDeathContinues{false};

        VectorMap<MapFeature, FeatureIdTag> features;
        // Activation order, oldest first. Only event instances are visited each tick.
        std::vector<FeatureId> activeFeatures;

        VectorMap<UnitState, UnitIdTag> units;

        VectorMap<Projectile, ProjectileIdTag> projectiles;

        std::deque<PathRequest> pathRequests;

        std::deque<UnitId> unitCreationRequests;

        GameTime gameTime{0};

        std::vector<GameEvent> events;
        // Synchronously drained at the outer death call; never pending at a save/tick boundary.
        std::deque<Projectile> deathExplosions;
        bool drainingDeathExplosions{false};

        SimScalar currentWindGenerationFactor{0_ss};

        const int minWindSpeed;

        const int maxWindSpeed;

        const int tidalStrength;

        GameTime nextWindSpeedChange;

        /**
         * OTA meteor-shower hazard state (retail TotalA.exe globals
         * 0x502c38..0x502c88). Applies the schema's Meteor*
         * configuration; drives environmental weapon-projectile
         * strikes from updateMeteorShower() inside tick().
         */
        MeteorShower meteorShower;

        explicit GameSimulation(MapTerrain&& terrain, unsigned char surfaceMetal, int minWindSpeed, int maxWindSpeed, int tidalStrength = 0);

        /**
         * Applies the resolved meteor-shower configuration and
         * initialises the scheduler exactly like retail's apply+init
         * pair (TotalA.exe 0x418ed0 + 0x418e50): timing fields convert
         * seconds->ticks at 30Hz, the first strike is scheduled for
         * hitStepTicks from game start, and the weapon resolves by
         * (uppercased) name only if it exists and carries the TDF
         * `meteor` flag.
         *
         * Must run before a saved game is overlaid: the save restores
         * the mutable scheduler fields.
         */
        void configureMeteorShower(const MeteorShowerParams& params, bool enabled);

        /**
         * Per-tick hazard scheduler (retail TotalA.exe 0x418f50,
         * called from the frame update at 0x47ccbd). Strike
         * target/origin rolls consume simulation RNG even while the
         * shower is disabled, matching retail.
         */
        void updateMeteorShower();

        std::optional<FeatureId> addFeature(MapFeature&& newFeature);

        std::optional<FeatureId> addFeature(FeatureDefinitionId featureType, int heightmapX, int heightmapZ);

        PlayerId addPlayer(const GamePlayerInfo& info);

        std::optional<UnitId> trySpawnUnit(const std::string& unitType, PlayerId owner, const SimVector& position, std::optional<SimAngle> rotation, bool allowOverlap = false, std::optional<UnitId> ignoreUnit = std::nullopt, bool bypassUnitLimit = false);

        /**
         * Live (non-dead) units owned by `owner`. The retail maxunits
         * cap counts units under construction and cargo aboard
         * transports; dead husks awaiting cleanup do not count.
         */
        unsigned int countLiveUnitsOwnedBy(PlayerId owner) const;

        /**
         * Returns true if the unit was really added, false otherwise.
         * A unit might not be added because it violates collision constraints.
         * allowOverlap skips that rejection: retail places authored
         * mission units unconditionally, including units stacked on
         * blocking features, other units, or partially off the map edge.
         */
        std::optional<UnitId> tryAddUnit(UnitState&& unit, bool allowOverlap = false, std::optional<UnitId> ignoreUnit = std::nullopt);

        /**
         * The single authoritative ownership-transfer path (retail
         * TotalA.exe 0x488570, invoked by Ground_Capture completion).
         * Retail rebuilds the unit under the new owner; RWE keeps the
         * entity and resets the volatile state the rebuild discards:
         * orders, weapon locks, factory/build-queue state, navigation
         * and foreign references (other units' weapon targets and
         * orders aimed at the unit). Position, rotation, hit points,
         * construction state, activation and mission metadata are
         * preserved.
         *
         * The mission objective event is dispatched before the owner
         * field changes so the evaluator observes `previousOwner`
         * (CaptureUnitType latch semantics).
         *
         * `performedBy` exempts the capturing unit's own order queue —
         * its running CaptureOrder completes normally on return.
         * Returns false when the unit is missing, dead or already
         * owned by `newOwner` (retail early-outs on same owner).
         */
        bool transferUnitOwnership(UnitId unitId, PlayerId newOwner, std::optional<UnitId> performedBy = std::nullopt);

        /**
         * The result of evaluating a building footprint against the terrain.
         * Follows retail placement semantics: yardmap cells are split into
         * support cells (o f O c y G), water cells (w C Y) and ignored
         * cells (.). Support cells rest on the terrain, all-water footprints
         * float with their base at seaLevel - waterline.
         */
        struct BuildingPlacement
        {
            /**
             * Authoritative world-space Y for the unit origin at this spot.
             * Computed even when the placement itself is invalid.
             */
            SimScalar baseY;
            bool valid;
        };

        /**
         * Evaluates whether a unit with the given movement class attributes
         * could be built at the given location on the map -- i.e. it is valid
         * terrain for the unit, it is not occupied by something else, and it
         * contains geo if required. Also computes the authoritative Y position
         * the structure's origin should be placed at.
         * Returns std::nullopt when the footprint is outside the map.
         */
        std::optional<BuildingPlacement> computeBuildingPlacement(const MovementClassDefinition& mc, const std::optional<Grid<YardMapCell>>& yardMap, bool yardMapContainsGeo, unsigned int waterline, unsigned int x, unsigned int y) const;

        DiscreteRect computeFootprintRegion(const SimVector& position, unsigned int footprintX, unsigned int footprintZ) const;

        DiscreteRect computeFootprintRegion(const SimVector& position, const UnitDefinition::MovementCollisionInfo& collisionInfo) const;

        bool anyFeatureOccupies(const DiscreteRect& rect) const;

        bool containsAnyGeoMatch(const Grid<YardMapCell>& yardMap, unsigned int x, unsigned int y) const;

        bool isCollisionAt(const DiscreteRect& rect) const;

        bool isCollisionAt(const GridRegion& region) const;

        bool isCollisionAt(const DiscreteRect& rect, UnitId self) const;

        bool isYardmapBlocked(unsigned int x, unsigned int y, const Grid<YardMapCell>& yardMap, bool open, UnitId self) const;

        bool isAdjacentToObstacle(const DiscreteRect& rect) const;

        void showObject(UnitId unitId, const std::string& name);

        void hideObject(UnitId unitId, const std::string& name);

        void enableShading(UnitId unitId, const std::string& name);

        void disableShading(UnitId unitId, const std::string& name);

        UnitState& getUnitState(UnitId id);

        const UnitState& getUnitState(UnitId id) const;

        UnitInfo getUnitInfo(UnitId id);

        ConstUnitInfo getUnitInfo(UnitId id) const;

        std::optional<std::reference_wrapper<UnitState>> tryGetUnitState(UnitId id);

        std::optional<std::reference_wrapper<const UnitState>> tryGetUnitState(UnitId id) const;

        std::optional<std::reference_wrapper<const UnitState>> tryGetUnitState(CobUnitId id) const;

        bool unitExists(UnitId id) const;

        MapFeature& getFeature(FeatureId id);

        const MapFeature& getFeature(FeatureId id) const;

        std::optional<std::reference_wrapper<MapFeature>> tryGetFeature(FeatureId id);

        std::optional<std::reference_wrapper<const MapFeature>> tryGetFeature(FeatureId id) const;

        /**
         * Removes a feature from the world, freeing its footprint cells.
         * Safe to call on a stale id; no-op if the feature is gone.
         */
        void removeFeature(FeatureId id);
        void damageFeature(FeatureId id, const WeaponDefinition& weapon);
        void transitionFeature(FeatureId id, FeatureEvent cause);
        void completeFeatureTransition(FeatureId id, FeatureEvent cause);
        bool igniteFeature(FeatureId id);
        void updateFeatures();
        void featureBurnEvent(FeatureId id);

        /**
         * Applies `workerTimePerTick` units of reclaim work to the feature and
         * returns the metal/energy amounts newly credited for this tick.
         * The credit split is proportional to the definition's metal/energy
         * pool; integer math guarantees the totals never exceed the pool.
         * Returns std::nullopt if the feature does not exist.
         */
        std::optional<std::pair<Energy, Metal>> applyReclaimProgress(FeatureId id, unsigned int workerTimePerTick);

        /**
         * True when the feature's reclaim pool is fully drained.
         */
        bool reclaimComplete(const MapFeature& feature) const;

        GamePlayerInfo& getPlayer(PlayerId player);

        const GamePlayerInfo& getPlayer(PlayerId player) const;

        /**
         * Recomputes every player's visibility sets and grids from
         * current unit positions. Runs at the top of each tick so that
         * behavior code sees detection state consistent with the
         * positions entered this tick.
         */
        void updateVisibility();

        /**
         * True if `unit` is currently inside `viewer`'s line of sight.
         * Always true for the viewer's own units and when
         * `lineOfSightEnabled` is false.
         */
        bool isLosVisibleToPlayer(UnitId unit, PlayerId viewer) const;

        /**
         * True if `unit` is currently detected by `viewer`'s radar but
         * not inside LOS. Always false when `lineOfSightEnabled` is
         * false (in that case units are fully visible instead).
         */
        bool isRadarDetectedByPlayer(UnitId unit, PlayerId viewer) const;

        /**
         * True if `unit` is currently detected by `viewer`'s sonar but
         * not inside LOS. Always false when `lineOfSightEnabled` is
         * false (in that case units are fully visible instead).
         */
        bool isSonarDetectedByPlayer(UnitId unit, PlayerId viewer) const;

        /**
         * True if `unit` should be treated as known to `viewer` at all:
         * owned by the viewer, LOS-visible, or a radar/sonar contact.
         * Governs whether the unit renders on the minimap.
         */
        bool isDetectedByPlayer(UnitId unit, PlayerId viewer) const;

        /**
         * True if `viewer`'s weapons may auto-acquire `unit`: it is
         * LOS-visible, or it is a radar-only contact while the viewer
         * has an active targeting facility (retail Targeting Facility
         * upgrade semantics — radar contacts become weapon targets).
         */
        bool isWeaponTargetableByPlayer(UnitId unit, PlayerId viewer) const;

        /**
         * True if the unit's active sensor systems (radar/sonar
         * emissions, jamming, and the selected-unit minimap range
         * circles) are on. Retail gates all of these on `unit+0x10e`
         * bit0, the powered-and-active composite that the per-tick
         * energy gate force-clears on shortfall; the clear path only
         * applies to `onoffable` units, so non-onoffable emitters are
         * always on. RWE's equivalent conjunction is
         * `activated && isSufficientlyPowered` for onoffable units.
         */
        static bool isUnitSensorOn(const UnitState& unit, const UnitDefinition& def);

        /**
         * The per-second cloak upkeep (retail TotalA.exe 0x401360,
         * driven by the same enclosing bookkeeping pass as the sensor
         * sweep at 0x467440). For each unit with `cloakRequested`:
         * a detected non-allied unit inside `mincloakdistance`
         * suppresses cloak and pushes `cloakBlockedUntil` 90 ticks out
         * (the ~3s decloak dwell); otherwise `cloakCost` (or
         * `cloakCostMoving` while in motion) is truncated to an
         * integer and debited from the owner's energy pool — on
         * shortfall effective cloak drops but the request persists,
         * so the unit recloaks automatically once energy recovers.
         */
        void updateCloak();

        void moveObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar position, SimScalar speed);

        void moveObjectNow(UnitId unitId, const std::string& name, SimAxis axis, SimScalar position);

        void turnObject(UnitId unitId, const std::string& name, SimAxis axis, SimAngle angle, SimScalar speed);

        void turnObjectNow(UnitId unitId, const std::string& name, SimAxis axis, SimAngle angle);

        void spinObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar speed, SimScalar acceleration);

        void stopSpinObject(UnitId unitId, const std::string& name, SimAxis axis, SimScalar deceleration);

        bool isPieceMoving(UnitId unitId, const std::string& name, SimAxis axis) const;

        bool isPieceTurning(UnitId unitId, const std::string& name, SimAxis axis) const;

        std::optional<SimVector> intersectLineWithTerrain(const Line3x<SimScalar>& line) const;

        void moveUnitOccupiedArea(const DiscreteRect& oldRect, const DiscreteRect& newRect, UnitId unitId);

        /**
         * Attach `unitId` as cargo aboard `transportId`, matching the
         * group-join semantics used by the retail `i` InitialMission
         * command: the target must be a transport that is not itself
         * carried, and the unit must not already be aboard or carrying
         * others. On success the unit is removed from terrain occupancy,
         * linked to the head of the transport's passenger list, and its
         * position pins to the carrier. Returns true when it boarded.
         */
        bool tryBoardTransport(UnitId unitId, UnitId transportId, int slot = -1);

        /**
         * Place a carried unit back on the map at `position`.
         * Unlinks it from its carrier, restores terrain occupancy and
         * snaps to terrain height. The caller must have already
         * determined that the footprint fits at `position`.
         * Returns true when the unit disembarked.
         */
        bool tryDisembarkUnit(UnitId unitId, const SimVector& position);

        void requestPath(UnitId unitId);

        Projectile createProjectileFromWeapon(PlayerId owner, const UnitWeapon& weapon, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit);

        Projectile createProjectileFromWeapon(PlayerId owner, const std::string& weaponType, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit, std::optional<SimVector> targetPosition = std::nullopt);

        void spawnProjectile(PlayerId owner, const UnitWeapon& weapon, const SimVector& position, const SimVector& direction, SimScalar distanceToTarget, std::optional<UnitId> targetUnit, std::optional<UnitId> firedBy, std::optional<ProjectileId> targetProjectile = std::nullopt, std::optional<SimVector> targetPosition = std::nullopt);

        /**
         * Retail interceptor acquisition (TotalA.exe 0x49d120):
         * scans live projectiles and returns the first enemy
         * `targetable` missile inside the weapon's coverage box on
         * both axes that no other live projectile is already
         * intercepting. Same-owner missiles are skipped; allied
         * missiles remain valid targets as in retail.
         */
        std::optional<ProjectileId> findInterceptorTarget(const UnitState& unit, const WeaponDefinition& weaponDefinition) const;

        WinStatus computeWinStatus() const;

        /**
         * Retail alliedWith test: true when both players carry the same
         * ally group (a non-empty value); a player is always allied
         * with themselves. Players without a group ally with nobody
         * else (TotalA.exe 0x446e90 -> record+0x108 table).
         */
        bool areAllied(PlayerId a, PlayerId b) const;

        /**
         * Build the campaign objective evaluator from missionInfo.
         * No-op unless running a campaign mission.
         */
        void initMissionObjectives();

        /** Campaign mission result; always InProgress for skirmish. */
        MissionResult getMissionResult() const;

        bool addResourceDelta(const UnitId& unitId, const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal);
        bool addResourceDelta(const UnitId& unitId, const Energy& energy, const Metal& metal);

        /**
         * addResourceDelta multiplied by the unit owner's campaign
         * difficulty coefficient (computerResourceScale). Use for the
         * flows retail routes through its difficulty-scaled per-unit
         * production accumulators: resource income, reclaim credits
         * and nanolathe-class build/repair payments. Upkeep use costs
         * and weapon per-shot costs are unscaled in retail and must
         * use plain addResourceDelta.
         */
        bool addScaledResourceDelta(const UnitId& unitId, const Energy& energy, const Metal& metal);
        bool addScaledResourceDelta(const UnitId& unitId, const Energy& apparentEnergy, const Metal& apparentMetal, const Energy& actualEnergy, const Metal& actualMetal);

        bool trySetYardOpen(const UnitId& unitId, bool open);

        void emitBuggerOff(const UnitId& unitId);

        void tellToBuggerOff(const UnitId& unitId, const DiscreteRect& rect);

        GameHash computeHash() const;

        /**
         * Save/load support: recompute the derived occupancy index
         * (occupiedGrid + flyingUnitsSet) after the units/features slot
         * tables were wholesale-restored by a save. Call once after
         * restore, before any tick runs.
         */
        void rebuildOccupancyAfterLoad();

        void activateUnit(UnitId unitId);

        void deactivateUnit(UnitId unitId);

        void quietlyKillUnit(UnitId unitId);

        Matrix4x<SimScalar> getUnitPieceLocalTransform(UnitId unitId, const std::string& pieceName) const;

        Matrix4x<SimScalar> getUnitPieceTransform(UnitId unitId, const std::string& pieceName) const;

        SimVector getUnitPiecePosition(UnitId unitId, const std::string& pieceName) const;

        /**
         * World position a passenger rides at when attached to `transport`
         * at COB piece `slot`. Invalid slots (-1 or out of range) ride at
         * the carrier's origin, matching retail 0x43def0.
         */
        SimVector computeAttachPosition(const UnitState& transport, int slot) const;

        void setBuildStance(UnitId unitId, bool value);

        void setYardOpen(UnitId unitId, bool value);

        void setBuggerOff(UnitId unitId, bool value);

        MovementClassDefinition getAdHocMovementClass(const UnitDefinition::MovementCollisionInfo& info) const;

        std::pair<unsigned int, unsigned int> getFootprintXZ(const UnitDefinition::MovementCollisionInfo& info) const;

        BoundingBox3x<SimScalar> createBoundingBox(const UnitState& unit) const;

        void killUnit(UnitId unitId, UnitDeathCause cause = UnitDeathCause::Normal);
        void selfDestructUnit(UnitId unitId);

        /** Debrief stats: owner loss + enemy kill credit via lastDamagedBy. */
        void recordUnitKilledForStats(const UnitState& unit);
        void creditUnitVeterancy(const UnitState& unit);

        /**
         * Applies damage to a unit. OTA `Immunity=1` (missionState)
         * skips the damage entirely -- matching retail, where the
         * immunity bit is only consulted while *selecting* targets and
         * iterating area-damage sweeps, never inside the damage
         * routine itself. Callers issuing forced/owner-initiated
         * damage that retail does not route through a sweep (the
         * SELFDESTRUCT countdown) must pass bypassImmunity=true so the
         * 30000-point self-damage lands on immune units as well.
         */
        void applyDamage(UnitId unitId, unsigned int damagePoints, std::optional<UnitId> attacker = std::nullopt, bool bypassImmunity = false, bool paralyzer = false, UnitDeathCause cause = UnitDeathCause::Normal, std::optional<uint16_t> attackerKills = std::nullopt);

        void applyDamageInRadius(const SimVector& position, SimScalar radius, const Projectile& projectile);

        void doProjectileImpact(const Projectile& projectile, ImpactType impactType);

        void updateProjectiles();

        void killPlayer(PlayerId playerId);

        void processVictoryCondition();

        void updateWind();

        void updateResources();

        /**
         * Retail corrosive-water mechanic (OTA waterdoesdamage /
         * waterdamage; TotalA.exe 0x48aed3..0x48af32). Every 30th tick
         * each live unit whose footprint terrain height is at or below
         * sea level takes `waterDamage`; hovercraft are exempt.
         */
        void updateCorrosiveWater();

        void trySpawnFeature(const std::string& featureType, const SimVector& position, SimAngle rotation);

        void deleteDeadUnits();

        void deleteDeadProjectiles();

        void spawnNewUnits();

        /**
         * Lazily-computed per-tick cache of weapon auto-acquisition
         * candidates: for each player, the enemy units eligible under
         * the acquisition scan's structural predicates, stored in
         * `units` iteration order. Scanning the owner's list yields the
         * same first-match target as scanning `units` — the in-loop
         * predicates are re-evaluated per candidate at scan time, so
         * mid-tick kills/cargo drops are handled; owner transfers and
         * mid-tick spawns invalidate the whole cache and force a
         * rebuild. Not serialized, not hashed.
         */
        std::optional<GameTime> weaponTargetCandidatesComputedAt;
        std::vector<std::vector<UnitId>> weaponTargetCandidatesByPlayer;

        /**
         * Auto-acquisition candidates for `viewer` this tick (may be
         * stale-superset on kills; never misses a candidate the scan
         * would test). See weaponTargetCandidatesByPlayer.
         */
        const std::vector<UnitId>& getWeaponTargetCandidates(PlayerId viewer);

        /** Drop the cached candidate lists; rebuilt on next query. */
        void invalidateWeaponTargetCandidates();

        // Derived indexes, maintained on spawn/death/dock/release and rebuilt on load.
        // No approaching aircraft reserves a pad in retail.
        std::set<UnitId> airServiceFacilities;
        std::map<std::pair<UnitId, int>, UnitId> airServiceOccupants;
        void rebuildAirServiceIndexes();
        void releaseAirService(UnitId aircraft);
        void destroyAirServiceDependents(UnitId facility, UnitDeathCause cause);


        /**
         * Profiling hook for the benchmark harness. When true, tick()
         * accumulates per-phase wall-clock nanoseconds into
         * tickPhaseTimings. Inert when false (one branch per phase).
         */
        bool collectTickPhaseTimings{false};
        TickPhaseTimings tickPhaseTimings;

        void tick();

        std::optional<FeatureDefinitionId> tryGetFeatureDefinitionId(const std::string& featureName) const;

        const FeatureDefinition& getFeatureDefinition(FeatureDefinitionId featureDefinitionId) const;
    };

    /**
     * Construct a UnitState for `unitType` (pieces + COB environment
     * built from the loaded definitions) without registering it in
     * the simulation. Used by trySpawnUnit and by save/load restore.
     */
    UnitState createUnit(
        GameSimulation& simulation,
        const std::string& unitType,
        PlayerId owner,
        const SimVector& position,
        std::optional<SimAngle> rotation);
}
