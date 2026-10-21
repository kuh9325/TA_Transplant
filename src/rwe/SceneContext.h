#pragma once

#include <rwe/AudioService.h>
#include <rwe/CampaignProgress.h>
#include <rwe/CursorService.h>
#include <rwe/GlobalConfig.h>
#include <rwe/PathMapping.h>
#include <rwe/TextureService.h>
#include <rwe/Viewport.h>
#include <rwe/io/sidedatatdf/SideData.h>
#include <rwe/rwe_time.h>
#include <rwe/scene/SceneManager.h>
#include <rwe/sdl/SdlContext.h>
#include <rwe/vfs/AbstractVirtualFileSystem.h>

namespace rwe
{
    class SceneContext
    {
    public:
        SdlContext* const sdl;
        Viewport* const viewport;
        GraphicsContext* const graphics;
        TextureService* const textureService;
        AudioService* const audioService;
        CursorService* const cursor;
        ShaderService* const shaders;
        AbstractVirtualFileSystem* const vfs;
        const ColorPalette* const palette;
        const ColorPalette* const guiPalette;
        SceneManager* const sceneManager;
        const std::unordered_map<std::string, SideData>* const sideData;
        TimeService* const timeService;
        const PathMapping* const pathMapping;
        GlobalConfig* const globalConfig;

        /**
         * Session-scoped campaign W/L/U state, shared by the campaign
         * frontend, the mission scene and the ENDMSN debrief.
         * Retail keeps the equivalent array in the frontend context;
         * persistence is Save/Load (C6), out of scope here.
         */
        CampaignProgress* const campaignProgress;

        SceneContext(
            SdlContext* const sdl,
            Viewport* const viewportService,
            GraphicsContext* const graphics,
            TextureService* const textureService,
            AudioService* const audioService,
            CursorService* const cursor,
            ShaderService* const shaders,
            AbstractVirtualFileSystem* const vfs,
            const ColorPalette* const palette,
            const ColorPalette* const guiPalette,
            SceneManager* const sceneManager,
            const std::unordered_map<std::string, SideData>* const sideData,
            TimeService* const timeService,
            const PathMapping* const pathMapping,
            GlobalConfig* const globalConfig,
            CampaignProgress* const campaignProgress)
            : sdl(sdl),
              viewport(viewportService),
              graphics(graphics),
              textureService(textureService),
              audioService(audioService),
              cursor(cursor),
              shaders(shaders),
              vfs(vfs),
              palette(palette),
              guiPalette(guiPalette),
              sceneManager(sceneManager),
              sideData(sideData),
              timeService(timeService),
              pathMapping(pathMapping),
              globalConfig(globalConfig),
              campaignProgress(campaignProgress)
        {
        }
    };
}
