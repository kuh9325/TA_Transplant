#pragma once

#include <memory>
#include <rwe/math/Vector3f.h>
#include <rwe/render/SpriteSeries.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/SimVector.h>
#include <variant>

namespace rwe
{
    struct ParticleFinishTimeEndOfFrames
    {
    };
    struct ParticleFinishTimeFixedTime
    {
        GameTime time;
    };
    using ParticleFinishTime = std::variant<ParticleFinishTimeEndOfFrames, ParticleFinishTimeFixedTime>;

    struct ParticleRenderTypeSprite
    {
        std::string gafName;
        std::string animName;
        ParticleFinishTime finishTime;
        GameTime frameDuration{4};
        bool translucent{false};
    };

    struct ParticleRenderTypeWake
    {
        GameTime finishTime;
    };

    /**
     * One square of the retail nanolathe spray (TotalA.exe effect
     * channel 6). Retail colors each particle from the palette ramp
     * 0xA1-0xA7 (bright-to-dark green): `rampIndex` starts at the
     * burst index and advances +1 per tick, wrapping after the last
     * entry. `finishTime` is the spawn tick + trunc(distance/4)
     * deadline; the particle dies on the first tick past it.
     */
    struct ParticleRenderTypeNanolathe
    {
        GameTime finishTime;
        unsigned int rampIndex;
    };

    using ParticleRenderType = std::variant<ParticleRenderTypeSprite, ParticleRenderTypeWake, ParticleRenderTypeNanolathe>;

    struct Particle
    {
        Vector3f position;
        Vector3f velocity;
        ParticleRenderType renderType;
        GameTime startTime;

        bool isStarted(GameTime currentTime) const;
        unsigned int getFrameIndex(GameTime currentTime, GameTime frameDuration, int totalFrames) const;
        bool isFinished(GameTime currentTime, const ParticleFinishTime& finishTime, GameTime frameDuration, int numberOfFrames) const;
    };
}
