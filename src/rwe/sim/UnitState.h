#pragma once

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <cstdint>
#include <rwe/cob/CobEnvironment.h>
#include <rwe/grid/DiscreteRect.h>
#include <rwe/math/Matrix4x.h>
#include <rwe/pathfinding/UnitPath.h>
#include <rwe/sim/Energy.h>
#include <rwe/sim/FeatureId.h>
#include <rwe/sim/MissionInfo.h>
#include <rwe/sim/Metal.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/sim/SimAngle.h>
#include <rwe/sim/SimAxis.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/SimVector.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitFireOrders.h>
#include <rwe/sim/UnitMesh.h>
#include <rwe/sim/UnitMoveOrders.h>
#include <rwe/sim/UnitOrder.h>
#include <rwe/sim/UnitWeapon.h>
#include <variant>

namespace rwe
{
    struct PathFollowingInfo
    {
        UnitPath path;
        GameTime pathCreationTime;
        std::vector<SimVector>::const_iterator currentWaypoint;
        explicit PathFollowingInfo(UnitPath&& path, GameTime creationTime)
            : path(std::move(path)), pathCreationTime(creationTime), currentWaypoint(this->path.waypoints.begin()) {}
    };

    struct NavigationGoalLandingLocation
    {
    };

    using NavigationGoal = std::variant<UnitId, SimVector, DiscreteRect, NavigationGoalLandingLocation>;

    using MovingStateGoal = std::variant<UnitId, SimVector, DiscreteRect>;

    using PathDestination = std::variant<SimVector, DiscreteRect>;

    struct UnitBehaviorStateIdle
    {
    };

    struct UnitCreationStatusPending
    {
    };

    struct UnitCreationStatusDone
    {
        UnitId unitId;
    };

    struct UnitCreationStatusFailed
    {
    };

    using UnitCreationStatus = std::variant<UnitCreationStatusPending, UnitCreationStatusFailed, UnitCreationStatusDone>;
    struct UnitBehaviorStateCreatingUnit
    {
        std::string unitType;
        PlayerId owner;
        SimVector position;
        UnitCreationStatus status{UnitCreationStatusPending()};
    };

    struct UnitBehaviorStateBuilding
    {
        UnitId targetUnit;
        std::optional<SimVector> nanoParticleOrigin;
    };

    struct UnitBehaviorStateRepairing
    {
        UnitId targetUnit;
        std::optional<SimVector> nanoParticleOrigin;

        /**
         * Virtual build-progress axis for this repair, in build-time ticks.
         * Anchored to the target's hit points when the state is entered and
         * re-anchored if the target takes damage while being repaired.
         */
        unsigned int repairProgress;
    };

    struct UnitBehaviorStateReclaiming
    {
        ReclaimTarget target;

        /**
         * World position of the reclaim target.
         * Features never move; unit targets are re-checked each tick.
         */
        SimVector targetPosition;

        std::optional<SimVector> nanoParticleOrigin;
    };

    struct UnitBehaviorStateCapturing
    {
        UnitId target;

        /** World position of the capture target; re-checked each tick. */
        SimVector targetPosition;

        std::optional<SimVector> nanoParticleOrigin;
    };

    using UnitBehaviorState = std::variant<
        UnitBehaviorStateIdle,
        UnitBehaviorStateCreatingUnit,
        UnitBehaviorStateBuilding,
        UnitBehaviorStateRepairing,
        UnitBehaviorStateReclaiming,
        UnitBehaviorStateCapturing>;

    struct NavigationStateIdle
    {
    };

    struct NavigationStateMoving
    {
        MovingStateGoal movementGoal;
        PathDestination pathDestination;
        std::optional<PathFollowingInfo> path;
        bool pathRequested;

        /**
         * Set when the unit has consumed a partial path: the mover's
         * best-effort path is exhausted at the closest reachable
         * point. Retail ends the move job here (the job-done flag in
         * unit+0x10f), so navigateTo reports PathExhausted instead of
         * InProgress. Cleared when a fresh path is assigned.
         */
        bool pathExhausted{false};

        /**
         * Earliest tick at which pathExhausted may be reported to the
         * order layer. A partial path means the search exhausted its
         * vertex budget -- a failed request -- and retail sleeps the
         * move job ~30 ticks on a failed request (TotalA.exe
         * 0x439e80, e.g. call sites 0x402f3a/0x411098), so delivery of
         * a partial path sets this ~30 ticks out. Without it a unit
         * whose partial drains instantly re-issues a full search every
         * few ticks (the AC05 pathfinding storm). Complete paths leave
         * it at 0, meaning no delay.
         */
        GameTime pathExhaustedAt{0};
    };

