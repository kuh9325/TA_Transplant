#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <rwe/sim/GameHash_util.h>
#include <rwe/util/OpaqueId_io.h>

namespace rwe
{
    struct IdTag;
    using Id = OpaqueId<unsigned int, IdTag>;

    enum class TestEnum
    {
        CaseA,
        CaseB,
        CaseC
    };

    using TestVariant = std::variant<int, bool, float>;

    TEST_CASE("computeHashOf")
    {
        SECTION("works on GameHash")
        {
            REQUIRE(computeHashOf(GameHash(1236)) == GameHash(1236));
        }
        SECTION("works on float")
        {
            // hashes the IEEE-754 bit pattern (5.0f == 0x40A00000)
            REQUIRE(computeHashOf(5.0f) == GameHash(0x40A00000u));
        }
        SECTION("works on negative float")
        {
            // -5.0f == 0xC0A00000; distinct from +5.0f and from zero
            REQUIRE(computeHashOf(-5.0f) == GameHash(0xC0A00000u));
            REQUIRE(computeHashOf(-5.0f) != computeHashOf(5.0f));
            REQUIRE(computeHashOf(-5.0f) != GameHash(0));
        }
        SECTION("distinct negative floats hash distinctly")
        {
            // -1.5f == 0xBFC00000, -2.5f == 0xC0200000
            REQUIRE(computeHashOf(-1.5f) == GameHash(0xBFC00000u));
            REQUIRE(computeHashOf(-2.5f) == GameHash(0xC0200000u));
            REQUIRE(computeHashOf(-1.5f) != computeHashOf(-2.5f));
        }
        SECTION("float hashing is stable across repeated calls")
        {
            REQUIRE(computeHashOf(-123.25f) == computeHashOf(-123.25f));
            REQUIRE(computeHashOf(-123.25f) == GameHash(0xC2F68000u));
        }
        SECTION("canonicalizes negative zero to positive zero")
        {
            REQUIRE(computeHashOf(0.0f) == GameHash(0));
            REQUIRE(computeHashOf(-0.0f) == GameHash(0));
            REQUIRE(computeHashOf(-0.0f) == computeHashOf(0.0f));
        }
        SECTION("canonicalizes NaN payloads")
        {
            const auto nanHash = GameHash(0x7FC00000u);
            REQUIRE(computeHashOf(std::numeric_limits<float>::quiet_NaN()) == nanHash);
            REQUIRE(computeHashOf(std::bit_cast<float>(0x7FC00001u)) == nanHash);
            REQUIRE(computeHashOf(std::bit_cast<float>(0xFFC00000u)) == nanHash);
            REQUIRE(computeHashOf(std::numeric_limits<float>::quiet_NaN())
                == computeHashOf(std::numeric_limits<float>::quiet_NaN()));
        }
        SECTION("infinite floats hash their bit patterns")
        {
            REQUIRE(computeHashOf(std::numeric_limits<float>::infinity())
                == GameHash(0x7F800000u));
            REQUIRE(computeHashOf(-std::numeric_limits<float>::infinity())
                == GameHash(0xFF800000u));
        }
        SECTION("works on bool")
        {
            REQUIRE(computeHashOf(false) == GameHash(0));
            REQUIRE(computeHashOf(true) == GameHash(1));
        }
        SECTION("works on unsigned int")
        {
            REQUIRE(computeHashOf(1234u) == GameHash(1234));
        }
        SECTION("works on int")
        {
            REQUIRE(computeHashOf(1234) == GameHash(1234));
            REQUIRE(computeHashOf(-50) == GameHash(4294967246));
        }
        SECTION("works on string")
        {
            REQUIRE(computeHashOf(std::string("A")) == GameHash(3289118412u));
            REQUIRE(computeHashOf(std::string("fred")) == GameHash(2561928520u));
        }
        SECTION("works on char*")
        {
            REQUIRE(computeHashOf("A") == GameHash(3289118412u));
            REQUIRE(computeHashOf("fred") == GameHash(2561928520u));
        }
        SECTION("works on optional")
        {
            REQUIRE(computeHashOf(std::make_optional(38)) == GameHash(158616734u));
            REQUIRE(computeHashOf(std::optional<int>()) == GameHash(84696351u));
        }
        SECTION("works on vector")
        {
            std::vector<int> v{1, 2, 3, 4};
            REQUIRE(computeHashOf(v) == GameHash(3236247163u));
        }
        SECTION("works on pair")
        {
            std::pair<int, int> v{1, 2};
            REQUIRE(computeHashOf(v) == GameHash(3983810698u));
        }
        SECTION("works on VectorMap")
        {
            VectorMap<int, IdTag> v;
            v.emplace(1);
            v.emplace(2);
            v.emplace(3);
            REQUIRE(computeHashOf(v) == GameHash(4283040018u));
        }
        SECTION("works on enum")
        {
            REQUIRE(computeHashOf(TestEnum::CaseA) == GameHash(0));
            REQUIRE(computeHashOf(TestEnum::CaseB) == GameHash(1));
            REQUIRE(computeHashOf(TestEnum::CaseC) == GameHash(2));
        }
        SECTION("works on variant")
        {
            REQUIRE(computeHashOf(TestVariant(12)) == GameHash(91653353u));
            REQUIRE(computeHashOf(TestVariant(true)) == GameHash(3967033079u));
            REQUIRE(computeHashOf(TestVariant(1.0f)) == GameHash(1241222303u));
        }
    }

