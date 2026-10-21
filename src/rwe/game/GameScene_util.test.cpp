#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <rwe/game/GameScene_util.h>
#include <rwe/grid/Grid.h>
#include <rwe/io/gui/gui.h>
#include <rwe/save/SavedGame.h>
#include <rwe/ui/UiPanel.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace fs = std::filesystem;

namespace rwe
{
    namespace
    {
        void expectQuad(const GlColoredVertex* v, float rx, float ry, float rw, float rh)
        {
            REQUIRE(v[0].x == rx);
            REQUIRE(v[0].y == ry);
            REQUIRE(v[1].x == rx);
            REQUIRE(v[1].y == ry + rh);
            REQUIRE(v[2].x == rx + rw);
            REQUIRE(v[2].y == ry + rh);
            REQUIRE(v[3].x == rx + rw);
            REQUIRE(v[3].y == ry + rh);
            REQUIRE(v[4].x == rx + rw);
            REQUIRE(v[4].y == ry);
            REQUIRE(v[5].x == rx);
            REQUIRE(v[5].y == ry);
        }
    }

    TEST_CASE("fillMinimapFogVertices emits nothing when the whole map is in LOS")
    {
        Grid<unsigned char> losGrid(4, 2, 1);
        Grid<unsigned char> exploredGrid(4, 2, 1);
        auto rect = Rectangle2f::fromTopLeft(0.0f, 0.0f, 100.0f, 50.0f);

        std::vector<GlColoredVertex> unexplored;
        std::vector<GlColoredVertex> explored;
        fillMinimapFogVertices(losGrid, exploredGrid, rect, unexplored, explored);

        REQUIRE(unexplored.empty());
        REQUIRE(explored.empty());
    }

    TEST_CASE("fillMinimapFogVertices coalesces a fully unexplored map into one quad")
    {
        Grid<unsigned char> losGrid(4, 2, 0);
        Grid<unsigned char> exploredGrid(4, 2, 0);
        auto rect = Rectangle2f::fromTopLeft(10.0f, 20.0f, 100.0f, 50.0f);

        std::vector<GlColoredVertex> unexplored;
        std::vector<GlColoredVertex> explored;
        fillMinimapFogVertices(losGrid, exploredGrid, rect, unexplored, explored);

        REQUIRE(explored.empty());
        // one quad per row -> 2 quads, 6 vertices each
        REQUIRE(unexplored.size() == 12);
        expectQuad(unexplored.data(), 10.0f, 20.0f, 100.0f, 25.0f);
        expectQuad(unexplored.data() + 6, 10.0f, 45.0f, 100.0f, 25.0f);
    }

    TEST_CASE("fillMinimapFogVertices splits runs at state boundaries")
    {
        // row 0: LOS, unexplored, unexplored, LOS
        // row 1: explored, LOS, LOS, unexplored
        Grid<unsigned char> losGrid(4, 2, 0);
        Grid<unsigned char> exploredGrid(4, 2, 0);
        losGrid.set(0, 0, 1);
        losGrid.set(3, 0, 1);
        exploredGrid.set(0, 1, 1);
        losGrid.set(1, 1, 1);
        losGrid.set(2, 1, 1);

        auto rect = Rectangle2f::fromTopLeft(0.0f, 0.0f, 100.0f, 50.0f);

        std::vector<GlColoredVertex> unexplored;
        std::vector<GlColoredVertex> explored;
        fillMinimapFogVertices(losGrid, exploredGrid, rect, unexplored, explored);

        // unexplored: row0 cells 1-2 -> one quad at x=25 w=50 y=0 h=25
        //             row1 cell 3     -> one quad at x=75 w=25 y=25 h=25
        REQUIRE(unexplored.size() == 12);
        expectQuad(unexplored.data(), 25.0f, 0.0f, 50.0f, 25.0f);
        expectQuad(unexplored.data() + 6, 75.0f, 25.0f, 25.0f, 25.0f);

        // explored: row1 cell 0 -> one quad at x=0 w=25 y=25 h=25
        REQUIRE(explored.size() == 6);
        expectQuad(explored.data(), 0.0f, 25.0f, 25.0f, 25.0f);
    }

    TEST_CASE("fillMinimapFogVertices prefers LOS over explored when both are set")
    {
        Grid<unsigned char> losGrid(2, 1, 1);
        Grid<unsigned char> exploredGrid(2, 1, 1);
        auto rect = Rectangle2f::fromTopLeft(0.0f, 0.0f, 100.0f, 50.0f);

        std::vector<GlColoredVertex> unexplored;
        std::vector<GlColoredVertex> explored;
        fillMinimapFogVertices(losGrid, exploredGrid, rect, unexplored, explored);

        REQUIRE(unexplored.empty());
        REQUIRE(explored.empty());
    }

    TEST_CASE("orderMarkerCursorType maps orders to their map marker cursors")
    {
        REQUIRE(orderMarkerCursorType(UnitOrder(MoveOrder(SimVector(1_ss, 2_ss, 3_ss)))) == CursorType::Move);
        REQUIRE(orderMarkerCursorType(UnitOrder(AttackOrder(SimVector(1_ss, 2_ss, 3_ss)))) == CursorType::Attack);
        REQUIRE(orderMarkerCursorType(UnitOrder(GuardOrder(UnitId(1)))) == CursorType::Guard);
        REQUIRE(orderMarkerCursorType(UnitOrder(RepairOrder(UnitId(1)))) == CursorType::Repair);
        REQUIRE(orderMarkerCursorType(UnitOrder(CompleteBuildOrder(UnitId(1)))) == CursorType::Repair);
        REQUIRE(orderMarkerCursorType(UnitOrder(ReclaimOrder(UnitId(1)))) == CursorType::Reclaim);
        REQUIRE(orderMarkerCursorType(UnitOrder(PatrolOrder({SimVector(1_ss, 2_ss, 3_ss)}))) == CursorType::Patrol);
        REQUIRE(orderMarkerCursorType(UnitOrder(CommandFireOrder(UnitId(1)))) == CursorType::CommandFire);
        REQUIRE(orderMarkerCursorType(UnitOrder(AttackUnitTypeOrder("armflash"))) == CursorType::Attack);
        REQUIRE(orderMarkerCursorType(UnitOrder(UnloadOrder(SimVector(1_ss, 2_ss, 3_ss)))) == CursorType::Unload);
        REQUIRE(orderMarkerCursorType(UnitOrder(LoadOrder(UnitId(1)))) == CursorType::Load);
    }

    TEST_CASE("orderMarkerCursorType returns nothing for orders without markers")
    {
        REQUIRE(orderMarkerCursorType(UnitOrder(BuildOrder("armsolar", SimVector(1_ss, 2_ss, 3_ss)))) == std::nullopt);
        REQUIRE(orderMarkerCursorType(UnitOrder(WaitOrder(30))) == std::nullopt);
        REQUIRE(orderMarkerCursorType(UnitOrder(SelfDestructOrder(30))) == std::nullopt);
        REQUIRE(orderMarkerCursorType(UnitOrder(BuildWeaponOrder(5))) == std::nullopt);
    }

    TEST_CASE("orderMarkerContainsPoint")
    {
        // sprite bounds: center (0,0), half-size (8,6)
        auto bounds = Rectangle2f::fromTopLeft(-8.0f, -6.0f, 16.0f, 12.0f);
        auto markerPos = Vector2f(100.0f, 50.0f);
        float slop = 8.0f;

        SECTION("exact marker center hits")
        {
            REQUIRE(orderMarkerContainsPoint(bounds, markerPos, markerPos, slop));
        }

        SECTION("edge of raw sprite bounds hits")
        {
            REQUIRE(orderMarkerContainsPoint(bounds, markerPos, Vector2f(107.0f, 55.0f), slop));
        }

        SECTION("outside sprite bounds but inside slop hits")
        {
            REQUIRE(orderMarkerContainsPoint(bounds, markerPos, Vector2f(114.0f, 63.0f), slop));
            REQUIRE(orderMarkerContainsPoint(bounds, markerPos, Vector2f(86.0f, 38.0f), slop));
        }

        SECTION("outside the expanded rect misses")
        {
            REQUIRE(!orderMarkerContainsPoint(bounds, markerPos, Vector2f(117.0f, 50.0f), slop));
            REQUIRE(!orderMarkerContainsPoint(bounds, markerPos, Vector2f(100.0f, 65.0f), slop));
        }

        SECTION("offset sprite centers the rect on bounds position")
        {
            // sprite drawn with its rect offset 4px right of the anchor
            auto offsetBounds = Rectangle2f::fromTopLeft(4.0f, -6.0f, 16.0f, 12.0f);
            REQUIRE(orderMarkerContainsPoint(offsetBounds, markerPos, Vector2f(112.0f, 50.0f), 0.0f));
            REQUIRE(!orderMarkerContainsPoint(offsetBounds, markerPos, Vector2f(96.0f, 50.0f), 0.0f));
        }
    }

