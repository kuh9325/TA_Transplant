#pragma once

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <memory>
#include <rwe/ColorPalette.h>
#include <rwe/Mesh.h>
#include <rwe/Viewport.h>
#include <rwe/geometry/CollisionMesh.h>
#include <rwe/math/Vector3f.h>
#include <rwe/render/FrameBufferHandle.h>
#include <rwe/render/GlMesh.h>
#include <rwe/render/RenderBufferHandle.h>
#include <rwe/render/ShaderHandle.h>
#include <rwe/render/ShaderProgramHandle.h>
#include <rwe/render/Sprite.h>
#include <rwe/render/SpriteSeries.h>
#include <rwe/render/TextureArrayHandle.h>
#include <rwe/render/TextureHandle.h>
#include <rwe/render/UniformLocation.h>
#include <rwe/sim/MapFeature.h>
#include <rwe/sim/MapTerrain.h>

namespace rwe
{
#pragma pack(1)
    struct GlTexturedVertex
    {
        GLfloat x;
        GLfloat y;
        GLfloat z;
        GLfloat u;
        GLfloat v;

        GlTexturedVertex() = default;
        GlTexturedVertex(const Vector3f& pos, const Vector2f& texCoord);
    };

    struct GlTextureArrayVertex
    {
        GLfloat x;
        GLfloat y;
        GLfloat z;
        GLfloat u;
        GLfloat v;
        GLfloat w;

        GlTextureArrayVertex() = default;
        GlTextureArrayVertex(const Vector3f& pos, const Vector3f& texCoord);
    };

    struct GlTexturedNormalVertex
    {
        GLfloat x;
        GLfloat y;
        GLfloat z;
        GLfloat u;
        GLfloat v;
        GLfloat nx;
        GLfloat ny;
        GLfloat nz;

        GlTexturedNormalVertex() = default;
        GlTexturedNormalVertex(const Vector3f& pos, const Vector2f& texCoord, const Vector3f& normal);
    };

    struct GlColoredVertex
    {
        GLfloat x;
        GLfloat y;
        GLfloat z;
        GLfloat r;
        GLfloat g;
        GLfloat b;

        GlColoredVertex() = default;
        GlColoredVertex(const Vector3f& pos, const Vector3f& color);
    };

    struct GlColoredNormalVertex
    {
        GLfloat x;
        GLfloat y;
        GLfloat z;
        GLfloat r;
        GLfloat g;
        GLfloat b;
        GLfloat nx;
        GLfloat ny;
        GLfloat nz;

        GlColoredNormalVertex() = default;
        GlColoredNormalVertex(const Vector3f& pos, const Vector3f& color, const Vector3f& normal);
    };
#pragma pack()

    struct AttribMapping
    {
        std::string name;
        GLuint location;

        AttribMapping(const std::string& name, GLuint location);
    };

    class GraphicsException : public std::runtime_error
    {
    public:
        explicit GraphicsException(const std::string& __arg);

        explicit GraphicsException(const char* string);
    };

    class OpenGlException : public GraphicsException
    {
    public:
        explicit OpenGlException(GLenum error);
    };

    struct FrameBufferInfo
    {
        TextureHandle texture;
        RenderBufferHandle depthBuffer;
        FrameBufferHandle frameBuffer;
    };

    /**
     * Per-frame GL activity counters. Instrumentation only -- cheap
     * integer increments; consumers snapshot and reset() per frame.
     */
    struct RenderStats
    {
        std::uint64_t drawCalls{0};
        std::uint64_t verticesDrawn{0};
        std::uint64_t textureBinds{0};
        std::uint64_t programBinds{0};
        std::uint64_t uniformUpdates{0};
        std::uint64_t uniformLookups{0};
        std::uint64_t bufferUploads{0};
        std::uint64_t bufferBytesUploaded{0};

        void reset() { *this = RenderStats(); }
    };

    class GraphicsContext
    {
    public:
        RenderStats renderStats;

        void clear();
        void clearColor();

        TextureHandle createTexture(const Grid<Color>& image);

        TextureHandle createTexture(unsigned int width, unsigned int height, const std::vector<Color>& image);

        TextureHandle createTexture(unsigned int width, unsigned int height, const Color* image);

        TextureHandle createEmptyTexture(unsigned int width, unsigned int height);

        TextureHandle createColorTexture(Color c);

        TextureArrayHandle createTextureArray(unsigned int width, unsigned int height, unsigned int mipMapLevels, std::vector<Color>& images);

        void enableDepthBuffer();

        void disableDepthBuffer();

        void enableDepthWrites();

        void disableDepthWrites();

        void enableDepthTest();

        void disableDepthTest();

        void enableCulling();


        ShaderHandle compileVertexShader(const std::string& source);

        ShaderHandle compileFragmentShader(const std::string& source);