    TEST_CASE("combineHashes")
    {
        SECTION("combines hashes")
        {
            auto hash = combineHashes(GameHash(5), GameHash(6));
            REQUIRE(hash == GameHash(1500134706u));
        }

        SECTION("hashes arbitrary items before combining")
        {
            auto hash = combineHashes(true, 25, GameHash(4));
            REQUIRE(hash == GameHash(1574996945u));
        }
    }
    TEST_CASE("Phase8B hash distinguishes recovery history and ordered commands", "[phase8b][hash]")
    {
        CampaignAiPlayerState::DeadlockState d;
        const auto before = computeHashOf(d);
        SECTION("stall cooldown") { d.reclaimStalledAt = GameTime(0); }
        SECTION("avoid builder") { d.reclaimAvoid.insert(0); }
        SECTION("approach distance") { d.reclaimBestDistance = 0_ss; }
        SECTION("approach deadline") { d.reclaimDeadline = GameTime(0); }
        SECTION("eviction count") { d.evictAttempts.emplace(0, 1); }
        REQUIRE(computeHashOf(d) != before);
    }

    TEST_CASE("Phase8B hash distinguishes command order presence and identities", "[phase8b][hash]")
    {
        CHECK(computeHashOf(std::deque<UnitOrder>{MoveOrder(SimVector(1_ss, 0_ss, 2_ss)), WaitOrder{5}})
            != computeHashOf(std::deque<UnitOrder>{WaitOrder{5}, MoveOrder(SimVector(1_ss, 0_ss, 2_ss))}));
        CHECK(computeHashOf(std::optional<GameTime>{}) != computeHashOf(std::optional<GameTime>{GameTime(0)}));
        CHECK(computeHashOf(std::string("ARMABC")) != computeHashOf(std::string("ARMCBA")));
        CHECK(computeHashOf(SimVector(1_ss, 2_ss, 3_ss)) != computeHashOf(SimVector(3_ss, 2_ss, 1_ss)));
        VectorMap<int, IdTag> a, b;
        auto a0 = a.emplace(7); auto a1 = a.emplace(7);
        auto b0 = b.emplace(7); auto b1 = b.emplace(7);
        a.remove(a0); a.remove(a1); b.remove(b1); b.remove(b0);
        CHECK(computeHashOf(a) != computeHashOf(b));
    }

