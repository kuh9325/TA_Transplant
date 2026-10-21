#include <GL/glew.h>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <rwe/RenderService.h>
#include <rwe/ShaderService.h>
#include <rwe/math/Matrix4f.h>
#include <rwe/math/Vector3f.h>
#include <rwe/render/GraphicsContext.h>
#include <rwe/sdl/SdlContextManager.h>

namespace rwe
{
    namespace
    {
        /**
         * A hidden SDL window hosting a real OpenGL context, plus the
         * services needed to run RenderService draw calls. Members are
         * ordered so the GL context is destroyed before its window and
         * SDL is torn down last.
         */
        struct GlEnvironment
        {
            SdlContextManager sdlManager;
            std::unique_ptr<SDL_Window, SdlContext::WindowDeleter> window;
            SdlContext::GlContextUniquePtr glContext;
            GraphicsContext graphics;
            Matrix4f viewProjection{Matrix4f::identity()};
            ShaderService shaders;
            RenderService renderService{&graphics, &shaders, &viewProjection};

            GlEnvironment()
            {
                auto sdl = sdlManager.getSdlContext();
                sdl->glSetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                sdl->glSetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
                sdl->glSetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
                sdl->glSetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

                window = sdl->createWindow("rwe_test", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
                if (!window)
                {
                    throw std::runtime_error(SDL_GetError());
                }

                glContext = sdl->glCreateContext(window.get());
                if (!glContext)
                {
                    throw std::runtime_error(SDL_GetError());
                }

                glewExperimental = GL_TRUE;
                if (glewInit() != GLEW_OK)
                {
                    throw std::runtime_error("glewInit failed");
                }
                // glew emits a benign GL_INVALID_ENUM under core contexts
                glGetError();

                shaders = ShaderService::createShaderService(graphics);
            }
        };

        std::unique_ptr<GlEnvironment> tryCreateGlEnvironment()
        {
            try
            {
                return std::make_unique<GlEnvironment>();
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }
    }

    TEST_CASE("render passes preserve the permanently-enabled blending state")
    {
        // The engine enables GL_BLEND once at startup and sprite, UI and
        // cursor draws rely on it staying on for the whole frame. A
        // translucent ColoredMeshBatch (e.g. the fog-of-war overlay) must
        // not leak a disabled blend state to subsequent draws.
        auto env = tryCreateGlEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }

        env->graphics.enableBlending();

        ColoredMeshBatch translucentBatch;
        translucentBatch.alpha = 0.55f;
        translucentBatch.triangles = {
            GlColoredVertex{Vector3f(0.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 0.0f)},
            GlColoredVertex{Vector3f(1.0f, 0.0f, 0.0f), Vector3f(0.0f, 0.0f, 0.0f)},
            GlColoredVertex{Vector3f(0.0f, 1.0f, 0.0f), Vector3f(0.0f, 0.0f, 0.0f)},
        };

        env->renderService.drawBatch(translucentBatch, env->viewProjection);

        REQUIRE(glIsEnabled(GL_BLEND) == GL_TRUE);

        ColoredMeshBatch opaqueBatch;
        opaqueBatch.triangles = translucentBatch.triangles;
        env->renderService.drawBatch(opaqueBatch, env->viewProjection);

        REQUIRE(glIsEnabled(GL_BLEND) == GL_TRUE);
    }
}
