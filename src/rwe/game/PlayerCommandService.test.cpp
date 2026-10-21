#include <catch2/catch_test_macros.hpp>
#include <rwe/game/PlayerCommandService.h>
#include <stdexcept>

namespace rwe
{
    TEST_CASE("PlayerCommandService hash checks terminate with no hash participants", "[followup][command-service]")
    {
        PlayerCommandService service;
        REQUIRE(service.checkHashes());

        service.registerPlayer(PlayerId(1), false);
        REQUIRE(service.checkHashes());
        REQUIRE_THROWS_AS(service.pushHash(PlayerId(1), GameHash(1)), std::out_of_range);
    }

    TEST_CASE("PlayerCommandService single player hashes stay bounded for one million ticks", "[followup][command-service][stress]")
    {
        PlayerCommandService service;
        const PlayerId human(0);
        service.registerPlayer(human);
        // The AI still owns a command queue, but emits no independent hash.
        for (unsigned int i = 1; i < 10; ++i)
        {
            service.registerPlayer(PlayerId(i), false);
        }

        unsigned int peakBufferedHashes = 0;
        bool allHashesMatched = true;
        for (unsigned int tick = 0; tick < 1000000; ++tick)
        {
            service.pushHash(human, GameHash(tick));
            auto count = service.bufferedHashCount(human);
            if (count > peakBufferedHashes)
            {
                peakBufferedHashes = count;
            }
            allHashesMatched = service.checkHashes() && allHashesMatched;
        }

        REQUIRE(allHashesMatched);
        REQUIRE(peakBufferedHashes == 1);
        REQUIRE(service.bufferedHashCount(human) == 0);
    }

    TEST_CASE("PlayerCommandService AI hash exclusion preserves command lockstep", "[followup][command-service]")
    {
        PlayerCommandService service;
        const PlayerId human(0);
        const PlayerId computer(1);
        service.registerPlayer(human);
        service.registerPlayer(computer, false);

        service.pushCommands(human, {PlayerPauseGameCommand{}});
        REQUIRE_FALSE(service.tryPopCommands().has_value());
        REQUIRE(service.bufferedCommandCount(human) == 1);

        service.pushCommands(computer, {});
        auto commands = service.tryPopCommands();
        REQUIRE(commands.has_value());
        REQUIRE(commands->size() == 2);
        for (const auto& [player, batch] : *commands)
        {
            if (player == human)
            {
                REQUIRE(batch.size() == 1);
                REQUIRE(std::holds_alternative<PlayerPauseGameCommand>(batch.front()));
            }
            else
            {
                REQUIRE(player == computer);
                REQUIRE(batch.empty());
            }
        }
        REQUIRE(service.bufferedCommandCount(human) == 0);
        REQUIRE(service.bufferedCommandCount(computer) == 0);
    }

    TEST_CASE("PlayerCommandService delayed multiplayer hashes retain exact FIFO alignment", "[followup][command-service]")
    {
        PlayerCommandService service;
        const PlayerId local(0);
        const PlayerId remote(1);
        const PlayerId remote2(2);
        service.registerPlayer(local);
        service.registerPlayer(remote);
        service.registerPlayer(remote2);
        for (unsigned int tick = 0; tick < 1000; ++tick)
        {
            service.pushHash(local, GameHash(tick));
            service.pushHash(remote, GameHash(tick));
        }
        REQUIRE(service.checkHashes());
        REQUIRE(service.bufferedHashCount(local) == 1000);
        REQUIRE(service.bufferedHashCount(remote) == 1000);

        for (unsigned int tick = 0; tick < 1000; ++tick)
        {
            service.pushHash(remote2, GameHash(tick));
            REQUIRE(service.checkHashes());
            REQUIRE(service.bufferedHashCount(local) == 999 - tick);
            REQUIRE(service.bufferedHashCount(remote) == 999 - tick);
            REQUIRE(service.bufferedHashCount(remote2) == 0);
        }
    }

