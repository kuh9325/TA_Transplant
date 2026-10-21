#include "SceneManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <rwe/util/SimpleLogger.h>
#include <sstream>
#include <vector>

namespace rwe
{
    namespace
    {
        /**
         * Per-frame phase timings collected across a reporting window.
         * Enabled by RWE_PERF; reports p50/p95/p99/max for each phase
         * rather than bare averages so a single 500ms present stall is
         * visible as a max instead of dissolving into the mean.
         */
        struct SceneFrameProfiler
        {
            struct Sample
            {
                std::uint64_t eventsNs;
                std::uint64_t updateNs;
                std::uint64_t renderNs;
                std::uint64_t overlayNs;
                std::uint64_t presentNs;
                std::uint64_t totalNs;
            };

            std::vector<Sample> samples;
            std::uint64_t draws{0};
            std::uint64_t verts{0};
            std::uint64_t texBinds{0};
            std::uint64_t uniforms{0};
            std::uint64_t lookups{0};
            std::uint64_t bufferUploads{0};
            std::uint64_t bufferBytes{0};
            std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();

            static double ms(std::uint64_t ns)
            {
                return static_cast<double>(ns) / 1e6;
            }

            static void percentiles(const char* name, const std::vector<Sample>& s, std::uint64_t Sample::* field, std::ostringstream& out)
            {
                if (s.empty())
                {
                    return;
                }
                std::vector<std::uint64_t> v;
                v.reserve(s.size());
                for (const auto& e : s)
                {
                    v.push_back(e.*field);
                }
                std::sort(v.begin(), v.end());
                auto at = [&](double p) { return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]; };
                out << " " << name << "=[p50 " << ms(at(0.50)) << " p95 " << ms(at(0.95))
                    << " p99 " << ms(at(0.99)) << " max " << ms(v.back()) << "]";
            }

            void reportAndReset()
            {
                auto now = std::chrono::steady_clock::now();
                auto windowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - windowStart).count();
                if (windowNs < 5000000000LL || samples.empty())
                {
                    return;
                }

                std::ostringstream out;
                out << "RWE_PERF_FRAME n=" << samples.size()
                    << " fps=" << (samples.size() / (static_cast<double>(windowNs) / 1e9));
                percentiles("frame_ms", samples, &Sample::totalNs, out);
                percentiles("update_ms", samples, &Sample::updateNs, out);
                percentiles("render_ms", samples, &Sample::renderNs, out);
                percentiles("overlay_ms", samples, &Sample::overlayNs, out);
                percentiles("present_ms", samples, &Sample::presentNs, out);
                percentiles("events_ms", samples, &Sample::eventsNs, out);
                double n = static_cast<double>(samples.size());
                out << " drawCalls=" << static_cast<std::uint64_t>(draws / n)
                    << " verts=" << static_cast<std::uint64_t>(verts / n)
                    << " texBinds=" << static_cast<std::uint64_t>(texBinds / n)
                    << " uniforms=" << static_cast<std::uint64_t>(uniforms / n)
                    << " uLookups=" << static_cast<std::uint64_t>(lookups / n)
                    << " bufUploads=" << static_cast<std::uint64_t>(bufferUploads / n)
                    << " bufKB=" << static_cast<std::uint64_t>(bufferBytes / n / 1024);
                LOG_INFO << out.str();

                samples.clear();
                draws = verts = texBinds = uniforms = lookups = bufferUploads = bufferBytes = 0;
                windowStart = now;
            }

            void noteFrame(const GraphicsContext* graphics)
            {
                const auto& s = graphics->renderStats;
                draws += s.drawCalls;
                verts += s.verticesDrawn;
                texBinds += s.textureBinds;
                uniforms += s.uniformUpdates;
                lookups += s.uniformLookups;
                bufferUploads += s.bufferUploads;
                bufferBytes += s.bufferBytesUploaded;
            }

