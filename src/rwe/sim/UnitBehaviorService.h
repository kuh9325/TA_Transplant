#pragma once

#include <optional>
#include <rwe/pathfinding/PathFindingService.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/ProjectilePhysicsType.h>
#include <rwe/sim/SimAngle.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitId.h>
#include <rwe/sim/UnitOrder.h>
#include <rwe/sim/UnitState.h>
#include <rwe/sim/UnitWeapon.h>
#include <string>
#include <utility>

namespace rwe
{
    class GameScene;

    class UnitBehaviorService
    {
    private:
        GameSimulation* const sim;

    public:
        UnitBehaviorService(GameSimulation* sim);
        void updateSelfDestruct(UnitId unitId);

        void onCreate(UnitId unitId);

        void updateWind(SimScalar windSpeed, SimAngle windDirection);

        void update(UnitId unitId);

        // FIXME: shouldn't really be public
        SimVector getSweetSpot(UnitId id);
        std::optional<SimVector> tryGetSweetSpot(UnitId id);

    private:
        /** Returns true if the order has been completed. */
        bool handleOrder(UnitInfo unitInfo, UnitOrder& moveOrder);
        bool startAircraftService(UnitInfo unit, UnitId facility, bool automatic);
        bool seekAircraftService(UnitInfo unit);
        bool updateAircraftService(UnitInfo unit, bool blocked);
        void steerAircraftService(UnitInfo unit);
        std::optional<int> queryFreeAirPad(UnitId facility, UnitId aircraft);
        bool isAirPadFree(UnitId facility, int piece, UnitId aircraft) const;
        bool validAirFacility(UnitInfo aircraft, UnitId facility) const;
        bool applyRepairStep(UnitInfo repairer, UnitInfo patient, unsigned int* progress);


        /** Returns true if the order has been completed. */
        bool handleMoveOrder(UnitInfo unitInfo, MoveOrder& moveOrder);

        /** Returns true if the order has been completed. */
        bool handleAttackOrder(UnitInfo unitInfo, const AttackOrder& attackOrder);

        /**
         * Drives the unit's command-fire (D-Gun) weapon onto the order's
         * target: approaches like an attack until inside the weapon's
         * own range, then aims and fires that weapon only. The order is
         * strictly one-shot and completes once the shot launches, for
         * unit and position targets alike.
         */
        bool handleCommandFireOrder(UnitInfo unitInfo, CommandFireOrder& order);

        /**
         * Returns the index of the unit's command-fire weapon -- the
         * weapon whose definition carries the `commandfire` TDF flag,
         * matching the retail "command fire weapon" pointer cached on
         * `candgun` units -- or nothing when the unit has none.
         */
        std::optional<unsigned int> findCommandFireWeaponIndex(const UnitState& unit) const;

        /** Returns true if the order has been completed. */
        bool handleBuildOrder(UnitInfo unitInfo, const BuildOrder& buildOrder);

        /** Returns true if the order has been completed. */
        bool handleBuggerOffOrder(UnitInfo unitInfo, const BuggerOffOrder& buggerOffOrder);

        /** Returns true if the order has been completed. */
        bool handleCompleteBuildOrder(UnitInfo unitInfo, const CompleteBuildOrder& buildOrder);

        /** Returns true if the order has been completed. */
        bool handleRepairOrder(UnitInfo unitInfo, const RepairOrder& repairOrder);

        /** Returns true if the order has been completed or abandoned. */
        bool handleReclaimOrder(UnitInfo unitInfo, const ReclaimOrder& reclaimOrder);

        bool handleCaptureOrder(UnitInfo unitInfo, CaptureOrder& order);

        bool handleGuardOrder(UnitInfo unitInfo, const GuardOrder& guardOrder);

        /**
         * Drives the unit toward the current patrol waypoint and advances
         * the waypoint cycle on arrival. Never completes on its own;
         * returns true (dropping the order) only when the unit cannot move
         * or the route is empty.
         */
        bool handlePatrolOrder(UnitInfo unitInfo, PatrolOrder& patrolOrder);

        /**
         * Retail RepairPatrol candidate scan (TotalA.exe HelpBuild,
         * 0x405980): collects allied assist/repair/reclaim work inside
         * the builder's buildDistance under the retail resource gates
         * and returns a random eligible candidate.
         */
        std::optional<PatrolDivertTarget> scanPatrolWork(UnitInfo unitInfo);

        /** Returns true once the wait duration has fully elapsed. */
        bool handleWaitOrder(UnitInfo unitInfo, WaitOrder& waitOrder);

        bool handleParalyzeOrder(UnitInfo unitInfo, ParalyzeOrder& order);

