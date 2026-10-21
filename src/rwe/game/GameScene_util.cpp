#include "GameScene_util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <rwe/io/tdf/tdf.h>
#include <rwe/sim/UnitState_util.h>
#include <rwe/ui/UiPanel.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/util/Index.h>
#include <rwe/util/match.h>

namespace rwe
{
    Vector3f colorToVector3f(const Color& color)
    {
        return Vector3f(static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f, static_cast<float>(color.b) / 255.0f);
    }

    void pushTriangle(std::vector<GlColoredVertex>& vs, const Triangle3f& tri, const Vector3f& color)
    {
        vs.emplace_back(tri.a, color);
        vs.emplace_back(tri.b, color);
        vs.emplace_back(tri.c, color);
    }

    void pushTriangle(std::vector<GlColoredVertex>& vs, const Triangle3f& tri)
    {
        pushTriangle(vs, tri, Vector3f(1.0f, 1.0f, 1.0f));
    }

    void pushTriangle(std::vector<GlColoredVertex>& vs, const Vector3f& a, const Vector3f& b, const Vector3f& c)
    {
        pushTriangle(vs, Triangle3f(a, b, c));
    }

    void pushTriangle(std::vector<GlColoredVertex>& vs, const Vector3f& a, const Vector3f& b, const Vector3f& c, const Vector3f& color)
    {
        pushTriangle(vs, Triangle3f(a, b, c), color);
    }

    void pushLine(std::vector<GlColoredVertex>& vs, const Line3f& line, const Vector3f& color)
    {
        vs.emplace_back(line.start, color);
        vs.emplace_back(line.end, color);
    }

    void pushLine(std::vector<GlColoredVertex>& vs, const Line3f& line)
    {
        vs.emplace_back(line.start, Vector3f(1.0f, 1.0f, 1.0f));
        vs.emplace_back(line.end, Vector3f(1.0f, 1.0f, 1.0f));
    }

    void pushLine(std::vector<GlColoredVertex>& vs, const Vector3f& start, const Vector3f& end)
    {
        pushLine(vs, Line3f(start, end));
    }

    void pushLine(std::vector<GlColoredVertex>& vs, const Vector3f& start, const Vector3f& end, const Vector3f& color)
    {
        pushLine(vs, Line3f(start, end), color);
    }

    void
    drawPathfindingVisualisation(const MapTerrain& terrain, const AStarPathInfo<Point, PathCost>& pathInfo, ColoredMeshBatch& batch)
    {
        for (const auto& item : pathInfo.closedVertices)
        {
            if (!item.second.predecessor)
            {
                continue;
            }

            auto start = (*item.second.predecessor)->vertex;
            auto end = item.second.vertex;
            drawTerrainArrow(terrain, start, end, Color(255, 0, 0), batch);
        }

        if (pathInfo.path.size() > 1)
        {
            for (auto it = pathInfo.path.begin() + 1; it != pathInfo.path.end(); ++it)
            {
                auto start = *(it - 1);
                auto end = *it;
                drawTerrainArrow(terrain, start, end, Color(0, 0, 255), batch);
            }
        }
    }

    void
    drawTerrainArrow(const MapTerrain& terrain, const Point& start, const Point& end, const Color& color, ColoredMeshBatch& batch)
    {
        auto worldStart = terrain.heightmapIndexToWorldCenter(start);
        worldStart.y = terrain.getHeightAt(worldStart.x, worldStart.z);
        auto worldStartF = simVectorToFloat(worldStart);

        auto worldEnd = terrain.heightmapIndexToWorldCenter(end);
        worldEnd.y = terrain.getHeightAt(worldEnd.x, worldEnd.z);
        auto worldEndF = simVectorToFloat(worldEnd);

        auto armTemplate = (worldStartF - worldEndF).normalized() * 6.0f;
        auto arm1 = Matrix4f::translation(worldEndF) * Matrix4f::rotationY(Pif / 6.0f) * armTemplate;
        auto arm2 = Matrix4f::translation(worldEndF) * Matrix4f::rotationY(-Pif / 6.0f) * armTemplate;

        auto floatColor = colorToVector3f(color);

        pushLine(batch.lines, worldStartF, worldEndF, floatColor);
        pushLine(batch.lines, worldEndF, arm1, floatColor);
        pushLine(batch.lines, worldEndF, arm2, floatColor);
    }

    std::optional<Vector3f> getOccupiedColor(const OccupiedCell& cell)
    {
        auto r = cell.mobileUnitId ? 1.0f : 0.0f;
        auto g = cell.buildingInfo ? 1.0f : 0.0f;
        auto b = cell.featureId ? 1.0f : 0.0f;
        if (r == 0.0f && g == 0.0f && b == 0.0f)
        {
            return std::nullopt;
        }
        return Vector3f(r, g, b);
    }