    TEST_CASE("nearestUiPoint")
    {
        std::vector<Vector2f> points{{10.0f, 10.0f}, {50.0f, 10.0f}, {90.0f, 10.0f}};

        SECTION("returns the nearest point")
        {
            REQUIRE(nearestUiPoint(points, Vector2f(60.0f, 12.0f)) == 1);
            REQUIRE(nearestUiPoint(points, Vector2f(11.0f, 9.0f)) == 0);
        }

        SECTION("keeps the earliest entry on a tie")
        {
            REQUIRE(nearestUiPoint(points, Vector2f(30.0f, 10.0f)) == 0);
            REQUIRE(nearestUiPoint(points, Vector2f(70.0f, 10.0f)) == 1);
        }

        SECTION("returns nothing for an empty list")
        {
            REQUIRE(nearestUiPoint({}, Vector2f(1.0f, 1.0f)) == std::nullopt);
        }
    }

    TEST_CASE("linearBuildCells")
    {
        SECTION("zero-length input produces just the anchor")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(10, 10), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(10, 10)});
        }

        SECTION("1x1 horizontal left-to-right")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(14, 10), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(11, 10), Point(12, 10), Point(13, 10), Point(14, 10)});
        }

        SECTION("1x1 horizontal right-to-left")
        {
            auto cells = linearBuildCells(Point(14, 10), Point(10, 10), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(14, 10), Point(13, 10), Point(12, 10), Point(11, 10), Point(10, 10)});
        }

        SECTION("1x1 vertical top-to-bottom")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(10, 13), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(10, 11), Point(10, 12), Point(10, 13)});
        }

        SECTION("1x1 vertical bottom-to-top")
        {
            auto cells = linearBuildCells(Point(10, 13), Point(10, 10), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(10, 13), Point(10, 12), Point(10, 11), Point(10, 10)});
        }

        SECTION("2x2 spacing abuts footprints")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(16, 10), 2, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(12, 10), Point(14, 10), Point(16, 10)});
        }

        SECTION("rectangular 4x2 footprint horizontal")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(22, 10), 4, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(14, 10), Point(18, 10), Point(22, 10)});
        }

        SECTION("rectangular 4x2 footprint vertical")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(10, 16), 4, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(10, 12), Point(10, 14), Point(10, 16)});
        }

        SECTION("dominant axis is normalized by footprint size")
        {
            // 4x2 footprint: dx=8 -> 2 placements, dz=5 -> 2.5 placements -> vertical wins
            auto cells = linearBuildCells(Point(10, 10), Point(18, 15), 4, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(10, 12), Point(10, 14)});
        }

        SECTION("exact normalized tie chooses horizontal")
        {
            // 4x2 footprint: dx=8 -> 2, dz=4 -> 2 -> tie -> horizontal
            auto cells = linearBuildCells(Point(10, 10), Point(18, 14), 4, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(14, 10), Point(18, 10)});
        }

        SECTION("1x1 exact tie chooses horizontal")
        {
            auto cells = linearBuildCells(Point(10, 10), Point(12, 12), 1, 1);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(11, 10), Point(12, 10)});
        }

        SECTION("cursor not aligned to step stops at the last fitting step")
        {
            // 1x1: anchor x=10, cursor x=13 but only 2 full... cursor cell 13 is a cell itself;
            // with step 1 every cell aligns. Use 2x2: anchor x=10, cursor x=15 -> steps at 10,12,14 (14<=15), 16>15 stops
            auto cells = linearBuildCells(Point(10, 10), Point(15, 10), 2, 2);
            REQUIRE(cells == std::vector<Point>{Point(10, 10), Point(12, 10), Point(14, 10)});
        }

        SECTION("generates no duplicate cells")
        {
            auto cells = linearBuildCells(Point(4, 4), Point(20, 11), 3, 2);
            std::unordered_set<Point> unique(cells.begin(), cells.end());
            REQUIRE(unique.size() == cells.size());
        }
    }

    TEST_CASE("queuedBuildSiteExists")
    {
        auto pos = SimVector(10_ss, 5_ss, 20_ss);
        auto otherPos = SimVector(30_ss, 5_ss, 40_ss);

        SECTION("matches a queued BuildOrder for the same unit and site")
        {
            std::deque<UnitOrder> orders{
                UnitOrder(MoveOrder(SimVector(1_ss, 2_ss, 3_ss))),
                UnitOrder(BuildOrder("armsolar", pos)),
                UnitOrder(AttackOrder(UnitId(7))),
            };
            REQUIRE(queuedBuildSiteExists(orders, "armsolar", pos));
        }

        SECTION("does not match a different position")
        {
            std::deque<UnitOrder> orders{UnitOrder(BuildOrder("armsolar", pos))};
            REQUIRE(!queuedBuildSiteExists(orders, "armsolar", otherPos));
        }

        SECTION("does not match a different unit type at the same site")
        {
            std::deque<UnitOrder> orders{UnitOrder(BuildOrder("armmex", pos))};
            REQUIRE(!queuedBuildSiteExists(orders, "armsolar", pos));
        }

        SECTION("does not match non-build orders")
        {
            std::deque<UnitOrder> orders{UnitOrder(MoveOrder(pos))};
            REQUIRE(!queuedBuildSiteExists(orders, "armsolar", pos));
        }

        SECTION("empty queue is not a match")
        {
            REQUIRE(!queuedBuildSiteExists({}, "armsolar", pos));
        }
    }

    TEST_CASE("appendUiLineVertices appends one white GL_LINES segment")
    {
        std::vector<GlColoredVertex> vertices;
        appendUiLineVertices(vertices, Vector2f(10.0f, 20.0f), Vector2f(30.0f, 40.0f));

        REQUIRE(vertices.size() == 2);
        REQUIRE(vertices[0].x == 10.0f);
        REQUIRE(vertices[0].y == 20.0f);
        REQUIRE(vertices[0].r == 1.0f);
        REQUIRE(vertices[0].g == 1.0f);
        REQUIRE(vertices[0].b == 1.0f);
        REQUIRE(vertices[1].x == 30.0f);
        REQUIRE(vertices[1].y == 40.0f);
    }

    TEST_CASE("appendMinimapSensorCircleVertices emits a closed coloured ring")
    {
        std::vector<GlColoredVertex> vertices;
        Vector3f sensorColor(0.35f, 0.50f, 0.85f);
        appendMinimapSensorCircleVertices(vertices, Vector2f(50.0f, 40.0f), 10.0f, sensorColor);

        // 32 segments x 2 endpoints of GL_LINES geometry
        REQUIRE(vertices.size() == 64);
        // first vertex sits on the circle at angle 0
        REQUIRE(vertices[0].x == Catch::Approx(60.0f));
        REQUIRE(vertices[0].y == Catch::Approx(40.0f));
        REQUIRE(vertices[0].z == 0.0f);
        REQUIRE(vertices[0].r == Catch::Approx(0.35f));
        REQUIRE(vertices[0].b == Catch::Approx(0.85f));
        // the last segment's endpoint closes the loop at angle 0
        REQUIRE(vertices[63].x == Catch::Approx(60.0f));
        REQUIRE(vertices[63].y == Catch::Approx(40.0f));
        // every vertex lies on the radius around the centre
        for (const auto& v : vertices)
        {
            auto dx = v.x - 50.0f;
            auto dy = v.y - 40.0f;
            REQUIRE(std::sqrt(dx * dx + dy * dy) == Catch::Approx(10.0f).margin(0.001f));
        }
    }

    TEST_CASE("appendUiBoxOutlineVertices")
    {
        SECTION("emits four edge quads for a normal box")
        {
            std::vector<GlColoredVertex> vertices;
            appendUiBoxOutlineVertices(vertices, 10.0f, 20.0f, 40.0f, 30.0f, Color(0, 255, 0), 2.0f);

            // 4 edges x 2 triangles x 3 verts
            REQUIRE(vertices.size() == 24);
            REQUIRE(vertices[0].r == 0.0f);
            REQUIRE(vertices[0].g == 1.0f);
            REQUIRE(vertices[0].b == 0.0f);
        }

        SECTION("falls back to a solid quad when thinner than twice the thickness")
        {
            std::vector<GlColoredVertex> vertices;
            appendUiBoxOutlineVertices(vertices, 10.0f, 20.0f, 3.0f, 30.0f, Color(0, 255, 0), 2.0f);
            REQUIRE(vertices.size() == 6);
        }

        SECTION("emits nothing for a degenerate box")
        {
            std::vector<GlColoredVertex> vertices;
            appendUiBoxOutlineVertices(vertices, 10.0f, 20.0f, 0.5f, 30.0f, Color(0, 255, 0), 2.0f);
            REQUIRE(vertices.empty());
        }

        SECTION("multiple boxes accumulate in order")
        {
            std::vector<GlColoredVertex> vertices;
            appendUiBoxOutlineVertices(vertices, 0.0f, 0.0f, 40.0f, 40.0f, Color(0, 255, 0), 2.0f);
            appendUiBoxOutlineVertices(vertices, 50.0f, 50.0f, 40.0f, 40.0f, Color(255, 0, 0), 2.0f);
            REQUIRE(vertices.size() == 48);
            REQUIRE(vertices[24].r == 1.0f);
            REQUIRE(vertices[24].g == 0.0f);
        }
    }

    TEST_CASE("inGameHelpText")
    {
        const std::string raw = R"(
[Help]
    {
    Line0 = CTRL+A|Select all units;
    Line1 = |;
    Line2 = ESC|Cancels the current command;
    Line3 = F2|Display the Options menu;
    }
)";

        SECTION("formats key|description pairs for the page")
        {
            auto text = inGameHelpText(raw, 0, 1);
            REQUIRE(text.find("CTRL+A  --  Select all units") != std::string::npos);
            REQUIRE(text.find("ESC  --  Cancels the current command") != std::string::npos);
        }

        SECTION("blank spacer entries produce empty lines")
        {
            auto text = inGameHelpText(raw, 0, 1);
            REQUIRE(text.find("\n\n") != std::string::npos);
        }

        SECTION("splits lines evenly across pages")
        {
            auto page0 = inGameHelpText(raw, 0, 2);
            auto page1 = inGameHelpText(raw, 1, 2);
            REQUIRE(page0.find("CTRL+A") != std::string::npos);
            REQUIRE(page0.find("ESC") == std::string::npos);
            REQUIRE(page1.find("ESC") != std::string::npos);
            REQUIRE(page1.find("CTRL+A") == std::string::npos);
        }

        SECTION("an out-of-range page yields nothing")
        {
            REQUIRE(inGameHelpText(raw, 5, 3).empty());
        }

        SECTION("malformed input yields nothing")
        {
            REQUIRE(inGameHelpText("not tdf at all", 0, 3).empty());
            REQUIRE(inGameHelpText("", 0, 3).empty());
        }
    }

    TEST_CASE("inGameHelpText against the real HELP.TDF")
    {
        // stock data: [Help] Line0..Line38, CRLF file, "key|desc"
        // entries with "|" blanks; 3 pages (the Page staged button)
        // split them 13/13/13. Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto rawBytes = vfs.readFile("gamedata/HELP.TDF");
        REQUIRE(rawBytes);
        std::string raw(rawBytes->begin(), rawBytes->end());

        auto page0 = inGameHelpText(raw, 0, 3);
        auto page1 = inGameHelpText(raw, 1, 3);
        auto page2 = inGameHelpText(raw, 2, 3);

        SECTION("each of the three pages has distinct authored content")
        {
            REQUIRE(page0.find("CTRL+A") != std::string::npos);
            REQUIRE(page1.find("P  --  Give a patrol order") != std::string::npos);
            REQUIRE(page2.find("F2  --  Display the Options menu") != std::string::npos);
            REQUIRE(page0 != page1);
            REQUIRE(page1 != page2);
        }

        SECTION("pages carry no control-box characters")
        {
            for (const auto* page : {&page0, &page1, &page2})
            {
                for (char ch : *page)
                {
                    REQUIRE((static_cast<unsigned char>(ch) >= 32 || ch == '\n'));
                }
            }
        }

        SECTION("page 3 is out of range")
        {
            REQUIRE(inGameHelpText(raw, 3, 3).empty());
        }
    }

    TEST_CASE("makeGameOptionsRows emits the retail GameSettings row set")
    {
        // TotalA.exe GameSettings builder (0x45f1d0) draws nine
        // label/value rows for non-multiplayer games, in this order
        auto rows = makeGameOptionsRows(
            "Great Divide 2",
            SkirmishCommanderDeath::GameEnds,
            SkirmishStartLocation::Fixed,
            false,
            0,
            SkirmishDifficulty::Medium,
            1000,
            1000,
            250);

        REQUIRE(rows.size() == 9);
        REQUIRE(rows[0] == std::make_pair(std::string("Commander Death:"), std::string("Game ends")));
        REQUIRE(rows[1] == std::make_pair(std::string("Starting Locations:"), std::string("Fixed")));
        REQUIRE(rows[2] == std::make_pair(std::string("Mapping Mode:"), std::string("Unmapped")));
        REQUIRE(rows[3] == std::make_pair(std::string("Line of Sight:"), std::string("Permanent")));
        REQUIRE(rows[4] == std::make_pair(std::string("Difficulty:"), std::string("Medium")));
        REQUIRE(rows[5] == std::make_pair(std::string("Map:"), std::string("Great Divide 2")));
        REQUIRE(rows[6] == std::make_pair(std::string("Starting Metal:"), std::string("1000")));
        REQUIRE(rows[7] == std::make_pair(std::string("Starting Energy:"), std::string("1000")));
        REQUIRE(rows[8] == std::make_pair(std::string("Max Units:"), std::string("250")));
    }

    TEST_CASE("makeGameOptionsRows maps runtime state to retail enum labels")
    {
        const auto d = SkirmishDifficulty::Medium;
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, true, 0, d, 1, 2, 250)[2].second == "Mapped");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 0, d, 1, 2, 250)[2].second == "Unmapped");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 0, d, 1, 2, 250)[3].second == "Permanent");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 1, d, 1, 2, 250)[3].second == "True");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 2, d, 1, 2, 250)[3].second == "Circular");
        // out-of-range mode falls back to the retail default
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 7, d, 1, 2, 250)[3].second == "Permanent");

        // the C13A-wired settings report their actual stage values
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::Continues, SkirmishStartLocation::Fixed, false, 0, d, 1, 2, 250)[0].second == "Continues");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Random, false, 0, d, 1, 2, 250)[1].second == "Random");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 0, SkirmishDifficulty::Easy, 1, 2, 250)[4].second == "Easy");
        REQUIRE(makeGameOptionsRows("X", SkirmishCommanderDeath::GameEnds, SkirmishStartLocation::Fixed, false, 0, SkirmishDifficulty::Hard, 1, 2, 250)[4].second == "Hard");
    }
}