    /**
     * Why a navigateTo call finished. Retail's move job has exactly
     * two terminal conditions -- the mover arrives inside the job's
     * goal radius, or its best-effort path is exhausted at the
     * closest reachable point (job-done in unit+0x10f). There is no
     * timeout. Range-gated orders (build/repair/reclaim/attack) must
     * only start work on Arrived; movement orders complete on either.
     */
    enum class NavigationResult
    {
        InProgress,
        Arrived,
        PathExhausted,
    };

    struct NavigationStateMovingToLandingSpot
    {
        SimVector landingLocation;
    };

    using NavigationState = std::variant<NavigationStateIdle, NavigationStateMoving, NavigationStateMovingToLandingSpot>;

    struct UnitPositionCache
    {
        UnitId unitId;
        SimVector position;
        GameTime cachedAtTime;
    };

    struct NavigationStateInfo
    {
        std::optional<NavigationGoal> desiredDestination;
        std::optional<UnitPositionCache> unitPositionCache;
        NavigationState state;
    };

    struct FactoryBehaviorStateIdle
    {
    };

    struct FactoryBehaviorStateCreatingUnit
    {
        std::string unitType;
        PlayerId owner;
        SimVector position;
        SimAngle rotation;
        UnitCreationStatus status{UnitCreationStatusPending()};
    };

    struct FactoryBehaviorStateBuilding
    {
        // the vector is the origin of nano particles
        std::optional<std::pair<UnitId, std::optional<SimVector>>> targetUnit;
    };

    using FactoryBehaviorState = std::variant<FactoryBehaviorStateIdle, FactoryBehaviorStateCreatingUnit, FactoryBehaviorStateBuilding>;

    UnitOrder createMoveOrder(const SimVector& destination);

    UnitOrder createAttackOrder(UnitId target);

    UnitOrder createAttackGroundOrder(const SimVector& target);

    bool isWater(YardMapCell cell);

    bool isGeo(YardMapCell cell);

    bool isPassable(YardMapCell cell, bool yardMapOpen);

    struct SteeringInfo
    {
        /** The angle we are trying to steer towards. */
        SimAngle targetAngle{0};

        /** The speed we are trying to accelerate/decelerate to */
        SimScalar targetSpeed{0};

        /** True if the unit should attempt to take off into the air */
        bool shouldTakeOff{false};
    };

    struct UnitPhysicsInfoGround
    {
        SteeringInfo steeringInfo;

        /**
         * Rate at which the unit is travelling forwards in game units/tick.
         */
        SimScalar currentSpeed{0};
    };

    struct AirMovementStateFlying
    {
        std::optional<SimVector> targetPosition;

        /** True if the unit should attempt to land at current position. */
        bool shouldLand{false};

        /**
         * Rate at which the unit is moving in game units/tick.
         */
        SimVector currentVelocity{0_ss, 0_ss, 0_ss};
    };

    struct AirMovementStateTakingOff
    {
    };

    struct AirMovementStateLanding
    {
        bool landingFailed{false};
        bool shouldAbort{false};
    };

    using AirMovementState = std::variant<AirMovementStateTakingOff, AirMovementStateFlying, AirMovementStateLanding>;

    struct UnitPhysicsInfoAir
    {
        AirMovementState movementState{AirMovementStateTakingOff()};
    };

    using UnitPhysicsInfo = std::variant<UnitPhysicsInfoGround, UnitPhysicsInfoAir>;

    bool isFlying(const UnitPhysicsInfo& physics);

    /** AirStrike's persistent legs; navigation itself remains the shared mover. */
    struct BomberState
    {
        enum class Phase : unsigned int { Takeoff, Reposition, Swing, Approach, Overfly, BreakAway };
        std::optional<AttackTarget> target;
        Phase phase{Phase::Takeoff};
        SimVector goal{0_ss, 0_ss, 0_ss};
        SimVector releaseTarget{0_ss, 0_ss, 0_ss};
        std::optional<GameTime> nextScan;
    };

    struct AircraftServiceState
    {
        enum class Phase : unsigned int { Takeoff, Waiting, Approach, Align, Descend, Repair, Parked, Depart };
        UnitId facility{0};
        int piece{-1};
        Phase phase{Phase::Takeoff};
        SimAngle loiterBearing{0};
        SimVector goal{0_ss, 0_ss, 0_ss};
        bool automatic{false};

