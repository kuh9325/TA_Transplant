#include <GL/glew.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <rwe/AudioService.h>
#include <rwe/ColorPalette.h>
#include <rwe/GlobalConfig.h>
#include <rwe/LoadingScene.h>
#include <rwe/MainMenuScene.h>
#include <rwe/PathMapping.h>
#include <rwe/SceneContext.h>
#include <rwe/ShaderService.h>
#include <rwe/Viewport.h>
#include <rwe/config.h>
#include <rwe/game/PlayerColorIndex.h>
#include <rwe/io/gui/gui.h>
#include <rwe/io/tdf/tdf.h>
#include <rwe/ip_util.h>
#include <rwe/render/GraphicsContext.h>
#include <rwe/render/OpenGlVersion.h>
#include <rwe/rwe_time.h>
#include <rwe/scene/SceneManager.h>
#include <rwe/save/SavedGame.h>
#include <rwe/sdl/SdlContext.h>
#include <rwe/sdl/SdlContextManager.h>
#include <rwe/sim/Energy.h>
#include <rwe/sim/Metal.h>
#include <rwe/ui/UiFactory.h>
#include <rwe/util.h>
#include <sstream>
#include <rwe/util/ConfigFile.h>
#include <rwe/util/OpaqueArgs.h>
#include <rwe/util/Result.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/vfs/CompositeVirtualFileSystem.h>

namespace fs = std::filesystem;

namespace rwe
{
    OpenGlVersion getOpenGlContextVersion()
    {
        int major;
        int minor;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        return OpenGlVersion(major, minor);
    }

    void doGlewInit()
    {
        // Must set glewExperimental to true, otherwise in glew <= 1.13.0 glewInit() will fail
        // and set glError to GL_INVALID_ENUM if called in an OpenGL core context.
        // GL_INVALID_ENUM may *still* be emitted anyway, but isn't critical.
        // Ubuntu 16.04 and earlier uses glew 1.13.0, Ubuntu 18.04 uses glew 2.0.0.
        // See: https://www.khronos.org/opengl/wiki/OpenGL_Loading_Library
        glewExperimental = GL_TRUE;
        if (auto result = glewInit(); result != GLEW_OK)
        {
            throw std::runtime_error(reinterpret_cast<const char*>(glewGetErrorString(result)));
        }

        if (auto error = glGetError(); error == GL_NO_ERROR)
        {
            // all good, continue on
        }
        else if (error == GL_INVALID_ENUM)
        {
            // Ignore, expected from glew even with glewExperimental = true.
            // It should have still worked anyway.

            // Check if there are any more errors.
            if (auto nextError = glGetError(); nextError != GL_NO_ERROR)
            {
                throw OpenGlException(error);
            }
        }
        else
        {
            throw OpenGlException(error);
        }
    }

    Result<SdlContext::GlContextUniquePtr, const char*> createOpenGlContext(SdlContext* sdlContext, SDL_Window* window, const OpenGlVersionInfo& requiredVersion)
    {
        LOG_INFO << "Requesting OpenGL version "
                 << requiredVersion.version.majorVersion << "."
                 << requiredVersion.version.minorVersion << ", "
                 << getOpenGlProfileName(requiredVersion.profile) << " profile";

        if (!sdlContext->glSetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, requiredVersion.version.majorVersion))
        {
            return Err(SDL_GetError());
        }
        if (!sdlContext->glSetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, requiredVersion.version.minorVersion))
        {
            return Err(SDL_GetError());
        }
        if (!sdlContext->glSetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, getSdlProfileMask(requiredVersion.profile)))
        {
            return Err(SDL_GetError());
        }
        if (!sdlContext->glSetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG))
        {
            return Err(SDL_GetError());
        }

        auto glContext = sdlContext->glCreateContext(window);
        if (glContext == nullptr)
        {
            return Err(SDL_GetError());
        }

        auto contextVersion = getOpenGlContextVersion();
        if (contextVersion < requiredVersion.version)
        {
            static const char* errMessage = "Created OpenGL context did not meet version requirements";
            return Err(errMessage);
        }

        return Ok(std::move(glContext));
    };