        /** Returns true once the watched unit has been attacked or destroyed. */
        bool handleWaitForAttackOrder(UnitInfo unitInfo, WaitForAttackOrder& waitOrder);

        /** Marks the mission unit selectable. Always completes immediately. */
        bool handleMakeSelectableOrder(UnitInfo unitInfo, const MakeSelectableOrder& order);

        /**
         * Drives the unit to attack enemies of the order's unit type.
         * Completes when a scan finds no living matching enemy.
         */
        bool handleAttackUnitTypeOrder(UnitInfo unitInfo, AttackUnitTypeOrder& order);

        /**
         * Picks the best enemy unit of the given type: nearest
         * distance-squared with simulation-RNG jitter, preferring the
         * later unit on ties. Omniscient like retail; ignores LOS.
         */
        std::optional<UnitId> findBestUnitTypeTarget(const UnitInfo& unitInfo, const std::string& unitType);

        /**
         * Drives a mobile constructor through normal build mechanics
         * with bounded placement retries. Completes on success or
         * when attempts are exhausted.
         */
        bool handleMissionBuildOrder(UnitInfo unitInfo, MissionBuildOrder& order);

        /**
         * Enqueues the requested production run on the factory's build
         * queue, then completes once the queue has fully drained.
         */
        bool handleMissionFactoryBuildOrder(UnitInfo unitInfo, MissionFactoryBuildOrder& order);

        /**
         * Runs the self-destruct countdown, then the random fuse,
         * then detonates through the normal damage path.
         * Returns true once the unit has been destroyed.
         */
        bool handleSelfDestructOrder(UnitInfo unitInfo, SelfDestructOrder& order);

        /**
         * Advances one self-destruct state by a tick: decrements the
         * countdown (emitting a countdown event at each whole-second
         * boundary), then lights the random fuse, then applies lethal
         * self-damage. Returns true once the unit has been destroyed.
         * Shared by the queued `d`/SELFDESTRUCTFG order and the
         * interactive SELFDESTRUCT unit state.
         */
        bool tickSelfDestruct(UnitInfo unitInfo, SelfDestructOrder& selfDestruct);
        void updateKamikazeStandby(UnitInfo unitInfo);
        bool isBomber(UnitInfo unitInfo) const;
        void updateBomberStandby(UnitInfo unitInfo);
        bool attackBomberTarget(UnitInfo unitInfo, const AttackTarget& target);
        void updateDroppedWeapon(UnitId id, unsigned int weaponIndex);
        bool attackKamikazeTarget(UnitInfo unitInfo, const AttackTarget& target);

        /**
         * Drops a single carried unit at the order destination.
         * Returns true once a passenger disembarked, there is nothing
         * left to unload, or placement retries are exhausted.
         */
        bool handleUnloadOrder(UnitInfo unitInfo, UnloadOrder& order);

        bool handleLoadOrder(UnitInfo unitInfo, LoadOrder& order);

        /** Per-tick interactive Load target validation (retail Pickup state 0). */
        bool isLoadTargetValid(UnitInfo unitInfo, const UnitState& target) const;

        /** Ground_Pickup path: approach + script-driven attach (retail 0x406780). */
        bool handleGroundLoadOrder(UnitInfo unitInfo, LoadOrder& order, const UnitState& target);

        /** VTOL_Pickup path: descend, QueryTransport, slot attach, climb (retail 0x4111b0). */
        bool handleVtolLoadOrder(UnitInfo unitInfo, LoadOrder& order, const UnitState& target);

        /**
         * Finds a free footprint position for the given definition
         * nearest to `destination`, scanning cells in expanding rings.
         */
        std::optional<SimVector> findUnloadPosition(UnitInfo unitInfo, const UnitDefinition& passengerDefinition, UnloadOrder& order);

        /**
         * Stockpiles ammunition for the unit's stockpile weapon,
         * draining build costs over the weapon's reload period.
         * Runs as a background order. Returns true when the requested
         * count has been built or the unit has no stockpile weapon.
         */
        bool handleBuildWeaponOrder(UnitInfo unitInfo, BuildWeaponOrder& order);

        bool handleBuild(UnitInfo unitInfo, const std::string& unitType);

        void clearBuild(UnitInfo unitInfo);

        void updateWeapon(UnitId id, unsigned int weaponIndex);

        SimVector changeDirectionByRandomAngle(const SimVector& direction, SimAngle maxAngle);

        void tryFireWeapon(UnitId id, unsigned int weaponIndex);

        SimVector getUnitPositionWithCache(UnitState& s, UnitId unitId);

        void updateNavigation(UnitInfo unitInfo);

        void applyUnitSteering(UnitInfo unitInfo);
        void updateUnitRotation(UnitInfo unitInfo);
        void updateUnitSpeed(UnitInfo unitInfo);