namespace rwe
{
    namespace
    {
        UnitDefinition makeOrdersPanelDef()
        {
            UnitDefinition def;
            def.canAttack = false;
            def.canMove = false;
            def.canGuard = false;
            def.canPatrol = false;
            def.canDGun = false;
            def.commander = false;
            def.maxHitPoints = 100;
            def.isMobile = false;
            def.floater = false;
            def.canHover = false;
            def.waterline = 0;
            def.canFly = false;
            def.cruiseAltitude = SimScalar(0);
            def.builder = false;
            def.buildTime = 0;
            def.buildCostEnergy = Energy(0);
            def.buildCostMetal = Metal(0);
            def.workerTimePerTick = 0;
            def.buildDistance = SimScalar(0);
            def.onOffable = false;
            def.activateWhenBuilt = false;
            def.energyMake = Energy(0);
            def.metalMake = Metal(0);
            def.energyUse = Energy(0);
            def.metalUse = Metal(0);
            def.makesMetal = Metal(0);
            def.extractsMetal = Metal(0);
            def.energyStorage = Energy(0);
            def.metalStorage = Metal(0);
            def.windGenerator = Energy(0);
            def.tidalGenerator = false;
            def.yardMapContainsGeo = false;
            def.transportCapacity = 0;
            return def;
        }