    TEST_CASE("Phase8C piece hashing retains contract two for every operation combination", "[phase8c][hash]")
    {
        // Independent explicit contract expression, including the slow path's
        // optional presence and variant tags. Exercise all 64 presence masks.
        for (unsigned n=0; n<1024; ++n)
        {
            UnitMesh p;
            p.offset=SimVector(SimScalar(float(n)-512), SimScalar(-0.0f), SimScalar(float(n)/7));
            p.rotationX=SimAngle(n); p.rotationY=SimAngle(n*3); p.rotationZ=SimAngle(n*7);
            if(n&1) p.xMoveOperation=UnitMesh::MoveOperation(1_ss,2_ss);
            if(n&2) p.yMoveOperation=UnitMesh::MoveOperation(-3_ss,4_ss);
            if(n&4) p.zMoveOperation=UnitMesh::MoveOperation(5_ss,6_ss);
            auto operation=p.xTurnOperation;
            switch((n/64)%3) {
                case 0: operation=UnitMesh::TurnOperation(SimAngle(12),3_ss); break;
                case 1: operation=UnitMesh::SpinOperation(1_ss,2_ss,3_ss); break;
                default: operation=UnitMesh::StopSpinOperation(4_ss,5_ss); break;
            }
            if(n&8) p.xTurnOperation=operation;
            if(n&16) p.yTurnOperation=operation;
            if(n&32) p.zTurnOperation=operation;
            const auto reference=combineHashes(p.offset,p.rotationX,p.rotationY,p.rotationZ,
                p.xMoveOperation,p.yMoveOperation,p.zMoveOperation,
                p.xTurnOperation,p.yTurnOperation,p.zTurnOperation);
            REQUIRE(computeHashOf(p)==reference);
        }
    }

    TEST_CASE("Phase8C canonical scratch preserves large and small unordered payloads", "[phase8c][hash]")
    {
        for (unsigned count : {0u,1u,63u,64u,65u,257u})
        {
            CAPTURE(count);
            MissionInfo mission;
            CampaignAiPlayerState::DeadlockState recovery;
            Projectile p{};
            for (unsigned i=0; i<count; ++i) {
                auto name="LONG_UNIT_TYPE_NAME_"+std::to_string(i);
                mission.useOnlyUnits.insert(name); recovery.reclaimAvoid.insert(i);
                recovery.evictAttempts.emplace(i,i+2); p.damage.emplace(name,i*3);
            }
            const std::set<std::string> names(mission.useOnlyUnits.begin(),mission.useOnlyUnits.end());
            REQUIRE(computeHashOf(mission)==combineHashes(mission.localPlayerId,std::vector<std::string>(names.begin(),names.end())));
            const std::set<unsigned> avoid(recovery.reclaimAvoid.begin(),recovery.reclaimAvoid.end());
            REQUIRE(computeHashOf(recovery)==combineHashes(recovery.blockedSince,recovery.reclaimTarget,recovery.reclaimBuilder,
                recovery.reclaimIssuedAt,recovery.reclaimAttempts,recovery.reclaimStalledAt,recovery.reclaimBestDistance,
                recovery.reclaimDeadline,std::vector<unsigned>(avoid.begin(),avoid.end()),
                std::map<unsigned,unsigned>(recovery.evictAttempts.begin(),recovery.evictAttempts.end())));
            const auto fields=combineHashes(p.owner,p.groundBounce,p.createdAt,p.weaponType,p.firedBy,p.targetUnit,
                p.isDead,p.dieOnFrame,p.position,p.origin,p.velocity,p.damageRadius,p.targetProjectile,p.targetPosition);
            REQUIRE(computeHashOf(p)==combineHashes(fields,std::map<std::string,unsigned>(p.damage.begin(),p.damage.end())));
            const auto original=combineHashes(mission,recovery,p);
            mission.useOnlyUnits.clear(); recovery.reclaimAvoid.clear(); recovery.evictAttempts.clear(); p.damage.clear();
            for (unsigned i=count; i>0; --i) {
                auto n=i-1; auto name="LONG_UNIT_TYPE_NAME_"+std::to_string(n);
                mission.useOnlyUnits.insert(name); recovery.reclaimAvoid.insert(n);
                recovery.evictAttempts.emplace(n,n+2); p.damage.emplace(name,n*3);
            }
            REQUIRE(combineHashes(mission,recovery,p)==original);
        }
    }

}