            /**
             * One line per stall frame (>50ms total): logs the exact
             * stage split so event-loop stalls are attributable
             * (events/update/render/overlay/present) without flooding
             * the log during sustained slow periods.
             */
            void noteStall(const Sample& s)
            {
                static constexpr std::uint64_t StallNs = 50000000;
                if (s.totalNs < StallNs)
                {
                    return;
                }
                LOG_INFO << "RWE_STALL frame=" << ms(s.totalNs)
                         << "ms [events " << ms(s.eventsNs)
                         << " update " << ms(s.updateNs)
                         << " render " << ms(s.renderNs)
                         << " overlay " << ms(s.overlayNs)
                         << " present " << ms(s.presentNs) << "]";
            }
        };
    }

    std::optional<MouseButtonEvent::MouseButton> convertSdlMouseButton(Uint8 button)
    {
        switch (button)
        {
            case SDL_BUTTON_LEFT:
                return MouseButtonEvent::MouseButton::Left;
            case SDL_BUTTON_MIDDLE:
                return MouseButtonEvent::MouseButton::Middle;
            case SDL_BUTTON_RIGHT:
                return MouseButtonEvent::MouseButton::Right;
            default:
                return std::nullopt;
        }
    }
    SceneManager::SceneManager(
        SdlContext* sdl,
        SDL_Window* window,
        GraphicsContext* graphics,
        TimeService* timeService,
        ImGuiContext* imGuiContext,
        CursorService* cursorService,
        GlobalConfig* globalConfig,
        UiRenderService&& uiRenderService,
        Viewport* viewport)
        : currentScene(),
          nextScene(),
          sdl(sdl),
          window(window),
          graphics(graphics),
          timeService(timeService),
          imGuiContext(imGuiContext),
          cursorService(cursorService),
          globalConfig(globalConfig),
          uiRenderService(std::move(uiRenderService)),
          viewport(viewport),
          requestedExit(false)
    {
    }

    void SceneManager::setNextScene(std::shared_ptr<Scene> scene)
    {
        nextScene = std::move(scene);
    }

    void dispatchToScene(const SDL_Event& event, Scene& currentScene)
    {
        switch (event.type)
        {
            case SDL_EVENT_KEY_DOWN:
                currentScene.onKeyDown(event.key);
                break;
            case SDL_EVENT_KEY_UP:
                currentScene.onKeyUp(event.key);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
                auto button = convertSdlMouseButton(event.button.button);
                if (!button)
                {
                    break;
                }

                MouseButtonEvent e(event.button.x, event.button.y, *button);
                currentScene.onMouseDown(e);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                auto button = convertSdlMouseButton(event.button.button);
                if (!button)
                {
                    break;
                }

                MouseButtonEvent e(event.button.x, event.button.y, *button);
                currentScene.onMouseUp(e);
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            {
                MouseMoveEvent e(event.motion.x, event.motion.y);
                currentScene.onMouseMove(e);
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                MouseWheelEvent e(event.wheel.x, event.wheel.y);
                currentScene.onMouseWheel(e);
                break;
            }

            default:
                // skip unrecognised events
                break;
        }
    }

    void SceneManager::execute()
    {
        const bool perfEnabled = std::getenv("RWE_PERF") != nullptr || std::getenv("RWE_SCENE_BENCH") != nullptr || std::getenv("RWE_SOAK") != nullptr;
        SceneFrameProfiler profiler;

        while (!requestedExit)
        {
            const auto frameClockStart = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (perfEnabled)
            {
                graphics->renderStats.reset();
            }

            if (nextScene)
            {
                currentScene = std::move(nextScene);
                currentScene->init();
            }

            auto startTime = timeService->getTicks();
            auto timeElapsed = lastFrameStartTime == 0 ? 0 : startTime - lastFrameStartTime;

            SDL_Event event;
            while (sdl->pollEvent(&event))
            {
                if (imGuiContext->processEvent(event))
                {
                    continue;
                }

                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11)
                {
                    showDebugWindow = true;
                    continue;
                }

                if (event.type == SDL_EVENT_QUIT)
                {
                    return;
                }

                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == sdl->getWindowId(window))
                {
                    viewport->setDimensions(event.window.data1, event.window.data2);
                    continue;
                }

                dispatchToScene(event, *currentScene);
            }
            const auto postEvents = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            if (imGuiContext->io->WantCaptureMouse)
            {
                imGuiContext->io->ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            }
            else
            {
                imGuiContext->io->ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            }
            imGuiContext->newFrame(window);
            currentScene->update(timeElapsed);
            const auto postUpdate = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (showDemoWindow)
            {
                ImGui::ShowDemoWindow(&showDemoWindow);
            }
            renderDebugWindow();
            imGuiContext->render();

            graphics->clear();
            currentScene->render();
            const auto postRender = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            // Cursor ownership transition. When ImGui wants the mouse
            // the native pointer must be visible over ImGui windows;
            // otherwise we hide it and render the game cursor ourselves.
            if (imGuiContext->io->WantCaptureMouse)
            {
                sdl->showCursor();
            }
            else
            {
                sdl->hideCursor();
            }

            // ImGui draws before the custom cursor so the game cursor
            // stays on top of scene/UI elements.
            imGuiContext->renderDrawData();

            if (!imGuiContext->io->WantCaptureMouse)
            {
                cursorService->render(uiRenderService);
            }
            const auto prePresent = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            sdl->glSwapWindow(window);
            const auto postPresent = perfEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            if (perfEnabled)
            {
                auto ns = [](auto a, auto b) {
                    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
                };
                SceneFrameProfiler::Sample frameSample{
                    ns(frameClockStart, postEvents),
                    ns(postEvents, postUpdate),
                    ns(postUpdate, postRender),
                    ns(postRender, prePresent),
                    ns(prePresent, postPresent),
                    ns(frameClockStart, postPresent)};
                profiler.noteStall(frameSample);
                profiler.samples.push_back(frameSample);
                profiler.noteFrame(graphics);
                profiler.reportAndReset();
            }

            auto finishTime = timeService->getTicks();
            auto lastFrameDurationMs = finishTime - startTime;
            lastFrameStartTime = startTime;
            frameTimes[frameTimesOffset] = lastFrameDurationMs;
            frameTimesOffset = (frameTimesOffset + 1) % 500;
        }
    }

    void SceneManager::requestExit()
    {
        requestedExit = true;
    }

    void SceneManager::renderDebugWindow()
    {
        if (!showDebugWindow)
        {
            return;
        }

        ImGui::Begin("Global Debug", &showDebugWindow);
        ImGui::Text("Last frame time: %.0fms", frameTimes[(frameTimesOffset + 499) % 500]);
        ImGui::PlotLines("Frame Times", frameTimes.data(), frameTimes.size(), frameTimesOffset);
        ImGui::Separator();
        ImGui::Checkbox("Left click interface mode", &globalConfig->leftClickInterfaceMode);
        ImGui::Separator();
        if (ImGui::Button("Grab Mouse"))
        {
            sdl->setWindowGrab(window, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Ungrab Mouse"))
        {
            sdl->setWindowGrab(window, false);
        }
        ImGui::Separator();
        if (ImGui::Button("Show Demo Window"))
        {
            showDemoWindow = true;
        }
        ImGui::End();
    }
}