        std::unique_ptr<UiStagedButton> makePanelButton(const std::string& name, int x, int y)
        {
            std::vector<UiStagedButton::StageInfo> stages;
            stages.emplace_back(nullptr, "");
            auto button = std::make_unique<UiStagedButton>(x, y, 55, 31, std::move(stages), nullptr, nullptr);
            button->setName(name);
            return button;
        }

        // mirrors the stock ARMGEN.GUI orders grid: ARMLOAD and
        // ARMBLAST share the (64,317) gadget rect and ARMLOAD is
        // declared first, so the later BLAST sprite draws on top
        UiPanel makeOrdersPanel(const std::string& prefix)
        {
            UiPanel panel(0, 0, 128, 400);
            panel.setName(prefix + "GEN");
            panel.appendChild(makePanelButton(prefix + "RECLAIM", 5, 172));
            panel.appendChild(makePanelButton(prefix + "UNLOAD", 64, 172));
            panel.appendChild(makePanelButton(prefix + "REPAIR", 5, 207));
            panel.appendChild(makePanelButton(prefix + "CAPTURE", 64, 207));
            panel.appendChild(makePanelButton(prefix + "MOVE", 5, 247));
            panel.appendChild(makePanelButton(prefix + "STOP", 64, 247));
            panel.appendChild(makePanelButton(prefix + "DEFEND", 5, 282));
            panel.appendChild(makePanelButton(prefix + "PATROL", 64, 282));
            panel.appendChild(makePanelButton(prefix + "ATTACK", 5, 317));
            panel.appendChild(makePanelButton(prefix + "LOAD", 64, 317));
            panel.appendChild(makePanelButton(prefix + "BLAST", 64, 317));
            return panel;
        }
    }

    TEST_CASE("aggregateOrdersPanelCapabilities ORs across the selection")
    {
        SECTION("an empty selection exposes nothing")
        {
            auto caps = aggregateOrdersPanelCapabilities({});
            REQUIRE(!caps.canMove);
            REQUIRE(!caps.canLoad);
            REQUIRE(!caps.canDGun);
        }

        SECTION("a transport selection sets only canLoad")
        {
            auto def = makeOrdersPanelDef();
            def.canMove = true;
            def.canStop = true;
            def.canPatrol = true;
            def.canLoad = true;

            auto caps = aggregateOrdersPanelCapabilities({&def});
            REQUIRE(caps.canLoad);
            REQUIRE(caps.canMove);
            REQUIRE(!caps.canDGun);
            REQUIRE(!caps.canCapture);
        }

        SECTION("mixed selection ORs capabilities")
        {
            auto transport = makeOrdersPanelDef();
            transport.canLoad = true;
            auto peewee = makeOrdersPanelDef();
            peewee.canAttack = true;

            auto caps = aggregateOrdersPanelCapabilities({&transport, &peewee});
            REQUIRE(caps.canLoad);
            REQUIRE(caps.canAttack);
        }
    }

    TEST_CASE("applyOrdersPanelCapabilities exposes transport gadgets per retail")
    {
        SECTION("a transport selection shows LOAD and hides BLAST")
        {
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canMove = true;
            caps.canStop = true;
            caps.canPatrol = true;
            caps.canLoad = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            auto load = panel.find<UiStagedButton>("ARMLOAD");
            auto blast = panel.find<UiStagedButton>("ARMBLAST");
            auto unload = panel.find<UiStagedButton>("ARMUNLOAD");
            REQUIRE(load);
            REQUIRE(blast);
            REQUIRE(unload);
            REQUIRE(!load->get().isHidden());
            REQUIRE(load->get().isEnabled());
            REQUIRE(blast->get().isHidden());
            REQUIRE(unload->get().isEnabled());
        }

        SECTION("a plain combat unit hides LOAD and greys UNLOAD")
        {
            // ARMPW: canattack but no canload/candgun -- retail
            // deactivates LOAD and greys UNLOAD and BLAST
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canMove = true;
            caps.canStop = true;
            caps.canAttack = true;
            caps.canGuard = true;
            caps.canPatrol = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            REQUIRE(panel.find<UiStagedButton>("ARMLOAD")->get().isHidden());
            REQUIRE(!panel.find<UiStagedButton>("ARMBLAST")->get().isHidden());
            REQUIRE(!panel.find<UiStagedButton>("ARMBLAST")->get().isEnabled());
            REQUIRE(!panel.find<UiStagedButton>("ARMUNLOAD")->get().isEnabled());
            REQUIRE(panel.find<UiStagedButton>("ARMATTACK")->get().isEnabled());
            REQUIRE(!panel.find<UiStagedButton>("ARMCAPTURE")->get().isEnabled());
        }

        SECTION("a commander keeps BLAST enabled while LOAD stays hidden")
        {
            // ARMCOM: candgun but no canload -- BLAST occupies the
            // shared slot and LOAD never appears
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canMove = true;
            caps.canStop = true;
            caps.canAttack = true;
            caps.canGuard = true;
            caps.canPatrol = true;
            caps.canReclaim = true;
            caps.canCapture = true;
            caps.canDGun = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            REQUIRE(panel.find<UiStagedButton>("ARMLOAD")->get().isHidden());
            auto blast = panel.find<UiStagedButton>("ARMBLAST");
            REQUIRE(!blast->get().isHidden());
            REQUIRE(blast->get().isEnabled());
            REQUIRE(panel.find<UiStagedButton>("ARMREPAIR")->get().isEnabled());
        }

        SECTION("CORE gadgets resolve through the COR prefix")
        {
            auto panel = makeOrdersPanel("COR");
            OrdersPanelCapabilities caps;
            caps.canLoad = true;
            applyOrdersPanelCapabilities(panel, "COR", caps);

            REQUIRE(!panel.find<UiStagedButton>("CORLOAD")->get().isHidden());
            REQUIRE(panel.find<UiStagedButton>("CORBLAST")->get().isHidden());
            REQUIRE(panel.find<UiStagedButton>("CORUNLOAD")->get().isEnabled());
        }

        SECTION("clicking the shared slot activates LOAD for a transport")
        {
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canLoad = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            panel.mouseDown(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "ARMLOAD");
        }

        SECTION("clicking the shared slot activates BLAST for a commander")
        {
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canDGun = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            panel.mouseDown(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "ARMBLAST");
        }

        SECTION("the shared slot is inert for a plain combat unit")
        {
            // LOAD hidden + BLAST greyed: neither may fire
            auto panel = makeOrdersPanel("ARM");
            OrdersPanelCapabilities caps;
            caps.canAttack = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            panel.mouseDown(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(90, 330, MouseButtonEvent::MouseButton::Left));

            REQUIRE(!received);
        }

        SECTION("a hidden gadget's quickkey is inert")
        {
            auto panel = makeOrdersPanel("ARM");
            auto load = panel.find<UiStagedButton>("ARMLOAD");
            load->get().setQuickKey(SDLK_L);

            OrdersPanelCapabilities caps;
            caps.canAttack = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            panel.keyDown(KeyEvent(SDLK_L));
            REQUIRE(!received);
        }

        SECTION("a visible gadget's quickkey still fires")
        {
            auto panel = makeOrdersPanel("ARM");
            auto load = panel.find<UiStagedButton>("ARMLOAD");
            load->get().setQuickKey(SDLK_L);

            OrdersPanelCapabilities caps;
            caps.canLoad = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            panel.keyDown(KeyEvent(SDLK_L));
            REQUIRE(received);
            REQUIRE(received->controlName == "ARMLOAD");
        }
    }