    void drawOccupiedGrid(const Vector3f& cameraPosition, float viewportWidth, float viewportHeight, const MapTerrain& terrain, const OccupiedGrid& occupiedGrid, ColoredMeshBatch& batch)
    {
        auto halfWidth = viewportWidth / 2.0f;
        auto halfHeight = viewportHeight / 2.0f;
        auto left = cameraPosition.x - halfWidth;
        auto top = cameraPosition.z - halfHeight;
        auto right = cameraPosition.x + halfWidth;
        auto bottom = cameraPosition.z + halfHeight;

        assert(left < right);
        assert(top < bottom);

        assert(terrain.getHeightMap().getWidth() >= 2);
        assert(terrain.getHeightMap().getHeight() >= 2);

        auto topLeftCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(left, 0.0f, top)));
        topLeftCell.x = std::clamp(topLeftCell.x, 0, static_cast<int>(occupiedGrid.getWidth() - 1));
        topLeftCell.y = std::clamp(topLeftCell.y, 0, static_cast<int>(occupiedGrid.getHeight() - 1));

        auto bottomRightCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(right, 0.0f, bottom)));
        bottomRightCell.y += 7; // compensate for height
        bottomRightCell.x = std::clamp(bottomRightCell.x, 0, static_cast<int>(occupiedGrid.getWidth() - 1));
        bottomRightCell.y = std::clamp(bottomRightCell.y, 0, static_cast<int>(occupiedGrid.getHeight() - 1));

        assert(topLeftCell.x <= bottomRightCell.x);
        assert(topLeftCell.y <= bottomRightCell.y);

        for (int y = topLeftCell.y; y <= bottomRightCell.y; ++y)
        {
            for (int x = topLeftCell.x; x <= bottomRightCell.x; ++x)
            {
                auto pos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y));
                pos.y = terrain.getHeightMap().get(x, y);

                auto rightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y));
                rightPos.y = terrain.getHeightMap().get(x + 1, y);

                auto downPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y + 1));
                downPos.y = terrain.getHeightMap().get(x, y + 1);

                pushLine(batch.lines, pos, rightPos);
                pushLine(batch.lines, pos, downPos);

                const auto& cell = occupiedGrid.get(x, y);
                if (auto c = getOccupiedColor(cell); c)
                {
                    auto downRightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y + 1));
                    downRightPos.y = terrain.getHeightMap().get(x + 1, y + 1);

                    pushTriangle(batch.triangles, pos, downPos, downRightPos, *c);
                    pushTriangle(batch.triangles, pos, downRightPos, rightPos, *c);
                }

                const auto insetAmount = 4.0f;

                if (cell.buildingInfo && !cell.buildingInfo->passable)
                {
                    auto topLeftPos = pos + Vector3f(insetAmount, 0.0f, insetAmount);
                    auto topRightPos = rightPos + Vector3f(-insetAmount, 0.0f, insetAmount);
                    auto bottomLeftPos = downPos + Vector3f(insetAmount, 0.0f, -insetAmount);
                    auto downRightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y + 1));
                    downRightPos.y = terrain.getHeightMap().get(x + 1, y + 1);
                    downRightPos += Vector3f(-insetAmount, 0.0f, -insetAmount);

                    pushTriangle(batch.triangles, topLeftPos, bottomLeftPos, downRightPos, Vector3f(1.0f, 0.0f, 0.0f));
                    pushTriangle(batch.triangles, topLeftPos, downRightPos, topRightPos, Vector3f(1.0f, 0.0f, 0.0f));
                }

                if (cell.buildingInfo && cell.buildingInfo->passable)
                {
                    auto topLeftPos = pos + Vector3f(insetAmount, 0.0f, insetAmount);
                    auto topRightPos = rightPos + Vector3f(-insetAmount, 0.0f, insetAmount);
                    auto bottomLeftPos = downPos + Vector3f(insetAmount, 0.0f, -insetAmount);
                    auto downRightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y + 1));
                    downRightPos.y = terrain.getHeightMap().get(x + 1, y + 1);
                    downRightPos += Vector3f(-insetAmount, 0.0f, -insetAmount);

                    pushTriangle(batch.triangles, topLeftPos, bottomLeftPos, downRightPos, Vector3f(1.0f, 1.0f, 1.0f));
                    pushTriangle(batch.triangles, topLeftPos, downRightPos, topRightPos, Vector3f(1.0f, 1.0f, 1.0f));
                }
            }
        }
    }

    void fillFogOfWarBatches(const Vector3f& cameraPosition, float viewportWidth, float viewportHeight, const MapTerrain& terrain, const PlayerVisibility& visibility, ColoredMeshBatch& unexploredBatch, ColoredMeshBatch& exploredBatch)
    {
        const auto& losGrid = visibility.losGrid;
        const auto& exploredGrid = visibility.exploredGrid;
        if (losGrid.getWidth() == 0 || losGrid.getHeight() == 0)
        {
            return;
        }

        auto halfWidth = viewportWidth / 2.0f;
        auto halfHeight = viewportHeight / 2.0f;
        auto left = cameraPosition.x - halfWidth;
        auto top = cameraPosition.z - halfHeight;
        auto right = cameraPosition.x + halfWidth;
        auto bottom = cameraPosition.z + halfHeight;

        auto topLeftCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(left, 0.0f, top)));
        topLeftCell.x = std::clamp(topLeftCell.x, 0, losGrid.getWidth() - 1);
        topLeftCell.y = std::clamp(topLeftCell.y, 0, losGrid.getHeight() - 1);

        auto bottomRightCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(right, 0.0f, bottom)));
        bottomRightCell.y += 7; // compensate for height, same as drawOccupiedGrid
        bottomRightCell.x = std::clamp(bottomRightCell.x, 0, losGrid.getWidth() - 1);
        bottomRightCell.y = std::clamp(bottomRightCell.y, 0, losGrid.getHeight() - 1);

        const auto& heightMap = terrain.getHeightMap();
        const auto black = Vector3f(0.0f, 0.0f, 0.0f);
        const auto lift = 1.0f;

        // worst case is every visible cell fogged: 2 triangles per cell
        auto cellsWide = bottomRightCell.x - topLeftCell.x + 1;
        auto cellsHigh = bottomRightCell.y - topLeftCell.y + 1;
        unexploredBatch.triangles.reserve(static_cast<std::size_t>(cellsWide) * cellsHigh * 6);
        exploredBatch.triangles.reserve(static_cast<std::size_t>(cellsWide) * cellsHigh * 6);

        for (int y = topLeftCell.y; y <= bottomRightCell.y; ++y)
        {
            for (int x = topLeftCell.x; x <= bottomRightCell.x; ++x)
            {
                if (losGrid.get(x, y) != 0)
                {
                    continue;
                }
                auto& batch = exploredGrid.get(x, y) != 0 ? exploredBatch : unexploredBatch;

                auto pos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y));
                pos.y = heightMap.get(x, y) + lift;
                auto rightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y));
                rightPos.y = heightMap.get(x + 1, y) + lift;
                auto downPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y + 1));
                downPos.y = heightMap.get(x, y + 1) + lift;
                auto downRightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y + 1));
                downRightPos.y = heightMap.get(x + 1, y + 1) + lift;

                pushTriangle(batch.triangles, pos, downPos, downRightPos, black);
                pushTriangle(batch.triangles, pos, downRightPos, rightPos, black);
            }
        }
    }

    void fillMinimapFogVertices(const Grid<unsigned char>& losGrid, const Grid<unsigned char>& exploredGrid, const Rectangle2f& rect, std::vector<GlColoredVertex>& unexploredVerts, std::vector<GlColoredVertex>& exploredVerts)
    {
        auto width = losGrid.getWidth();
        auto height = losGrid.getHeight();
        if (width == 0 || height == 0)
        {
            return;
        }

        auto cellW = rect.width() / width;
        auto cellH = rect.height() / height;
        const Vector3f black(0.0f, 0.0f, 0.0f);
        const auto* losData = losGrid.getData();
        const auto* exploredData = exploredGrid.getData();

        for (int y = 0; y < height; ++y)
        {
            const auto rowBase = y * width;
            int x = 0;
            while (x < width)
            {
                auto state = losData[rowBase + x] != 0 ? 0 : (exploredData[rowBase + x] != 0 ? 1 : 2);
                int runEnd = x + 1;
                while (runEnd < width)
                {
                    auto s = losData[rowBase + runEnd] != 0 ? 0 : (exploredData[rowBase + runEnd] != 0 ? 1 : 2);
                    if (s != state)
                    {
                        break;
                    }
                    ++runEnd;
                }
                if (state != 0)
                {
                    auto& verts = state == 2 ? unexploredVerts : exploredVerts;
                    auto rx = rect.left() + x * cellW;
                    auto ry = rect.top() + y * cellH;
                    auto rw = (runEnd - x) * cellW;
                    auto rh = cellH;
                    verts.push_back({{rx, ry, 0.0f}, black});
                    verts.push_back({{rx, ry + rh, 0.0f}, black});
                    verts.push_back({{rx + rw, ry + rh, 0.0f}, black});
                    verts.push_back({{rx + rw, ry + rh, 0.0f}, black});
                    verts.push_back({{rx + rw, ry, 0.0f}, black});
                    verts.push_back({{rx, ry, 0.0f}, black});
                }
                x = runEnd;
            }
        }
    }

    void drawMovementClassCollisionGrid(
        const MapTerrain& terrain,
        const Grid<char>& movementClassGrid,
        const Vector3f& cameraPosition,
        float viewportWidth,
        float viewportHeight,
        ColoredMeshBatch& batch)
    {
        auto halfWidth = viewportWidth / 2.0f;
        auto halfHeight = viewportHeight / 2.0f;
        auto left = cameraPosition.x - halfWidth;
        auto top = cameraPosition.z - halfHeight;
        auto right = cameraPosition.x + halfWidth;
        auto bottom = cameraPosition.z + halfHeight;

        assert(left < right);
        assert(top < bottom);

        assert(terrain.getHeightMap().getWidth() >= 2);
        assert(terrain.getHeightMap().getHeight() >= 2);
        assert(movementClassGrid.getWidth() <= terrain.getHeightMap().getWidth());
        assert(movementClassGrid.getHeight() <= terrain.getHeightMap().getHeight());

        auto topLeftCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(left, 0.0f, top)));
        topLeftCell.x = std::clamp(topLeftCell.x, 0, static_cast<int>(movementClassGrid.getWidth() - 1));
        topLeftCell.y = std::clamp(topLeftCell.y, 0, static_cast<int>(movementClassGrid.getHeight() - 1));

        auto bottomRightCell = terrain.worldToHeightmapCoordinate(floatToSimVector(Vector3f(right, 0.0f, bottom)));
        bottomRightCell.y += 7; // compensate for height
        bottomRightCell.x = std::clamp(bottomRightCell.x, 0, static_cast<int>(movementClassGrid.getWidth() - 1));
        bottomRightCell.y = std::clamp(bottomRightCell.y, 0, static_cast<int>(movementClassGrid.getHeight() - 1));

        assert(topLeftCell.x <= bottomRightCell.x);
        assert(topLeftCell.y <= bottomRightCell.y);

        for (int y = topLeftCell.y; y <= bottomRightCell.y; ++y)
        {
            for (int x = topLeftCell.x; x <= bottomRightCell.x; ++x)
            {
                auto pos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y));
                pos.y = terrain.getHeightMap().get(x, y);

                auto rightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y));
                rightPos.y = terrain.getHeightMap().get(x + 1, y);

                auto downPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x, y + 1));
                downPos.y = terrain.getHeightMap().get(x, y + 1);

                pushLine(batch.lines, pos, rightPos);
                pushLine(batch.lines, pos, downPos);

                if (!movementClassGrid.get(x, y))
                {
                    auto downRightPos = simVectorToFloat(terrain.heightmapIndexToWorldCorner(x + 1, y + 1));
                    downRightPos.y = terrain.getHeightMap().get(x + 1, y + 1);

                    pushTriangle(batch.triangles, pos, downPos, downRightPos);
                    pushTriangle(batch.triangles, pos, downRightPos, rightPos);
                }
            }
        }
    }

    /**
     * Computes every piece's model-space world transform in a single
     * top-down pass. Pieces are stored pre-order (parents before
     * children) so each transform is built exactly once:
     * world[i] = world[parent] * local[i]. This replaces the previous
     * per-piece walk up the parent chain, which was O(pieces x depth)
     * with a toUpper + hash lookup per node per frame.
     */
    void computePieceTransformsForRender(
        const UnitModelDefinition& modelDefinition,
        const std::vector<UnitMesh>& pieces,
        float frac,
        std::vector<Matrix4f>& worldTransforms)
    {
        assert(modelDefinition.pieces.size() == pieces.size());
        assert(modelDefinition.parentPieceIndices.size() == modelDefinition.pieces.size());
        worldTransforms.resize(pieces.size());

        for (Index i = 0; i < getSize(pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];
            const auto& pieceState = pieces[i];

            auto position = lerp(simVectorToFloat(pieceDef.origin + pieceState.previousOffset), simVectorToFloat(pieceDef.origin + pieceState.offset), frac);
            auto rotationX = angleLerp(toRadians(pieceState.previousRotationX).value, toRadians(pieceState.rotationX).value, frac);
            auto rotationY = angleLerp(toRadians(pieceState.previousRotationY).value, toRadians(pieceState.rotationY).value, frac);
            auto rotationZ = angleLerp(toRadians(pieceState.previousRotationZ).value, toRadians(pieceState.rotationZ).value, frac);
            auto local = Matrix4f::translation(position) * Matrix4f::rotationZXY(Vector3f(rotationX, rotationY, rotationZ));

            int parent = modelDefinition.parentPieceIndices[i];
            worldTransforms[i] = (parent >= 0 ? worldTransforms[parent] : Matrix4f::identity()) * local;
        }
    }

    /** Static variant for unanimated geometry: pieces sit at their origins. */
    void computePieceOriginTransformsForRender(const UnitModelDefinition& modelDefinition, std::vector<Matrix4f>& worldTransforms)
    {
        assert(modelDefinition.parentPieceIndices.size() == modelDefinition.pieces.size());
        worldTransforms.resize(modelDefinition.pieces.size());

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            auto local = Matrix4f::translation(simVectorToFloat(modelDefinition.pieces[i].origin));
            int parent = modelDefinition.parentPieceIndices[i];
            worldTransforms[i] = (parent >= 0 ? worldTransforms[parent] : Matrix4f::identity()) * local;
        }
    }

    Matrix4f getPieceTransformForRender(const std::string& pieceName, const UnitModelDefinition& modelDefinition, const std::vector<UnitMesh>& pieces, float frac)
    {
        assert(modelDefinition.pieces.size() == pieces.size());

        std::optional<std::string> parentPiece = pieceName;
        auto matrix = Matrix4f::identity();

        do
        {
            auto pieceIndexIt = modelDefinition.pieceIndicesByName.find(toUpper(*parentPiece));
            if (pieceIndexIt == modelDefinition.pieceIndicesByName.end())
            {
                // stock scripts reference pieces the model doesn't
                // have; retail ignores them, so render at the
                // accumulated transform rather than crashing
                break;
            }
            const auto& pieceDef = modelDefinition.pieces[pieceIndexIt->second];

            parentPiece = pieceDef.parent;

            auto pieceStateIt = pieces.begin() + pieceIndexIt->second;

            auto position = lerp(simVectorToFloat(pieceDef.origin + pieceStateIt->previousOffset), simVectorToFloat(pieceDef.origin + pieceStateIt->offset), frac);
            auto rotationX = angleLerp(toRadians(pieceStateIt->previousRotationX).value, toRadians(pieceStateIt->rotationX).value, frac);
            auto rotationY = angleLerp(toRadians(pieceStateIt->previousRotationY).value, toRadians(pieceStateIt->rotationY).value, frac);
            auto rotationZ = angleLerp(toRadians(pieceStateIt->previousRotationZ).value, toRadians(pieceStateIt->rotationZ).value, frac);
            matrix = Matrix4f::translation(position) * Matrix4f::rotationZXY(Vector3f(rotationX, rotationY, rotationZ)) * matrix;
        } while (parentPiece);

        return matrix;
    }

    void drawShaderMesh(
        const Matrix4f& viewProjectionMatrix,
        const ShaderMesh& mesh,
        const Matrix4f& matrix,
        bool shaded,
        PlayerColorIndex playerColorIndex,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        std::vector<UnitTextureMeshRenderInfo>& batch,
        float alpha = 1.0f)
    {
        auto mvpMatrix = viewProjectionMatrix * matrix;

        if (mesh.vertices)
        {
            batch.push_back(UnitTextureMeshRenderInfo{&*mesh.vertices, matrix, mvpMatrix, shaded, unitTextureAtlas, alpha});
        }
        if (mesh.teamVertices)
        {
            batch.push_back(UnitTextureMeshRenderInfo{&*mesh.teamVertices, matrix, mvpMatrix, shaded, unitTeamTextureAtlases.at(playerColorIndex.value).get(), alpha});
        }
    }

    void drawShaderMeshShadow(
        const Matrix4f& viewProjectionMatrix,
        const ShaderMesh& mesh,
        const Matrix4f& matrix,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        std::vector<UnitTextureShadowMeshRenderInfo>& batch)
    {
        if (mesh.vertices)
        {
            batch.push_back(UnitTextureShadowMeshRenderInfo{&*mesh.vertices, matrix, viewProjectionMatrix, unitTextureAtlas, groundHeight});
        }
        if (mesh.teamVertices)
        {
            batch.push_back(UnitTextureShadowMeshRenderInfo{&*mesh.teamVertices, matrix, viewProjectionMatrix, unitTeamTextureAtlases.at(0).get(), groundHeight});
        }
    }

    void drawUnitMesh(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const std::string& objectName,
        const UnitModelDefinition& modelDefinition,
        const std::vector<UnitMesh>& meshes,
        const Matrix4f& modelMatrix,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch,
        float alpha = 1.0f)
    {
        assert(modelDefinition.pieces.size() == meshes.size());

        std::vector<Matrix4f> worldTransforms;
        computePieceTransformsForRender(modelDefinition, meshes, frac, worldTransforms);

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];
            const auto& mesh = meshes[i];
            if (!mesh.visible)
            {
                continue;
            }

            auto matrix = modelMatrix * worldTransforms[i];

            const auto& resolvedMesh = *gameMediaDatabase.getUnitPieceMesh(objectName, pieceDef.name).value().get().mesh;
            drawShaderMesh(viewProjectionMatrix, resolvedMesh, matrix, mesh.shaded, playerColorIndex, unitTextureAtlas, unitTeamTextureAtlases, batch.meshes, alpha);
        }
    }

    void drawUnitShadowMesh(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const std::string& objectName,
        const UnitModelDefinition& modelDefinition,
        const std::vector<UnitMesh>& meshes,
        const Matrix4f& modelMatrix,
        float frac,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch)
    {
        assert(modelDefinition.pieces.size() == meshes.size());

        std::vector<Matrix4f> worldTransforms;
        computePieceTransformsForRender(modelDefinition, meshes, frac, worldTransforms);

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];
            const auto& mesh = meshes[i];
            if (!mesh.visible)
            {
                continue;
            }

            auto matrix = modelMatrix * worldTransforms[i];

            const auto& resolvedMesh = *gameMediaDatabase.getUnitPieceMesh(objectName, pieceDef.name).value().get().mesh;
            drawShaderMeshShadow(viewProjectionMatrix, resolvedMesh, matrix, groundHeight, unitTextureAtlas, unitTeamTextureAtlases, batch.meshes);
        }
    }

    void drawUnitShadowMeshNoPieces(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const std::string& objectName,
        const UnitModelDefinition& modelDefinition,
        const Matrix4f& modelMatrix,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch)
    {
        std::vector<Matrix4f> worldTransforms;
        computePieceOriginTransformsForRender(modelDefinition, worldTransforms);

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];

            auto matrix = modelMatrix * worldTransforms[i];

            const auto& resolvedMesh = *gameMediaDatabase.getUnitPieceMesh(objectName, pieceDef.name).value().get().mesh;
            drawShaderMeshShadow(viewProjectionMatrix, resolvedMesh, matrix, groundHeight, unitTextureAtlas, unitTeamTextureAtlases, batch.meshes);
        }
    }

    void drawBuildingShaderMesh(
        const Matrix4f& viewProjectionMatrix,
        const ShaderMesh& mesh,
        const Matrix4f& matrix,
        bool shaded,
        float percentComplete,
        float unitY,
        PlayerColorIndex playerColorIndex,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        std::vector<UnitBuildingMeshRenderInfo>& batch)
    {
        auto mvpMatrix = viewProjectionMatrix * matrix;
        if (mesh.vertices)
        {
            batch.push_back(UnitBuildingMeshRenderInfo{&*mesh.vertices, matrix, mvpMatrix, shaded, unitTextureAtlas, percentComplete, unitY});
        }
        if (mesh.teamVertices)
        {
            batch.push_back(UnitBuildingMeshRenderInfo{&*mesh.teamVertices, matrix, mvpMatrix, shaded, unitTeamTextureAtlases.at(playerColorIndex.value).get(), percentComplete, unitY});
        }
    }

    void drawBuildingUnitMesh(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const std::string& objectName,
        const UnitModelDefinition& modelDefinition,
        const std::vector<UnitMesh>& meshes,
        const Matrix4f& modelMatrix,
        float percentComplete,
        float unitY,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch)
    {
        std::vector<Matrix4f> worldTransforms;
        computePieceTransformsForRender(modelDefinition, meshes, frac, worldTransforms);

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];
            const auto& mesh = meshes[i];
            if (!mesh.visible)
            {
                continue;
            }

            auto matrix = modelMatrix * worldTransforms[i];

            const auto& resolvedMesh = *gameMediaDatabase.getUnitPieceMesh(objectName, pieceDef.name).value().get().mesh;
            drawBuildingShaderMesh(viewProjectionMatrix, resolvedMesh, matrix, mesh.shaded, percentComplete, unitY, playerColorIndex, unitTextureAtlas, unitTeamTextureAtlases, batch.buildingMeshes);
        }
    }

    void drawProjectileUnitMesh(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const std::string& objectName,
        const UnitModelDefinition& modelDefinition,
        const Matrix4f& modelMatrix,
        PlayerColorIndex playerColorIndex,
        bool shaded,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch)
    {
        std::vector<Matrix4f> worldTransforms;
        computePieceOriginTransformsForRender(modelDefinition, worldTransforms);

        for (Index i = 0; i < getSize(modelDefinition.pieces); ++i)
        {
            const auto& pieceDef = modelDefinition.pieces[i];
            auto matrix = modelMatrix * worldTransforms[i];
            const auto& resolvedMesh = *gameMediaDatabase.getUnitPieceMesh(objectName, pieceDef.name).value().get().mesh;
            drawShaderMesh(viewProjectionMatrix, resolvedMesh, matrix, shaded, playerColorIndex, unitTextureAtlas, unitTeamTextureAtlases, batch.meshes);
        }
    }

    void drawUnit(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch,
        float alpha)
    {
        auto position = lerp(simVectorToFloat(unit.previousPosition), simVectorToFloat(unit.position), frac);
        auto rotation = angleLerp(toRadians(unit.previousRotation).value, toRadians(unit.rotation).value, frac);
        auto transform = Matrix4f::translation(position) * Matrix4f::rotationY(rotation);
        if (unit.isBeingBuilt(unitDefinition))
        {
            drawBuildingUnitMesh(gameMediaDatabase, viewProjectionMatrix, unitDefinition.objectName, modelDefinition, unit.pieces, transform, unit.getPreciseCompletePercent(unitDefinition), position.y, playerColorIndex, frac, unitTextureAtlas, unitTeamTextureAtlases, batch);
        }
        else
        {
            drawUnitMesh(gameMediaDatabase, viewProjectionMatrix, unitDefinition.objectName, modelDefinition, unit.pieces, transform, playerColorIndex, frac, unitTextureAtlas, unitTeamTextureAtlases, batch, alpha);
        }
    }

    void drawAttachedUnit(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        const UnitState& carrier,
        const UnitModelDefinition& carrierModelDefinition,
        int attachSlot,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch,
        float alpha)
    {
        auto carrierPosition = lerp(simVectorToFloat(carrier.previousPosition), simVectorToFloat(carrier.position), frac);
        auto carrierRotation = angleLerp(toRadians(carrier.previousRotation).value, toRadians(carrier.rotation).value, frac);
        auto carrierTransform = Matrix4f::translation(carrierPosition) * Matrix4f::rotationY(carrierRotation);

        // retail draws an attached member under its slot piece's
        // transform; an invalid slot (-1) parks it at the carrier
        // origin inside the hull (0x43def0 zero offset)
        const auto* script = carrier.cobEnvironment ? carrier.cobEnvironment->_script : nullptr;
        if (!script || attachSlot < 0 || static_cast<unsigned int>(attachSlot) >= script->pieces.size())
        {
            drawUnitMesh(gameMediaDatabase, viewProjectionMatrix, unitDefinition.objectName, modelDefinition, unit.pieces, carrierTransform, playerColorIndex, frac, unitTextureAtlas, unitTeamTextureAtlases, batch, alpha);
            return;
        }

        auto slotTransform = carrierTransform * getPieceTransformForRender(script->pieces[attachSlot], carrierModelDefinition, carrier.pieces, frac);
        drawUnitMesh(gameMediaDatabase, viewProjectionMatrix, unitDefinition.objectName, modelDefinition, unit.pieces, slotTransform, playerColorIndex, frac, unitTextureAtlas, unitTeamTextureAtlases, batch, alpha);
    }

    void drawMeshFeature(
        const std::unordered_map<std::string, UnitModelDefinition>& modelDefinitions,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const MapFeature& feature,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch)
    {
        const auto& featureMediaInfo = gameMediaDatabase.getFeature(feature.featureName);

        if (auto objectInfo = std::get_if<FeatureObjectInfo>(&featureMediaInfo.renderInfo); objectInfo != nullptr)
        {
            const auto& modelDefinition = modelDefinitions.at(objectInfo->objectName);
            auto matrix = Matrix4f::translation(simVectorToFloat(feature.position)) * Matrix4f::rotationY(toRadians(feature.rotation).value);
            drawProjectileUnitMesh(gameMediaDatabase, viewProjectionMatrix, objectInfo->objectName, modelDefinition, matrix, PlayerColorIndex(0), true, unitTextureAtlas, unitTeamTextureAtlases, batch);
        }
    }

    void drawUnitShadow(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        float frac,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch)
    {
        auto position = lerp(simVectorToFloat(unit.previousPosition), simVectorToFloat(unit.position), frac);
        auto rotation = angleLerp(toRadians(unit.previousRotation).value, toRadians(unit.rotation).value, frac);
        auto transform = Matrix4f::translation(position) * Matrix4f::rotationY(rotation);

        drawUnitShadowMesh(gameMediaDatabase, viewProjectionMatrix, unitDefinition.objectName, modelDefinition, unit.pieces, transform, frac, groundHeight, unitTextureAtlas, unitTeamTextureAtlases, batch);
    }

    void drawFeatureMeshShadow(
        const std::unordered_map<std::string, UnitModelDefinition>& modelDefinitions,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const MapFeature& feature,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch)
    {
        const auto& featureMediaInfo = gameMediaDatabase.getFeature(feature.featureName);

        auto objectInfo = std::get_if<FeatureObjectInfo>(&featureMediaInfo.renderInfo);
        if (objectInfo == nullptr)
        {
            return;
        }

        const auto& modelDefinition = modelDefinitions.at(objectInfo->objectName);

        const auto& position = feature.position;
        auto matrix = Matrix4f::translation(simVectorToFloat(position)) * Matrix4f::rotationY(toRadians(feature.rotation).value);

        drawUnitShadowMeshNoPieces(gameMediaDatabase, viewProjectionMatrix, objectInfo->objectName, modelDefinition, matrix, groundHeight, unitTextureAtlas, unitTeamTextureAtlases, batch);
    }

    float featureEventCullRadius(const GameMediaDatabase& media, const MapFeature& feature)
    {
        if (feature.event != FeatureEvent::None)
        {
            if (auto sprite = std::get_if<FeatureSpriteInfo>(&media.getFeature(feature.featureName).renderInfo))
            {
                return sprite->eventCullRadii.at(static_cast<unsigned int>(feature.event) - 1);
            }
        }
        return 320.0f;
    }

    void drawFeature(
        const GameMediaDatabase& gameMediaDatabase,
        const MapFeature& feature,
        const FeatureDefinition& featureDefinition,
        const Matrix4f& viewProjectionMatrix,
        SpriteBatch& batch)
    {
        const auto& featureMediaInfo = gameMediaDatabase.getFeature(feature.featureName);

        auto spriteInfo = std::get_if<FeatureSpriteInfo>(&featureMediaInfo.renderInfo);
        if (spriteInfo == nullptr)
        {
            return;
        }

        const auto& series = feature.event == FeatureEvent::None ? spriteInfo->animation
            : spriteInfo->events.at(static_cast<unsigned int>(feature.event) - 1);
        auto frame = feature.event == FeatureEvent::None ? 0u : feature.animationFrame;
        if (!series || frame >= series->sprites.size())
        {
            return;
        }

        auto position = simVectorToFloat(feature.position);
        const auto& sprite = *series->sprites[frame];

        Vector3f snappedPosition(std::round(position.x), truncateToInterval(position.y, 2.0f), std::round(position.z));

        // Convert to a model position that makes sense in the game world.
        // For standing (blocking) features we stretch y-dimension values by 2x
        // to correct for TA camera distortion.
        Matrix4f conversionMatrix = featureDefinition.isStanding()
            ? Matrix4f::scale(Vector3f(1.0f, -2.0f, 1.0f))
            : Matrix4f::rotationX(-Pif / 2.0f) * Matrix4f::scale(Vector3f(1.0f, -1.0f, 1.0f));

        auto modelMatrix = Matrix4f::translation(snappedPosition) * conversionMatrix * sprite.getTransform();

        auto mvpMatrix = viewProjectionMatrix * modelMatrix;

        batch.sprites.push_back(SpriteRenderInfo{&sprite, mvpMatrix, spriteInfo->transparentAnimation});
    }

    void drawFeatureShadow(
        const GameMediaDatabase& gameMediaDatabase,
        const MapFeature& feature,
        const FeatureDefinition& featureDefinition,
        const Matrix4f& viewProjectionMatrix,
        SpriteBatch& batch)
    {
        const auto& featureMediaInfo = gameMediaDatabase.getFeature(feature.featureName);

        auto spriteInfo = std::get_if<FeatureSpriteInfo>(&featureMediaInfo.renderInfo);
        if (spriteInfo == nullptr)
        {
            return;
        }

        const auto& series = feature.event == FeatureEvent::None ? spriteInfo->shadowAnimation.value_or(nullptr)
            : spriteInfo->eventShadows.at(static_cast<unsigned int>(feature.event) - 1);
        auto frame = feature.event == FeatureEvent::None ? 0u : feature.shadowFrame;
        if (!series || frame >= series->sprites.size())
        {
            return;
        }
        auto position = simVectorToFloat(feature.position);
        const auto& sprite = *series->sprites[frame];

        Vector3f snappedPosition(std::round(position.x), truncateToInterval(position.y, 2.0f), std::round(position.z));

        // Convert to a model position that makes sense in the game world.
        // For standing (blocking) features we stretch y-dimension values by 2x
        // to correct for TA camera distortion.
        Matrix4f conversionMatrix = featureDefinition.isStanding()
            ? Matrix4f::scale(Vector3f(1.0f, -2.0f, 1.0f))
            : Matrix4f::rotationX(-Pif / 2.0f) * Matrix4f::scale(Vector3f(1.0f, -1.0f, 1.0f));

        auto modelMatrix = Matrix4f::translation(snappedPosition) * conversionMatrix * sprite.getTransform();

        auto mvpMatrix = viewProjectionMatrix * modelMatrix;

        batch.sprites.push_back(SpriteRenderInfo{&sprite, mvpMatrix, spriteInfo->transparentShadow});
    }

    /**
     * The retail nanolathe palette ramp (PALETTE.PAL indices
     * 0xA1-0xA7, bright to dark green). A particle enters at its burst
     * index and cycles forward one entry per tick.
     */
    const std::array<Vector3f, 7> NanolathePaletteRamp = {
        Vector3f(0x7f / 255.0f, 0xe7 / 255.0f, 0xab / 255.0f),
        Vector3f(0x5b / 255.0f, 0xd3 / 255.0f, 0x83 / 255.0f),
        Vector3f(0x3f / 255.0f, 0xbf / 255.0f, 0x67 / 255.0f),
        Vector3f(0x2b / 255.0f, 0xab / 255.0f, 0x4b / 255.0f),
        Vector3f(0x2b / 255.0f, 0x97 / 255.0f, 0x43 / 255.0f),
        Vector3f(0x27 / 255.0f, 0x87 / 255.0f, 0x37 / 255.0f),
        Vector3f(0x1b / 255.0f, 0x77 / 255.0f, 0x2f / 255.0f),
    };

    /**
     * Retail caps the channel-6 effect table at 400 bursts of 5
     * particles each (TotalA.exe 0x4720d0 insert path); cap the live
     * pool the same way so extreme worker counts cannot grow it
     * without bound.
     */
    constexpr std::size_t MaxNanolatheParticles = 400 * 5;

    constexpr unsigned int NanolatheParticlesPerTick = 5;

    /**
     * Retail shrinks every emit-side bound to its central 3/11 band:
     * the allowed parameter for each axis is [4/11, 7/11] of the
     * original min->max span (TotalA.exe 0x473b50, /11 via the
     * 0x2e8ba2e9 multiply-shift).
     */
    constexpr float NanolatheShrinkLo = 4.0f / 11.0f;
    constexpr float NanolatheShrinkHi = 7.0f / 11.0f;

    /**
     * The target's bound box (the emitter's second bound). The
     * emit-side bound needs no fields: at the call site (TotalA.exe
     * 0x4720d0) the emitter copies the QueryNanoPiece vector into its
     * argument block twice, producing the degenerate (min == max) box
     * [FROM, FROM] -- so every particle starts exactly at the nozzle
     * and the stream visibly leaves the emitter piece.
     */
    struct NanolatheTargetBounds
    {
        Vector3f boxMin;
        Vector3f boxMax;
    };

    std::optional<NanolatheTargetBounds> nanolatheTargetBounds(const GameSimulation& simulation, const UnitState::NanolatheTargetRef& target)
    {
        return match(
            target,
            [&](const UnitId& targetUnitId) -> std::optional<NanolatheTargetBounds> {
                auto targetUnit = simulation.tryGetUnitState(targetUnitId);
                if (!targetUnit)
                {
                    return std::nullopt;
                }
                const auto& targetUnitRef = targetUnit->get();
                auto position = simVectorToFloat(targetUnitRef.position);

                const auto& targetDefinition = simulation.unitDefinitions.at(targetUnitRef.unitType);
                auto [footprintX, footprintZ] = simulation.getFootprintXZ(targetDefinition.movementCollisionInfo);
                float halfX = footprintX * 16.0f / 2.0f;
                float halfZ = footprintZ * 16.0f / 2.0f;

                float height = 0.0f;
                auto modelIt = simulation.unitModelDefinitions.find(targetDefinition.objectName);
                if (modelIt != simulation.unitModelDefinitions.end())
                {
                    height = simScalarToFloat(modelIt->second.height);
                }

                // unitdef +0x15e..+0x172 box: pos +/- footprint
                // half-extents, y from ground level to model height
                Vector3f boxMin(position.x - halfX, position.y, position.z - halfZ);
                Vector3f boxMax(position.x + halfX, position.y + height, position.z + halfZ);
                return NanolatheTargetBounds{boxMin, boxMax};
            },
            [&](const FeatureId& targetFeatureId) -> std::optional<NanolatheTargetBounds> {
                auto targetFeature = simulation.tryGetFeature(targetFeatureId);
                if (!targetFeature)
                {
                    return std::nullopt;
                }
                const auto& feature = targetFeature->get();
                auto position = simVectorToFloat(feature.position);

                // feature fields +0x94/+0x96 (footprintx/z cells) and
                // +0xfa (height wu); retail's box runs corner->corner,
                // i.e. +/- half the footprint around the centred RWE
                // position
                const auto& featureDefinition = simulation.getFeatureDefinition(feature.featureName);
                float halfX = featureDefinition.footprintX * 16.0f / 2.0f;
                float halfZ = featureDefinition.footprintZ * 16.0f / 2.0f;
                float height = simScalarToFloat(featureDefinition.height);

                Vector3f boxMin(position.x - halfX, position.y, position.z - halfZ);
                Vector3f boxMax(position.x + halfX, position.y + height, position.z + halfZ);
                return NanolatheTargetBounds{boxMin, boxMax};
            });
    }

    /**
     * Uniform sample of the shrunk bound: min + size*t with t uniform
     * in [4/11, 7/11] per axis (rand scaled by span/0x8000 in retail).
     */
    Vector3f nanolatheSampleShrunk(const Vector3f& boundMin, const Vector3f& boundMax, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> dist(NanolatheShrinkLo, NanolatheShrinkHi);
        Vector3f size = boundMax - boundMin;
        return Vector3f(
            boundMin.x + size.x * dist(rng),
            boundMin.y + size.y * dist(rng),
            boundMin.z + size.z * dist(rng));
    }

    void emitNanolatheParticles(const GameSimulation& simulation, GameTime currentTime, std::vector<Particle>& particles, std::mt19937& rng)
    {
        std::size_t nanolatheCount = 0;
        for (const auto& particle : particles)
        {
            if (std::holds_alternative<ParticleRenderTypeNanolathe>(particle.renderType))
            {
                ++nanolatheCount;
            }
        }

        for (const auto& [_, unit] : simulation.units)
        {
            if (unit.carriedBy)
            {
                continue;
            }
            auto nanolatheTarget = unit.getActiveNanolatheTarget();
            if (!nanolatheTarget)
            {
                continue;
            }

            auto bounds = nanolatheTargetBounds(simulation, nanolatheTarget->first);
            if (!bounds)
            {
                // target died/vanished between the sim update and the
                // emit; retail simply drops that tick's burst
                continue;
            }

            Vector3f from = simVectorToFloat(nanolatheTarget->second);

            for (unsigned int i = 0; i < NanolatheParticlesPerTick; ++i)
            {
                if (nanolatheCount >= MaxNanolatheParticles)
                {
                    return;
                }

                // per particle: spawn exactly at the QueryNanoPiece
                // nozzle (retail's emit bound is the degenerate box
                // [FROM, FROM] built at 0x4720d0), endpoint a random
                // point in the shrunk target box
                Vector3f start = from;
                Vector3f end = nanolatheSampleShrunk(bounds->boxMin, bounds->boxMax, rng);

                Vector3f delta = end - start;
                float length = delta.length();

                // retail: N = beamLength/4 ticks (16.16 fixed, high
                // word taken); a beam shorter than four world units
                // emits nothing for that particle
                unsigned int lifeTicks = static_cast<unsigned int>(length / 4.0f);
                if (lifeTicks == 0)
                {
                    continue;
                }

                Particle particle;
                particle.position = start;
                particle.velocity = delta / static_cast<float>(lifeTicks);
                particle.startTime = currentTime;
                particle.renderType = ParticleRenderTypeNanolathe{
                    GameTime(currentTime.value + lifeTicks),
                    i};
                particles.push_back(particle);
                ++nanolatheCount;
            }
        }
    }

    void drawNanolatheParticle(const GameSimulation& simulation, PlayerId localPlayerId, float worldHalfExtent, const Particle& particle, ColoredMeshBatch& batch)
    {
        auto nanolatheRenderInfo = std::get_if<ParticleRenderTypeNanolathe>(&particle.renderType);
        if (nanolatheRenderInfo == nullptr)
        {
            return;
        }

        // retail tests the visibility grid at each particle's own
        // position (TotalA.exe 0x473a00): sparks that drift into fog
        // stop drawing even while the beam continues
        if (simulation.lineOfSightEnabled)
        {
            if (localPlayerId.value >= simulation.playerVisibility.size())
            {
                return;
            }
            const auto& losGrid = simulation.playerVisibility[localPlayerId.value].losGrid;
            auto cell = simulation.terrain.worldToHeightmapCoordinate(floatToSimVector(particle.position));
            if (cell.x < 0 || cell.y < 0 || cell.x >= losGrid.getWidth() || cell.y >= losGrid.getHeight() || losGrid.get(cell.x, cell.y) == 0)
            {
                return;
            }
        }

        const auto position = particle.position;
        const auto topLeft = position + Vector3f(-worldHalfExtent, 0.0f, -worldHalfExtent);
        const auto topRight = position + Vector3f(worldHalfExtent, 0.0f, -worldHalfExtent);
        const auto bottomLeft = position + Vector3f(-worldHalfExtent, 0.0f, worldHalfExtent);
        const auto bottomRight = position + Vector3f(worldHalfExtent, 0.0f, worldHalfExtent);

        const auto& color = NanolathePaletteRamp[nanolatheRenderInfo->rampIndex % NanolathePaletteRamp.size()];

        pushTriangle(batch.triangles, topLeft, bottomLeft, bottomRight, color);
        pushTriangle(batch.triangles, topLeft, bottomRight, topRight, color);
    }

    void drawWakeParticle(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, const Matrix4f& viewProjectionMatrix, const Particle& particle, ColoredMeshBatch& batch)
    {
        auto wakeRenderInfo = std::get_if<ParticleRenderTypeWake>(&particle.renderType);
        if (wakeRenderInfo == nullptr)
        {
            return;
        }

        if (!particle.isStarted(currentTime) || currentTime >= wakeRenderInfo->finishTime)
        {
            return;
        }

        const auto topLeft = particle.position + Vector3f(-1.0f, 0.0f, -1.0f);
        const auto topRight = particle.position + Vector3f(1.0f, 0.0f, -1.0f);
        const auto bottomLeft = particle.position + Vector3f(-1.0f, 0.0f, 1.0f);
        const auto bottomRight = particle.position + Vector3f(1.0f, 0.0f, 1.0f);

        Vector3f startColor(1.0f, 1.0f, 1.0f);
        Vector3f finishColor(0.4f, 0.5f, 1.0f);
        auto duration = wakeRenderInfo->finishTime - particle.startTime;
        auto timeElapsed = currentTime - particle.startTime;
        auto color = lerp(startColor, finishColor, static_cast<float>(timeElapsed.value) / static_cast<float>(duration.value));

        pushTriangle(batch.triangles, topLeft, bottomLeft, bottomRight, color);
        pushTriangle(batch.triangles, topLeft, bottomRight, topRight, color);
    }

    void drawSpriteParticle(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, const Matrix4f& viewProjectionMatrix, const Particle& particle, SpriteBatch& batch)
    {
        auto spriteRenderInfo = std::get_if<ParticleRenderTypeSprite>(&particle.renderType);
        if (spriteRenderInfo == nullptr)
        {
            return;
        }

        auto spriteSeries = gameMediaDatabase.getSpriteSeries(spriteRenderInfo->gafName, spriteRenderInfo->animName).value();

        if (!particle.isStarted(currentTime) || particle.isFinished(currentTime, spriteRenderInfo->finishTime, spriteRenderInfo->frameDuration, spriteSeries->sprites.size()))
        {
            return;
        }

        auto frameIndex = particle.getFrameIndex(currentTime, spriteRenderInfo->frameDuration, spriteSeries->sprites.size());
        const auto& sprite = *spriteSeries->sprites[frameIndex];

        Vector3f snappedPosition(
            std::round(particle.position.x),
            truncateToInterval(particle.position.y, 2.0f),
            std::round(particle.position.z));

        // Convert to a model position that makes sense in the game world.
        // For standing (blocking) features we stretch y-dimension values by 2x
        // to correct for TA camera distortion.
        Matrix4f conversionMatrix = Matrix4f::scale(Vector3f(1.0f, -2.0f, 1.0f));

        auto modelMatrix = Matrix4f::translation(snappedPosition) * conversionMatrix * sprite.getTransform();
        auto mvpMatrix = viewProjectionMatrix * modelMatrix;

        batch.sprites.push_back(SpriteRenderInfo{&sprite, mvpMatrix, spriteRenderInfo->translucent});
    }

    void updateParticles(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, std::vector<Particle>& particles)
    {
        auto end = particles.end();
        for (auto it = particles.begin(); it != end;)
        {
            auto& particle = *it;
            auto isFinished = match(
                particle.renderType,
                [&](const ParticleRenderTypeSprite& s) {
                    const auto anim = gameMediaDatabase.getSpriteSeries(s.gafName, s.animName).value();
                    return particle.isFinished(currentTime, s.finishTime, s.frameDuration, anim->sprites.size());
                },
                [&](const ParticleRenderTypeWake& w) {
                    return currentTime >= w.finishTime;
                },
                [&](const ParticleRenderTypeNanolathe& n) {
                    // retail (TotalA.exe 0x473b30): dead only once the
                    // tick is past the deadline, so a particle with
                    // life N is drawn for N+1 ticks
                    return currentTime > n.finishTime;
                });

            if (isFinished)
            {
                particle = std::move(*--end);
                continue;
            }

            particle.position += particle.velocity;
            if (auto* n = std::get_if<ParticleRenderTypeNanolathe>(&particle.renderType))
            {
                // retail (TotalA.exe 0x4739b0) advances the palette
                // ramp one entry per tick, wrapping after index 7
                n->rampIndex = (n->rampIndex + 1) % NanolathePaletteRamp.size();
            }

            ++it;
        }
        particles.erase(end, particles.end());
    }

    /**
     * Assumes input vector is normalised.
     */
    Matrix4f pointDirection(const Vector3f& direction)
    {
        auto right = direction.cross(Vector3f(0.0f, 1.0f, 0.0f)).normalizedOr(Vector3f(1.0f, 0.0f, 0.0f));
        auto realUp = right.cross(direction);
        return Matrix4f::rotationToAxes(right, realUp, direction);
    }

    Matrix4f rotationModeToMatrix(ProjectileRenderTypeModel::RotationMode r)
    {
        switch (r)
        {
            case ProjectileRenderTypeModel::RotationMode::HalfZ:
                return Matrix4f::rotationZ(0.0f, -1.0f); // 180 degrees
            case ProjectileRenderTypeModel::RotationMode::QuarterY:
                return Matrix4f::rotationY(1.0f, 0.0f); // 90 degrees
            case ProjectileRenderTypeModel::RotationMode::None:
                return Matrix4f::identity();
            default:
                throw std::logic_error("Unknown RotationMode");
        }
    }

    unsigned int getFrameIndex(GameTime currentTime, unsigned int numFrames)
    {
        return (currentTime.value / 2) % numFrames;
    }

    void drawProjectiles(
        const GameSimulation& sim,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const VectorMap<Projectile, ProjectileIdTag>& projectiles,
        GameTime currentTime,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        ColoredMeshBatch& coloredMeshbatch,
        SpriteBatch& spriteBatch,
        UnitMeshBatch& unitMeshBatch)
    {
        for (const auto& e : projectiles)
        {
            const auto& projectile = e.second;
            auto position = lerp(simVectorToFloat(projectile.previousPosition), simVectorToFloat(projectile.position), frac);

            const auto& weaponMediaInfo = gameMediaDatabase.getWeapon(projectile.weaponType);

            match(
                weaponMediaInfo.renderType,
                [&](const ProjectileRenderTypeLaser& l) {
                    Vector3f pixelOffset(0.0f, 0.0f, -1.0f);

                    std::vector<GlColoredVertex> laserVertices;
                    auto backPosition = lerp(simVectorToFloat(projectile.getPreviousBackPosition(l.duration)), simVectorToFloat(projectile.getBackPosition(l.duration)), frac);

                    coloredMeshbatch.lines.emplace_back(position, l.color);
                    coloredMeshbatch.lines.emplace_back(backPosition, l.color);

                    coloredMeshbatch.lines.emplace_back(position + pixelOffset, l.color2);
                    coloredMeshbatch.lines.emplace_back(backPosition + pixelOffset, l.color2);
                },
                [&](const ProjectileRenderTypeModel& m) {
                    auto transform = Matrix4f::translation(position)
                        * pointDirection(simVectorToFloat(projectile.velocity).normalized())
                        * rotationModeToMatrix(m.rotationMode);
                    const auto& modelDefinition = sim.unitModelDefinitions.at(m.objectName);
                    drawProjectileUnitMesh(gameMediaDatabase, viewProjectionMatrix, m.objectName, modelDefinition, transform, PlayerColorIndex(0), false, unitTextureAtlas, unitTeamTextureAtlases, unitMeshBatch);
                },
                [&](const ProjectileRenderTypeSprite& s) {
                    Vector3f snappedPosition(
                        std::round(position.x),
                        truncateToInterval(position.y, 2.0f),
                        std::round(position.z));
                    Matrix4f conversionMatrix = Matrix4f::scale(Vector3f(1.0f, -2.0f, 1.0f));
                    const auto spriteSeries = gameMediaDatabase.getSpriteSeries(s.gaf, s.anim).value();
                    const auto& sprite = *spriteSeries->sprites[getFrameIndex(currentTime, spriteSeries->sprites.size())];
                    auto modelMatrix = Matrix4f::translation(snappedPosition) * conversionMatrix * sprite.getTransform();
                    auto mvpMatrix = viewProjectionMatrix * modelMatrix;
                    spriteBatch.sprites.push_back(SpriteRenderInfo{&sprite, mvpMatrix, false});
                },
                [&](const ProjectileRenderTypeFlamethrower&) {
                    Vector3f snappedPosition(
                        std::round(position.x),
                        truncateToInterval(position.y, 2.0f),
                        std::round(position.z));
                    Matrix4f conversionMatrix = Matrix4f::scale(Vector3f(1.0f, -2.0f, 1.0f));
                    const auto spriteSeries = gameMediaDatabase.getSpriteSeries("FX", "flamestream").value();
                    auto timeSinceSpawn = currentTime - projectile.createdAt;
                    auto fullLifetime = projectile.dieOnFrame.value() - projectile.createdAt;
                    auto percentComplete = static_cast<float>(timeSinceSpawn.value) / static_cast<float>(fullLifetime.value);
                    auto frameIndex = static_cast<unsigned int>(percentComplete * spriteSeries->sprites.size());
                    assert(frameIndex < spriteSeries->sprites.size());
                    const auto& sprite = *spriteSeries->sprites[frameIndex];
                    auto modelMatrix = Matrix4f::translation(snappedPosition) * conversionMatrix * sprite.getTransform();
                    auto mvpMatrix = viewProjectionMatrix * modelMatrix;
                    spriteBatch.sprites.push_back(SpriteRenderInfo{&sprite, mvpMatrix, true});
                },
                [&](const ProjectileRenderTypeLightning& l) {
                    Vector3f pixelOffset(0.0f, 0.0f, -1.0f);

                    std::vector<GlColoredVertex> laserVertices;
                    auto backPosition = lerp(simVectorToFloat(projectile.getPreviousBackPosition(l.duration)), simVectorToFloat(projectile.getBackPosition(l.duration)), frac);

                    coloredMeshbatch.lines.emplace_back(position, l.color);
                    coloredMeshbatch.lines.emplace_back(backPosition, l.color);
                },
                [&](const ProjectileRenderTypeMindgun&) {
                    // TODO: implement mindgun if anyone actually uses it
                },
                [&](const ProjectileRenderTypeNone&) {
                    // do nothing
                });
        }
    }

    void drawSelectionRect(const GameMediaDatabase& gameMediaDatabase, const Matrix4f& viewProjectionMatrix, const UnitState& unit, const UnitDefinition& unitDefinition, float frac, ColoredMeshesBatch& batch)
    {
        auto selectionMesh = gameMediaDatabase.getSelectionMesh(unitDefinition.objectName);

        auto position = lerp(simVectorToFloat(unit.previousPosition), simVectorToFloat(unit.position), frac);

        // try to ensure that the selection rectangle vertices
        // are aligned with the middle of pixels,
        // to prevent discontinuities in the drawn lines.
        Vector3f snappedPosition(
            snapToInterval(position.x, 1.0f) + 0.5f,
            snapToInterval(position.y, 2.0f),
            snapToInterval(position.z, 1.0f) + 0.5f);

        auto rotation = angleLerp(toRadians(unit.previousRotation).value, toRadians(unit.rotation).value, frac);
        auto matrix = Matrix4f::translation(snappedPosition) * Matrix4f::rotationY(rotation);
        auto mvpMatrix = viewProjectionMatrix * matrix;

        batch.meshes.push_back(ColoredMeshRenderInfo{selectionMesh.value().get(), mvpMatrix});
    }

    void appendDecloakRingVertices(std::vector<GlColoredVertex>& vertices, const UnitState& unit, float radius, float frac)
    {
        auto position = lerp(simVectorToFloat(unit.previousPosition), simVectorToFloat(unit.position), frac);

        const auto segmentCount = 32;
        const Vector3f color(0.75f, 0.75f, 0.75f);
        for (int i = 0; i < segmentCount; ++i)
        {
            auto angle0 = static_cast<float>(i) / segmentCount * 2.0f * 3.14159265f;
            auto angle1 = static_cast<float>(i + 1) / segmentCount * 2.0f * 3.14159265f;
            vertices.push_back({Vector3f(position.x + radius * std::cos(angle0), position.y, position.z + radius * std::sin(angle0)), color});
            vertices.push_back({Vector3f(position.x + radius * std::cos(angle1), position.y, position.z + radius * std::sin(angle1)), color});
        }
    }

    void appendMinimapSensorCircleVertices(std::vector<GlColoredVertex>& vertices, const Vector2f& center, float radius, const Vector3f& color)
    {
        const auto segmentCount = 32;
        for (int i = 0; i < segmentCount; ++i)
        {
            auto angle0 = static_cast<float>(i) / segmentCount * 2.0f * 3.14159265f;
            auto angle1 = static_cast<float>(i + 1) / segmentCount * 2.0f * 3.14159265f;
            vertices.push_back({Vector3f(center.x + radius * std::cos(angle0), center.y + radius * std::sin(angle0), 0.0f), color});
            vertices.push_back({Vector3f(center.x + radius * std::cos(angle1), center.y + radius * std::sin(angle1), 0.0f), color});
        }
    }

    std::optional<CursorType> orderMarkerCursorType(const UnitOrder& order)
    {
        return match(
            order,
            [&](const BuildOrder&) { return std::optional<CursorType>(); },
            [&](const MoveOrder&) { return std::optional<CursorType>(CursorType::Move); },
            [&](const AttackOrder&) { return std::optional<CursorType>(CursorType::Attack); },
            [&](const BuggerOffOrder&) { return std::optional<CursorType>(); },
            [&](const CompleteBuildOrder&) { return std::optional<CursorType>(CursorType::Repair); },
            [&](const GuardOrder&) { return std::optional<CursorType>(CursorType::Guard); },
            [&](const RepairOrder&) { return std::optional<CursorType>(CursorType::Repair); },
            [&](const ReclaimOrder&) { return std::optional<CursorType>(CursorType::Reclaim); },
            [&](const PatrolOrder&) { return std::optional<CursorType>(CursorType::Patrol); },
            [&](const WaitOrder&) { return std::optional<CursorType>(); },
            [&](const ParalyzeOrder&) { return std::optional<CursorType>(); },
            [&](const LandOrder&) { return std::optional<CursorType>(CursorType::Move); },
            [&](const WaitForAttackOrder&) { return std::optional<CursorType>(); },
            [&](const MakeSelectableOrder&) { return std::optional<CursorType>(); },
            [&](const AttackUnitTypeOrder&) { return std::optional<CursorType>(CursorType::Attack); },
            [&](const MissionBuildOrder&) { return std::optional<CursorType>(); },
            [&](const MissionFactoryBuildOrder&) { return std::optional<CursorType>(); },
            [&](const SelfDestructOrder&) { return std::optional<CursorType>(); },
            [&](const UnloadOrder&) { return std::optional<CursorType>(CursorType::Unload); },
            [&](const LoadOrder&) { return std::optional<CursorType>(CursorType::Load); },
            [&](const BuildWeaponOrder&) { return std::optional<CursorType>(); },
            [&](const CommandFireOrder&) { return std::optional<CursorType>(CursorType::CommandFire); },
            [&](const CaptureOrder&) { return std::optional<CursorType>(CursorType::Capture); });
    }

    SimVector orderMarkerPosition(const GameSimulation& simulation, const UnitOrder& order, const SimVector& fallbackPos)
    {
        auto tryGetUnit = [&](UnitId id) { return simulation.tryGetUnitState(id); };

        return match(
            order,
            [&](const BuildOrder& o) { return o.position; },
            [&](const MoveOrder& o) { return o.destination; },
            [&](const AttackOrder& o) { return match(
                                                o.target,
                                                [&](const SimVector& v) { return v; },
                                                [&](const UnitId& u) {
                                                    auto unitOption = tryGetUnit(u);
                                                    if (!unitOption)
                                                    {
                                                        return fallbackPos;
                                                    }
                                                    return unitOption->get().position;
                                                }); },
            [&](const BuggerOffOrder&) { return fallbackPos; },
            [&](const CompleteBuildOrder& o) {
                auto unitOption = tryGetUnit(o.target);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const GuardOrder& o) {
                auto unitOption = tryGetUnit(o.target);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const RepairOrder& o) {
                auto unitOption = tryGetUnit(o.target);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const ReclaimOrder& o) {
                return match(
                    o.target,
                    [&](const FeatureId& featureId) {
                        auto featureOption = simulation.tryGetFeature(featureId);
                        if (!featureOption)
                        {
                            return fallbackPos;
                        }
                        return featureOption->get().position;
                    },
                    [&](const UnitId& unitId) {
                        auto unitOption = tryGetUnit(unitId);
                        if (!unitOption)
                        {
                            return fallbackPos;
                        }
                        return unitOption->get().position;
                    });
            },
            [&](const CaptureOrder& o) {
                auto unitOption = tryGetUnit(o.target);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const PatrolOrder& o) {
                if (o.waypoints.empty())
                {
                    return fallbackPos;
                }
                auto index = o.nextWaypointIndex < o.waypoints.size() ? o.nextWaypointIndex : 0u;
                return o.waypoints[index];
            },
            [&](const WaitOrder&) { return fallbackPos; },
            [&](const WaitForAttackOrder&) { return fallbackPos; },
            [&](const MakeSelectableOrder&) { return fallbackPos; },
            [&](const AttackUnitTypeOrder& o) {
                if (!o.currentTarget)
                {
                    return fallbackPos;
                }
                auto unitOption = tryGetUnit(*o.currentTarget);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const MissionBuildOrder& o) { return o.position; },
            [&](const MissionFactoryBuildOrder&) { return fallbackPos; },
            [&](const SelfDestructOrder&) { return fallbackPos; },
            [&](const ParalyzeOrder&) { return fallbackPos; },
            [&](const LandOrder& o) {
                auto unit = tryGetUnit(o.target);
                return unit ? unit->get().position : fallbackPos;
            },
            [&](const UnloadOrder& o) { return o.destination; },
            [&](const LoadOrder& o) {
                auto unitOption = tryGetUnit(o.target);
                if (!unitOption)
                {
                    return fallbackPos;
                }
                return unitOption->get().position;
            },
            [&](const BuildWeaponOrder&) { return fallbackPos; },
            [&](const CommandFireOrder& o) { return match(
                o.target,
                [&](const SimVector& v) { return v; },
                [&](const UnitId& u) {
                    auto unitOption = tryGetUnit(u);
                    if (!unitOption)
                    {
                        return fallbackPos;
                    }
                    return unitOption->get().position;
                }); });
    }

    bool orderMarkerContainsPoint(const Rectangle2f& spriteBounds, const Vector2f& uiMarkerPos, const Vector2f& uiMousePos, float hitSlop)
    {
        auto center = uiMarkerPos + spriteBounds.position;
        auto halfSize = spriteBounds.extents + Vector2f(hitSlop, hitSlop);
        return std::abs(uiMousePos.x - center.x) <= halfSize.x
            && std::abs(uiMousePos.y - center.y) <= halfSize.y;
    }

    std::optional<std::size_t> nearestUiPoint(const std::vector<Vector2f>& uiPoints, const Vector2f& uiMousePos)
    {
        std::optional<std::size_t> best;
        float bestDistanceSquared = 0.0f;
        for (std::size_t i = 0; i < uiPoints.size(); ++i)
        {
            auto distanceSquared = (uiPoints[i] - uiMousePos).lengthSquared();
            if (!best || distanceSquared < bestDistanceSquared)
            {
                best = i;
                bestDistanceSquared = distanceSquared;
            }
        }
        return best;
    }

    std::vector<Point> linearBuildCells(const Point& anchorTopLeftCell, const Point& cursorTopLeftCell, unsigned int footprintW, unsigned int footprintH)
    {
        std::vector<Point> cells;
        if (footprintW == 0 || footprintH == 0)
        {
            cells.push_back(anchorTopLeftCell);
            return cells;
        }

        auto dx = cursorTopLeftCell.x - anchorTopLeftCell.x;
        auto dz = cursorTopLeftCell.y - anchorTopLeftCell.y;
        auto absDx = std::abs(dx);
        auto absDz = std::abs(dz);

        // dominant axis by approximate placement count; exact tie -> horizontal
        bool horizontal = absDx * static_cast<int>(footprintH) >= absDz * static_cast<int>(footprintW);

        if (horizontal)
        {
            int step = static_cast<int>(footprintW);
            int dir = dx < 0 ? -1 : 1;
            for (int i = 0, n = absDx / step; i <= n; ++i)
            {
                cells.push_back(Point(anchorTopLeftCell.x + i * step * dir, anchorTopLeftCell.y));
            }
        }
        else
        {
            int step = static_cast<int>(footprintH);
            int dir = dz < 0 ? -1 : 1;
            for (int i = 0, n = absDz / step; i <= n; ++i)
            {
                cells.push_back(Point(anchorTopLeftCell.x, anchorTopLeftCell.y + i * step * dir));
            }
        }

        return cells;
    }

    bool queuedBuildSiteExists(const std::deque<UnitOrder>& orders, const std::string& unitType, const SimVector& position)
    {
        return std::any_of(orders.begin(), orders.end(), [&](const UnitOrder& order) {
            if (auto buildOrder = std::get_if<BuildOrder>(&order))
            {
                return buildOrder->unitType == unitType && buildOrder->position == position;
            }
            return false;
        });
    }

    std::vector<std::pair<std::string, std::string>> makeGameOptionsRows(
        const std::string& mapName,
        SkirmishCommanderDeath commanderDeath,
        SkirmishStartLocation startLocation,
        bool mappingEnabled,
        int lineOfSightMode,
        SkirmishDifficulty difficulty,
        unsigned int startingMetal,
        unsigned int startingEnergy,
        unsigned int maxUnits)
    {
        static const std::array<const char*, 3> lineOfSightNames{"Permanent", "True", "Circular"};

        std::vector<std::pair<std::string, std::string>> rows;
        rows.emplace_back("Commander Death:", toString(commanderDeath));
        rows.emplace_back("Starting Locations:", toString(startLocation));
        rows.emplace_back("Mapping Mode:", mappingEnabled ? "Mapped" : "Unmapped");
        rows.emplace_back(
            "Line of Sight:",
            lineOfSightMode >= 0 && lineOfSightMode < 3 ? lineOfSightNames[lineOfSightMode] : lineOfSightNames[0]);
        rows.emplace_back("Difficulty:", toString(difficulty));
        rows.emplace_back("Map:", mapName);
        rows.emplace_back("Starting Metal:", std::to_string(startingMetal));
        rows.emplace_back("Starting Energy:", std::to_string(startingEnergy));
        // retail's Max Units row shows the live UnitLimit (default
        // 250, clamped [20,500]) — the same per-player cap the sim
        // enforces at unit creation
        rows.emplace_back("Max Units:", std::to_string(maxUnits));
        return rows;
    }

    std::string inGameHelpText(const std::string& raw, unsigned int page, unsigned int pageCount)
    {
        std::vector<std::string> entries;
        try
        {
            auto tdf = parseTdfFromString(raw);
            if (auto help = tdf.findBlock("Help"))
            {
                for (unsigned int i = 0;; ++i)
                {
                    auto value = help->get().findValue("Line" + std::to_string(i));
                    if (!value)
                    {
                        break;
                    }
                    entries.push_back(*value);
                }
            }
        }
        catch (const std::exception&)
        {
            return "";
        }

        if (entries.empty() || pageCount == 0 || page >= pageCount)
        {
            return "";
        }

        const auto linesPerPage = (entries.size() + pageCount - 1) / pageCount;
        const auto begin = std::min(entries.size(), linesPerPage * page);
        const auto end = std::min(entries.size(), begin + linesPerPage);

        std::string text;
        for (auto i = begin; i < end; ++i)
        {
            auto& entry = entries[i];
            auto sep = entry.find('|');
            if (sep == std::string::npos)
            {
                text += entry;
            }
            else if (sep == 0)
            {
                // stock uses "|;" for blank spacer lines
                text += "";
            }
            else
            {
                text += entry.substr(0, sep) + "  --  " + entry.substr(sep + 1);
            }
            text += "\n";
        }
        return text;
    }

    namespace
    {
        void appendUiQuad(std::vector<GlColoredVertex>& vertices, float x, float y, float width, float height, const Vector3f& color)
        {
            vertices.push_back({{x, y, 0.0f}, color});
            vertices.push_back({{x, y + height, 0.0f}, color});
            vertices.push_back({{x + width, y + height, 0.0f}, color});
            vertices.push_back({{x + width, y + height, 0.0f}, color});
            vertices.push_back({{x + width, y, 0.0f}, color});
            vertices.push_back({{x, y, 0.0f}, color});
        }
    }

    void appendUiLineVertices(std::vector<GlColoredVertex>& vertices, const Vector2f& start, const Vector2f& end)
    {
        auto floatColor = Vector3f(1.0f, 1.0f, 1.0f);
        vertices.push_back({{start.x, start.y, 0.0f}, floatColor});
        vertices.push_back({{end.x, end.y, 0.0f}, floatColor});
    }

    void appendUiBoxOutlineVertices(std::vector<GlColoredVertex>& vertices, float x, float y, float width, float height, const Color& color, float thickness)
    {
        if (width < 1.0f || height < 1.0f)
        {
            return;
        }

        auto floatColor = Vector3f(color.r, color.g, color.b) / 255.0f;

        if (width <= thickness * 2.0f || height <= thickness * 2.0f)
        {
            appendUiQuad(vertices, x, y, width, height, floatColor);
            return;
        }

        // top
        appendUiQuad(vertices, x, y, width, thickness, floatColor);
        // bottom
        appendUiQuad(vertices, x, y + height - thickness, width, thickness, floatColor);
        // left
        appendUiQuad(vertices, x, y + thickness, thickness, height - (thickness * 2.0f), floatColor);
        // right
        appendUiQuad(vertices, x + width - thickness, y + thickness, thickness, height - (thickness * 2.0f), floatColor);
    }
}

