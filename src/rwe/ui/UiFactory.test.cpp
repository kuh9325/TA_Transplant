#include <GL/glew.h>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <vector>

#include <rwe/CampaignProgress.h>
#include <rwe/ColorPalette.h>
#include <rwe/EndMissionScene.h>
#include <rwe/GameParameters.h>
#include <rwe/PathMapping.h>
#include <rwe/ShaderService.h>
#include <rwe/TextureService.h>
#include <rwe/UiRenderService.h>
#include <rwe/Viewport.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/io/gui/gui.h>
#include <rwe/io/ota/ota.h>
#include <rwe/io/tdf/TdfBlock.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/render/GraphicsContext.h>
#include <rwe/save/SaveListDialog.h>
#include <rwe/sdl/SdlContextManager.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/ui/UiFactory.h>
#include <rwe/ui/UiLabel.h>
#include <rwe/ui/UiListBox.h>
#include <rwe/ui/UiScrollBar.h>
#include <rwe/ui/UiStagedButton.h>
#include <rwe/ui/UiTextBox.h>
#include <rwe/util/rwe_string.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace rwe
{
    namespace
    {
        /**
         * A hidden SDL window hosting a real OpenGL context at the
         * retail 640x480 UI resolution, so panels can be rendered and
         * their pixels read back for verification.
         */
        struct GlUiEnvironment
        {
            SdlContextManager sdlManager;
            std::unique_ptr<SDL_Window, SdlContext::WindowDeleter> window;
            SdlContext::GlContextUniquePtr glContext;
            GraphicsContext graphics;
            ShaderService shaders;
            Viewport viewport{0, 0, 640, 480};
            UiRenderService renderService{&graphics, &shaders, &viewport};

            GlUiEnvironment()
            {
                auto sdl = sdlManager.getSdlContext();
                sdl->glSetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                sdl->glSetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
                sdl->glSetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
                sdl->glSetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);

                window = sdl->createWindow("rwe_test", 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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

            std::vector<unsigned char> readRegion(int x, int y, int width, int height)
            {
                std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 3);
                glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                return pixels;
            }
        };

        std::unique_ptr<GlUiEnvironment> tryCreateGlUiEnvironment()
        {
            try
            {
                return std::make_unique<GlUiEnvironment>();
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }

        unsigned int countMatchingPixels(
            const std::vector<unsigned char>& pixels,
            unsigned char r,
            unsigned char g,
            unsigned char b,
            unsigned char tolerance)
        {
            unsigned int count = 0;
            for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
            {
                if (std::abs(static_cast<int>(pixels[i]) - r) <= tolerance
                    && std::abs(static_cast<int>(pixels[i + 1]) - g) <= tolerance
                    && std::abs(static_cast<int>(pixels[i + 2]) - b) <= tolerance)
                {
                    ++count;
                }
            }
            return count;
        }
    }

    TEST_CASE("the BRIEFING panel resolves stock button art and briefing text")
    {
        // Human-QA regression: the in-game campaign briefing dialog
        // (in-game menu -> MISSION -> BRIEFING.GUI) rendered its body
        // blank and its OK button as the magenta default sprite.
        // Renders the real panel from stock data and inspects pixels.
        // Skips without RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        auto panel = uiFactory.panelFromGuiFile("BRIEFING");
        REQUIRE(panel);

        // GameScene binds igmbrief.pcx as the dialog background
        panel->setBackground(textureService.getBitmapRegion("igmbrief", 0, 0, panel->getWidth(), panel->getHeight()));

        // populate TextRegion via the same data path as
        // GameScene::populateBriefingDialog for Metal Maze
        // (brief=T09Brief.txt, a stock OTA carrying its extension)
        auto otaRaw = vfs.readFile("maps/Metal Maze.ota");
        REQUIRE(otaRaw);
        auto ota = parseOta(parseTdfFromString(std::string(otaRaw->begin(), otaRaw->end())));
        REQUIRE(!ota.brief.empty());
        auto briefRaw = vfs.readFile(pathMapping.camps + "/briefs/" + briefFileName(ota.brief));
        REQUIRE(briefRaw);
        auto text = sanitizeBriefText(std::string(briefRaw->begin(), briefRaw->end()), true);
        REQUIRE(!text.empty());

        auto region = panel->find<UiLabel>("TextRegion");
        REQUIRE(region);
        auto font = textureService.getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");
        auto lines = wrapTextLines(text, region->get().getWidth(), [&](const std::string& s) {
            return env->renderService.getTextWidth(s, *font);
        });
        auto linesPerPage = std::max(static_cast<unsigned int>(region->get().getHeight() / 15.0f), 1u);
        auto pages = paginateLines(lines, linesPerPage);
        REQUIRE(!pages.empty());
        region->get().setAlignment(UiLabel::Alignment::LeftWrapped);
        region->get().setText(pages.front());

        env->graphics.enableBlending();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        panel->render(env->renderService);
        glFinish();

        // panel-local gadget rects translated by the panel's on-screen
        // position; glReadPixels measures from the bottom-left
        auto panelX = panel->getX();
        auto panelY = panel->getY();

        SECTION("the OK button does not render the magenta default sprite")
        {
            // BRIEFING.GUI: OK at (478,165) 95x20
            auto pixels = env->readRegion(panelX + 478, 480 - (panelY + 165 + 20), 95, 20);
            auto magenta = countMatchingPixels(pixels, 255, 0, 255, 32);
            REQUIRE(magenta == 0);
        }

        SECTION("the TextRegion renders visible briefing text")
        {
            // BRIEFING.GUI: TextRegion at (24,17) 360x148
            auto pixels = env->readRegion(panelX + 24, 480 - (panelY + 17 + 148), 360, 148);
            // body text is bright-on-dark; require a meaningful number
            // of bright glyph pixels inside the region
            auto bright = 0u;
            for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
            {
                if (pixels[i] + pixels[i + 1] + pixels[i + 2] > 384)
                {
                    ++bright;
                }
            }
            REQUIRE(bright > 500);
        }
    }

    TEST_CASE("the MSNBRIEF campaign briefing renders its background, button art and text")
    {
        // Companion coverage for the frontend mission briefing
        // (campaign -> mission -> MSNBRIEF.GUI), the screen shown
        // before Start launches the mission. Skips without
        // RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        auto guiRaw = vfs.readFile(pathMapping.guis + "/MSNBRIEF.GUI");
        REQUIRE(guiRaw);
        auto parsedGui = parseGuiFromBytes(*guiRaw);
        REQUIRE(parsedGui);

        // Metal Maze is CORE-side: the briefing background resolves
        // through the side's nameprefix as mbriefcor
        auto panel = uiFactory.panelFromGuiFile("MSNBRIEF", "mbriefcor", *parsedGui);
        REQUIRE(panel);

        auto region = panel->find<UiLabel>("TextRegion");
        REQUIRE(region);
        region->get().setAlignment(UiLabel::Alignment::LeftWrapped);
        region->get().setText("Metal Maze\n\nThe Arm vermin have just set up a Galactic Gate among the canals of Sector 4395.");

        env->graphics.enableBlending();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        panel->render(env->renderService);
        glFinish();

        auto panelX = panel->getX();
        auto panelY = panel->getY();

        SECTION("the Start button does not render the magenta default sprite")
        {
            // MSNBRIEF.GUI: Start at (499,432) 96x31
            auto pixels = env->readRegion(panelX + 499, 480 - (panelY + 432 + 31), 96, 31);
            auto magenta = countMatchingPixels(pixels, 255, 0, 255, 32);
            REQUIRE(magenta == 0);
        }

        SECTION("the briefing background bitmap is not magenta")
        {
            // interior of the panel, away from gadgets
            auto pixels = env->readRegion(200, 480 - (0 + 460), 240, 20);
            auto magenta = countMatchingPixels(pixels, 255, 0, 255, 32);
            REQUIRE(magenta == 0);
        }

        SECTION("the TextRegion renders visible briefing text")
        {
            // MSNBRIEF.GUI: TextRegion at (48,285) 360x148
            auto pixels = env->readRegion(panelX + 48, 480 - (panelY + 285 + 148), 360, 148);
            auto bright = 0u;
            for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
            {
                if (pixels[i] + pixels[i + 1] + pixels[i + 2] > 384)
                {
                    ++bright;
                }
            }
            REQUIRE(bright > 200);
        }
    }

    TEST_CASE("the ENDMSN debrief stamps the result banner at its authored position")
    {
        // Human-QA regression: the victory/defeat bitmap was drawn
        // through drawSprite with a manual offset, which lands the
        // GAF frame's 320/240 authored position mostly off-screen.
        // Renders the real EndMissionScene for a Metal Maze victory
        // and inspects pixels. Skips without RWE_DATA_PATH or GL.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        CampaignProgress campaignProgress;

        SceneContext sceneContext(
            nullptr,
            nullptr,
            &env->graphics,
            &textureService,
            nullptr,
            nullptr,
            &env->shaders,
            &vfs,
            &*palette,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &pathMapping,
            nullptr,
            &campaignProgress);

        EndMissionInfo info;
        info.won = true;
        info.campaign = CampaignMissionParameters{
            "Battle Tactics - Core Medium",
            "CORE",
            0,
            "Metal Maze.ota",
            "Metal Maze",
            "Medium"};
        info.gameTime = GameTime(36000);
        info.killMul = 1.0;
        info.timeMul = 1.0;
        info.stats = {
            DebriefPlayerRow{"PLAYER", 12, 3, 4000, 2000, 300, 150, 0},
        };

        // ENDMSN now owns the finished mission's simulation +
        // parameters so its SaveGame can serialize them
        GameParameters params("Metal Maze.ota", 0);
        params.campaign = info.campaign;
        auto sim = std::make_unique<GameSimulation>(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);

        EndMissionScene scene(sceneContext, &soundLookup, info, std::move(sim), std::move(params));
        scene.init();

        env->graphics.enableBlending();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        scene.render();
        glFinish();

        // ENDMSN.GAF victory: 129x29 authored at pos (320,240);
        // the glyphs are bright on the dark stats-table body.
        // A single section: the GL harness is created per leaf
        // section and rapid context cycling can crash the Apple
        // GL stack.
        {
            auto countBright = [&](unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int threshold) {
                auto pixels = env->readRegion(x, 480 - (y + h), w, h);
                auto bright = 0u;
                for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
                {
                    if (pixels[i] + pixels[i + 1] + pixels[i + 2] > threshold)
                    {
                        ++bright;
                    }
                }
                return bright;
            };

            // banner lands at its authored (320,240) position
            REQUIRE(countBright(320, 240, 129, 29, 200) > 100);

            // the game-time text renders in the free band
            // (drawText y is the baseline, so the glyphs sit above 312)
            REQUIRE(countBright(140, 300, 150, 14, 384) > 100);
        }
    }

    TEST_CASE("the ENDMSN debrief gates Start/LoadGame/SaveGame on continuation")
    {
        // Retail 0x41f400-0x41f532: victory with a next mission and
        // defeat enable the full control set (Start, LoadGame,
        // SaveGame, mission controls, MainMenu); a campaign-final
        // victory enables MainMenu only. Constructs the real scene
        // against stock campaign descriptors. Skips without
        // RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";

        auto campaigns = loadCampaignDescriptors(vfs, pathMapping.camps);
        const CampaignDescriptorFile* campaign = nullptr;
        for (const auto& c : campaigns)
        {
            if (c.descriptor.missions.size() >= 2)
            {
                campaign = &c;
                break;
            }
        }
        REQUIRE(campaign != nullptr);
        const auto& firstMission = campaign->descriptor.missions.front();
        const auto& lastMission = campaign->descriptor.missions.back();

        CampaignProgress campaignProgress;
        SceneContext sceneContext(
            nullptr,
            nullptr,
            &env->graphics,
            &textureService,
            nullptr,
            nullptr,
            &env->shaders,
            &vfs,
            &*palette,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &pathMapping,
            nullptr,
            &campaignProgress);

        auto makeScene = [&](unsigned int missionIndex, const std::string& missionFile, bool won) {
            EndMissionInfo info;
            info.won = won;
            info.campaign = CampaignMissionParameters{
                campaign->displayName,
                campaign->descriptor.campaignSide,
                missionIndex,
                missionFile,
                "Mission",
                "Medium"};
            info.gameTime = GameTime(60);
            info.killMul = 1.0;
            info.timeMul = 1.0;
            info.stats = {DebriefPlayerRow{"PLAYER", 0, 0, 0, 0, 0, 0, 0}};

            GameParameters params(missionFile, 0);
            params.campaign = info.campaign;
            auto sim = std::make_unique<GameSimulation>(MapTerrain(Grid<unsigned char>(65, 65, 64), 0_ss), 255, 0, 100);
            auto scene = std::make_unique<EndMissionScene>(
                sceneContext, &soundLookup, info, std::move(sim), std::move(params));
            scene->init();
            return scene;
        };

        auto buttonEnabled = [](EndMissionScene& scene, const char* name) {
            auto b = scene.getPanelForTest()->find<UiStagedButton>(name);
            REQUIRE(b);
            return b->get().isEnabled();
        };

        SECTION("victory with a next mission enables Start/LoadGame/SaveGame")
        {
            auto scene = makeScene(firstMission.index, firstMission.missionFile, true);
            REQUIRE(buttonEnabled(*scene, "Start"));
            REQUIRE(buttonEnabled(*scene, "LoadGame"));
            REQUIRE(buttonEnabled(*scene, "SaveGame"));
            REQUIRE(buttonEnabled(*scene, "MainMenu"));
        }

        SECTION("defeat enables Start/LoadGame/SaveGame")
        {
            auto scene = makeScene(firstMission.index, firstMission.missionFile, false);
            REQUIRE(buttonEnabled(*scene, "Start"));
            REQUIRE(buttonEnabled(*scene, "LoadGame"));
            REQUIRE(buttonEnabled(*scene, "SaveGame"));
            REQUIRE(buttonEnabled(*scene, "MainMenu"));
        }

        SECTION("campaign-final victory exposes MainMenu only")
        {
            auto scene = makeScene(lastMission.index, lastMission.missionFile, true);
            REQUIRE(!buttonEnabled(*scene, "Start"));
            REQUIRE(!buttonEnabled(*scene, "LoadGame"));
            REQUIRE(!buttonEnabled(*scene, "SaveGame"));
            REQUIRE(!buttonEnabled(*scene, "Difficulty"));
            REQUIRE(buttonEnabled(*scene, "MainMenu"));
        }
    }

    TEST_CASE("the LOADGAME panel binds the stock dialog art and preserves authored geometry")
    {
        // Human-QA regression: the frontend LoadGame screen bound
        // bitmaps/Loadgame2bg.pcx -- the mission-loading splash -- as
        // its panel background, so the "LOADING..." title and the
        // splash's slot-box column rendered under the dialog widgets.
        // The screen's authored art is the DLoadgame2/DSavegame2
        // bitmap pair (panel-space crop of the 640x480 PCX). Verifies
        // the bound texture, every authored gadget rectangle, listbox
        // population and rendered pixels. Skips without RWE_DATA_PATH
        // or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        auto countBright = [&](unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int threshold) {
            auto pixels = env->readRegion(x, 480 - (y + h), w, h);
            auto bright = 0u;
            for (std::size_t i = 0; i + 2 < pixels.size(); i += 3)
            {
                if (pixels[i] + pixels[i + 1] + pixels[i + 2] > threshold)
                {
                    ++bright;
                }
            }
            return bright;
        };

        std::vector<SavedGameMetadata> entries(3);
        for (unsigned int i = 0; i < entries.size(); ++i)
        {
            entries[i].saveName = "QA_SAVE_" + std::to_string(i);
            entries[i].isCampaign = true;
            entries[i].missionName = "Mission " + std::to_string(i);
            entries[i].localSide = "ARM";
            entries[i].difficulty = "Medium";
            entries[i].gameTime = GameTime(1800 * (i + 1));
        }

        SECTION("load mode binds DLoadgame2 and lays out the authored gadgets")
        {
            auto panel = createFrontendLoadGamePanel(uiFactory, &textureService, &vfs, false, &entries);
            REQUIRE(panel);

            // HEADER gadget0: screen position (81,27), size 494x420
            REQUIRE(panel->getX() == 81);
            REQUIRE(panel->getY() == 27);
            REQUIRE(panel->getWidth() == 494);
            REQUIRE(panel->getHeight() == 420);

            // the bound background is the dialog bitmap, cropped
            // panel-space (0,0,494,420) from the 640x480 PCX
            auto expected = textureService.getBitmapRegion("DLoadgame2", 0, 0, 494, 420);
            REQUIRE(panel->getBackground().has_value());
            REQUIRE((*panel->getBackground())->texture.get() == expected->texture.get());

            // authored child rectangles are preserved verbatim
            // (panel-relative coordinates)
            auto requireRect = [&](const char* name, int x, int y, unsigned int w, unsigned int h) {
                auto c = panel->find<UiComponent>(name);
                REQUIRE(c);
                REQUIRE(c->get().getX() == x);
                REQUIRE(c->get().getY() == y);
                REQUIRE(c->get().getWidth() == w);
                REQUIRE(c->get().getHeight() == h);
            };
            requireRect("GAMES", 65, 71, 242, 176);
            requireRect("SLIDER", 317, 63, 16, 184);
            requireRect("LOAD", 353, 324, 96, 20);
            requireRect("CANCEL", 353, 359, 96, 20);
            requireRect("DELETE", 367, 207, 96, 20);
            requireRect("GAMENAME", 111, 275, 230, 20);
            requireRect("GAMETYPE", 162, 315, 147, 15);
            requireRect("SIDE", 162, 330, 157, 18);
            requireRect("MISSION", 162, 346, 157, 18);
            requireRect("DIFF", 162, 362, 155, 16);
            requireRect("TIME", 162, 378, 155, 15);
            requireRect("RADAR", 348, 71, 121, 113);
            requireRect("SaveGame", 48, 17, 56, 12);

            // gadget types
            REQUIRE(panel->find<UiListBox>("GAMES"));
            REQUIRE(panel->find<UiScrollBar>("SLIDER"));
            REQUIRE(panel->find<UiTextBox>("GAMENAME"));

            // load mode: the name field is read-only
            REQUIRE(!panel->find<UiTextBox>("GAMENAME")->get().isEnabled());

            // populate with several entries -- a collapsed list must
            // not pass silently
            populateSaveList(*panel, entries);
            auto games = panel->find<UiListBox>("GAMES");
            REQUIRE(games->get().getItems().size() == entries.size());

            // selecting an entry syncs the read-only name field and
            // the metadata labels
            games->get().setSelectedIndex(1);
            REQUIRE(panel->find<UiTextBox>("GAMENAME")->get().getText() == "QA_SAVE_1");
            REQUIRE(panel->find<UiLabel>("MISSION")->get().getText() == "Mission 1");
            REQUIRE(panel->find<UiLabel>("SIDE")->get().getText() == "ARM");

            env->graphics.enableBlending();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            panel->render(env->renderService);
            glFinish();

            // panel-space measurements (+81,+27 for screen space):
            // the baked "LOAD GAME" title occupies (65,28)-(165,53)
            REQUIRE(countBright(81 + 65, 27 + 28, 100, 25, 300) > 200);

            // the loading splash has a bright slot-box column inside
            // what is the dark list interior in the real dialog art;
            // it must stay dark (list rows render thin text only)
            REQUIRE(countBright(81 + 100, 27 + 120, 180, 110, 300) < 150);

            // the baked button wells sit right of the info captions
            REQUIRE(countBright(81 + 340, 27 + 315, 115, 70, 300) > 200);

            // a populated row renders bright text inside the list rect
            games->get().clearItems();
            for (const auto& e : entries)
            {
                games->get().appendItem(e.saveName);
            }
            glClear(GL_COLOR_BUFFER_BIT);
            panel->render(env->renderService);
            glFinish();
            REQUIRE(countBright(81 + 70, 27 + 76, 160, 12, 300) > 40);
        }

        SECTION("save mode binds DSavegame2 and enables the name field")
        {
            auto panel = createFrontendLoadGamePanel(uiFactory, &textureService, &vfs, true, &entries);
            REQUIRE(panel);

            auto expected = textureService.getBitmapRegion("DSavegame2", 0, 0, 494, 420);
            REQUIRE(panel->getBackground().has_value());
            REQUIRE((*panel->getBackground())->texture.get() == expected->texture.get());

            auto box = panel->find<UiTextBox>("GAMENAME");
            REQUIRE(box);
            REQUIRE(box->get().isEnabled());

            env->graphics.enableBlending();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            panel->render(env->renderService);
            glFinish();

            // the "SAVE GAME" title occupies the same authored area
            REQUIRE(countBright(81 + 65, 27 + 28, 100, 25, 300) > 200);
        }

        SECTION("the in-game load dialog keeps the field read-only")
        {
            auto panel = createInGameSaveListPanel(uiFactory, &textureService, &vfs, false, &entries);
            REQUIRE(panel);
            auto box = panel->find<UiTextBox>("GAMENAME");
            REQUIRE(box);
            REQUIRE(!box->get().isEnabled());

            auto savePanel = createInGameSaveListPanel(uiFactory, &textureService, &vfs, true, &entries);
            REQUIRE(savePanel);
            REQUIRE(savePanel->find<UiTextBox>("GAMENAME")->get().isEnabled());
        }
    }

    TEST_CASE("the save dialog survives an activation that retires it mid-broadcast")
    {
        // Human-QA regression: pressing Enter in the in-game save
        // dialog crashed with EXC_BAD_ACCESS inside UiPanel::keyDown.
        // GameScene maps Enter to the crdefault control (SAVELIST's
        // LOAD), whose activation closed the menu and destroyed the
        // panel while keyDown was still iterating its children; the
        // stock O/C/D gadget quickkeys crash the same way. This test
        // builds the real SAVELIST panel from stock data and replays
        // the dispatch at panel level: the owner retires the panel on
        // activation, as the scene dispatch scopes now do.
        // Skips without RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        std::vector<SavedGameMetadata> entries;

        auto panelOwner = createInGameSaveListPanel(uiFactory, &textureService, &vfs, true, &entries);
        REQUIRE(panelOwner);
        UiPanel* panel = panelOwner.get();
        std::unique_ptr<UiPanel> retired;

        unsigned int loadActivations = 0;
        unsigned int cancelActivations = 0;
        panel->groupMessages().subscribe([&](const GroupMessage& msg) {
            if (!std::holds_alternative<ActivateMessage>(msg.message))
            {
                return;
            }
            if (msg.controlName == "LOAD")
            {
                ++loadActivations;
            }
            else if (msg.controlName == "CANCEL")
            {
                ++cancelActivations;
            }
            // the scene pops the activated dialog; the retired hold
            // keeps it alive until the broadcast unwinds
            if (panelOwner)
            {
                retired = std::move(panelOwner);
            }
        });

        SECTION("the crdefault Enter path activates LOAD exactly once")
        {
            // save mode focuses GAMENAME; 't' and '7' are not SAVELIST
            // quickkey letters (O/C/D), so they reach the field
            panel->setFocusByName("GAMENAME");
            panel->keyDown(KeyEvent(SDLK_T));
            panel->keyDown(KeyEvent(SDLK_7));

            // GameScene resolves Enter to the crdefault gadget, then
            // broadcasts SPACE to activate it
            REQUIRE(panel->getCrDefault() == "LOAD");
            panel->setFocusByName(panel->getCrDefault());
            panel->keyDown(KeyEvent(SDLK_SPACE));

            REQUIRE(loadActivations == 1);
            REQUIRE(cancelActivations == 0);
            // the panel lived through its own broadcast
            REQUIRE(retired.get() == panel);
            retired.reset();
        }

        SECTION("a quickkey letter fired mid-broadcast activates CANCEL once")
        {
            // CANCEL's 'c' quickkey only fires while the text gadget
            // does NOT own keyboard focus (retail 0x4a70a7); clear the
            // authored GAMENAME focus so the letter reaches the button
            panel->clearFocus();
            panel->keyDown(KeyEvent(SDLK_C));

            REQUIRE(cancelActivations == 1);
            REQUIRE(retired.get() == panel);
            // the retired panel is still coherent after the broadcast
            REQUIRE(retired->find<UiTextBox>("GAMENAME"));
            retired.reset();
        }
    }

    TEST_CASE("the save dialog routes focused TextBox input before quickkeys")
    {
        // Human-QA regression: typing 'c' into GAMENAME fired CANCEL's
        // quickkey and closed the dialog. Retail suppresses sibling
        // quickkeys while a TextBox owns keyboard focus -- the focused
        // gadget (type 3) consumes characters, Enter, and Escape
        // (TotalA.exe 0x4a7290 vs 0x4a70a7/0x4a9c80). This test drives
        // the real SAVELIST.GUI panel from stock data.
        // Skips without RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        std::vector<SavedGameMetadata> entries;
        auto panel = createInGameSaveListPanel(uiFactory, &textureService, &vfs, true, &entries);
        REQUIRE(panel);

        unsigned int loadActivations = 0;
        unsigned int cancelActivations = 0;
        unsigned int deleteActivations = 0;
        panel->groupMessages().subscribe([&](const GroupMessage& msg) {
            if (!std::holds_alternative<ActivateMessage>(msg.message))
            {
                return;
            }
            if (msg.controlName == "LOAD")
            {
                ++loadActivations;
            }
            else if (msg.controlName == "CANCEL")
            {
                ++cancelActivations;
            }
            else if (msg.controlName == "DELETE")
            {
                ++deleteActivations;
            }
        });

        auto& gamename = panel->find<UiTextBox>("GAMENAME")->get();

        SECTION("typing c/o/d quickkey letters enters text and keeps the dialog open")
        {
            panel->setFocusByName("GAMENAME");
            gamename.setText("");
            REQUIRE(gamename.isFocused());

            // every SAVELIST quickkey letter: O=LOAD, C=CANCEL, D=DELETE
            panel->keyDown(KeyEvent(SDLK_C));
            panel->keyDown(KeyEvent(SDLK_7));
            panel->keyDown(KeyEvent(SDLK_Q));
            panel->keyDown(KeyEvent(SDLK_A));
            REQUIRE(gamename.getText() == "c7qa");
            REQUIRE(cancelActivations == 0);
            REQUIRE(loadActivations == 0);
            REQUIRE(deleteActivations == 0);

            gamename.setText("");
            for (int kc : {SDLK_C, SDLK_O, SDLK_D})
            {
                panel->keyDown(KeyEvent(kc));
            }
            REQUIRE(gamename.getText() == "cod");
            REQUIRE(cancelActivations == 0);
            REQUIRE(loadActivations == 0);
            REQUIRE(deleteActivations == 0);
        }

        SECTION("Enter drops TextBox focus without confirming; crdefault fires once unfocused")
        {
            panel->setFocusByName("GAMENAME");
            gamename.setText("c7qa");

            // retail preprocessor (0x4a9c80): Enter while a TextBox owns
            // focus does not resolve crdefault; the gadget consumes the
            // key and drops focus, so the dialog stays open
            REQUIRE(panel->keyDown(KeyEvent(SDLK_RETURN)));
            REQUIRE(loadActivations == 0);
            REQUIRE(cancelActivations == 0);
            REQUIRE(gamename.getText() == "c7qa");

            // focus released -- Enter now resolves crdefault=LOAD once
            REQUIRE_FALSE(panel->keyDown(KeyEvent(SDLK_RETURN)));
            panel->setFocusByName(panel->getCrDefault());
            panel->keyDown(KeyEvent(SDLK_SPACE));
            REQUIRE(loadActivations == 1);
            REQUIRE(cancelActivations == 0);
        }

        SECTION("Escape drops TextBox focus without cancelling")
        {
            panel->setFocusByName("GAMENAME");
            gamename.setText("c7qa");

            // SAVELIST's escdefault is empty: retail consumes Escape in
            // the focused TextBox (defocus only); no gadget sees it
            REQUIRE(panel->keyDown(KeyEvent(SDLK_ESCAPE)));
            REQUIRE_FALSE(gamename.isFocused());
            REQUIRE(cancelActivations == 0);
            REQUIRE(loadActivations == 0);

            // with focus released, 'c' reaches CANCEL's quickkey
            REQUIRE(panel->keyDown(KeyEvent(SDLK_C)));
            REQUIRE(cancelActivations == 1);
        }

        SECTION("quickkeys fire normally when the TextBox is not focused")
        {
            panel->clearFocus();
            REQUIRE(panel->keyDown(KeyEvent(SDLK_O)));
            REQUIRE(loadActivations == 1);
            REQUIRE(cancelActivations == 0);

            panel->clearFocus();
            REQUIRE(panel->keyDown(KeyEvent(SDLK_D)));
            REQUIRE(deleteActivations == 1);
        }

        SECTION("a real click into GAMENAME still suppresses sibling quickkeys")
        {
            // Human-QA path: F2 -> Save Game -> CLICK inside the
            // GAMENAME field -> press 'c'. The click travels
            // UiPanel::mouseDown -> setFocus(i) -> TextBox::mouseDown,
            // so the focused textbox must still consume the letter.
            gamename.setText("");

            // SAVELIST.GUI: dialog at (81,27), GAMENAME at
            // (111,275,230x20) in dialog space; panel mouseDown takes
            // parent coordinates, so click the gadget's center.
            panel->mouseDown(MouseButtonEvent(81 + 111 + 115, 27 + 275 + 10, MouseButtonEvent::MouseButton::Left));
            panel->mouseUp(MouseButtonEvent(81 + 111 + 115, 27 + 275 + 10, MouseButtonEvent::MouseButton::Left));

            REQUIRE(gamename.isFocused());

            panel->keyDown(KeyEvent(SDLK_C));
            REQUIRE(gamename.getText() == "c");
            REQUIRE(cancelActivations == 0);
            REQUIRE(loadActivations == 0);
            REQUIRE(deleteActivations == 0);
        }
    }

    TEST_CASE("the in-game Save Game stack keeps TextBox quickkey suppression through the real scene dispatch topology")
    {
        // Human-QA regression: F2 -> Save Game -> click inside
        // GAMENAME -> 'c' closed the dialog. The previous fix and its
        // panel-level test proved the contract for a bare UiPanel, but
        // the live path wraps the SAVELIST panel in GameScene's dialog
        // stack: ARMOPT beneath it, a pointer-target click, then the
        // dual topDialog/keyTarget dispatch in onKeyDown. This test
        // replays that exact topology against real stock panels.
        // Skips without RWE_DATA_PATH or an OpenGL context.
        auto env = tryCreateGlUiEnvironment();
        if (!env)
        {
            SKIP("no OpenGL context available on this host");
        }
        const char* dataPathEnv = std::getenv("RWE_DATA_PATH");
        if (dataPathEnv == nullptr || !std::filesystem::exists(std::filesystem::path(dataPathEnv)))
        {
            SKIP("no Total Annihilation data path configured");
        }

        auto vfs = constructVfs(std::filesystem::path(dataPathEnv));
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        REQUIRE(paletteBytes);
        auto palette = readPalette(*paletteBytes);
        REQUIRE(palette);

        TextureService textureService(&env->graphics, &vfs, &*palette);
        TdfBlock soundLookup;
        PathMapping pathMapping;
        pathMapping.guis = "guis";
        pathMapping.camps = "camps";
        pathMapping.anims = "anims";
        pathMapping.bitmaps = "bitmaps";
        UiFactory uiFactory(&textureService, nullptr, &soundLookup, &vfs, &pathMapping, 640, 480);

        std::vector<SavedGameMetadata> entries;

        // GameScene dialog stack: ARMOPT opened by F2, SAVELIST pushed
        // on top as a non-companion dialog, GAMENAME focused at open
        auto options = uiFactory.panelFromGuiFile("ARMOPT");
        REQUIRE(options);
        auto saveOwner = createInGameSaveListPanel(uiFactory, &textureService, &vfs, true, &entries);
        REQUIRE(saveOwner);
        saveOwner->setFocusByName("GAMENAME");
        UiPanel* savePanel = saveOwner.get();

        std::vector<UiPanel*> dialogs{options.get(), savePanel};
        std::vector<bool> companions{false, false};

        // the scene's Save Game plumbing retires the panel when a
        // control activates; capture activations instead
        std::string lastActivation;
        savePanel->groupMessages().subscribe([&](const GroupMessage& msg) {
            if (std::holds_alternative<ActivateMessage>(msg.message))
            {
                lastActivation = msg.topic + "/" + msg.controlName;
            }
        });

        // inGameMenuPointerTarget: topmost dialog containing the point
        auto pointerTarget = [&](int x, int y) -> UiPanel* {
            for (auto i = dialogs.size(); i-- > 0;)
            {
                if (dialogs[i]->contains(x, y))
                {
                    return dialogs[i];
                }
                if (!companions[i])
                {
                    return nullptr;
                }
            }
            return nullptr;
        };
        // inGameMenuKeyTarget: topmost non-companion
        auto keyTarget = [&]() -> UiPanel* {
            for (auto i = dialogs.size(); i-- > 0;)
            {
                if (!companions[i])
                {
                    return dialogs[i];
                }
            }
            return nullptr;
        };

        // CLICK inside GAMENAME: scene converts coords then calls
        // mouseDown on the pointer target; SAVELIST's GAMENAME sits at
        // (81+111+115, 27+275+10) in menu space
        auto* clickTarget = pointerTarget(81 + 111 + 115, 27 + 275 + 10);
        REQUIRE(clickTarget == savePanel);
        clickTarget->mouseDown(MouseButtonEvent(81 + 111 + 115, 27 + 275 + 10, MouseButtonEvent::MouseButton::Left));
        clickTarget->mouseUp(MouseButtonEvent(81 + 111 + 115, 27 + 275 + 10, MouseButtonEvent::MouseButton::Left));

        auto& gamename = savePanel->find<UiTextBox>("GAMENAME")->get();
        gamename.setText("");
        REQUIRE(gamename.isFocused());

        // GameScene::onKeyDown for a non-Enter/Esc key: topDialog then
        // keyTarget when the two differ
        auto dispatchKey = [&](int key) {
            UiPanel* topDialog = dialogs.back();
            UiPanel* target = keyTarget();
            topDialog->keyDown(KeyEvent(key));
            if (target != topDialog)
            {
                target->keyDown(KeyEvent(key));
            }
        };

        dispatchKey(SDLK_C);
        REQUIRE(gamename.getText() == "c");
        REQUIRE(lastActivation.empty());

        dispatchKey(SDLK_7);
        dispatchKey(SDLK_Q);
        dispatchKey(SDLK_A);
        REQUIRE(gamename.getText() == "c7qa");
        REQUIRE(lastActivation.empty());
    }
}