#ifdef __APPLE__
    struct FolderPickResult
    {
        std::optional<fs::path> path;
    };

    void SDLCALL folderPickCallback(void* userdata, const char* const* filelist, int /*filter*/)
    {
        auto* result = static_cast<FolderPickResult*>(userdata);
        if (filelist != nullptr && filelist[0] != nullptr)
        {
            result->path = fs::path(filelist[0]);
        }
    }

    /**
     * Modal NSOpenPanel via SDL's Cocoa dialog backend: with no parent
     * window the backend runs the panel with -[NSOpenPanel runModal]
     * and invokes the callback before returning, so this is
     * synchronous on macOS once SDL's video subsystem has registered
     * the app (SdlContext). Loops until a plausible TA data folder is
     * chosen or the user cancels.
     */
    std::optional<fs::path> promptForGameDataFolder()
    {
        for (;;)
        {
            FolderPickResult result;
            SDL_ShowOpenFolderDialog(&folderPickCallback, &result, nullptr, nullptr, false);
            if (!result.path)
            {
                return std::nullopt;
            }
            if (isPlausibleGameDataRoot(*result.path))
            {
                return result.path;
            }
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR,
                "TA Transplant",
                "The selected folder does not look like a Total Annihilation data folder.\n"
                "Choose the folder containing the original game archives (totala1.hpi, rev31.gp3, ...).",
                nullptr);
        }
    }
#endif

    /**
     * Game-data root precedence:
     *   1. --data-path arguments / data-path= config lines (a previous
     *      Finder-picker selection lands in the config layer)
     *   2. <localData>/Data
     *   3. macOS only: a native folder-selection panel whose result is
     *      persisted back into the config file
     * Supplied directories are used verbatim when at least one is a
     * plausible TA data root; missing directories are dropped with a
     * warning (the VFS would throw on them anyway). Returns nullopt
     * when the user dismissed the picker without a usable data root.
     */
    std::optional<std::vector<fs::path>> resolveGameDataPaths(
        const std::vector<std::string>& supplied,
        const fs::path& localDataPath,
        const fs::path& configFilePath)
    {
        std::vector<fs::path> roots;
        bool anyPlausible = false;
        for (const auto& s : supplied)
        {
            std::error_code ec;
            fs::path p(s);
            if (!fs::is_directory(p, ec) || ec)
            {
                LOG_WARN << "Ignoring missing data path: " << s;
                continue;
            }
            anyPlausible = anyPlausible || isPlausibleGameDataRoot(p);
            roots.push_back(p);
        }
        if (anyPlausible)
        {
            return roots;
        }
        roots.clear();
        if (!supplied.empty())
        {
            LOG_WARN << "None of the supplied data paths is a plausible TA data root; continuing down the fallback chain";
        }

        fs::path defaultDataPath = localDataPath / "Data";
        if (isPlausibleGameDataRoot(defaultDataPath))
        {
            return std::vector<fs::path>{defaultDataPath};
        }

#ifdef __APPLE__
        if (auto picked = promptForGameDataFolder())
        {
            try
            {
                writeConfigValue(configFilePath, "data-path", picked->string());
            }
            catch (const std::exception& e)
            {
                LOG_WARN << "Could not persist the selected data path: " << e.what();
            }
            return std::vector<fs::path>{*picked};
        }

        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "TA Transplant",
            "No Total Annihilation data folder was selected. "
            "The original game's data files are required.",
            nullptr);
        return std::nullopt;
#else
        // non-Apple platforms keep the historical behavior verbatim
        if (!supplied.empty())
        {
            return std::vector<fs::path>(supplied.begin(), supplied.end());
        }
        return std::vector<fs::path>{defaultDataPath};