namespace rwe
{
    OrdersPanelCapabilities aggregateOrdersPanelCapabilities(
        const std::vector<const UnitDefinition*>& selectedDefinitions,
        const std::unordered_map<std::string, WeaponDefinition>& weaponDefinitions)
    {
        OrdersPanelCapabilities caps;
        for (const auto* def : selectedDefinitions)
        {
            caps.canMove = caps.canMove || def->canMove;
            caps.canStop = caps.canStop || def->canStop;
            caps.canAttack = caps.canAttack || def->canAttack;
            caps.canGuard = caps.canGuard || def->canGuard;
            caps.canPatrol = caps.canPatrol || def->canPatrol;
            caps.canReclaim = caps.canReclaim || def->canReclaim;
            caps.canCapture = caps.canCapture || def->canCapture;
            caps.canLoad = caps.canLoad || def->canLoad;
            // BLAST lights for `candgun` units and for units carrying a
            // commandfire weapon (nuke/anti silos fire stockpiled
            // rounds through the same command)
            caps.canDGun = caps.canDGun || def->canDGun || unitDefinitionHasCommandFireWeapon(weaponDefinitions, *def);
            caps.canCloak = caps.canCloak || (def->cloakCost > 0.0f);
        }
        return caps;
    }

    unsigned int cloakPanelStage(const GameSimulation& sim, const std::unordered_set<UnitId>& selectedUnits)
    {
        // 0x41b2e0: slot inits to 3; the first cloak-capable unit
        // writes its requested bit; every further capable unit
        // latches 2 unconditionally
        unsigned int stage = 3;
        for (const auto& unitId : selectedUnits)
        {
            auto unit = sim.tryGetUnitState(unitId);
            if (!unit)
            {
                continue;
            }
            if (!(sim.unitDefinitions.at(unit->get().unitType).cloakCost > 0.0f))
            {
                continue;
            }
            if (stage == 3)
            {
                stage = unit->get().cloakRequested ? 1 : 0;
            }
            else
            {
                stage = 2;
            }
        }
        return stage;
    }