    TEST_CASE("aggregateOrdersPanelCapabilities lights BLAST for a commandfire weapon")
    {
        // a stockpile silo has no FBI candgun flag; retail still arms
        // BLAST via its commandfire=1 weapon (the same order the
        // commander D-gun uses)
        auto silo = makeOrdersPanelDef();
        silo.weapon1 = "NUKEWEAPON";

        std::unordered_map<std::string, WeaponDefinition> weapons;
        WeaponDefinition wd{};
        wd.commandFire = true;
        weapons.insert_or_assign("NUKEWEAPON", wd);

        auto caps = aggregateOrdersPanelCapabilities({&silo}, weapons);
        REQUIRE(caps.canDGun);
    }

    TEST_CASE("unitDefinitionHasStockpileWeapon and unitDefinitionHasCommandFireWeapon read the weapon TDF flags")
    {
        GameSimulation sim(MapTerrain(Grid<unsigned char>(16, 16, 64), 0_ss), 255, 0, 0);

        WeaponDefinition stock{};
        stock.stockpile = true;
        stock.commandFire = true;
        sim.weaponDefinitions.insert_or_assign("NUCLEAR_MISSILE", stock);

        WeaponDefinition plain{};
        sim.weaponDefinitions.insert_or_assign("PEASHOOTER", plain);

        UnitDefinition silo{};
        silo.weapon1 = "nuclear_missile"; // names are case-folded on lookup
        REQUIRE(unitDefinitionHasStockpileWeapon(sim, silo));
        REQUIRE(unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, silo));

        UnitDefinition tank{};
        tank.weapon1 = "PEASHOOTER";
        REQUIRE(!unitDefinitionHasStockpileWeapon(sim, tank));
        REQUIRE(!unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, tank));

        UnitDefinition unarmed{};
        REQUIRE(!unitDefinitionHasStockpileWeapon(sim, unarmed));
        REQUIRE(!unitDefinitionHasCommandFireWeapon(sim.weaponDefinitions, unarmed));
    }

    TEST_CASE("initialUnitGuiInfo opens the Build section for stockpile-capable units")
    {
        UnitDefinition def{};

        REQUIRE(initialUnitGuiInfo(def, true).section == UnitGuiInfo::Section::Build);
        REQUIRE(initialUnitGuiInfo(def, false).section == UnitGuiInfo::Section::Orders);

        def.builder = true;
        REQUIRE(initialUnitGuiInfo(def, false).section == UnitGuiInfo::Section::Build);
    }

    TEST_CASE("shared player commands preserve queue and stance semantics", "[player-command]")
    {
        static CobScript emptyScript{{}, {}, {}, 0};
        UnitState unit(std::vector<UnitMesh>(), std::make_unique<CobEnvironment>(&emptyScript));
        unit.position = SimVector(10_ss, 0_ss, 20_ss);
        using Order = PlayerUnitCommand::IssueOrder;
        const SimVector a(30_ss, 0_ss, 40_ss), b(50_ss, 0_ss, 60_ss);
        REQUIRE(applyBasicPlayerUnitCommand(unit, Order(MoveOrder(a), Order::Immediate)));
        REQUIRE(applyBasicPlayerUnitCommand(unit, Order(MoveOrder(b), Order::Queued)));
        REQUIRE(unit.orders.size() == 2);
        REQUIRE((std::get<MoveOrder>(unit.orders.front()).destination == a));
        REQUIRE((std::get<MoveOrder>(unit.orders.back()).destination == b));
        REQUIRE(applyBasicPlayerUnitCommand(unit, PlayerUnitCommand::SetFireOrders{UnitFireOrders::HoldFire}));
        REQUIRE(unit.fireOrders == UnitFireOrders::HoldFire);
        REQUIRE(unit.orders.size() == 2);
        REQUIRE(applyBasicPlayerUnitCommand(unit, PlayerUnitCommand::ModifyBuildQueue{2, "ARMPW"}));
        REQUIRE(unit.buildQueue.front().second == 2);
        REQUIRE(applyBasicPlayerUnitCommand(unit, PlayerUnitCommand::Stop{}));
        REQUIRE(unit.orders.empty());
        REQUIRE(unit.buildQueue.front().second == 2);
        REQUIRE(unit.fireOrders == UnitFireOrders::HoldFire);
        REQUIRE(applyBasicPlayerUnitCommand(unit, Order(PatrolOrder({a}), Order::Immediate)));
        REQUIRE(applyBasicPlayerUnitCommand(unit, Order(PatrolOrder({b}), Order::Queued)));
        REQUIRE(unit.orders.size() == 1);
        const auto& waypoints = std::get<PatrolOrder>(unit.orders.front()).waypoints;
        REQUIRE(waypoints.size() == 3);
        REQUIRE((waypoints[0] == a));
        REQUIRE((waypoints[1] == b));
        REQUIRE((waypoints[2] == unit.position));
        REQUIRE_FALSE(applyBasicPlayerUnitCommand(unit, PlayerUnitCommand::SetCloak{true}));
        REQUIRE(unit.orders.size() == 1);
    }

    TEST_CASE("applyModifyWeaponQueue queues and cancels stockpile rounds")
    {
        static CobScript emptyScript{{}, {}, {}, 0};
        UnitState unit(std::vector<UnitMesh>(), std::make_unique<CobEnvironment>(&emptyScript));

        // primary clicks queue one round each, merging onto the single
        // background stockpile order
        applyModifyWeaponQueue(unit, 1);
        applyModifyWeaponQueue(unit, 1);
        REQUIRE(unit.backgroundOrders.size() == 1);
        REQUIRE(std::get<BuildWeaponOrder>(unit.backgroundOrders.front()).remaining == 2);

        // shift-click queues five
        applyModifyWeaponQueue(unit, 5);
        REQUIRE(std::get<BuildWeaponOrder>(unit.backgroundOrders.front()).remaining == 7);

        // secondary clicks cancel pending rounds
        applyModifyWeaponQueue(unit, -3);
        REQUIRE(std::get<BuildWeaponOrder>(unit.backgroundOrders.front()).remaining == 4);

        // draining the queue removes the order entirely
        applyModifyWeaponQueue(unit, -10);
        REQUIRE(unit.backgroundOrders.empty());

        // cancelling with nothing queued is a no-op
        applyModifyWeaponQueue(unit, -1);
        REQUIRE(unit.backgroundOrders.empty());
    }

    TEST_CASE("real stockpile GUIs expose a weapon build button")
    {
        // every retail stockpile unit authors its own GUI page whose
        // commonattribs=8 build-button gadget is the only player-facing
        // stockpile control. Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }
        auto vfs = constructVfs(fs::path(dataPathEnv));

        for (const auto* guiName : {"ARMSILO1", "CORSILO1", "ARMAMD1", "CORFMD1", "ARMEMP1", "CORTRON1"})
        {
            auto guiBytes = vfs.readFile(std::string("guis/") + guiName + ".GUI");
            REQUIRE(guiBytes);
            auto entries = parseGuiFromBytes(*guiBytes);
            REQUIRE(entries);
            auto it = std::find_if(entries->begin(), entries->end(), [](const auto& e) {
                return (e.common.commonAttribs & GuiCommonAttrib::WeaponBuildButton)
                    && (e.common.attribs & GuiButtonAttrib::BehaviorBuildButton);
            });
            INFO(guiName);
            REQUIRE(it != entries->end());
        }
    }

    TEST_CASE("real ARMGEN.GUI orders-panel exposure")
    {
        // end-to-end check on authentic stock data: ARMLOAD and
        // ARMBLAST share one gadget slot in the authored GUI, so the
        // panel must hide whichever gadget the selection cannot use.
        // Skips without RWE_DATA_PATH.
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !fs::exists(fs::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(fs::path(dataPathEnv));
        auto guiBytes = vfs.readFile("guis/ARMGEN.GUI");
        REQUIRE(guiBytes);
        auto entries = parseGuiFromBytes(*guiBytes);
        REQUIRE(entries);

        auto find = [&](const std::string& name) {
            return std::find_if(entries->begin(), entries->end(), [&](const auto& e) { return e.common.name == name; });
        };

        SECTION("the stock GUI declares ARMLOAD and ARMBLAST at the same rect")
        {
            auto load = find("ARMLOAD");
            auto blast = find("ARMBLAST");
            auto unload = find("ARMUNLOAD");
            REQUIRE(load != entries->end());
            REQUIRE(blast != entries->end());
            REQUIRE(unload != entries->end());
            REQUIRE(load->common.xpos == blast->common.xpos);
            REQUIRE(load->common.ypos == blast->common.ypos);
        }

        SECTION("a transport selection exposes ARMLOAD on the real panel")
        {
            // build the panel exactly as componentFromGuiEntry orders
            // it: every gadget instantiated in GUI order
            UiPanel panel(0, 0, 128, 400);
            for (std::size_t i = 1; i < entries->size(); ++i)
            {
                const auto& e = (*entries)[i];
                auto c = makePanelButton(e.common.name, e.common.xpos, e.common.ypos);
                panel.appendChild(std::move(c));
            }

            OrdersPanelCapabilities caps;
            caps.canLoad = true;
            applyOrdersPanelCapabilities(panel, "ARM", caps);

            auto load = panel.find<UiStagedButton>("ARMLOAD");
            auto blast = panel.find<UiStagedButton>("ARMBLAST");
            REQUIRE(load);
            REQUIRE(blast);
            REQUIRE(!load->get().isHidden());
            REQUIRE(blast->get().isHidden());

            // the visible control must produce the ARMLOAD group
            // message the GameScene dispatch consumes
            std::optional<GroupMessage> received;
            auto sub = panel.groupMessages().subscribe([&received](const auto& m) { received = m; });

            const auto& loadEntry = *find("ARMLOAD");
            auto cx = loadEntry.common.xpos + (loadEntry.common.width / 2);
            auto cy = loadEntry.common.ypos + (loadEntry.common.height / 2);
            panel.mouseDown(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));
            panel.mouseUp(MouseButtonEvent(cx, cy, MouseButtonEvent::MouseButton::Left));

            REQUIRE(received);
            REQUIRE(received->controlName == "ARMLOAD");
        }
    }
}