#endif
    }

    int run(const std::vector<std::string>& suppliedDataPaths, const fs::path& localDataPath, const fs::path& configFilePath, const PathMapping& pathMapping, const std::optional<GameParameters>& gameParameters, std::optional<DecodedSaveGame> savedGame, unsigned int desiredWindowWidth, unsigned int desiredWindowHeight, bool fullscreen, const std::string& imGuiIniPath, GlobalConfig& globalConfig)
    {
        LOG_INFO << ProjectNameVersion;
        LOG_INFO << "Current directory: " << fs::current_path().string();

        TimeService timeService(getTimestamp());

        LOG_INFO << "Initializing SDL";
        SdlContextManager sdlManager;

        auto sdlContext = sdlManager.getSdlContext();

        // resolve data roots only after SDL video init: on macOS the
        // folder picker needs the application registered with Cocoa
        auto searchPathResult = resolveGameDataPaths(suppliedDataPaths, localDataPath, configFilePath);
        if (!searchPathResult)
        {
            return 0;
        }
        const auto& searchPath = *searchPathResult;

        // require a stencil buffer of some kind
        if (!sdlContext->glSetAttribute(SDL_GL_STENCIL_SIZE, 1))
        {
            throw std::runtime_error(SDL_GetError());
        }

        auto window = sdlContext->createWindow(
            "RWE",
            desiredWindowWidth,
            desiredWindowHeight,
            SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (window == nullptr)
        {
            throw std::runtime_error(SDL_GetError());
        }

        if (fullscreen)
        {
            SDL_DisplayMode targetMode;
            auto displayID = sdlContext->getWindowDisplayIndex(window.get());
            if (displayID == 0)
            {
                throw std::runtime_error(SDL_GetError());
            }

            if (!sdlContext->getClosestDisplayMode(displayID, desiredWindowWidth, desiredWindowHeight, 0.0f, &targetMode))
            {
                throw std::runtime_error(SDL_GetError());
            }

            if (!sdlContext->setWindowDisplayMode(window.get(), &targetMode))
            {
                throw std::runtime_error(SDL_GetError());
            }
            sdlContext->setWindowSize(window.get(), desiredWindowWidth, desiredWindowHeight);
        }

        // Prevent the mouse from leaving the window.
        // We rely on nudging the edges of the screen to pan the camera,
        // so this is necessary for the game to work.
        sdlContext->setWindowGrab(window.get(), true);

        int windowWidth;
        int windowHeight;
        sdlContext->getWindowSize(window.get(), &windowWidth, &windowHeight);
        Viewport viewport(0, 0, windowWidth, windowHeight);

        LOG_INFO << "Initializing OpenGL context";

        auto glContextResult = createOpenGlContext(sdlContext, window.get(), OpenGlVersionInfo(3, 2, OpenGlProfile::Core));
        if (!glContextResult)
        {
            LOG_ERROR << "Failed to create preferred OpenGL context: " << glContextResult.getErr();
            glContextResult = createOpenGlContext(sdlContext, window.get(), OpenGlVersionInfo(3, 0, OpenGlProfile::Compatibility));
            if (!glContextResult)
            {
                throw std::runtime_error(glContextResult.getErr());
            }
        }

        auto glContext = std::move(*glContextResult);
        if (glContext == nullptr)
        {
            throw std::runtime_error(SDL_GetError());
        }

        doGlewInit();

        // log opengl context info
        LOG_INFO << "OpenGL version: " << glGetString(GL_VERSION);
        LOG_INFO << "OpenGL vendor: " << glGetString(GL_VENDOR);
        LOG_INFO << "OpenGL renderer: " << glGetString(GL_RENDERER);
        LOG_INFO << "OpenGL shading language version: " << glGetString(GL_SHADING_LANGUAGE_VERSION);
        LOG_DEBUG << "OpenGL extensions:";
        int openGlExtensionCount;
        glGetIntegerv(GL_NUM_EXTENSIONS, &openGlExtensionCount);
        for (int i = 0; i < openGlExtensionCount; ++i)
        {
            LOG_DEBUG << "  " << glGetStringi(GL_EXTENSIONS, i);
        }

        LOG_INFO << "Initializing Dear ImGui";
        ImGuiContext imGuiContext(imGuiIniPath, window.get(), glContext.get());

        LOG_INFO << "Initializing virtual file system";
        CompositeVirtualFileSystem vfs;
        for (const auto& path : searchPath)
        {
            addToVfs(vfs, path.string());
        }

        LOG_INFO << "Loading palette";
        auto paletteBytes = vfs.readFile("palettes/PALETTE.PAL");
        if (!paletteBytes)
        {
            throw std::runtime_error("Couldn't find palette");
        }

        auto palette = readPalette(*paletteBytes);
        if (!palette)
        {
            throw std::runtime_error("Couldn't read palette");
        }

        LOG_INFO << "Loading GUI palette";
        auto guiPaletteBytes = vfs.readFile("palettes/GUIPAL.PAL");
        if (!guiPaletteBytes)
        {
            throw std::runtime_error("Couldn't find palette");
        }

        auto guiPalette = readPalette(*guiPaletteBytes);
        if (!guiPalette)
        {
            throw std::runtime_error("Couldn't read GUI palette");
        }

        LOG_INFO << "Initializing services";
        GraphicsContext graphics;
        graphics.enableCulling();
        graphics.enableBlending();

        // CWD-relative shader lookup first; when the process was
        // launched without the project working directory (Finder,
        // relocated bundle), fall back to the executable's resource
        // root — Contents/Resources inside an .app, the executable's
        // own directory otherwise
        ShaderService shaders = ShaderService::createShaderService(graphics, getResourceRoot());

        TextureService textureService(&graphics, &vfs, &*palette);

        AudioService audioService(sdlContext, sdlManager.getSdlMixerContext(), &vfs);
        // Allocate a pool of tracks for sound playback
        audioService.allocateTracks(256);

        // load sound definitions
        LOG_INFO << "Loading global sound definitions";
        auto allSoundBytes = vfs.readFile("gamedata/ALLSOUND.TDF");
        if (!allSoundBytes)
        {
            throw std::runtime_error("Couldn't read ALLSOUND.TDF");
        }

        std::string allSoundString(allSoundBytes->data(), allSoundBytes->size());
        auto allSoundTdf = parseTdfFromString(allSoundString);

        LOG_INFO << "Loading cursors";
        Cursors cursors;
        cursors[*CursorType::Normal] = textureService.getGafEntry("anims/CURSORS.GAF", "cursornormal");
        cursors[*CursorType::Select] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorselect");
        cursors[*CursorType::Attack] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorattack");
        cursors[*CursorType::Move] = textureService.getGafEntry("anims/CURSORS.GAF", "cursormove");
        cursors[*CursorType::Guard] = textureService.getGafEntry("anims/CURSORS.GAF", "cursordefend");
        cursors[*CursorType::Repair] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorrepair");
        cursors[*CursorType::Reclaim] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorreclamate");
        cursors[*CursorType::Patrol] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorpatrol");
        cursors[*CursorType::CommandFire] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorairstrike");
        cursors[*CursorType::Capture] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorcapture");
        cursors[*CursorType::Load] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorload");
        cursors[*CursorType::Unload] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorunload");
        cursors[*CursorType::Hourglass] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorhourglass");
        cursors[*CursorType::Red] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorred");
        cursors[*CursorType::Green] = textureService.getGafEntry("anims/CURSORS.GAF", "cursorgrn");
        CursorService cursor(sdlContext, &timeService, cursors);

        sdlContext->hideCursor();

        SceneManager sceneManager(sdlContext, window.get(), &graphics, &timeService, &imGuiContext, &cursor, &globalConfig, UiRenderService(&graphics, &shaders, &viewport), &viewport);

        LOG_INFO << "Loading side data";
        auto sideDataBytes = vfs.readFile("gamedata/SIDEDATA.TDF");
        if (!sideDataBytes)
        {
            throw std::runtime_error("Missing side data");
        }
        std::string sideDataString(sideDataBytes->data(), sideDataBytes->size());
        std::unordered_map<std::string, SideData> sideDataMap;
        {
            auto sideData = parseSidesFromSideData(parseTdfFromString(sideDataString));
            for (auto& side : sideData)
            {
                std::string name = side.name;
                sideDataMap.insert({std::move(name), std::move(side)});
            }
        }

        rwe::CampaignProgress campaignProgress;
        SceneContext sceneContext(
            sdlContext,
            &viewport,
            &graphics,
            &textureService,
            &audioService,
            &cursor,
            &shaders,
            &vfs,
            &*palette,
            &*guiPalette,
            &sceneManager,
            &sideDataMap,
            &timeService,
            &pathMapping,
            &globalConfig,
            &campaignProgress);

        if (gameParameters)
        {
            LOG_INFO << "Launching into game on map: " << gameParameters->mapName
                     << (savedGame ? " (from saved game)" : "");
            auto scene = std::make_unique<LoadingScene>(
                sceneContext,
                &allSoundTdf,
                AudioService::LoopToken(),
                *gameParameters,
                std::move(savedGame));
            sceneManager.setNextScene(std::shared_ptr<Scene>(std::move(scene)));
        }
        else
        {
            LOG_INFO << "Launching into the main menu";
            auto scene = std::make_unique<MainMenuScene>(
                sceneContext,
                &allSoundTdf,
                viewport.width(),
                viewport.height());
            sceneManager.setNextScene(std::shared_ptr<Scene>(std::move(scene)));
        }

        LOG_INFO << "Entering main loop";
        sceneManager.execute();

        LOG_INFO << "Finished main loop, exiting";

        return 0;
    }

    PlayerControllerType parseControllerFromString(const std::string& controllerString)
    {
        auto components = utf8Split(controllerString, ',');
        if (components.empty())
        {
            throw std::runtime_error("controller string was empty?");
        }

        if (components[0] == "Human")
        {
            return PlayerControllerTypeHuman();
        }

        if (components[0] == "Computer")
        {
            return PlayerControllerTypeComputer();
        }

        if (components[0] == "Network")
        {
            if (components.size() != 2)
            {
                throw std::runtime_error("Invalid network player string format");
            }
            auto hostAndPort = getHostAndPort(components[1]);
            if (!hostAndPort)
            {
                throw std::runtime_error("Invalid network player address format");
            }
            return PlayerControllerTypeNetwork{hostAndPort->first, hostAndPort->second};
        }

        throw std::runtime_error("Unknown controller string");
    }

    std::string parseSideFromString(const std::string& side)
    {
        if (side == "ARM" || side == "CORE")
        {
            return side;
        }

        throw std::runtime_error("Unknown side string");
    }

    PlayerColorIndex parseColorFromString(const std::string& colorString)
    {
        std::stringstream s(colorString);
        unsigned int i;
        s >> i;
        if (s.fail())
        {
            throw std::runtime_error("Invalid player colour string");
        }
        if (i > 9)
        {
            throw std::runtime_error("Invalid player colour string");
        }
        return PlayerColorIndex(i);
    }

    std::optional<PlayerInfo> parsePlayerInfoFromArg(const std::string& playerString)
    {
        if (playerString == "empty")
        {
            return std::nullopt;
        }

        auto components = rwe::utf8Split(playerString, ';');
        if (components.size() != 4)
        {
            throw std::runtime_error("Invalid player arg");
        }

        auto name = components[0];
        auto controller = parseControllerFromString(components[1]);
        auto side = parseSideFromString(components[2]);
        auto color = parseColorFromString(components[3]);

        return PlayerInfo{name, controller, side, color, Metal(1000), Energy(1000)};
    }
}