    TEST_CASE("PlayerCommandService reports a multiplayer mismatch without skipping later hashes", "[followup][command-service]")
    {
        PlayerCommandService service;
        const PlayerId local(0);
        const PlayerId remote(1);
        service.registerPlayer(local);
        service.registerPlayer(remote);
        service.pushHash(local, GameHash(11));
        service.pushHash(local, GameHash(22));
        service.pushHash(local, GameHash(33));
        service.pushHash(remote, GameHash(11));
        service.pushHash(remote, GameHash(99));
        service.pushHash(remote, GameHash(33));

        REQUIRE_FALSE(service.checkHashes());
        REQUIRE(service.bufferedHashCount(local) == 1);
        REQUIRE(service.bufferedHashCount(remote) == 1);
        REQUIRE(service.checkHashes());
        REQUIRE(service.bufferedHashCount(local) == 0);
        REQUIRE(service.bufferedHashCount(remote) == 0);
    }

    TEST_CASE("PlayerCommandService rejects duplicate registration without changing queued data", "[followup][command-service]")
    {
        PlayerCommandService service;
        const PlayerId player(0);
        service.registerPlayer(player);
        service.pushHash(player, GameHash(12));
        service.pushCommands(player, {});
        REQUIRE_THROWS_AS(service.registerPlayer(player, false), std::logic_error);
        REQUIRE(service.bufferedHashCount(player) == 1);
        REQUIRE(service.bufferedCommandCount(player) == 1);
    }

    TEST_CASE("PlayerCommandService large movement batches preserve tick order and payload", "[followup][command-service][stress]")
    {
        PlayerCommandService service;
        const PlayerId local(0);
        const PlayerId remote(1);
        service.registerPlayer(local);
        service.registerPlayer(remote);
        constexpr unsigned int tickCount = 16;
        constexpr unsigned int unitCount = 2048;
        for (unsigned int tick = 0; tick < tickCount; ++tick)
        {
            std::vector<PlayerCommand> commands;
            commands.reserve(unitCount);
            for (unsigned int unit = 0; unit < unitCount; ++unit)
            {
                const SimVector destination(intToSimScalar(tick), SimScalar(0), intToSimScalar(unit));
                commands.emplace_back(PlayerUnitCommand(UnitId(unit), PlayerUnitCommand::IssueOrder(MoveOrder(destination), PlayerUnitCommand::IssueOrder::Queued)));
            }
            service.pushCommands(local, commands);
        }
        REQUIRE_FALSE(service.tryPopCommands().has_value());
        REQUIRE(service.bufferedCommandCount(local) == tickCount);

        for (unsigned int tick = 0; tick < tickCount; ++tick)
        {
            service.pushCommands(remote, {});
            auto commands = service.tryPopCommands();
            REQUIRE(commands.has_value());
            REQUIRE(commands->size() == 2);
            for (const auto& [player, batch] : *commands)
            {
                if (player != local)
                {
                    REQUIRE(player == remote);
                    REQUIRE(batch.empty());
                    continue;
                }
                REQUIRE(batch.size() == unitCount);
                for (unsigned int unit = 0; unit < unitCount; ++unit)
                {
                    const auto& command = std::get<PlayerUnitCommand>(batch[unit]);
                    REQUIRE(command.unit == UnitId(unit));
                    const auto& order = std::get<PlayerUnitCommand::IssueOrder>(command.command);
                    REQUIRE(order.issueKind == PlayerUnitCommand::IssueOrder::Queued);
                    const auto& destination = std::get<MoveOrder>(order.order).destination;
                    REQUIRE(destination.x == intToSimScalar(tick));
                    REQUIRE(destination.z == intToSimScalar(unit));
                }
            }
        }
        REQUIRE_FALSE(service.tryPopCommands().has_value());
        REQUIRE(service.bufferedCommandCount(local) == 0);
        REQUIRE(service.bufferedCommandCount(remote) == 0);
    }
}
