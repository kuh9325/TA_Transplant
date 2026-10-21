#pragma once

#include <rwe/grid/Grid.h>
#include <rwe/sim/Energy.h>
#include <rwe/sim/Metal.h>
#include <rwe/sim/MovementClassId.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/UnitFireOrders.h>
#include <rwe/sim/UnitMoveOrders.h>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace rwe
{
    enum class YardMapCell
    {
        GroundPassableWhenOpen,
        WaterPassableWhenOpen,
        GroundNoFeature,
        GroundGeoPassableWhenOpen,
        Geo,
        Ground,
        GroundPassableWhenClosed,
        Water,
        GroundPassable,
        WaterPassable,
        Passable
    };

    struct UnitDefinition
    {
        struct NamedMovementClass
        {
            MovementClassId movementClassId;
        };
        struct AdHocMovementClass
        {
            unsigned int footprintX;
            unsigned int footprintZ;
            unsigned int maxSlope;
            unsigned int maxWaterSlope;
            unsigned int minWaterDepth;
            unsigned int maxWaterDepth;
        };
        using MovementCollisionInfo = std::variant<NamedMovementClass, AdHocMovementClass>;

        // FIXME: these two things should probably be in unit media info??
        // They are not needed for sim.
        std::string unitName;
        std::string unitDescription;

        std::string objectName;

        MovementCollisionInfo movementCollisionInfo;

        /**
         * Rate at which the unit turns in world angular units/tick.
         */
        SimScalar turnRate;

        /**
         * Maximum speed the unit can travel forwards in game units/tick.
         */
        SimScalar maxVelocity;

        /**
         * Speed at which the unit accelerates in game units/tick.
         */
        SimScalar acceleration;

        /**
         * Speed at which the unit brakes in game units/tick.
         */
        SimScalar brakeRate;

        bool canAttack;
        bool canMove;
        bool canGuard;

        /** If true, the unit supports the patrol order (FBI CanPatrol). */
        bool canPatrol;

        /** FBI `canstop`: the unit accepts the Stop order. */
        bool canStop{false};

        /**
         * FBI `CanReclamate`: the unit may issue Reclaim and Repair
         * orders (retail shares the flag for both panel gadgets).
         */
        bool canReclaim{false};

        /**
         * If true, the unit is eligible for the command-fire
         * (D-Gun/BLAST) order (FBI `candgun`).
         */
        bool canDGun;

        /** Initial standing orders taken from the unit's FBI entry. */
        UnitFireOrders standingFireOrder;
        UnitMoveOrders standingMoveOrder;

        /** If true, the unit is considered a commander for victory conditions. */
        bool commander;

        unsigned int maxHitPoints;

        bool isMobile;

        bool floater;
        bool canHover;

        /**
         * Heightmap units below sea level at which the base of an
         * all-water structure floats (FBI Waterline). Unused by units
         * whose yardmap contains support cells.
         */
        unsigned int waterline;

        bool canFly;
        /** Authored horizontal bombing-run length (retail unsigned low word). */
        SimScalar attackRunLength{0_ss};

        /** Distance above the ground that the unit flies at. */
        SimScalar cruiseAltitude;

        std::string weapon1;
        std::string weapon2;
        std::string weapon3;

        std::string explodeAs;
        std::string selfDestructAs;
        bool kamikaze{false};
        SimScalar kamikazeDistance{0_ss};
        /** Only the authored Standby_Mine default has the grounded proximity trigger. */
        bool standbyMine{false};
        bool shootMe{false};
        std::unordered_set<std::string> noChaseCategories;

        bool isAirBase{false};

        bool builder;
        unsigned int buildTime;
        Energy buildCostEnergy;
        Metal buildCostMetal;

        /**
         * Retail FBI `CanCapture=1` (unitdef flag 0x1000): the unit may
         * issue Capture orders. On a *target* the same flag grants
         * capture immunity — commanders cannot be captured
         * (TotalA.exe 0x4042cf, 0x4042ef).
         */
        bool canCapture{false};

        unsigned int workerTimePerTick;

        SimScalar buildDistance;

        bool onOffable;
        bool activateWhenBuilt;

        Energy energyMake;
        Metal metalMake;
        Energy energyUse;
        Metal metalUse;

        Metal makesMetal;
        Metal extractsMetal;

        Energy energyStorage;
        Metal metalStorage;

        Energy windGenerator;
        bool tidalGenerator;

        std::optional<Grid<YardMapCell>> yardMap;
        bool yardMapContainsGeo;

        /**
         * Maximum number of carried units (retail unitdef byte at +0x22b).
         * Non-zero marks the unit as a transport.
         */
        unsigned int transportCapacity;

        /**
         * Retail `CanLoad=1` (unitdef flag 0x100): the unit may be issued
         * interactive player Load orders.
         */
        bool canLoad{false};

        /**
         * Retail `TransportSize` (unitdef byte at +0x22a): maximum
         * footprintX of a passenger this transport may board.
         */
        unsigned int transportSize{0};

        /**
         * Retail `CantBeTransported=1` (unitdef flag 0x40000): the unit
         * can never board a transport.
         */
        bool cantBeTransported{false};

        /** Seconds of countdown before a self-destruct detonates (retail default 5). */
        unsigned int selfDestructCountdown{5};

        /**
         * FBI `CloakCost` (retail unitdef float +0x1da): energy drained
         * per upkeep pass (once per second) while cloaked and
         * stationary. `cloakCost > 0` is the cloak capability bit
         * (retail unitdef +0x245 bit13) — it gates the CLOAK orders
         * and the orders-panel gadget.
         */
        float cloakCost{0.0f};

        /**
         * FBI `CloakCostMoving` (retail unitdef float +0x1de): energy
         * drained per upkeep pass while cloaked and moving. Retail
         * selects it whenever the in-motion pair (unit+0x110 bits 2-3)
         * is set; a zero moving cost cloaks a moving unit for free.
         */
        float cloakCostMoving{0.0f};

        /**
         * FBI `DamageModifier` stored as 16.16 fixed-point (retail
         * unitdef +0x1aa). While the unit's armored flag is set, the
         * damage routine multiplies incoming damage by this value and
         * shifts right 16, but only for hits below 30000 raw points
         * (TotalA.exe 0x489bc3). Defaults to 1.0 (0x10000).
         */
        std::uint32_t damageModifier{0x10000};

        /**
         * FBI `mincloakdistance` (retail unitdef word +0x208): radius in
         * world units; a detected non-allied unit inside it suppresses
         * effective cloak and starts the ~3s decloak dwell.
         */
        SimScalar minCloakDistance{0_ss};

        /**
         * FBI `init_cloaked` (retail unitdef +0x241 bit4): the unit
         * spawns with its cloak request already on (stock mines).
         */
        bool initCloaked{false};

        std::string corpse;

        bool hideDamage;
        bool showPlayerName;

        /**
         * Radius in world units inside which this unit visually detects
         * enemy units and reveals terrain (FBI SightDistance).
         */
        SimScalar sightDistance{0_ss};

        /**
         * Radius in world units inside which this unit detects enemy
         * units as radar contacts (FBI RadarDistance). Radar contacts
         * appear on the minimap but are not world-rendered and do not
         * satisfy weapon auto-acquisition.
         */
        SimScalar radarDistance{0_ss};

        /**
         * Radius in world units for sonar detection of units at or
         * below sea level (FBI SonarDistance; retail unitdef +0x206).
         * Sonar contacts appear on the minimap but are not
         * world-rendered and never satisfy weapon auto-acquisition
         * (retail collects no weapon-target list for sonar contacts).
         */
        SimScalar sonarDistance{0_ss};

        /**
         * Radius in world units inside which this unit jams enemy
         * radar detection (FBI RadarDistanceJam; retail unitdef
         * +0x20a). Enemy units inside the radius of a powered jammer
         * cannot be radar contacts; LOS visibility is unaffected.
         */
        SimScalar radarDistanceJam{0_ss};

        /**
         * Radius in world units inside which this unit jams enemy
         * sonar detection (FBI SonarDistanceJam; retail unitdef
         * +0x20c). Same suppression model as radarDistanceJam,
         * applied to sonar contacts.
         */
        SimScalar sonarDistanceJam{0_ss};

        /**
         * FBI `Stealth` (retail unitdef +0x241 bit8): the unit is
         * skipped by enemy radar AND sonar detection entirely.
         * Stealth does not affect LOS visibility, weapon targeting
         * once seen, or the unit's own emissions. Distinct from
         * Cloak (C4.6): stealth needs no energy, never wears off,
         * and is purely a detection-immunity property.
         */
        bool stealth{false};
        bool immuneToParalyzer{false};

        /**
         * FBI `antiweapons` (retail unitdef +0x241 bit29): while the
         * unit is selected, each of its `interceptor` weapons draws a
         * coverage ring on the minimap (TotalA.exe 0x467082; the ring
         * radius is `coverage - 0x200`, colour `ctx+0xf`).
         */
        bool antiWeapons{false};

        /**
         * FBI `istargetingupgrade`: while this unit is activated and
         * powered, the owning player may auto-acquire radar-only
         * contacts with weapons (retail ARMTARG/CORTARG Targeting
         * Facility behavior; per-player flag at TotalA.exe +0x79).
         */
        bool isTargetingUpgrade{false};

        std::string soundCategory;

        /**
         * FBI `Category` tokens, uppercased (e.g. `ARM`, `KBOT`,
         * `LEVEL1`, `CONSTR`, `NOWEAPON`). Campaign AI profile
         * `Weight`/`Limit` directives resolve their name argument
         * against these tags as well as exact unit names
         * (TotalA.exe resolver 0x488d30).
         */
        std::unordered_set<std::string> categories;

        /**
         * The unit's own AI directive line, taken from FBI `ai_limit`
         * (retail unitdef text field at +0xbe). Per AI player it is
         * re-parsed as a `weight`/`limit` directive for unit types the
         * loaded profile did not already lock (TotalA.exe
         * 0x409f80/0x40a040). Stock data uses it only on CORDECOM.
         */
        std::string aiSelfDirective;

        /**
         * Uppercase names of units this builder/factory can produce,
         * in GUI-page order (retail unitdef build list at +0x156).
         * Populated from the unit's build GUI pages after any mission
         * `useonly` filtering, so campaign build restrictions are
         * already baked in.
         */
        std::vector<std::string> buildOptions;
    };
}
