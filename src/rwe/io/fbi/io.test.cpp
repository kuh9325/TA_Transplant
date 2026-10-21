#include <catch2/catch_test_macros.hpp>
#include <rwe/io/fbi/io.h>
#include <rwe/io/tdf/tdf.h>

namespace rwe
{
    namespace
    {
        TdfBlock makeUnitInfo(std::initializer_list<std::pair<const char*, const char*>> entries)
        {
            TdfBlock info;
            for (const auto& [k, v] : entries)
            {
                info.insertOrAssignProperty(k, v);
            }

            TdfBlock root;
            root.insertOrAssignBlock("UNITINFO", info);
            return root;
        }
    }

    TEST_CASE("parseUnitFbi placement defaults")
    {
        SECTION("absent fields get retail defaults")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            // retail movement records default to effectively unbounded
            // terrain limits; missing fields must not silently become zero
            REQUIRE(fbi.maxWaterDepth == 255);
            REQUIRE(fbi.minWaterDepth == 0);
            REQUIRE(fbi.maxSlope == 255);
            REQUIRE(fbi.maxWaterSlope == 255);
            REQUIRE(fbi.waterline == 0);
        }

        SECTION("explicit fields override defaults")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"MaxWaterDepth", "12"},
                {"MinWaterDepth", "30"},
                {"MaxSlope", "10"},
                {"MaxWaterSlope", "40"},
                {"Waterline", "8"},
            }));

            REQUIRE(fbi.maxWaterDepth == 12);
            REQUIRE(fbi.minWaterDepth == 30);
            REQUIRE(fbi.maxSlope == 10);
            REQUIRE(fbi.maxWaterSlope == 40);
            REQUIRE(fbi.waterline == 8);
        }

        SECTION("explicit MaxWaterDepth=0 is preserved")
        {
            // floating structures such as ARMFMKR ship with
            // MinWaterDepth=11, MaxWaterDepth=0 and rely on the
            // all-water placement rules, so zero must survive parsing
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"MaxWaterDepth", "0"},
            }));

            REQUIRE(fbi.maxWaterDepth == 0);
        }
    }

    TEST_CASE("parseUnitFbi command-fire eligibility")
    {
        SECTION("candgun parses as canDGun")
        {
            // stock ARMCOM/CORCOM carry `candgun=1`; the field is the
            // retail gate for the BLAST order, not a unit-name check
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"candgun", "1"},
            }));

            REQUIRE(fbi.canDGun);
        }

        SECTION("candgun defaults to false")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(!fbi.canDGun);
        }
    }

    TEST_CASE("parseUnitFbi capture eligibility")
    {
        SECTION("CanCapture parses as canCapture")
        {
            // stock ARMCOM/CORCOM are the only CanCapture=1 units;
            // the same flag on a target makes it immune to capture
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"CanCapture", "1"},
            }));

            REQUIRE(fbi.canCapture);
        }

        SECTION("CanCapture defaults to false")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(!fbi.canCapture);
        }
    }

    TEST_CASE("parseUnitFbi transport fields")
    {
        SECTION("transport fields parse")
        {
            // stock ARMATLAS/CORVALK/ARMTHOVR/CORTHOVR/ARMTSHIP/CORTSHIP
            // carry the transport block; the retail loader reads
            // CanLoad (bit 0x100), TransportSize (+0x22a),
            // TransportCapacity (+0x22b) and CantBeTransported
            // (bit 0x40000)
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"CanLoad", "1"},
                {"TransportSize", "4"},
                {"TransportCapacity", "6"},
                {"CantBeTransported", "1"},
            }));

            REQUIRE(fbi.canLoad);
            REQUIRE(fbi.transportSize == 4);
            REQUIRE(fbi.transportCapacity == 6);
            REQUIRE(fbi.cantBeTransported);
        }

        SECTION("transport fields default to zero")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(!fbi.canLoad);
            REQUIRE(fbi.transportSize == 0);
            REQUIRE(fbi.transportCapacity == 0);
            REQUIRE(!fbi.cantBeTransported);
        }

        SECTION("TransportMaxUnits is ignored")
        {
            // retail parses the key but never reads it -- only
            // TransportCapacity (+0x22b) gates the passenger count
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"TransportMaxUnits", "9"},
                {"TransportCapacity", "2"},
            }));

            REQUIRE(fbi.transportCapacity == 2);
        }
    }

    TEST_CASE("parseUnitFbi orders-panel flags")
    {
        // stock data carries canstop on orderable units and
        // CanReclamate on nanolathe-capable units; both feed the
        // retail orders-panel enable mask (unitdef +0x245)
        SECTION("canstop parses as canStop")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"canstop", "1"},
            }));

            REQUIRE(fbi.canStop);
        }

        SECTION("CanReclamate parses as canReclaim")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"CanReclamate", "1"},
            }));

            REQUIRE(fbi.canReclaim);
        }

        SECTION("both default to false")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(!fbi.canStop);
            REQUIRE(!fbi.canReclaim);
        }
    }

    TEST_CASE("parseUnitFbi cloak fields")
    {
        SECTION("the retail cloak block parses")
        {
            // stock ARMCOM carries CloakCost=200, CloakCostMoving=1000,
            // mincloakdistance=40; mines additionally set init_cloaked=1
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"CloakCost", "200"},
                {"CloakCostMoving", "1000"},
                {"mincloakdistance", "40"},
                {"init_cloaked", "1"},
            }));

            REQUIRE(fbi.cloakCost == 200.0f);
            REQUIRE(fbi.cloakCostMoving == 1000.0f);
            REQUIRE(fbi.minCloakDistance == 40);
            REQUIRE(fbi.initCloaked);
        }

        SECTION("cloak fields default to no cloak")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(fbi.cloakCost == 0.0f);
            REQUIRE(fbi.cloakCostMoving == 0.0f);
            REQUIRE(fbi.minCloakDistance == 0);
            REQUIRE(!fbi.initCloaked);
        }
    }

    TEST_CASE("parseUnitFbi sensor fields")
    {
        SECTION("the retail sensor block parses")
        {
            // stock ARMSJAM-class jammer/stealth fields
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"SightDistance", "400"},
                {"RadarDistance", "1440"},
                {"SonarDistance", "1180"},
                {"RadarDistanceJam", "700"},
                {"SonarDistanceJam", "650"},
                {"Stealth", "1"},
            }));

            REQUIRE(fbi.sightDistance == 400);
            REQUIRE(fbi.radarDistance == 1440);
            REQUIRE(fbi.sonarDistance == 1180);
            REQUIRE(fbi.radarDistanceJam == 700);
            REQUIRE(fbi.sonarDistanceJam == 650);
            REQUIRE(fbi.stealth);
        }

        SECTION("sensor fields default to no sensors")
        {
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
            }));

            REQUIRE(fbi.sightDistance == 0);
            REQUIRE(fbi.radarDistance == 0);
            REQUIRE(fbi.sonarDistance == 0);
            REQUIRE(fbi.radarDistanceJam == 0);
            REQUIRE(fbi.sonarDistanceJam == 0);
            REQUIRE(!fbi.stealth);
        }
    }

    TEST_CASE("parseUnitFbi fractional cloak cost")
    {
        SECTION("fractional cloak costs survive parsing")
        {
            // retail stores the raw float (unitdef +0x1da) and truncates
            // only at the energy debit -- parsing must not quantize
            auto fbi = parseUnitFbi(makeUnitInfo({
                {"UnitName", "X"},
                {"Objectname", "X"},
                {"SoundCategory", "X"},
                {"CloakCost", "12.75"},
            }));

            REQUIRE(fbi.cloakCost == 12.75f);
        }
    }
}
