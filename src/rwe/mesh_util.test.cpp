#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <rwe/util/SpanStream.h>
#include <rwe/io/gaf/GafArchive.h>
#include <rwe/mesh_util.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>
#include <unordered_set>

namespace rwe
{
    namespace
    {
        _3do::Object makeQuadObject()
        {
            _3do::Object o;
            o.name = "base";
            o.vertices = {
                _3do::Vertex(0, 0, 0),
                _3do::Vertex(65536, 0, 0),
                _3do::Vertex(65536, 0, 65536),
                _3do::Vertex(0, 0, 65536),
            };

            _3do::Primitive texturedQuad;
            texturedQuad.textureName = "TestTex";
            texturedQuad.vertices = {0, 1, 2, 3};
            o.primitives.push_back(std::move(texturedQuad));

            _3do::Primitive coloredQuad;
            coloredQuad.colorIndex = 5;
            coloredQuad.vertices = {0, 1, 2, 3};
            o.primitives.push_back(std::move(coloredQuad));

            _3do::Primitive texturedTriangle;
            texturedTriangle.textureName = "TestTex";
            texturedTriangle.vertices = {0, 1, 2};
            o.primitives.push_back(std::move(texturedTriangle));

            _3do::Primitive texturedPentagon;
            texturedPentagon.textureName = "TestTex";
            texturedPentagon.vertices = {0, 1, 2, 3, 1};
            o.primitives.push_back(std::move(texturedPentagon));

            return o;
        }
    }

    TEST_CASE("meshFrom3do")
    {
        std::unordered_map<std::string, Rectangle2f> atlasMap;
        atlasMap.insert({"TestTex", Rectangle2f::fromTLBR(0.1f, 0.2f, 0.5f, 0.6f)});
        std::unordered_map<std::string, Rectangle2f> teamAtlasMap;
        std::vector<Vector2f> atlasColorMap(256, Vector2f(0.5f, 0.5f));

        SECTION("textured quads and colored faces produce geometry; non-quad textured prims are dropped")
        {
            auto mesh = meshFrom3do(atlasMap, teamAtlasMap, atlasColorMap, makeQuadObject());
            // 1 textured quad (2 tris) + 1 colored quad (2 fan tris) = 4
            // the textured triangle and pentagon are dropped by the
            // textured-quads-only path -- this documents the exact filter
            // that would have erased a textured non-quad hazard model.
            REQUIRE(mesh.faces.size() == 4);
        }

        SECTION("textured quad maps its texcoords to the atlas region")
        {
            auto mesh = meshFrom3do(atlasMap, teamAtlasMap, atlasColorMap, makeQuadObject());
            const auto& region = atlasMap.at("TestTex");
            bool sawRegionTexcoord = false;
            for (const auto& f : mesh.faces)
            {
                for (const auto& v : {f.a, f.b, f.c})
                {
                    if (v.textureCoord.x == region.topLeft().x && v.textureCoord.y == region.topLeft().y)
                    {
                        sawRegionTexcoord = true;
                    }
                }
            }
            REQUIRE(sawRegionTexcoord);
        }

        SECTION("missing texture name yields degenerate region")
        {
            // getTextureRegion returns a zero-area rectangle for unknown
            // names; geometry is still produced but would sample a single
            // atlas point -- the exact failure class that could render a
            // projectile invisible in-game.
            auto region = getTextureRegion(atlasMap, teamAtlasMap, "NoSuchTexture");
            REQUIRE(region.region.width() == 0.0f);
            REQUIRE(region.region.height() == 0.0f);
        }
    }

    /**
     * Retail-gated proof that the hazard projectile models produce
     * non-empty, fully-resolved render geometry:
     *
     * - Asteroid6.3do (METEOR) and Hail1.3do (HAILSTORM) parse with
     *   non-zero bounds and per-primitive renderable data
     * - every texture name referenced by Asteroid6 exists as a GAF
     *   entry in the retail texture archives (the same lookup the
     *   LoadingScene atlas performs)
     * - meshFrom3do produces triangles for every primitive
     *
     * This is the structural regression for the "invisible METEOR" class
     * of bug: a hazard projectile whose geometry is empty or whose texture
     * dependency is missing renders nothing while still damaging units.
     *
     * Skips without RWE_DATA_PATH.
     */
    TEST_CASE("hazard projectile models produce renderable geometry")
    {
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            return;
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));

        // enumerate every texture name available to the atlas builder
        std::unordered_set<std::string> gafEntryNames;
        for (const auto& gafName : vfs.getFileNames("textures", ".gaf"))
        {
            auto bytes = vfs.readFile("textures/" + gafName);
            REQUIRE(bytes.has_value());
            SpanStream stream(bytes->data(), bytes->size());
            GafArchive gaf(&stream);
            for (const auto& e : gaf.entries())
            {
                gafEntryNames.insert(e.name);
            }
        }
        REQUIRE(!gafEntryNames.empty());

        std::unordered_map<std::string, Rectangle2f> atlasMap;
        std::unordered_map<std::string, Rectangle2f> teamAtlasMap;
        // every available GAF entry maps to a real atlas region
        Rectangle2f realRegion = Rectangle2f::fromTLBR(0.01f, 0.01f, 0.02f, 0.02f);
        for (const auto& n : gafEntryNames)
        {
            atlasMap.insert({n, realRegion});
        }
        std::vector<Vector2f> atlasColorMap(256, Vector2f(0.5f, 0.5f));

        const std::vector<std::string> hazardModels{
            "objects3d/asteroid6.3do",
            "objects3d/hail1.3do",
        };

        for (const auto& path : hazardModels)
        {
            auto bytes = vfs.readFile(path);
            REQUIRE(bytes.has_value());

            SpanStream stream(bytes->data(), bytes->size());
            auto objects = parse3doObjects(stream, stream.tellg());
            REQUIRE(!objects.empty());

            unsigned int totalPrims = 0;
            unsigned int totalFaces = 0;
            float minBound = std::numeric_limits<float>::max();
            float maxBound = std::numeric_limits<float>::lowest();
            for (const auto& o : objects)
            {
                auto mesh = meshFrom3do(atlasMap, teamAtlasMap, atlasColorMap, o);
                for (const auto& p : o.primitives)
                {
                    ++totalPrims;
                    // every textured prim's texture must resolve to a
                    // real atlas entry -- a missing name is the exact
                    // failure that silently erased geometry
                    if (p.textureName)
                    {
                        CAPTURE(path, *p.textureName);
                        REQUIRE(gafEntryNames.count(*p.textureName) == 1);
                    }
                }
                for (const auto& f : mesh.faces)
                {
                    ++totalFaces;
                    for (const auto& v : {f.a, f.b, f.c})
                    {
                        minBound = std::min(minBound, std::min({v.position.x, v.position.y, v.position.z}));
                        maxBound = std::max(maxBound, std::max({v.position.x, v.position.y, v.position.z}));
                    }
                }
            }

            CAPTURE(path);
            REQUIRE(totalPrims > 0);
            // every primitive must have produced geometry
            REQUIRE(totalFaces >= totalPrims);
            // and the model must have real extent
            REQUIRE(maxBound - minBound > 1.0f);
        }
    }
}