        bool docked() const { return phase == Phase::Repair || phase == Phase::Parked; }
    };

    class UnitState
    {
    public:
        struct LifeStateAlive
        {
        };
        struct LifeStateDead
        {
            bool leaveCorpse;
        };
        using LifeState = std::variant<LifeStateAlive, LifeStateDead>;

    public:
        std::string unitType;
        std::vector<UnitMesh> pieces;
        std::unordered_map<std::string, int> pieceNameToIndices;
        SimVector position;
        SimVector previousPosition;
        std::unique_ptr<CobEnvironment> cobEnvironment;
        PlayerId owner;

        /**
         * Anticlockwise rotation of the unit around the Y axis in radians.
         * The other two axes of rotation are normally determined
         * by the normal of the terrain the unit is standing on.
         */
        SimAngle rotation{0};
        SimAngle previousRotation{0};

        UnitPhysicsInfo physics{UnitPhysicsInfoGround()};

        unsigned int hitPoints{0};

        // Retail credited kills (unit+0xb8), independent of player statistics
        // and the OTA's retained initialization metadata. Wraps at 65536.
        uint16_t kills{0};

        LifeState lifeState{LifeStateAlive()};

        std::deque<UnitOrder> orders;
        UnitBehaviorState behaviourState;

        NavigationStateInfo navigationState;

        /**
         * State we remember related to the current order.
         * This is cleared every time an order is completed.
         * Right now this is just a unit ID for build orders.
         */
        std::optional<UnitId> buildOrderUnitId;

        // Allocate only for bombers; ordinary units carry no run payload.
        std::unique_ptr<BomberState> bomberState;
        std::unique_ptr<AircraftServiceState> aircraftService;
        std::optional<GameTime> nextServiceScan;

        void cancelServiceApproach();

        bool inBuildStance{false};
        bool yardOpen{false};

        /**
         * True if the unit attempted to move last frame
         * and its movement was limited (or prevented entirely) by a collision.
         */
        bool inCollision{false};

        std::array<std::optional<UnitWeapon>, 3> weapons;

        UnitFireOrders fireOrders{UnitFireOrders::FireAtWill};

        /**
         * The unit's standing move order: how far it may leave its position
         * to engage targets it acquired itself.
         */
        UnitMoveOrders moveOrders{UnitMoveOrders::Maneuver};

        /**
         * The last game time at which this unit took damage.
         * Used for return-fire acquisition and WaitForAttack semantics.
         */
        GameTime lastDamagedAt{0};

        /**
         * The unit that most recently damaged this unit, if known.
         * Used to acquire a target under the ReturnFire standing order.
         */
        std::optional<UnitId> lastDamagedBy;

        /**
         * The player that most recently damaged this unit, resolved at
         * damage time. Unlike lastDamagedBy it stays valid after the
         * attacker dies, so the ENDMSN debrief can still credit the
         * kill (retail player record +0xfc).
         */
        std::optional<PlayerId> lastDamagedByPlayer;

        unsigned int buildTimeCompleted{0};

        /**
         * Mission data retained when this unit was spawned by a
         * campaign mission's [unitN] block. Absent in skirmish games.
         * `selectable` gates owner selection while a scripted
         * InitialMission queue is running.
         */
        std::optional<MissionUnitState> missionState;

        bool activated{false};

        /**
         * Armored flag (retail unit+0x10e bit1), toggled only by COB
         * `set ARMORED`. While set, the damage routine multiplies
         * incoming hits below 30000 raw points by the unitdef
         * DamageModifier (TotalA.exe 0x489bc3).
         */
        bool armored{false};

        /**
         * Busy flag (retail unit+0x10f bit1), toggled only by COB
         * `set BUSY`. Retail keeps it purely as a script-visible
         * marker — stock transport scripts raise it while a transport
         * has passengers to move — with no engine-side consumer.
         */
        bool busy{false};

        /**
         * BuggerOff flag (retail unit+0x10f bit3), toggled by COB
         * `set BUGGER_OFF`. Factory scripts raise it while the yard
         * is opening; setting it also emits the scatter order to
         * units standing in the factory footprint.
         */
        bool buggerOff{false};

        bool isSufficientlyPowered{false};

