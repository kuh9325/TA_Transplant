#include <catch2/catch_test_macros.hpp>
#include <rwe/game/SceneTickScheduler.h>
#include <rwe/sim/SimVector.h>

namespace rwe
{
    namespace
    {
        std::uint64_t drainTicks(SceneTickAccumulator& accumulator)
        {
            std::uint64_t ticks = 0;
            while (accumulator.consumeTick())
            {
                ++ticks;
            }
            return ticks;
        }
    }

    TEST_CASE("scene pacing has exactly thirty ticks per second for a simulated day", "[followup][timing][stress]")
    {
        SceneTickAccumulator accumulator;
        std::uint64_t ticks = 0;
        // Irregular 1..47 ms frames reproduce sustained render pacing jitter.
        int elapsed = 0;
        constexpr int dayMilliseconds = 24 * 60 * 60 * 1000;
        unsigned int frame = 0;
        while (elapsed < dayMilliseconds)
        {
            const auto delta = std::min(static_cast<int>(frame++ % 47) + 1, dayMilliseconds - elapsed);
            elapsed += delta;
            accumulator.addElapsed(delta, 10);
            ticks += drainTicks(accumulator);
        }
        REQUIRE(ticks == 24 * 60 * 60 * SimTicksPerSecond);
        REQUIRE(accumulator.interpolationFraction() == 0.0f);
    }

    TEST_CASE("scene pacing retains fractional speed across frame boundaries", "[followup][timing]")
    {
        for (int speed = 1; speed <= 20; ++speed)
        {
            INFO("speed " << speed);
            SceneTickAccumulator smallFrames;
            SceneTickAccumulator largeFrame;
            std::uint64_t smallTicks = 0;
            for (int i = 0; i < 10000; ++i)
            {
                smallFrames.addElapsed(1, speed);
                smallTicks += drainTicks(smallFrames);
            }
            largeFrame.addElapsed(10000, speed);
            REQUIRE(smallTicks == drainTicks(largeFrame));
            REQUIRE(smallTicks == static_cast<unsigned int>(speed * SimTicksPerSecond));
            REQUIRE(smallFrames.interpolationFraction() == largeFrame.interpolationFraction());
        }

        SceneTickAccumulator accumulator;
        accumulator.addElapsed(33, 10);
        REQUIRE_FALSE(accumulator.consumeTick());
        accumulator.addElapsed(1, 10);
        REQUIRE(accumulator.consumeTick());
        REQUIRE_FALSE(accumulator.consumeTick());
        REQUIRE(accumulator.interpolationFraction() > 0.0f);
        REQUIRE(accumulator.interpolationFraction() < 1.0f);
        accumulator.addElapsed(-100, 10);
        REQUIRE_FALSE(accumulator.consumeTick());
    }

    TEST_CASE("command horizon uses simulation ticks including game speed", "[followup][timing]")
    {
        REQUIRE(commandBufferTarget(0.0f, 10) == 7);
        REQUIRE(commandBufferTarget(16.0f, 10) == 7);
        REQUIRE(commandBufferTarget(200.0f, 10) == 14);
        REQUIRE(commandBufferTarget(2000.0f, 10) == 82);
        REQUIRE(commandBufferTarget(9999.0f, 10) == 82);
        REQUIRE(commandBufferTarget(16.0f, 20) == 14);
        REQUIRE(commandBufferTarget(16.0f, 1) == 1);
    }

    TEST_CASE("long render frames refill local and AI command slots per tick", "[followup][timing]")
    {
        PlayerCommandService service;
        const PlayerId local(0);
        const PlayerId computer(1);
        service.registerPlayer(local);
        service.registerPlayer(computer, false);
        const auto horizon = commandBufferTarget(16.0f, 10);
        for (unsigned int i = 0; i < horizon; ++i)
        {
            service.pushCommands(local, {});
        }
        SceneTickAccumulator accumulator;
        accumulator.addElapsed(2000, 20);
        unsigned int ticks = 0;
        unsigned int forwardedEmptyFrames = 0;
        while (accumulator.consumeTick())
        {
            REQUIRE(sceneTickMayAdvance(SceneTime(ticks), SceneTime(3), false));
            forwardedEmptyFrames += ensureLocalCommandFrame(service, local) ? 1 : 0;
            ensureLocalCommandFrame(service, computer);
            REQUIRE(service.tryPopCommands().has_value());
            ++ticks;
        }
        REQUIRE(ticks == 120);
        REQUIRE(forwardedEmptyFrames == ticks - horizon);
        REQUIRE(service.bufferedCommandCount(local) == 0);
        REQUIRE(service.bufferedCommandCount(computer) == 0);
        // The same five-tick boundary still throttles a peer ahead of the
        // tolerated multiplayer clock, preserving the synchronization rule.
        REQUIRE_FALSE(sceneTickMayAdvance(SceneTime(5), SceneTime(3), true));
        REQUIRE(sceneTickMayAdvance(SceneTime(5), SceneTime(5), true));
        REQUIRE(sceneTickMayAdvance(SceneTime(4), SceneTime(3), true));
    }

    TEST_CASE("catch-up never manufactures remote frames or reorders movement bursts", "[followup][timing][stress]")
    {
        PlayerCommandService service;
        const PlayerId local(0);
        const PlayerId remote(1);
        const PlayerId computer(2);
        service.registerPlayer(local);
        service.registerPlayer(remote);
        service.registerPlayer(computer, false);
        constexpr unsigned int commandCount = 10000;
        std::vector<PlayerCommand> burst;
        for (unsigned int i = 0; i < commandCount; ++i)
        {
            burst.emplace_back(PlayerUnitCommand(UnitId(i), PlayerUnitCommand::IssueOrder(
                MoveOrder{SimVector(SimScalar(static_cast<int>(i)), 0_ss, 10_ss)}, PlayerUnitCommand::IssueOrder::Immediate)));
        }
        service.pushCommands(local, burst);
        for (unsigned int i = 0; i < 1000; ++i)
        {
            ensureLocalCommandFrame(service, local);
            ensureLocalCommandFrame(service, computer);
            REQUIRE_FALSE(service.tryPopCommands().has_value());
        }
        REQUIRE(service.bufferedCommandCount(local) == 1);
        REQUIRE(service.bufferedCommandCount(computer) == 1);
        REQUIRE(service.bufferedCommandCount(remote) == 0);

        service.pushCommands(remote, {PlayerPauseGameCommand{}});
        const auto commands = service.tryPopCommands();
        REQUIRE(commands.has_value());
        for (const auto& [player, batch] : *commands)
        {
            if (player == local)
            {
                REQUIRE(batch.size() == commandCount);
                for (unsigned int i = 0; i < commandCount; ++i)
                {
                    const auto& command = std::get<PlayerUnitCommand>(batch[i]);
                    REQUIRE(command.unit == UnitId(i));
                    const auto& move = std::get<MoveOrder>(std::get<PlayerUnitCommand::IssueOrder>(command.command).order);
                    REQUIRE(move.destination.x == SimScalar(static_cast<int>(i)));
                }
            }
            else if (player == remote)
            {
                REQUIRE(batch.size() == 1);
                REQUIRE(std::holds_alternative<PlayerPauseGameCommand>(batch.front()));
            }
            else
            {
                REQUIRE(batch.empty());
            }
        }
    }
}