    void applyOrdersPanelCapabilities(UiPanel& panel, const std::string& sidePrefix, const OrdersPanelCapabilities& caps)
    {
        auto setEnabled = [&panel, &sidePrefix](const std::string& suffix, bool enabled) {
            if (auto button = panel.find<UiStagedButton>(sidePrefix + suffix))
            {
                button->get().setEnabled(enabled);
            }
        };

        setEnabled("MOVE", caps.canMove);
        setEnabled("STOP", caps.canStop);
        setEnabled("ATTACK", caps.canAttack);
        setEnabled("DEFEND", caps.canGuard);
        setEnabled("PATROL", caps.canPatrol);
        setEnabled("RECLAIM", caps.canReclaim);
        // retail feeds the same flag bit to RECLAIM and REPAIR
        // (unitdef +0x245 bit 9 is a copy of the canreclamate bit 10,
        // TotalA.exe 0x42ca37-0x42ca4d)
        setEnabled("REPAIR", caps.canReclaim);
        setEnabled("CAPTURE", caps.canCapture);
        setEnabled("UNLOAD", caps.canLoad);
        // retail deactivates (greys) the CLOAK strip when no selected
        // unit carries the cloak bit (mask value 3 -> 0x4a1200)
        setEnabled("CLOAK", caps.canCloak);

        // LOAD and BLAST occupy the same gadget rect (ARMGEN.GUI
        // 64,317): retail deactivates whichever does not apply
        // (gadget active byte +0x29)
        if (auto load = panel.find<UiStagedButton>(sidePrefix + "LOAD"))
        {
            load->get().setHidden(!caps.canLoad);
            load->get().setEnabled(caps.canLoad);
        }
        if (auto blast = panel.find<UiStagedButton>(sidePrefix + "BLAST"))
        {
            blast->get().setHidden(caps.canLoad);
            blast->get().setEnabled(caps.canDGun);
        }
    }

