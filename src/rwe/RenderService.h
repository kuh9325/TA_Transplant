#pragma once

#include <rwe/ShaderService.h>
#include <rwe/game/FlashEffect.h>
#include <rwe/game/MapTerrainGraphics.h>
#include <rwe/render/GraphicsContext.h>
#include <rwe/sim/GameTime.h>
#include <vector>

namespace rwe
{
    struct ColoredMeshBatch
    {
        std::vector<GlColoredVertex> lines;
        std::vector<GlColoredVertex> triangles;

        /**
         * Draw alpha override; 1 = opaque. Used by the fog-of-war
         * overlay to dim explored terrain rather than black it out.
         */
        float alpha{1.0f};
    };

    struct ColoredMeshRenderInfo
    {
        const GlMesh* mesh;
        Matrix4f mvpMatrix;
    };

    struct ColoredMeshesBatch
    {
        std::vector<ColoredMeshRenderInfo> meshes;
    };

    struct UnitTextureMeshRenderInfo
    {
        const GlMesh* mesh;
        Matrix4f modelMatrix;
        Matrix4f mvpMatrix;
        bool shaded;
        TextureIdentifier texture;

        /**
         * Fragment alpha; 1 = opaque. Below 1 renders the unit
         * translucent -- used for the owner's cloaked units (retail
         * draws them through its ghost blit variant so the owner can
         * still see them).
         */
        float alpha{1.0f};
    };

    struct UnitBuildingMeshRenderInfo
    {
        const GlMesh* mesh;
        Matrix4f modelMatrix;
        Matrix4f mvpMatrix;
        bool shaded;
        TextureIdentifier texture;
        float percentComplete;
        float unitY;
    };

    struct UnitTextureShadowMeshRenderInfo
    {
        const GlMesh* mesh;
        Matrix4f modelMatrix;
        Matrix4f vpMatrix;
        TextureIdentifier texture;
        float groundHeight;
    };

    struct UnitMeshBatch
    {
        std::vector<UnitTextureMeshRenderInfo> meshes;
        std::vector<UnitBuildingMeshRenderInfo> buildingMeshes;
    };

    struct UnitShadowMeshBatch
    {
        std::vector<UnitTextureShadowMeshRenderInfo> meshes;
    };

    /**
     * Per-instance GPU record for instanced unit draws. Must match the
     * attribute layout expected by drawTrianglesInstanced: mat4 iModel
     * (locations 3-6, offset 0) + mat4 iMvp (locations 8-11, offset 64)
     * + vec4 iParams (location 7, offset 128), 144 bytes. iMvp carries
     * the CPU-precomputed vp*model so vertex positions round exactly as
     * the per-draw path did; the shadow shader ignores it (it projects
     * a terrain-flattened point through the shared vpMatrix instead).
     * param meanings per shader:
     *   unitTextureInstanced: x = shade, y = alpha
     *   unitShadowInstanced:  x = groundHeight
     *   unitBuildInstanced:   x = shade, y = percentComplete, z = unitY
     */
    struct UnitInstanceData
    {
        float modelMatrix[16];
        float mvpMatrix[16];
        float params[4];
    };
    static_assert(sizeof(UnitInstanceData) == 144);

    struct SpriteRenderInfo
    {
        const Sprite* sprite;
        Matrix4f mvpMatrix;
        bool translucent;
    };

    struct SpriteBatch
    {
        std::vector<SpriteRenderInfo> sprites;
    };

    class RenderService
    {
    private:
        GraphicsContext* graphics;
        ShaderService* shaders;

        const Matrix4f* const viewProjectionMatrix;

    public:
        RenderService(
            GraphicsContext* graphics,
            ShaderService* shaders,
            const Matrix4f* viewProjectionMatrix);

        void drawMapTerrain(const MapTerrainGraphics& terrain, const Vector3f& cameraPosition, float viewportWidth, float viewportHeight);

        void drawMapTerrain(const MapTerrainGraphics& terrain, unsigned int x, unsigned int y, unsigned int width, unsigned int height);

        void fillScreen(float r, float g, float b, float a);

        void drawFlashes(GameTime currentTime, const std::vector<FlashEffect>& flashes);

        void drawBatch(const ColoredMeshBatch& batch, const Matrix4f& vpMatrix);

        void drawUnitMeshBatch(const UnitMeshBatch& batch, float seaLevel, float time);

        void drawUnitShadowMeshBatch(const UnitShadowMeshBatch& batch);

        void drawSpriteBatch(const SpriteBatch& batch);

        void drawLineLoopsBatch(const ColoredMeshesBatch& batch);

        /**
         * Toggles the instanced unit draw path (default on, or off when
         * RWE_DISABLE_INSTANCING=1). The screenshot A/B harness flips this
         * between renders of identical state.
         */
        void setUnitInstancingEnabled(bool enabled);

    private:
        void drawUnitMeshBatchInstanced(const UnitMeshBatch& batch, float seaLevel, float time);
        void drawUnitShadowMeshBatchInstanced(const UnitShadowMeshBatch& batch);

        bool unitInstancingEnabled{true};
    };
}
