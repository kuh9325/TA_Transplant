#pragma once

#include <filesystem>
#include <rwe/render/GraphicsContext.h>
#include <unordered_map>

namespace rwe
{
    struct BasicColorShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
        UniformLocation alpha;
    };

    struct BasicTextureShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
        UniformLocation tint;
    };

    struct MapTerrainShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
    };

    struct UnitTextureShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
        UniformLocation modelMatrix;
        UniformLocation seaLevel;
        UniformLocation shade;
        UniformLocation alpha;
    };

    struct UnitShadowShader
    {
        ShaderProgramHandle handle;
        UniformLocation vpMatrix;
        UniformLocation modelMatrix;
        UniformLocation groundHeight;
    };

    struct UnitBuildShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
        UniformLocation modelMatrix;
        UniformLocation unitY;
        UniformLocation seaLevel;
        UniformLocation shade;
        UniformLocation percentComplete;
        UniformLocation time;
    };

    struct FlashEffectShader
    {
        ShaderProgramHandle handle;
        UniformLocation mvpMatrix;
        UniformLocation intensity;
        UniformLocation color;
    };

    struct WorldPostShader
    {
        ShaderProgramHandle handle;
        UniformLocation dodgeMask;
    };

    /**
     * Instanced unit shaders: per-piece model matrix and flags arrive as
     * instance attributes (mat4 iModel at locations 3-6, vec4 iParams at
     * location 7) so a whole mesh group submits in one draw call.
     */
    struct UnitTextureInstancedShader
    {
        ShaderProgramHandle handle;
        UniformLocation vpMatrix;
        UniformLocation seaLevel;
    };

    struct UnitShadowInstancedShader
    {
        ShaderProgramHandle handle;
        UniformLocation vpMatrix;
    };

    struct UnitBuildInstancedShader
    {
        ShaderProgramHandle handle;
        UniformLocation vpMatrix;
        UniformLocation seaLevel;
        UniformLocation time;
    };

    class ShaderService
    {
    public:
        static ShaderService createShaderService(GraphicsContext& graphics, const std::filesystem::path& resourceRoot = std::filesystem::path());

    private:
        std::string slurpFile(const std::string& filename) const;

        ShaderProgramHandle loadShader(GraphicsContext& graphics, const std::string& vertexShaderName, const std::string& fragmentShaderName, const std::vector<AttribMapping>& attribs) const;

        /**
         * Fallback root for shader files, used when the CWD-relative
         * path misses (e.g. Finder-launched .app). See
         * getResourceRoot().
         */
        std::filesystem::path resourceRoot;

    public:
        BasicColorShader basicColor;
        BasicTextureShader basicTexture;
        MapTerrainShader mapTerrain;
        UnitTextureShader unitTexture;
        UnitShadowShader unitShadow;
        UnitBuildShader unitBuild;
        UnitTextureInstancedShader unitTextureInstanced;
        UnitShadowInstancedShader unitShadowInstanced;
        UnitBuildInstancedShader unitBuildInstanced;
        FlashEffectShader flashEffect;
        WorldPostShader worldPost;
    };
}