        /**
         * The player-requested cloak toggle (retail unit+0x110 bit11;
         * set by the CLOAK_ON order, cleared by CLOAK_OFF, gated on
         * the unitdef cloak-capability bit +0x245 bit13). Persists
         * while effective cloak is suppressed by energy shortfall or
         * enemy proximity — the unit recloaks automatically once the
         * blocker clears.
         */
        bool cloakRequested{false};

        /**
         * Effective cloak state (retail unit+0x10e bit2), written only
         * by the per-second cloak upkeep. Removes the unit from enemy
         * LOS visibility (retail 0x465ac0), picking and weapon
         * acquisition; radar contacts are unaffected.
         */
        bool cloaked{false};

        /**
         * Retail unit+0xb0 blocked-until tick as used by the cloak
         * upkeep: while gameTime < this, the cloak energy debit is
         * skipped and the unit stays decloaked. A detected enemy
         * inside mincloakdistance pushes it 90 ticks out (retail
         * 0x4676d5), producing the ~3s decloak dwell after contact.
         */
        GameTime cloakBlockedUntil{0};

        Energy energyProductionBuffer{0};
        Metal metalProductionBuffer{0};
        Energy previousEnergyConsumptionBuffer{0};
        Metal previousMetalConsumptionBuffer{0};
        Energy energyConsumptionBuffer{0};
        Metal metalConsumptionBuffer{0};

        /**
         * Orders that run in parallel with the main order queue,
         * corresponding to retail's background-order execution slot.
         * Currently used by the mission `bw` (build-weapon) command.
         */
        std::deque<UnitOrder> backgroundOrders;

        /**
         * The transport currently carrying this unit, if any.
         * Carried units are removed from terrain occupancy, skip
         * behavior/weapon/COB updates and are not selectable.
         */
        std::optional<UnitId> carriedBy;

        /**
         * COB attach-slot index on the carrier, matching retail's
         * script piece index passed to ATTACH_UNIT / the VTOL attach
         * helper 0x48aac0. -1 means an invalid slot: the unit rides at
         * the carrier's origin (retail 0x43def0 returns a zero offset).
         * Scripted `i` boards with slot -1; interactive VTOL loading
         * boards at the slot returned by QueryTransport.
         */
        int attachSlot{-1};

        /**
         * Units currently carried aboard this transport.
         * Ordered with the most recently boarded unit first,
         * matching the retail group/member chain used by `i`.
         */
        std::deque<UnitId> passengers;

        /**
         * Interactive self-destruct countdown state (retail's
         * SELFDESTRUCT unit command, dispatched on Ctrl/Alt+D at
         * TotalA.exe 0x49641d). Unlike the queued `d`/SELFDESTRUCTFG
         * order this lives outside the order queue and counts down in
         * parallel with normal orders; issuing the command again while
         * armed cancels it (retail "canceldestruct" command and the
         * "Self destruct terminated" announcement).
         */
        std::optional<SelfDestructOrder> selfDestruct;
        /** Standby opportunity scan deadline; absent starts its one-tick initial wait. */
        std::optional<GameTime> nextKamikazeScan;

        std::deque<std::pair<std::string, int>> buildQueue;
        FactoryBehaviorState factoryState;

        static SimAngle toRotation(const SimVector& direction);

        static SimVector toDirection(SimAngle rotation);

        UnitState(const std::vector<UnitMesh>& pieces, std::unique_ptr<CobEnvironment>&& cobEnvironment);

        bool isBeingBuilt(const UnitDefinition& unitDefinition) const;

        unsigned int getBuildPercentLeft(const UnitDefinition& unitDefinition) const;

        float getPreciseCompletePercent(const UnitDefinition& unitDefinition) const;

        struct BuildCostInfo
        {
            unsigned int workerTime;
            Energy energyCost;
            Metal metalCost;
        };

        BuildCostInfo getBuildCostInfo(const UnitDefinition& unitDefinition, unsigned int buildTimeContribution);

        bool addBuildProgress(const UnitDefinition& unitDefinition, unsigned int buildTimeContribution);

        /**
         * The virtual build-progress axis value (in build-time ticks)
         * corresponding to the unit's current hit points.
         */
        unsigned int getRepairProgressForHitPoints(const UnitDefinition& unitDefinition) const;

        /**
         * The resource cost of advancing a repair progress axis
         * by up to `buildTimeContribution` ticks from `repairProgress`.
         */
        BuildCostInfo getRepairCostInfo(const UnitDefinition& unitDefinition, unsigned int repairProgress, unsigned int buildTimeContribution) const;