    MissionEndAction missionEndAction(MissionResult result, bool alreadyDeclared)
    {
        if (alreadyDeclared)
        {
            return MissionEndAction::None;
        }
        switch (result)
        {
            case MissionResult::Victory:
                return MissionEndAction::DeclareVictory;
            case MissionResult::Defeat:
                return MissionEndAction::DeclareDefeat;
            case MissionResult::InProgress:
            default:
                return MissionEndAction::None;
        }
    }

    bool unitDefinitionHasStockpileWeapon(const GameSimulation& simulation, const UnitDefinition& unitDefinition)
    {
        for (const auto& weaponName : {unitDefinition.weapon1, unitDefinition.weapon2, unitDefinition.weapon3})
        {
            if (weaponName.empty())
            {
                continue;
            }
            auto it = simulation.weaponDefinitions.find(toUpper(weaponName));
            if (it != simulation.weaponDefinitions.end() && it->second.stockpile)
            {
                return true;
            }
        }
        return false;
    }

    bool unitDefinitionHasCommandFireWeapon(const std::unordered_map<std::string, WeaponDefinition>& weaponDefinitions, const UnitDefinition& unitDefinition)
    {
        for (const auto& weaponName : {unitDefinition.weapon1, unitDefinition.weapon2, unitDefinition.weapon3})
        {
            if (weaponName.empty())
            {
                continue;
            }
            auto it = weaponDefinitions.find(toUpper(weaponName));
            if (it != weaponDefinitions.end() && it->second.commandFire)
            {
                return true;
            }
        }
        return false;
    }