std::shared_ptr<rwe::SimpleLogger> createLogger(const fs::path& logFile)
{
    return std::make_shared<rwe::SimpleLogger>(logFile.string(), true);
}

std::shared_ptr<rwe::SimpleLogger> createLoggerInDir(const fs::path& logDir)
{
    for (int i = 0; i < 3; ++i)
    {
        fs::path logPath(logDir);
        if (i == 0)
        {
            logPath /= "rwe.log";
        }
        else
        {
            logPath /= "rwe" + std::to_string(i) + ".log";
        }

        try
        {
            return std::make_shared<rwe::SimpleLogger>(logPath.string(), true);
        }
        catch (const std::exception&)
        {
        }
    }

    throw std::runtime_error("Failed to create logger");
}

rwe::PathMapping constructDefaultPathMapping()
{
    rwe::PathMapping m;

    m.ai = "ai";
    m.anims = "anims";
    m.bitmaps = "bitmaps";
    m.camps = "camps";
    m.downloads = "downloads";
    m.features = "features";
    m.fonts = "fonts";
    m.gamedata = "gamedata";
    m.guis = "guis";
    m.maps = "maps";
    m.objects3d = "objects3d";
    m.palettes = "palettes";
    m.scripts = "scripts";
    m.sounds = "sounds";
    m.textures = "textures";
    m.unitpics = "unitpics";
    m.units = "units";
    m.weapons = "weapons";

    return m;
}

