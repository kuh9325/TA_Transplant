#pragma once

#include <string>

namespace rwe
{
    struct UnitFbi
    {
        std::string unitName;
        std::string objectName;
        std::string soundCategory;
        std::string movementClass;

        std::string name;
        std::string description;

        unsigned int turnRate;
        float maxVelocity;
        float acceleration;
        float brakeRate;

        unsigned int footprintX;
        unsigned int footprintZ;
        unsigned int maxSlope;
        unsigned int maxWaterSlope;
        unsigned int minWaterDepth;
        unsigned int maxWaterDepth;

        /**
         * Heightmap units below the waterline that the base of a floating
         * structure sits at. Only meaningful for all-water yardmaps.
         */
        unsigned int waterline;

        bool canAttack;
        bool canMove;
        bool canGuard;
        bool canPatrol;

        /**
         * FBI `canstop` (unitdef flag 0x8): the unit accepts the
         * Stop order. Gates the orders-panel STOP gadget
         * (TotalA.exe selection mask +0x37ec1 bit 0x01).
         */
        bool canStop{false};

        /**
         * FBI `CanReclamate` (unitdef flag 0x400, copied to 0x200 for
         * the REPAIR gadget): the unit may issue Reclaim and Repair
         * orders (TotalA.exe 0x42ca37-0x42ca4d).
         */
        bool canReclaim{false};

        /**
         * FBI `candgun`: the unit is eligible for the command-fire
         * (D-Gun/BLAST) order and caches "the command-fire weapon"
         * at creation, per retail.
         */
        bool canDGun;

        /**
         * Initial standing orders, matching the retail InitialMission `o`
         * command and UnitFireOrders/UnitMoveOrders enum values.
         * StandingFireOrder: 0=hold fire, 1=return fire, 2=fire at will.
         * StandingMoveOrder: 0=hold position, 1=maneuver, 2=roam.
         */
        int standingFireOrder;
        int standingMoveOrder;

        bool commander;

        unsigned int maxDamage;

        bool bmCode;

        bool floater;
        bool canHover;

        bool canFly;
        unsigned int attackRunLength{0};

        unsigned int cruiseAlt;

        std::string weapon1;
        std::string weapon2;
        std::string weapon3;

        std::string explodeAs;
        std::string selfDestructAs;
        bool kamikaze{false};
        unsigned int kamikazeDistance{0};
        std::string defaultMissionType;
        bool shootMe{false};
        std::string noChaseCategory;

        bool isAirBase{false};
        bool builder;
        unsigned int buildTime;
        unsigned int buildCostEnergy;
        unsigned int buildCostMetal;

        /**
         * Retail `CanCapture=1`: the unit may issue Capture orders.
         * In stock data only ARMCOM/CORCOM set it; the same flag on a
         * *target* makes that target immune to capture (TotalA.exe
         * unitdef flag 0x1000, checked at 0x4042cf / 0x4042ef).
         */
        bool canCapture{false};

        unsigned int workerTime;

        unsigned int buildDistance;

        bool onOffable;
        bool activateWhenBuilt;

        float energyMake;
        float metalMake;
        float energyUse;
        float metalUse;

        float makesMetal;
        float extractsMetal;

        unsigned int energyStorage;
        unsigned int metalStorage;

        unsigned int windGenerator;
        bool tidalGenerator;

        bool hideDamage;
        bool showPlayerName;

        unsigned int sightDistance{0};
        unsigned int radarDistance{0};
        unsigned int sonarDistance{0};
        unsigned int radarDistanceJam{0};
        unsigned int sonarDistanceJam{0};
        bool stealth{false};
        bool immuneToParalyzer{false};
        bool isTargetingUpgrade{false};

        /**
         * FBI `antiweapons` (retail unitdef +0x241 bit29): the unit
         * carries anti-missile weapons; while selected it draws its
         * interceptor weapons' `coverage` ring on the minimap
         * (TotalA.exe minimap painter 0x467082). Stock data: ARMAMD
         * and CORFMD only.
         */
        bool antiWeapons{false};

        /**
         * Maximum number of units this transport can carry.
         * A non-zero value marks the unit as a transport
         * (retail unitdef byte at +0x22b).
         */
        unsigned int transportCapacity;

        /**
         * Retail `CanLoad=1` (unitdef flag 0x100): the unit may be
         * issued interactive Load orders. Stock transports set it.
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

        /**
         * Number of seconds of audible countdown before a self-destruct
         * detonates. Retail defaults to 5 when unspecified.
         */
        unsigned int selfDestructCountdown{5};

        /**
         * Retail `CloakCost` (unitdef float at +0x1da): energy drained
         * per upkeep pass (once per second) while cloaked and
         * stationary. The cloak capability bit (unitdef +0x245 bit13)
         * is set iff this parses to a value > 0 -- there is no
         * separate "cancloak" FBI key (TotalA.exe flag builder at
         * 0x42ca50).
         */
        float cloakCost{0.0f};

        /**
         * Retail `CloakCostMoving` (unitdef float at +0x1de): energy
         * drained per upkeep pass while cloaked and moving (the
         * in-motion pair, unit+0x110 bits 2-3, selects between this
         * and `cloakCost`). Zero when absent -- a moving unit with no
         * moving cost cloaks for free.
         */
        float cloakCostMoving{0.0f};

        /**
         * Retail `DamageModifier` (unitdef 16.16 fixed at +0x1aa):
         * damage multiplier applied while the unit's armored flag
         * (COB `set ARMORED`, unit+0x10e bit1) is set, for incoming
         * damage below 30000 (TotalA.exe apply at 0x489bc3).
         * Neutral default of 1.0 when absent.
         */
        float damageModifier{1.0f};

        /**
         * Retail `mincloakdistance` (unitdef word at +0x208): radius in
         * world units; a detected non-allied unit inside it suppresses
         * effective cloak and starts the ~3s decloak dwell
         * (TotalA.exe 0x4676b6-0x4676d5). Zero disables proximity
         * decloak entirely (e.g. ARMCKFUS).
         */
        unsigned int minCloakDistance{0};

        /**
         * Retail `init_cloaked` (unitdef +0x241 bit4): the unit spawns
         * with its cloak request already on. In stock data only the
         * twelve ARMMINE/CORMINE units set it.
         */
        bool initCloaked{false};

        /**
         * FBI `Category` (retail unitdef category bitmask): whitespace
         * separated capability tags such as `ARM KBOT LEVEL1 CONSTR
         * NOWEAPON`. AI profile `Weight`/`Limit` directives resolve
         * their name argument against unit names and these category
         * tokens (TotalA.exe 0x488d30).
         */
        std::string category;

        /**
         * FBI `ai_limit` (retail unitdef text field at +0xbe): the
         * unit's own AI directive line, re-parsed per AI player for
         * unit types the loaded profile did not already lock
         * (TotalA.exe 0x409f80/0x40a040). Stock data uses it only on
         * CORDECOM (`ai_limit=limit CORDECOM 2`).
         */
        std::string aiLimit;

        /**
         * FBI `ai_weight` (retail unitdef text field at +0xfe). Loaded
         * by retail's unitdef reader but never consulted -- both
         * per-unit AI passes read the +0xbe (`ai_limit`) buffer
         * instead. Retained here for format completeness only.
         */
        std::string aiWeight;

        std::string yardMap;

        std::string corpse;
    };
}