    bool applyBasicPlayerUnitCommand(UnitState& unit, const PlayerUnitCommand::Command& command)
    {
        if (const auto* c = std::get_if<PlayerUnitCommand::IssueOrder>(&command))
        {
            if (c->issueKind == PlayerUnitCommand::IssueOrder::Immediate)
            {
                unit.clearOrders();
                unit.addOrder(expandPatrolOrder(unit, c->order));
            }
            else if (c->issueKind == PlayerUnitCommand::IssueOrder::Queued) { enqueueUnitOrder(unit, c->order); }
            else { return false; }
        }
        else if (std::holds_alternative<PlayerUnitCommand::Stop>(command)) { unit.clearOrders(); }
        else if (const auto* c = std::get_if<PlayerUnitCommand::SetFireOrders>(&command)) { unit.fireOrders = c->orders; }
        else if (const auto* c = std::get_if<PlayerUnitCommand::ModifyBuildQueue>(&command)) { unit.modifyBuildQueue(c->unitType, c->count); }
        else { return false; }
        return true;
    }

    void applyModifyWeaponQueue(UnitState& unit, int count)
    {
        auto& backgroundOrders = unit.backgroundOrders;
        if (count > 0)
        {
            // clicks accumulate onto the queued stockpile order rather
            // than stacking separate orders
            auto it = std::find_if(backgroundOrders.begin(), backgroundOrders.end(), [](const UnitOrder& o) { return std::holds_alternative<BuildWeaponOrder>(o); });
            if (it != backgroundOrders.end())
            {
                std::get<BuildWeaponOrder>(*it).remaining += static_cast<unsigned int>(count);
            }
            else
            {
                backgroundOrders.push_back(BuildWeaponOrder{static_cast<unsigned int>(count)});
            }
        }
        else if (count < 0)
        {
            // a secondary click cancels pending rounds from the queue
            int toCancel = -count;
            for (auto it = backgroundOrders.begin(); it != backgroundOrders.end() && toCancel > 0;)
            {
                if (auto* order = std::get_if<BuildWeaponOrder>(&*it))
                {
                    if (order->remaining > static_cast<unsigned int>(toCancel))
                    {
                        order->remaining -= static_cast<unsigned int>(toCancel);
                        toCancel = 0;
                    }
                    else
                    {
                        toCancel -= static_cast<int>(order->remaining);
                        it = backgroundOrders.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
    }

    UnitGuiInfo initialUnitGuiInfo(const UnitDefinition& unitDefinition, bool hasWeaponBuildPanel)
    {
        return UnitGuiInfo{
            (unitDefinition.builder || hasWeaponBuildPanel) ? UnitGuiInfo::Section::Build : UnitGuiInfo::Section::Orders,
            0};
    }

    void rebuildUnitGuiInfos(const GameSimulation& simulation, std::unordered_map<UnitId, UnitGuiInfo>& out)
    {
        for (const auto& pair : simulation.units)
        {
            const auto& unitDefinition = simulation.unitDefinitions.at(pair.second.unitType);
            out.insert_or_assign(pair.first, initialUnitGuiInfo(unitDefinition, unitDefinitionHasStockpileWeapon(simulation, unitDefinition)));
        }
    }
}