int main(int argc, char* argv[])
{
    try
    {
        auto localDataPath = rwe::getLocalDataPath();
        if (!localDataPath)
        {
            throw std::runtime_error("Failed to determine local data path");
        }

        fs::create_directories(*localDataPath);

        fs::path configFilePath(*localDataPath);
        configFilePath /= "rwe.cfg";

        fs::path imGuiIniFilePath(*localDataPath);
        imGuiIniFilePath /= "imgui.ini";

        rwe::OpaqueArgs args;
        args.parse(argc, argv);
        // explicit CLI keys beat the config file; a malformed file is
        // ignored entirely so corrupt settings fall back to defaults
        if (auto savedConfig = rwe::readConfigFile(configFilePath))
        {
            std::ostringstream savedConfigStream;
            for (const auto& [key, value] : *savedConfig)
            {
                savedConfigStream << key << "=" << value << "\n";
            }
            auto savedConfigText = savedConfigStream.str();
            std::istringstream savedConfigInput(savedConfigText);
            args.parseConfig(savedConfigInput);
        }

        if (args.isHelpRequested())
        {
            std::cout << "Usage: rwe [options]\n"
                      << "  --help                Show this message\n"
                      << "  --log <path>          Log output file path\n"
                      << "  --state-log <path>    Sim-state log file (desync debugging)\n"
                      << "  --width <pixels>      Window width (default: 800)\n"
                      << "  --height <pixels>     Window height (default: 600)\n"
                      << "  --fullscreen          Start in fullscreen mode\n"
                      << "  --interface-mode <m>  left-click or right-click (default: left-click)\n"
                      << "  --data-path <path>    Game data search path (repeatable)\n"
                      << "  --map <name>          Launch directly into a game on this map\n"
                      << "  --campaign <spec>     Launch a campaign mission: missionfile;side;difficulty\n"
                      << "  --port <port>         Network port (default: 1337)\n"
                      << "  --player <spec>       Player spec: name;type;side;color (repeatable)\n"
                      << "  --dir-<name> <dir>    Override directory name for a data category\n"
                      << std::endl;
            return 0;
        }

        auto logger = args.contains("log") ? createLogger(fs::path(args.getString("log"))) : createLoggerInDir(*localDataPath);
        rwe::setGlobalLogger(logger);

        try
        {
            rwe::GlobalConfig config;
            config.configFilePath = configFilePath;
            config.leftClickInterfaceMode = args.getString("interface-mode", "left-click") != "right-click";
            std::optional<rwe::GameParameters> gameParameters;
            std::optional<rwe::DecodedSaveGame> savedGame;
            if (args.contains("load-save"))
            {
                // --load-save <file.sav>: decode the save and launch the
                // reconstructed game directly (same path as the load
                // dialog in MainMenuScene::loadSelectedSave)
                fs::path savePath(args.getString("load-save"));
                std::ifstream saveFile(savePath, std::ios::binary);
                if (!saveFile)
                {
                    throw std::runtime_error("cannot open save file: " + savePath.string());
                }
                std::vector<char> bytes((std::istreambuf_iterator<char>(saveFile)), std::istreambuf_iterator<char>());
                savedGame = rwe::decodeSavedGame(bytes);
                gameParameters = rwe::gameParametersFromSave(*savedGame);
            }
            else if (args.contains("map"))
            {
                const auto& mapName = args.getString("map");
                const auto& players = args.getMulti("player");

                gameParameters = rwe::GameParameters{mapName, 0};
                if (args.contains("state-log"))
                {
                    gameParameters->stateLogFile = args.getString("state-log");
                }
                gameParameters->localNetworkPort = args.getString("port", "1337");
                unsigned int playerIndex = 0;
                if (players.size() > 10)
                {
                    throw std::runtime_error("too many players");
                }
                for (const auto& playerString : players)
                {
                    gameParameters->players[playerIndex] = rwe::parsePlayerInfoFromArg(playerString);
                    ++playerIndex;
                }
            }
            else if (args.contains("campaign"))
            {
                auto components = rwe::utf8Split(args.getString("campaign"), ';');
                if (components.size() != 3)
                {
                    throw std::runtime_error("Invalid campaign arg, expected missionfile;side;difficulty");
                }

                auto missionFile = components[0];
                auto mapName = missionFile;
                if (mapName.size() >= 4 && rwe::toUpper(mapName.substr(mapName.size() - 4)) == ".OTA")
                {
                    mapName.resize(mapName.size() - 4);
                }

                gameParameters = rwe::GameParameters{mapName, 0};
                if (args.contains("state-log"))
                {
                    gameParameters->stateLogFile = args.getString("state-log");
                }
                gameParameters->localNetworkPort = args.getString("port", "1337");

                rwe::CampaignMissionParameters campaign;
                campaign.campaignName = components[0];
                campaign.campaignSide = components[1];
                campaign.missionIndex = 0;
                campaign.missionFile = missionFile;
                campaign.missionName = components[0];
                campaign.difficulty = components[2];
                gameParameters->campaign = campaign;
            }

            // --data-path values (CLI + config file) are resolved
            // inside run() after SDL init: with none usable, macOS
            // shows a folder picker and persists the selection
            auto dataPaths = args.getMulti("data-path");

            auto screenWidth = args.getUint("width", 800);
            auto screenHeight = args.getUint("height", 600);
            auto fullscreen = args.getBool("fullscreen");

            auto pathMapping = constructDefaultPathMapping();
            pathMapping.ai = args.getString("dir-ai", "ai");
            pathMapping.anims = args.getString("dir-anims", "anims");
            pathMapping.bitmaps = args.getString("dir-bitmaps", "bitmaps");
            pathMapping.camps = args.getString("dir-camps", "camps");
            pathMapping.downloads = args.getString("dir-downloads", "downloads");
            pathMapping.fonts = args.getString("dir-fonts", "fonts");
            pathMapping.gamedata = args.getString("dir-gamedata", "gamedata");
            pathMapping.guis = args.getString("dir-guis", "guis");
            pathMapping.maps = args.getString("dir-maps", "maps");
            pathMapping.objects3d = args.getString("dir-objects3d", "objects3d");
            pathMapping.palettes = args.getString("dir-palettes", "palettes");
            pathMapping.scripts = args.getString("dir-scripts", "scripts");
            pathMapping.sounds = args.getString("dir-sounds", "sounds");
            pathMapping.textures = args.getString("dir-textures", "textures");
            pathMapping.unitpics = args.getString("dir-unitpics", "unitpics");
            pathMapping.units = args.getString("dir-units", "units");
            pathMapping.weapons = args.getString("dir-weapons", "weapons");

            return rwe::run(dataPaths, *localDataPath, configFilePath, pathMapping, gameParameters, std::move(savedGame), screenWidth, screenHeight, fullscreen, imGuiIniFilePath.string(), config);
        }
        catch (const std::exception& e)
        {
            LOG_CRITICAL << e.what();
            throw;
        }
    }
    catch (const std::exception& e)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Critical Error", e.what(), nullptr);
        return 1;
    }
}