        void updateGroundUnitPosition(UnitInfo unitInfo, const UnitPhysicsInfoGround& physics);
        void updateUnitPosition(UnitInfo unitInfo);

        bool tryApplyMovementToPosition(UnitInfo unitInfo, const SimVector& newPosition);

        std::optional<int> runCobQuery(UnitId id, const std::string& name);

        SimVector getAimingPoint(UnitId id, unsigned int weaponIndex);
        SimVector getLocalAimingPoint(UnitId id, unsigned int weaponIndex);

        SimVector getLocalFiringPoint(UnitId id, unsigned int weaponIndex);

        SimVector getNanoPoint(UnitId id);

        SimVector getPieceLocalPosition(UnitId id, unsigned int pieceId);
        SimVector getPiecePosition(UnitId id, unsigned int pieceId);

        SimAngle getPieceXZRotation(UnitId id, unsigned int pieceId);

        struct BuildPieceInfo
        {
            SimVector position;
            SimAngle rotation;
        };

        BuildPieceInfo getBuildPieceInfo(UnitId id);

        std::optional<SimVector> getTargetPosition(const UnitWeaponAttackTarget& target);

        std::optional<SimVector> getTargetPosition(const AttackTarget& target);

        PathDestination resolvePathDestination(UnitState& s, const MovingStateGoal& goal);

        void groundUnitMoveTo(UnitInfo unitInfo, const MovingStateGoal& goal);

        bool flyingUnitMoveTo(UnitInfo unitInfo, const MovingStateGoal& goal);

        NavigationResult navigateTo(UnitInfo unitInfo, const NavigationGoal& goal, SimScalar arrivalRadius = 8_ss, std::optional<SimVector> arrivalPosition = std::nullopt);

        void moveTo(UnitInfo unitInfo, const MovingStateGoal& goal);

        bool attackTarget(UnitInfo unitInfo, const AttackTarget& target);

        bool buildUnit(UnitInfo unitInfo, const std::string& unitType, const SimVector& position);

        UnitCreationStatus createNewUnit(UnitInfo unitInfo, const std::string& unitType, const SimVector& position);

        bool buildExistingUnit(UnitInfo unitInfo, UnitId targetUnitId);

        /**
         * True when the target's footprint perimeter is within the builder's
         * buildDistance. TA working range is measured to the target's edge,
         * not its center -- a short-range builder must be able to service a
         * large-footprint target it can only approach as far as the perimeter.
         */
        bool inWorkingRange(UnitInfo unitInfo, const UnitState& targetUnit, const UnitDefinition& targetUnitDefinition);

        /** Footprint-perimeter working-range check against a map feature. */
        bool inWorkingRange(UnitInfo unitInfo, const MapFeature& targetFeature, const FeatureDefinition& targetFeatureDefinition);

        /** Returns true if the repair order has been completed or abandoned. */
        bool repairExistingUnit(UnitInfo unitInfo, UnitId targetUnitId);

        /** Returns true if the reclaim has been completed or abandoned. */
        bool reclaimFeature(UnitInfo unitInfo, FeatureId targetFeatureId);

        /** Returns true if the reclaim has been completed or abandoned. */
        bool reclaimUnit(UnitInfo unitInfo, UnitId targetUnitId);

        /** Returns true if the capture order has been completed or abandoned. */
        bool captureUnit(UnitInfo unitInfo, CaptureOrder& order);

        /**
         * Removes the reclaimed feature and, if the definition names a
         * `featurereclamate` remnant, spawns it in place.
         */
        void finishReclaim(FeatureId targetFeatureId);

        /**
         * Consumes a fully-reclaimed live unit: marks it dead with no
         * corpse and emits a Deleted death event so listeners (selection,
         * GUI info) release their references. The unit leaves no wreck.
         */
        void finishReclaim(UnitId targetUnitId);

        void changeState(UnitState& unit, const UnitBehaviorState& newState);

        bool deployBuildArm(UnitInfo unitInfo, UnitId targetUnitId);

        bool deployRepairArm(UnitInfo unitInfo, UnitId targetUnitId);

        bool deployReclaimArm(UnitInfo unitInfo, const ReclaimTarget& target);

        bool deployCaptureArm(UnitInfo unitInfo, CaptureOrder& order);

        bool climbToCruiseAltitude(UnitInfo unitInfo);

        bool descendToGroundLevel(UnitInfo unitInfo);

        void transitionFromGroundToAir(UnitInfo unitInfo);
        bool tryTransitionFromAirToGround(UnitInfo unitInfo);

        bool flyTowardsGoal(UnitInfo unitInfo, const MovingStateGoal& goal);
    };
}
