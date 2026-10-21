#pragma once

#include <algorithm>
#include <cstdint>
#include <rwe/game/PlayerCommandService.h>
#include <rwe/sim/SimTicksPerSecond.h>

namespace rwe
{
    /** Wall-clock pacing only; simulation steps and saved game time stay integral. */
    class SceneTickAccumulator
    {
    private:
        // A tenth-speed millisecond contributes SimTicksPerSecond units.
        // 10000 units are exactly one tick, avoiding the truncated 33 ms period
        // and frame-dependent float rounding at fractional game speeds.
        static constexpr std::int64_t UnitsPerTick = 10000;
        std::int64_t units{0};

    public:
        void addElapsed(int milliseconds, int speedLevel)
        {
            if (milliseconds > 0)
            {
                units += static_cast<std::int64_t>(milliseconds) * speedLevel * SimTicksPerSecond;
            }
        }

        bool consumeTick()
        {
            if (units < UnitsPerTick)
            {
                return false;
            }
            units -= UnitsPerTick;
            return true;
        }

        float interpolationFraction() const
        {
            return std::min(1.0f, static_cast<float>(units) / static_cast<float>(UnitsPerTick));
        }

        float pendingMilliseconds() const
        {
            return static_cast<float>(units) / static_cast<float>(10 * SimTicksPerSecond);
        }
    };

    /** Convert the RTT safety horizon into command ticks at the actual tick rate. */
    inline unsigned int commandBufferTarget(float averageRttMilliseconds, int speedLevel)
    {
        const auto rtt = std::clamp(averageRttMilliseconds, 16.0f, 2000.0f);
        const auto horizonMilliseconds = rtt + rtt / 4.0f + 200.0f;
        return static_cast<unsigned int>(horizonMilliseconds * SimTicksPerSecond * speedLevel / 10000.0f) + 1;
    }

    /** Single-player catch-up has no remote clock ceiling. */
    inline bool sceneTickMayAdvance(SceneTime sceneTime, SceneTime highSceneTime, bool synchroniseSceneTime)
    {
        return !synchroniseSceneTime || sceneTime % SceneTime(5) != SceneTime(0) || sceneTime <= highSceneTime;
    }

    /**
     * Computer AI runs inside simulation.tick(), so it needs an empty frame for
     * every attempted tick, including multiple catch-up ticks in one update.
     * The local human may also exhaust its prefilled horizon in a long update.
     * Remote players must never use this helper: their frames arrive over UDP.
     * Returns true if a new frame needs forwarding to the local network stream.
     */
    inline bool ensureLocalCommandFrame(PlayerCommandService& commands, PlayerId player)
    {
        if (commands.bufferedCommandCount(player) != 0)
        {
            return false;
        }
        commands.pushCommands(player, {});
        return true;
    }
}