namespace rwe
{
    namespace
    {
        struct CloakPanelFixture
        {
            GameSimulation sim;
            PlayerId playerId;

            CloakPanelFixture()
                : sim(MapTerrain(Grid<unsigned char>(16, 16, 64), 0_ss), 255, 0, 0)
            {
                UnitDefinition cloaker{};
                cloaker.unitName = "CLOAKER";
                cloaker.objectName = "CLOAKER";
                cloaker.cloakCost = 100.0f;
                cloaker.isMobile = true;
                cloaker.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
                sim.unitDefinitions.insert_or_assign("CLOAKER", cloaker);

                UnitDefinition plain{};
                plain.unitName = "PLAIN";
                plain.objectName = "PLAIN";
                plain.isMobile = true;
                plain.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
                sim.unitDefinitions.insert_or_assign("PLAIN", plain);

                for (const auto& name : {"CLOAKER", "PLAIN"})
                {
                    sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                    sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
                }

                playerId = sim.addPlayer(GamePlayerInfo{
                    std::nullopt,
                    GamePlayerType::Human,
                    PlayerColorIndex(0),
                    GamePlayerStatus::Alive,
                    "ARM",
                    std::nullopt,
                    Metal(10000),
                    Energy(10000),
                    Metal(10000),
                    Energy(10000),
                    Metal(10000),
                    Energy(10000)});
            }

            UnitId spawn(const std::string& unitType, int x = 4, int y = 4)
            {
                auto id = sim.trySpawnUnit(unitType, playerId, sim.terrain.heightmapIndexToWorldCenter(x, y), std::nullopt);
                REQUIRE(id);
                return *id;
            }
        };
    }

    TEST_CASE("cloakPanelStage")
    {
        // retail 0x41b2e0: the CLOAK mask slot inits to 3, the first
        // cloak-capable unit writes its requested bit, and every
        // further capable unit latches 2 unconditionally
        CloakPanelFixture f;

        SECTION("no selected units deactivates the strip")
        {
            REQUIRE(cloakPanelStage(f.sim, {}) == 3);
        }

        SECTION("a selection without a cloakable unit deactivates the strip")
        {
            auto id = f.spawn("PLAIN");
            REQUIRE(cloakPanelStage(f.sim, {id}) == 3);
        }

        SECTION("one decloaked cloakable shows the off stage")
        {
            auto id = f.spawn("CLOAKER");
            REQUIRE(cloakPanelStage(f.sim, {id}) == 0);
        }

        SECTION("one cloaking-requested unit shows the on stage")
        {
            auto id = f.spawn("CLOAKER");
            f.sim.getUnitState(id).cloakRequested = true;
            REQUIRE(cloakPanelStage(f.sim, {id}) == 1);
        }

        SECTION("two cloakables latch the mixed stage even in agreement")
        {
            // the retail quirk: agreement is never checked, a second
            // capable unit always forces stage 2 -- the strip can then
            // only decloak the selection
            auto a = f.spawn("CLOAKER");
            auto b = f.spawn("CLOAKER", 8, 8);
            f.sim.getUnitState(a).cloakRequested = true;
            f.sim.getUnitState(b).cloakRequested = true;
            REQUIRE(cloakPanelStage(f.sim, {a, b}) == 2);
        }

        SECTION("a cloakable mixed with a plain unit shows the cloakable's state")
        {
            auto cloaker = f.spawn("CLOAKER");
            auto plain = f.spawn("PLAIN", 8, 8);
            f.sim.getUnitState(cloaker).cloakRequested = true;
            REQUIRE(cloakPanelStage(f.sim, {cloaker, plain}) == 1);
        }

        SECTION("a stale selection entry is skipped")
        {
            auto cloaker = f.spawn("CLOAKER");
            f.sim.getUnitState(cloaker).cloakRequested = true;

            std::unordered_set<UnitId> selection{cloaker, UnitId(9999)};
            REQUIRE(cloakPanelStage(f.sim, selection) == 1);
        }
    }
}

namespace rwe
{
    TEST_CASE("missionEndAction gates the campaign result transition")
    {
        // the GameScene latch around the committed evaluator result:
        // the igvictory/igdefeat banner + ENDMSN handoff must fire
        // exactly once for a stable result, never while InProgress,
        // and never again after declaration (retail +0x3923b commit).

        SECTION("InProgress never commits")
        {
            REQUIRE(missionEndAction(MissionResult::InProgress, false) == MissionEndAction::None);
            REQUIRE(missionEndAction(MissionResult::InProgress, true) == MissionEndAction::None);
        }

        SECTION("a stable result commits exactly once")
        {
            REQUIRE(missionEndAction(MissionResult::Victory, false) == MissionEndAction::DeclareVictory);
            REQUIRE(missionEndAction(MissionResult::Defeat, false) == MissionEndAction::DeclareDefeat);
        }

        SECTION("declared results never re-fire the transition")
        {
            REQUIRE(missionEndAction(MissionResult::Victory, true) == MissionEndAction::None);
            REQUIRE(missionEndAction(MissionResult::Defeat, true) == MissionEndAction::None);
        }
    }

    TEST_CASE("rebuildUnitGuiInfos registers GUI state for units restored without spawn events")
    {
        // Restored saves insert units via createUnit+restoreSlotsChecked,
        // which never emits UnitSpawnedEvent -- the only writer of
        // GameScene's unitGuiInfos. Selection then hit
        // "Gui info not found for unit 0". This test drives the real
        // restore path (encode -> decode -> applySavedGame) so the
        // rebuilt map is proven against genuine post-load state.

        auto makeSim = []() {
            return GameSimulation(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);
        };

        auto makePlayer = [](PlayerColorIndex color, const std::string& side) {
            return GamePlayerInfo{
                std::nullopt,
                GamePlayerType::Human,
                color,
                GamePlayerStatus::Alive,
                side,
                std::nullopt,
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000)};
        };

