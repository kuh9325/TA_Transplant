#pragma once

#include <algorithm>
#include <cstdint>
#include <rwe/game/SceneTime.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/rwe_time.h>

namespace rwe
{
    float ema(float val, float average, float alpha);

    template <typename Range>
    unsigned int estimateAverageSceneTimeStatic(SceneTime localSceneTime, Range sceneTimes, Timestamp time)
    {
        std::uint64_t accum = localSceneTime.value;
        std::uint64_t count = 1;
        for (const auto& lastKnownSceneTime : sceneTimes)
        {
            auto elapsedTimeMillis = std::chrono::duration_cast<std::chrono::milliseconds>(time - lastKnownSceneTime.second).count();
            // SceneTime counts 30 Hz lockstep ticks, not render frames.
            // A future sample contributes no negative elapsed time.
            auto extraFrames = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsedTimeMillis)) * SimTicksPerSecond / 1000;

            auto peerSceneTime = lastKnownSceneTime.first.value + extraFrames;
            accum += peerSceneTime;
            count += 1;
        }
        return static_cast<unsigned int>(accum / count);
    }

    void writeInt(char* sendBuffer, unsigned int crcResult);

    unsigned int readInt(const char* buffer);

    unsigned int computeCrc(const char* buffer, unsigned int size);
}
