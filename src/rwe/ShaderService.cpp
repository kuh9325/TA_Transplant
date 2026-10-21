#include "ShaderService.h"
#include <fstream>
#include <sstream>

namespace rwe
{
    ShaderService ShaderService::createShaderService(GraphicsContext& graphics, const std::filesystem::path& resourceRoot)
    {
        ShaderService s;
        s.resourceRoot = resourceRoot;

        std::vector<AttribMapping> texturedVertexAttribs{
            AttribMapping{"position", 0},
            AttribMapping{"texCoord", 1}};

        std::vector<AttribMapping> coloredVertexAttribs{
            AttribMapping{"position", 0},
            AttribMapping{"color", 1}};

        s.basicColor.handle = s.loadShader(graphics, "shaders/basicColor.vert", "shaders/basicColor.frag", coloredVertexAttribs);
        s.basicColor.mvpMatrix = graphics.getUniformLocation(s.basicColor.handle.get(), "mvpMatrix");
        s.basicColor.alpha = graphics.getUniformLocation(s.basicColor.handle.get(), "alpha");

        s.basicTexture.handle = s.loadShader(graphics, "shaders/basicTexture.vert", "shaders/basicTexture.frag", texturedVertexAttribs);
        s.basicTexture.mvpMatrix = graphics.getUniformLocation(s.basicTexture.handle.get(), "mvpMatrix");
        s.basicTexture.tint = graphics.getUniformLocation(s.basicTexture.handle.get(), "tint");

        s.mapTerrain.handle = s.loadShader(graphics, "shaders/mapTerrain.vert", "shaders/mapTerrain.frag", texturedVertexAttribs);
        s.mapTerrain.mvpMatrix = graphics.getUniformLocation(s.mapTerrain.handle.get(), "mvpMatrix");

        s.unitTexture.handle = s.loadShader(graphics, "shaders/unitTexture.vert", "shaders/unitTexture.frag", texturedVertexAttribs);
        s.unitTexture.mvpMatrix = graphics.getUniformLocation(s.unitTexture.handle.get(), "mvpMatrix");
        s.unitTexture.modelMatrix = graphics.getUniformLocation(s.unitTexture.handle.get(), "modelMatrix");
        s.unitTexture.seaLevel = graphics.getUniformLocation(s.unitTexture.handle.get(), "seaLevel");
        s.unitTexture.shade = graphics.getUniformLocation(s.unitTexture.handle.get(), "shade");
        s.unitTexture.alpha = graphics.getUniformLocation(s.unitTexture.handle.get(), "alpha");

        s.unitShadow.handle = s.loadShader(graphics, "shaders/unitShadow.vert", "shaders/unitShadow.frag", texturedVertexAttribs);
        s.unitShadow.vpMatrix = graphics.getUniformLocation(s.unitShadow.handle.get(), "vpMatrix");
        s.unitShadow.modelMatrix = graphics.getUniformLocation(s.unitShadow.handle.get(), "modelMatrix");
        s.unitShadow.groundHeight = graphics.getUniformLocation(s.unitShadow.handle.get(), "groundHeight");

        s.unitBuild.handle = s.loadShader(graphics, "shaders/unitBuild.vert", "shaders/unitBuild.frag", texturedVertexAttribs);
        s.unitBuild.mvpMatrix = graphics.getUniformLocation(s.unitBuild.handle.get(), "mvpMatrix");
        s.unitBuild.unitY = graphics.getUniformLocation(s.unitBuild.handle.get(), "unitY");
        s.unitBuild.modelMatrix = graphics.getUniformLocation(s.unitBuild.handle.get(), "modelMatrix");
        s.unitBuild.seaLevel = graphics.getUniformLocation(s.unitBuild.handle.get(), "seaLevel");
        s.unitBuild.shade = graphics.getUniformLocation(s.unitBuild.handle.get(), "shade");
        s.unitBuild.percentComplete = graphics.getUniformLocation(s.unitBuild.handle.get(), "percentComplete");
        s.unitBuild.time = graphics.getUniformLocation(s.unitBuild.handle.get(), "time");

        // instanced unit shaders: position=0, texCoord=1, normal=2,
        // mat4 iModel occupies 3-6, vec4 iParams at 7, mat4 iMvp at 8-11
        std::vector<AttribMapping> instancedUnitAttribs{
            AttribMapping{"position", 0},
            AttribMapping{"texCoord", 1},
            AttribMapping{"normal", 2},
            AttribMapping{"iModel", 3},
            AttribMapping{"iParams", 7},
            AttribMapping{"iMvp", 8}};

        s.unitTextureInstanced.handle = s.loadShader(graphics, "shaders/unitTextureInstanced.vert", "shaders/unitTextureInstanced.frag", instancedUnitAttribs);
        s.unitTextureInstanced.vpMatrix = graphics.getUniformLocation(s.unitTextureInstanced.handle.get(), "vpMatrix");
        s.unitTextureInstanced.seaLevel = graphics.getUniformLocation(s.unitTextureInstanced.handle.get(), "seaLevel");

        s.unitShadowInstanced.handle = s.loadShader(graphics, "shaders/unitShadowInstanced.vert", "shaders/unitShadowInstanced.frag", instancedUnitAttribs);
        s.unitShadowInstanced.vpMatrix = graphics.getUniformLocation(s.unitShadowInstanced.handle.get(), "vpMatrix");

        s.unitBuildInstanced.handle = s.loadShader(graphics, "shaders/unitBuildInstanced.vert", "shaders/unitBuildInstanced.frag", instancedUnitAttribs);
        s.unitBuildInstanced.vpMatrix = graphics.getUniformLocation(s.unitBuildInstanced.handle.get(), "vpMatrix");
        s.unitBuildInstanced.seaLevel = graphics.getUniformLocation(s.unitBuildInstanced.handle.get(), "seaLevel");
        s.unitBuildInstanced.time = graphics.getUniformLocation(s.unitBuildInstanced.handle.get(), "time");

        s.flashEffect.handle = s.loadShader(graphics, "shaders/flashEffect.vert", "shaders/flashEffect.frag", texturedVertexAttribs);
        s.flashEffect.mvpMatrix = graphics.getUniformLocation(s.flashEffect.handle.get(), "mvpMatrix");
        s.flashEffect.intensity = graphics.getUniformLocation(s.flashEffect.handle.get(), "intensity");
        s.flashEffect.color = graphics.getUniformLocation(s.flashEffect.handle.get(), "color");

        s.worldPost.handle = s.loadShader(graphics, "shaders/worldPost.vert", "shaders/worldPost.frag", texturedVertexAttribs);
        s.worldPost.dodgeMask = graphics.getUniformLocation(s.worldPost.handle.get(), "dodgeMask");

        return s;
    }

    std::string ShaderService::slurpFile(const std::string& filename) const
    {
        {
            std::ifstream inFile(filename, std::ios::binary);
            if (!inFile.fail())
            {
                std::stringstream strStream;
                strStream << inFile.rdbuf();
                return strStream.str();
            }
        }

        if (!resourceRoot.empty())
        {
            std::ifstream inFile(resourceRoot / filename, std::ios::binary);
            if (!inFile.fail())
            {
                std::stringstream strStream;
                strStream << inFile.rdbuf();
                return strStream.str();
            }
        }

        throw std::runtime_error("Failed to open file: " + filename);
    }

    ShaderProgramHandle ShaderService::loadShader(
        GraphicsContext& graphics,
        const std::string& vertexShaderName,
        const std::string& fragmentShaderName,
        const std::vector<AttribMapping>& attribs) const
    {
        auto vertexShaderSource = slurpFile(vertexShaderName);
        auto vertexShader = graphics.compileVertexShader(vertexShaderSource);

        auto fragmentShaderSource = slurpFile(fragmentShaderName);
        auto fragmentShader = graphics.compileFragmentShader(fragmentShaderSource);

        return graphics.linkShaderProgram(vertexShader.get(), fragmentShader.get(), attribs);
    }
}