        auto makeDef = [](const std::string& name, bool builder) {
            UnitDefinition def{};
            def.unitName = name;
            def.objectName = name;
            def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
            def.turnRate = SimScalar(1024);
            def.maxVelocity = SimScalar(10);
            def.acceleration = SimScalar(1);
            def.brakeRate = SimScalar(1);
            def.canMove = true;
            def.isMobile = true;
            def.builder = builder;
            def.maxHitPoints = 500;
            def.buildTime = 100;
            def.buildCostEnergy = Energy(1000);
            def.buildCostMetal = Metal(500);
            return def;
        };

        auto populateDefinitions = [&](GameSimulation& sim) {
            for (const auto& name : {"ARMCOM", "ARMTANK"})
            {
                sim.unitDefinitions.insert_or_assign(name, makeDef(name, name == std::string("ARMCOM")));
                sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(0_ss, {}));
                sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }
        };

        // source world: builder gets UnitId 0, then a non-builder
        auto simA = makeSim();
        populateDefinitions(simA);
        auto playerA = simA.addPlayer(makePlayer(PlayerColorIndex(0), "ARM"));
        simA.addPlayer(makePlayer(PlayerColorIndex(1), "CORE"));

        auto commander = simA.trySpawnUnit("ARMCOM", playerA, simA.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(commander);
        auto tank = simA.trySpawnUnit("ARMTANK", playerA, simA.terrain.heightmapIndexToWorldCenter(20, 8), std::nullopt);
        REQUIRE(tank);

        GameParameters params("TESTMAP", 0);
        params.players[0] = PlayerInfo{
            std::nullopt,
            PlayerControllerType(PlayerControllerTypeHuman()),
            "ARM",
            PlayerColorIndex(0),
            Metal(10000),
            Energy(10000)};
        params.players[1] = PlayerInfo{
            std::nullopt,
            PlayerControllerType(PlayerControllerTypeComputer()),
            "CORE",
            PlayerColorIndex(1),
            Metal(10000),
            Energy(10000)};

        const auto bytes = encodeSavedGame(simA, params, nullptr, "TESTSAVE", 0);
        auto decoded = decodeSavedGame(bytes);

        // LoadingScene builds the def tables from map+schema, then applies
        auto simB = makeSim();
        populateDefinitions(simB);
        applySavedGame(simB, decoded, nullptr);

        // what GameScene::init() now runs before any selection/panel query
        std::unordered_map<UnitId, UnitGuiInfo> guiInfos;
        rebuildUnitGuiInfos(simB, guiInfos);

        // every live unit -- including UnitId 0 -- is covered
        size_t liveUnits = 0;
        for (const auto& pair : simB.units)
        {
            ++liveUnits;
            REQUIRE(guiInfos.count(pair.first) == 1);
        }
        REQUIRE(liveUnits == 2);
        REQUIRE(guiInfos.size() == liveUnits);

        REQUIRE(guiInfos.at(UnitId(0)).section == UnitGuiInfo::Section::Build);
        REQUIRE(guiInfos.at(UnitId(0)).currentBuildPage == 0);

        auto tankIt = guiInfos.find(*tank);
        REQUIRE(tankIt != guiInfos.end());
        REQUIRE(tankIt->second.section == UnitGuiInfo::Section::Orders);
        REQUIRE(tankIt->second.currentBuildPage == 0);
    }

    namespace
    {
        GameSimulation makeNanolatheSim()
        {
            GameSimulation sim(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);

            for (const auto& name : {"ARMCOM", "ARMTANK"})
            {
                UnitDefinition def{};
                def.unitName = name;
                def.objectName = name;
                def.movementCollisionInfo = UnitDefinition::AdHocMovementClass{2, 2, 0, 0, 0, 0};
                def.turnRate = SimScalar(1024);
                def.maxVelocity = SimScalar(10);
                def.acceleration = SimScalar(1);
                def.brakeRate = SimScalar(1);
                def.canMove = true;
                def.isMobile = true;
                def.builder = name == std::string("ARMCOM");
                def.maxHitPoints = 500;
                def.buildTime = 100;
                def.buildCostEnergy = Energy(1000);
                def.buildCostMetal = Metal(500);
                sim.unitDefinitions.insert_or_assign(name, def);
                sim.unitModelDefinitions.insert_or_assign(name, createUnitModelDefinition(10_ss, {}));
                sim.unitScriptDefinitions.insert_or_assign(name, CobScript{{}, {}, {}, 0});
            }

            return sim;
        }

        PlayerId addNanolathePlayer(GameSimulation& sim, PlayerColorIndex color, const std::string& side)
        {
            return sim.addPlayer(GamePlayerInfo{
                std::nullopt,
                GamePlayerType::Human,
                color,
                GamePlayerStatus::Alive,
                side,
                std::nullopt,
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000),
                Metal(10000),
                Energy(10000)});
        }
    }

    TEST_CASE("emitNanolatheParticles spawns the retail five-particle burst per worker per tick")
    {
        // TotalA.exe emitter 0x4720d0: one burst of 5 per work tick.
        // Each particle spawns exactly at the QueryNanoPiece nozzle --
        // the emitter's emit-side bound is the degenerate box
        // [FROM, FROM] -- and converges on its own random point inside
        // the shrunk (4/11..7/11) target box. Life N =
        // trunc(distance/4) ticks; palette index = burst index
        // (0xA1 + i), ramp advances one entry per tick.
        auto sim = makeNanolatheSim();
        auto player = addNanolathePlayer(sim, PlayerColorIndex(0), "ARM");

        auto builderId = sim.trySpawnUnit("ARMCOM", player, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        auto tankId = sim.trySpawnUnit("ARMTANK", player, sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
        REQUIRE(builderId);
        REQUIRE(tankId);

        auto& builder = sim.getUnitState(*builderId);
        auto builderPos = builder.position;
        auto targetPos = sim.getUnitState(*tankId).position;
        builder.activeNanolatheTarget = std::make_pair(UnitState::NanolatheTargetRef(*tankId), builderPos);

        std::vector<Particle> particles;
        std::mt19937 rng(1234);
        emitNanolatheParticles(sim, GameTime(10), particles, rng);

        REQUIRE(particles.size() == 5);

        // ARMTANK footprint 2x2 -> bound box +/-16wu on x/z, y up to
        // +10 model height; the shrunk endpoint region is the central
        // 3/11 of that box
        auto from = simVectorToFloat(builderPos);
        auto center = simVectorToFloat(targetPos);
        float halfExtent = 16.0f;
        float boxSize = halfExtent * 2.0f;
        float shrunkHalf = boxSize * (3.0f / 11.0f) / 2.0f;
        Vector3f shrunkCenter = center + Vector3f(0.0f, (10.0f * (4.0f / 11.0f + 7.0f / 11.0f) / 2.0f), 0.0f);

        for (unsigned int i = 0; i < particles.size(); ++i)
        {
            const auto& particle = particles[i];
            const auto* nano = std::get_if<ParticleRenderTypeNanolathe>(&particle.renderType);
            REQUIRE(nano != nullptr);
            REQUIRE(nano->rampIndex == i);
            REQUIRE(particle.startTime == GameTime(10));
            auto lifeTicks = nano->finishTime.value - particle.startTime.value;
            REQUIRE(lifeTicks > 0);

            // spawn point is exactly the QueryNanoPiece nozzle: the
            // retail emit bound is the degenerate box [FROM, FROM]
            REQUIRE(particle.position.x == from.x);
            REQUIRE(particle.position.y == from.y);
            REQUIRE(particle.position.z == from.z);

            // reconstructing the endpoint B = pos + vel * N must land
            // inside the shrunk target box on every axis
            Vector3f end = particle.position + (particle.velocity * static_cast<float>(lifeTicks));
            REQUIRE(end.x >= shrunkCenter.x - shrunkHalf - 0.001f);
            REQUIRE(end.x <= shrunkCenter.x + shrunkHalf + 0.001f);
            REQUIRE(end.z >= shrunkCenter.z - shrunkHalf - 0.001f);
            REQUIRE(end.z <= shrunkCenter.z + shrunkHalf + 0.001f);
            REQUIRE(end.y >= center.y + 10.0f * (4.0f / 11.0f) - 0.001f);
            REQUIRE(end.y <= center.y + 10.0f * (7.0f / 11.0f) + 0.001f);
        }

        // the nozzle position is re-read every work tick: when the
        // emitter piece moves, the next tick's spawn points follow it
        Vector3f movedFrom = from + Vector3f(5.0f, 3.0f, -2.0f);
        builder.activeNanolatheTarget = std::make_pair(UnitState::NanolatheTargetRef(*tankId), floatToSimVector(movedFrom));
        std::vector<Particle> nextTickParticles;
        emitNanolatheParticles(sim, GameTime(11), nextTickParticles, rng);
        REQUIRE(nextTickParticles.size() == 5);
        for (const auto& particle : nextTickParticles)
        {
            REQUIRE(particle.position.x == movedFrom.x);
            REQUIRE(particle.position.y == movedFrom.y);
            REQUIRE(particle.position.z == movedFrom.z);
        }
    }

    TEST_CASE("emitNanolatheParticles is not LOS-gated; the draw path culls per particle")
    {
        // retail emits unconditionally; the draw-time visibility-grid
        // test (0x473a00) is what hides sparks in fog
        auto sim = makeNanolatheSim();
        auto player = addNanolathePlayer(sim, PlayerColorIndex(0), "ARM");
        auto enemy = addNanolathePlayer(sim, PlayerColorIndex(1), "CORE");

        auto builderId = sim.trySpawnUnit("ARMCOM", enemy, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        auto tankId = sim.trySpawnUnit("ARMTANK", enemy, sim.terrain.heightmapIndexToWorldCenter(10, 8), std::nullopt);
        auto idleId = sim.trySpawnUnit("ARMCOM", player, sim.terrain.heightmapIndexToWorldCenter(8, 20), std::nullopt);
        REQUIRE(builderId);
        REQUIRE(tankId);
        REQUIRE(idleId);

        auto& builder = sim.getUnitState(*builderId);
        builder.activeNanolatheTarget = std::make_pair(UnitState::NanolatheTargetRef(*tankId), builder.position);

        std::vector<Particle> particles;
        std::mt19937 rng(1234);

        sim.lineOfSightEnabled = true;
        emitNanolatheParticles(sim, GameTime(0), particles, rng);
        REQUIRE(particles.size() == 5);

        // every particle sits in fog for the local player -> nothing drawn
        sim.playerVisibility.emplace_back();
        sim.playerVisibility[0].losGrid = Grid<unsigned char>(65, 65, 0);
        for (const auto& particle : particles)
        {
            ColoredMeshBatch batch;
            drawNanolatheParticle(sim, player, 1.0f, particle, batch);
            REQUIRE(batch.triangles.empty());
        }

        // mark a particle's cell visible -> it draws again
        auto& liveParticle = particles.front();
        auto cell = sim.terrain.worldToHeightmapCoordinate(floatToSimVector(liveParticle.position));
        sim.playerVisibility[0].losGrid.set(GridCoordinates(cell.x, cell.y), 1);
        ColoredMeshBatch batch;
        drawNanolatheParticle(sim, player, 1.0f, liveParticle, batch);
        REQUIRE(batch.triangles.size() == 6);

        // LOS off entirely -> all particles draw
        sim.lineOfSightEnabled = false;
        for (const auto& particle : particles)
        {
            ColoredMeshBatch openBatch;
            drawNanolatheParticle(sim, player, 1.0f, particle, openBatch);
            REQUIRE(openBatch.triangles.size() == 6);
        }
    }

    TEST_CASE("emitNanolatheParticles emits nothing when the beam is shorter than four world units")
    {
        auto sim = makeNanolatheSim();
        auto player = addNanolathePlayer(sim, PlayerColorIndex(0), "ARM");

        auto builderId = sim.trySpawnUnit("ARMCOM", player, sim.terrain.heightmapIndexToWorldCenter(8, 8), std::nullopt);
        REQUIRE(builderId);

        // a zero-extent feature collapses the target box to its
        // position: spawn sits ~[0.73, 1.27]wu along the shrunk
        // 2wu segment and the endpoint is the feature itself, so
        // every particle's distance truncates to zero ticks
        FeatureDefinition wreckDef{};
        wreckDef.name = "DOTWRECK";
        wreckDef.reclaimable = true;
        auto wreckDefId = sim.featureDefinitions.insert(wreckDef);
        auto featureId = sim.addFeature(wreckDefId, 10, 8);
        REQUIRE(featureId);

        auto& builder = sim.getUnitState(*builderId);
        auto featurePos = sim.getFeature(*featureId).position;
        builder.position = featurePos + SimVector(2_ss, 0_ss, 0_ss);
        builder.activeNanolatheTarget = std::make_pair(UnitState::NanolatheTargetRef(*featureId), builder.position);

        std::vector<Particle> particles;
        std::mt19937 rng(1234);
        emitNanolatheParticles(sim, GameTime(0), particles, rng);

        REQUIRE(particles.empty());
    }

    TEST_CASE("updateParticles integrates, ramps and culls nanolathe particles past their deadline")
    {
        // retail 0x4739b0/0x473b30: pos += vel and palette ramp +1 per
        // tick; dead only when tick > deadline
        GameMediaDatabase gameMediaDatabase;
        std::vector<Particle> particles;

        Particle particle;
        particle.position = Vector3f(0.0f, 0.0f, 0.0f);
        particle.velocity = Vector3f(2.0f, 0.0f, 0.0f);
        particle.startTime = GameTime(0);
        particle.renderType = ParticleRenderTypeNanolathe{GameTime(3), 6};
        particles.push_back(particle);

        updateParticles(gameMediaDatabase, GameTime(1), particles);
        REQUIRE(particles.size() == 1);
        REQUIRE(particles[0].position.x == 2.0f);
        // ramp wrapped 6 -> 0
        REQUIRE(std::get<ParticleRenderTypeNanolathe>(particles[0].renderType).rampIndex == 0);

        // deadline tick itself still draws
        updateParticles(gameMediaDatabase, GameTime(3), particles);
        REQUIRE(particles.size() == 1);

        // one tick past the deadline it is gone
        updateParticles(gameMediaDatabase, GameTime(4), particles);
        REQUIRE(particles.empty());
    }

    TEST_CASE("drawNanolatheParticle emits one quad in the particle's palette-ramp color")
    {
        auto sim = makeNanolatheSim();
        auto player = addNanolathePlayer(sim, PlayerColorIndex(0), "ARM");

        Particle particle;
        particle.position = Vector3f(10.0f, 5.0f, 20.0f);
        particle.velocity = Vector3f(0.0f, 0.0f, 0.0f);
        particle.startTime = GameTime(0);
        particle.renderType = ParticleRenderTypeNanolathe{GameTime(9), 2};

        // retail's inclusive (x,y)-(x+1,y+1) blit is a fixed 2x2
        // screen-pixel square; the world half-extent of one pixel
        // keeps it 2px under the orthographic projection
        ColoredMeshBatch batch;
        drawNanolatheParticle(sim, player, 0.5f, particle, batch);

        REQUIRE(batch.lines.empty());
        REQUIRE(batch.triangles.size() == 6);
        for (const auto& v : batch.triangles)
        {
            // ramp index 2 = palette 0xA3 = #3fbf67
            REQUIRE(v.r == Catch::Approx(0x3f / 255.0f));
            REQUIRE(v.g == Catch::Approx(0xbf / 255.0f));
            REQUIRE(v.b == Catch::Approx(0x67 / 255.0f));
            REQUIRE(v.y == 5.0f);
            REQUIRE(std::abs(v.x - 10.0f) == Catch::Approx(0.5f));
            REQUIRE(std::abs(v.z - 20.0f) == Catch::Approx(0.5f));
        }
    }
}