        ShaderProgramHandle linkShaderProgram(ShaderIdentifier vertexShader, ShaderIdentifier fragmentShader, const std::vector<AttribMapping>& attribs);

        void enableColorBuffer();
        void disableColorBuffer();
        void enableStencilBuffer();
        void useStencilBufferForWrites();
        void useStencilBufferAsMask();
        void clearStencilBuffer();
        void disableStencilBuffer();

        GlMesh createTexturedMesh(const std::vector<GlTexturedVertex>& vertices, GLenum usage);

        GlMesh createTextureArrayMesh(const std::vector<GlTextureArrayVertex>& vertices, GLenum usage);

        GlMesh createColoredMesh(const std::vector<GlColoredVertex>& vertices, GLenum usage);

        GlMesh createTexturedNormalMesh(const std::vector<GlTexturedNormalVertex>& vertices, GLenum usage);

        GlMesh createColoredNormalMesh(const std::vector<GlColoredNormalVertex>& vertices, GLenum usage);

        void bindShader(ShaderProgramIdentifier shader);

        void unbindShader();

        void bindTexture(TextureIdentifier texture);

        void bindTextureArray(TextureArrayIdentifier texture);

        void unbindTexture();

        void unbindTextureArray();

        void bindFrameBuffer(FrameBufferIdentifier frameBuffer);

        void unbindFrameBuffer();

        void enableBlending();

        /**
         * The engine enables blending once at startup and relies on it
         * staying on for the whole frame -- sprite, UI and cursor draws
         * do not re-enable it. Disabling mid-frame leaks the state to
         * every subsequent draw; render passes must not call this.
         */
        void disableBlending();

        UniformLocation getUniformLocation(ShaderProgramIdentifier shader, const std::string& name);

        void setUniformInt(UniformLocation location, int value);
        void setUniformFloat(UniformLocation location, float value);
        void setUniformVec3(UniformLocation location, float a, float b, float c);
        void setUniformVec4(UniformLocation location, float a, float b, float c, float d);
        void setUniformMatrix(UniformLocation location, const Matrix4f& matrix);
        void setUniformBool(UniformLocation location, bool value);

        void drawTriangles(const GlMesh& mesh);
        void drawLines(const GlMesh& mesh);
        void drawLineLoop(const GlMesh& mesh);

        /**
         * Draws `mesh` once per instance record. Instance data layout is
         * fixed: 144-byte records = mat4 iModel (locations 3-6) +
         * mat4 iMvp (locations 8-11) + vec4 iParams (location 7). Used by
         * the instanced unit/shadow/build shaders; attribute state is
         * restored after the call so the mesh stays usable by the plain
         * draw paths.
         */
        void drawTrianglesInstanced(const GlMesh& mesh, const void* instanceData, std::size_t instanceCount);

        Sprite createSprite(const Rectangle2f& bounds, const Rectangle2f& textureRegion, const SharedTextureHandle& texture);

        GlMesh createUnitTexturedQuad(const Rectangle2f& textureRegion);

        GlMesh createUnitTexturedQuadFlipped(const Rectangle2f& textureRegion);

        void setViewport(int x, int y, int width, int height);

        /**
         * Enables scissor testing with the given rectangle.
         * Coordinates are in device pixels with the origin at the
         * bottom-left of the viewport, as required by glScissor.
         */
        void setScissor(int x, int y, int width, int height);

        void clearScissor();

        /** Returns the current GL viewport as x, y, width, height. */
        Viewport getDeviceViewport() const;

        FrameBufferInfo createFrameBuffer(int width, int height);

        void bindFrameBufferColorBuffer(TextureIdentifier texture);

        void setActiveTextureSlot0();
        void setActiveTextureSlot1();

        /**
         * Reads the currently bound framebuffer's RGBA pixels into `out`
         * (bottom-up GL ordering). Used by the screenshot harness.
         */
        void readPixelsRGBA(int x, int y, int width, int height, std::vector<unsigned char>& out);

    private:
        ShaderHandle compileShader(GLenum shaderType, const std::string& source);

        /** lazily-created streaming VBO for instanced unit draws */
        VboHandle instancedStreamVbo;
        std::size_t instancedStreamVboCapacity{0};

        VboHandle genBuffer();
        VaoHandle genVertexArray();
        void bindBuffer(GLenum type, VboIdentifier id);
        void unbindBuffer(GLenum type);
        void bindVertexArray(VaoIdentifier id);
        void unbindVertexArray();
        void drawMesh(
            GLenum mode,
            const GlMesh& mesh,
            const Matrix4f& mvpMatrix,
            ShaderProgramIdentifier shader);
        void drawUnitMesh(
            const GlMesh& mesh,
            const Matrix4f& modelMatrix,
            const Matrix4f& mvpMatrix,
            float seaLevel,
            ShaderProgramIdentifier shader);
    };
}