        /**
         * Advances hit-point repair by up to `buildTimeContribution` ticks
         * along the caller-owned `repairProgress` axis.
         * If the unit took damage since the last repair tick, the axis is
         * first re-anchored to current hit points so lost health is paid for again.
         * Returns true when hit points reach the maximum.
         */
        bool addRepairProgress(const UnitDefinition& unitDefinition, unsigned int& repairProgress, unsigned int buildTimeContribution);

        void moveObject(const std::string& pieceName, SimAxis axis, SimScalar targetPosition, SimScalar speed);

        void moveObjectNow(const std::string& pieceName, SimAxis axis, SimScalar targetPosition);

        void turnObject(const std::string& pieceName, SimAxis axis, SimAngle targetAngle, SimScalar speed);

        void turnObjectNow(const std::string& pieceName, SimAxis axis, SimAngle targetAngle);

        void spinObject(const std::string& pieceName, SimAxis axis, SimScalar speed, SimScalar acceleration);

        void stopSpinObject(const std::string& pieceName, SimAxis axis, SimScalar deceleration);

        bool isMoveInProgress(const std::string& pieceName, SimAxis axis) const;

        bool isTurnInProgress(const std::string& pieceName, SimAxis axis) const;

        bool isOwnedBy(PlayerId playerId) const;

        bool isAlive() const;
        bool isDead() const;

        void markAsDead();
        void markAsDeadNoCorpse();

        void finishBuilding(const UnitDefinition& unitDefinition);

        void clearOrders();

        /** EMP's active mark belongs to the scheduled primary-head wait. */
        bool isParalyzed() const;

        /**
         * Removes the order at `index` in the queue; does nothing
         * when the index is out of range. Removing the active order
         * (index 0) additionally clears in-progress build and weapon
         * targets, matching the work the order was driving.
         */
        void removeOrderAt(std::size_t index);

        void replaceOrders(const std::deque<UnitOrder>& newOrders);

        void addOrder(const UnitOrder& order);

        void setWeaponTarget(unsigned int weaponIndex, UnitId target);
        void setWeaponTarget(unsigned int weaponIndex, const SimVector& target);
        void setWeaponTarget(unsigned int weaponIndex, ProjectileId target);
        void clearWeaponTarget(unsigned int weaponIndex);
        void clearWeaponTargets();

        Matrix4x<SimScalar> getTransform() const;
        Matrix4x<SimScalar> getInverseTransform() const;

        bool isSelectableBy(const UnitDefinition& unitDefinition, PlayerId player) const;

        void activate();

        void deactivate();

        Metal getMetalMake() const;
        Energy getEnergyMake() const;
        Metal getMetalUse() const;
        Energy getEnergyUse() const;

        void addEnergyDelta(const Energy& energy);
        void addMetalDelta(const Metal& metal);

        void resetResourceBuffers();

        void modifyBuildQueue(const std::string& buildUnitType, int count);

        std::unordered_map<std::string, int> getBuildQueueTotals() const;

        int getBuildQueueTotal(const std::string& unitType) const;

        /**
         * What an active nanolathe beam is pointed at:
         * a unit under construction/repair, or a feature being reclaimed.
         */
        using NanolatheTargetRef = std::variant<UnitId, FeatureId>;

        /**
         * The nanolathe work applied by this unit's work handler during the
         * current tick: target reference plus beam origin. This is the
         * authoritative "retail would invoke the emitter this tick" signal --
         * cleared at the start of every unit update and set only by work
         * handlers on ticks where they actually apply work, so it can never
         * outlive the work it describes. Transient: not serialized or hashed.
         * The behavior-state nanoParticleOrigin fields serve a different role:
         * they persist across ticks to detect work-start edges for events.
         */
        std::optional<std::pair<NanolatheTargetRef, SimVector>> activeNanolatheTarget;

        /**
         * Returns the beam endpoint reference and beam origin for the unit's
         * active nanolathe work this tick, if any. The caller resolves the
         * reference to a world position.
         */
        std::optional<std::pair<NanolatheTargetRef, SimVector>> getActiveNanolatheTarget() const;

        std::optional<std::reference_wrapper<const UnitMesh>> findPiece(const std::string& pieceName) const;

        std::optional<std::reference_wrapper<UnitMesh>> findPiece(const std::string& pieceName);
    };
}
