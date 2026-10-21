#include <catch2/catch_test_macros.hpp>

#include <rwe/game/GameNetworkService.h>
#include <rwe/network_util.h>
#include <rwe/proto/serialization.h>
#include <thread>
#include <filesystem>
#include <rwe/util/SimpleLogger.h>

namespace rwe
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        template <typename Predicate>
        bool eventually(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(8))
        {
            const auto deadline = Clock::now() + timeout;
            do
            {
                if (predicate()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (Clock::now() < deadline);
            return false;
        }

        struct Peer
        {
            std::shared_ptr<SimpleLogger> previousLogger{globalLogger()};
            asio::io_context context;
            asio::ip::udp::socket socket{context, asio::ip::udp::endpoint(asio::ip::udp::v6(), 0)};
            asio::ip::udp::endpoint endpoint{asio::ip::address_v6::loopback(), socket.local_endpoint().port()};

            Peer()
            {
                // The production entry point initializes the shared logger;
                // standalone library tests must fulfill the same contract.
                if (!globalLogger())
                {
                    const auto path = std::filesystem::temp_directory_path() /
                        ("rwe-network-tests-" + std::to_string(Clock::now().time_since_epoch().count()) + ".log");
                    setGlobalLogger(std::make_shared<SimpleLogger>(path.string(), false));
                }
                socket.non_blocking(true);
            }

            ~Peer() { setGlobalLogger(std::move(previousLogger)); }

            template <typename Predicate>
            bool receiveUntil(Predicate predicate)
            {
                return eventually([&]() {
                    std::array<char, 65507> buffer;
                    asio::ip::udp::endpoint sender;
                    asio::error_code error;
                    const auto size = socket.receive_from(asio::buffer(buffer), sender, 0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) return false;
                    REQUIRE_FALSE(error);
                    REQUIRE(size >= 4);
                    REQUIRE(readInt(buffer.data() + size - 4) == computeCrc(buffer.data(), size - 4));
                    proto::NetworkMessage message;
                    REQUIRE(message.ParseFromArray(buffer.data(), size - 4));
                    return predicate(message.game_update(), size);
                });
            }

            void send(proto::NetworkMessage message, unsigned short port)
            {
                // Proto2 presence is significant even for zero-valued fields.
                // Preserve explicit test ACK/sequence values and fill every
                // other required field before serialization.
                auto& update = *message.mutable_game_update();
                if (!update.has_next_command_set_to_receive()) update.set_next_command_set_to_receive(0);
                if (!update.has_next_command_set_to_send()) update.set_next_command_set_to_send(0);
                if (!update.has_ack_delay()) update.set_ack_delay(0);
                if (!update.has_current_scene_time()) update.set_current_scene_time(0);
                if (!update.has_player_id()) update.set_player_id(1);
                if (!update.has_packet_id()) update.set_packet_id(0);
                if (!update.has_next_game_hash_to_send()) update.set_next_game_hash_to_send(0);
                if (!update.has_next_game_hash_to_receive()) update.set_next_game_hash_to_receive(0);
                REQUIRE(message.IsInitialized());
                std::string bytes;
                REQUIRE(message.SerializeToString(&bytes));
                const auto crc = computeCrc(bytes.data(), bytes.size());
                const auto size = bytes.size();
                bytes.resize(size + 4);
                writeInt(bytes.data() + size, crc);
                socket.send_to(asio::buffer(bytes), asio::ip::udp::endpoint(asio::ip::address_v6::loopback(), port));
            }
        };

        std::vector<GameNetworkService::EndpointInfo> endpointFor(const Peer& peer)
        {
            return {{PlayerId(1), peer.endpoint}};
        }

        void assertResponsive(GameNetworkService& network)
        {
            const auto begin = Clock::now();
            for (unsigned int i = 0; i < 1000; ++i)
            {
                REQUIRE(network.estimateAvergeSceneTime(SceneTime(i)) == SceneTime(i));
                REQUIRE(network.getMaxAverageRttMillis() == 0.0f);
            }
            REQUIRE(Clock::now() - begin < std::chrono::seconds(1));
        }
    }

    TEST_CASE("Single-player network service does not bind a UDP port", "[network][followup]")
    {
        Peer occupiedPort;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), occupiedPort.endpoint.port(), {}, &commands);
        network.start();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Running);
        REQUIRE(network.getLocalPort() == 0);
        for (unsigned int i = 0; i < 10000; ++i)
        {
            network.submitCommands(SceneTime(i), {});
            network.submitGameHash(GameHash(i));
        }
        assertResponsive(network);
        network.stop();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);
    }

    TEST_CASE("Game network getters do not depend on a dispatching executor", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        // Submissions queued before start deliberately leave the executor idle.
        network.submitCommands(SceneTime(0), {});
        assertResponsive(network);
        network.stop();
        network.start(); // A stopped service is not restarted with stale tasks.
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);
        assertResponsive(network);
    }

    TEST_CASE("Game network handles duplicate lifecycle calls and stops without incoming traffic", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));
        REQUIRE(network.getLocalPort() != 0);
        assertResponsive(network);
        const auto begin = Clock::now();
        network.stop();
        network.stop();
        REQUIRE(Clock::now() - begin < std::chrono::seconds(1));
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);
        assertResponsive(network);
    }

    TEST_CASE("Game network UDP port conflict fails without blocking the game thread", "[network][followup]")
    {
        Peer occupiedPort;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), occupiedPort.endpoint.port(), endpointFor(occupiedPort), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Failed; }));
        REQUIRE_FALSE(network.getFailureReason().empty());
        assertResponsive(network);
        network.submitCommands(SceneTime(0), {});
        network.submitGameHash(GameHash(0));
        network.stop();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Failed);
    }

    TEST_CASE("Game network handler exceptions report failure and leave getters responsive", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        // An unregistered remote player deliberately makes pushCommands throw.
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));
        proto::NetworkMessage message;
        auto& update = *message.mutable_game_update();
        update.set_player_id(1);
        update.add_command_set();
        peer.send(message, network.getLocalPort());
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Failed; }));
        REQUIRE_FALSE(network.getFailureReason().empty());
        assertResponsive(network);
        network.stop();
    }

    TEST_CASE("Game network preserves one large movement command set in a UDP datagram", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        GameNetworkService::CommandSet moves;
        for (unsigned int i = 0; i < 1000; ++i)
        {
            moves.emplace_back(PlayerUnitCommand(UnitId(i), PlayerUnitCommand::IssueOrder(
                MoveOrder(SimVector(100_ss, 0_ss, 200_ss)), PlayerUnitCommand::IssueOrder::Immediate)));
        }
        network.submitCommands(SceneTime(123), moves);
        network.start();
        REQUIRE(peer.receiveUntil([&](const auto& message, auto size) {
            if (message.command_set_size() == 0) return false;
            REQUIRE(size > 1500);
            REQUIRE(size <= 65507);
            REQUIRE(message.current_scene_time() == 123);
            REQUIRE(message.next_command_set_to_send() == 0);
            REQUIRE(message.command_set_size() == 1);
            const auto decoded = deserializeCommandSet(message.command_set(0));
            REQUIRE(decoded.size() == moves.size());
            for (unsigned int i = 0; i < decoded.size(); ++i)
            {
                const auto& command = std::get<PlayerUnitCommand>(decoded[i]);
                REQUIRE(command.unit == UnitId(i));
                const auto& issue = std::get<PlayerUnitCommand::IssueOrder>(command.command);
                REQUIRE(issue.issueKind == PlayerUnitCommand::IssueOrder::Immediate);
                REQUIRE((std::get<MoveOrder>(issue.order).destination == SimVector(100_ss, 0_ss, 200_ss)));
            }
            return true;
        }));
        REQUIRE(network.getStatus() == GameNetworkService::Status::Running);
    }

    TEST_CASE("Game network sends bounded prefixes and drains a prolonged command backlog in order", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        constexpr unsigned int ticks = 2000;
        for (unsigned int i = 0; i < ticks; ++i)
        {
            network.submitCommands(SceneTime(i), {PlayerUnitCommand(UnitId(i), PlayerUnitCommand::Stop{})});
            network.submitGameHash(GameHash(i));
        }
        network.start();
        unsigned int nextCommand = 0;
        unsigned int nextHash = 0;
        REQUIRE(peer.receiveUntil([&](const auto& update, auto size) {
            REQUIRE(size <= 1500);
            if (update.next_command_set_to_send() != nextCommand || update.next_game_hash_to_send() != nextHash) return false;
            for (const auto& set : update.command_set())
            {
                const auto decoded = deserializeCommandSet(set);
                REQUIRE(decoded.size() == 1);
                REQUIRE(std::get<PlayerUnitCommand>(decoded[0]).unit == UnitId(nextCommand++));
            }
            for (const auto hash : update.game_hashes()) REQUIRE(hash == nextHash++);
            proto::NetworkMessage ack;
            auto& message = *ack.mutable_game_update();
            message.set_player_id(1);
            message.set_next_command_set_to_receive(nextCommand);
            message.set_next_game_hash_to_receive(nextHash);
            peer.send(ack, network.getLocalPort());
            return nextCommand == ticks && nextHash == ticks;
        }));
        REQUIRE(network.getStatus() == GameNetworkService::Status::Running);
    }

    TEST_CASE("Game network idle heartbeat ACKs do not accumulate the age of an old RTT sample", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        unsigned int heartbeats = 0;
        REQUIRE(peer.receiveUntil([&](const auto&, auto) {
            proto::NetworkMessage ack;
            ack.mutable_game_update()->set_player_id(1);
            peer.send(ack, network.getLocalPort());
            return ++heartbeats == 12;
        }));
        // Loopback latency is tiny compared with 1.1 s of heartbeats. Before
        // consuming RTT samples, idle ACKs repeatedly measured the initial
        // packet's age and drove the EMA above 400 ms here.
        REQUIRE(network.getMaxAverageRttMillis() < 250.0f);
        REQUIRE(network.getStatus() == GameNetworkService::Status::Running);
    }

    TEST_CASE("Game network reports an indivisible command set beyond UDP capacity", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        GameNetworkService::CommandSet oversized(40000, PlayerPauseGameCommand{});
        network.submitCommands(SceneTime(0), oversized);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Failed; }));
        REQUIRE(network.getFailureReason().find("UDP payload limit") != std::string::npos);
        assertResponsive(network);
    }

    TEST_CASE("Game network tolerates reordered and duplicated peer updates", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        commands.registerPlayer(PlayerId(1));
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));

        auto updateWithSets = [&](unsigned int firstSet, unsigned int lastSet) {
            proto::NetworkMessage message;
            auto& update = *message.mutable_game_update();
            update.set_player_id(1);
            update.set_next_command_set_to_send(firstSet);
            for (unsigned int i = firstSet; i <= lastSet; ++i)
            {
                auto& set = *update.add_command_set();
                serializePlayerCommand(PlayerCommand(PlayerUnitCommand(UnitId(i), PlayerUnitCommand::Stop{})), *set.add_command());
            }
            peer.send(std::move(message), network.getLocalPort());
        };

        updateWithSets(0, 2);
        REQUIRE(eventually([&]() { return commands.bufferedCommandCount(PlayerId(1)) == 3; }));

        // duplicated packets must not re-deliver already received sets
        updateWithSets(0, 2);
        updateWithSets(0, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(commands.bufferedCommandCount(PlayerId(1)) == 3);

        // a packet starting past the expected sequence is dropped whole;
        // the retransmitted prefix then delivers the missing sets in order
        updateWithSets(5, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(commands.bufferedCommandCount(PlayerId(1)) == 3);
        updateWithSets(3, 5);
        REQUIRE(eventually([&]() { return commands.bufferedCommandCount(PlayerId(1)) == 6; }));

        // a retransmit overlapping the tail is suppressed as well
        updateWithSets(3, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(commands.bufferedCommandCount(PlayerId(1)) == 6);

        for (unsigned int expected = 0; expected < 6; ++expected)
        {
            const auto batch = commands.tryPopCommands();
            REQUIRE(batch.has_value());
            REQUIRE(batch->size() == 1);
            const auto& [player, set] = batch->front();
            REQUIRE(player == PlayerId(1));
            REQUIRE(set.size() == 1);
            REQUIRE(std::get<PlayerUnitCommand>(set.front()).unit == UnitId(expected));
        }
        REQUIRE(commands.bufferedCommandCount(PlayerId(1)) == 0);
        network.stop();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);
    }

    TEST_CASE("Game network ignores stale and overrun acknowledgements", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));
        for (unsigned int i = 0; i < 3; ++i)
        {
            network.submitCommands(SceneTime(0), {PlayerUnitCommand(UnitId(i), PlayerUnitCommand::Stop{})});
        }

        REQUIRE(peer.receiveUntil([&](const auto& update, auto) {
            return update.next_command_set_to_send() == 0 && update.command_set_size() == 3;
        }));

        auto ack = [&](unsigned int nextReceive) {
            proto::NetworkMessage message;
            auto& update = *message.mutable_game_update();
            update.set_player_id(1);
            update.set_next_command_set_to_receive(nextReceive);
            peer.send(std::move(message), network.getLocalPort());
        };

        ack(3);  // drain the backlog; a stale duplicate ack must not rewind it
        ack(1);

        network.submitCommands(SceneTime(1), {PlayerUnitCommand(UnitId(3), PlayerUnitCommand::Stop{})});

        REQUIRE(peer.receiveUntil([&](const auto& update, auto) {
            if (update.command_set_size() != 1)
            {
                return false;
            }
            REQUIRE(update.next_command_set_to_send() == 3);
            const auto decoded = deserializeCommandSet(update.command_set(0));
            REQUIRE(decoded.size() == 1);
            REQUIRE(std::get<PlayerUnitCommand>(decoded.front()).unit == UnitId(3));
            return true;
        }));

        // an ack beyond everything sent logs an error and stays bounded;
        // the sequence number must never rewind and the service survives
        ack(99);
        REQUIRE(peer.receiveUntil([&](const auto& update, auto) {
            return update.next_command_set_to_send() >= 3;
        }));
        REQUIRE(network.getStatus() == GameNetworkService::Status::Running);
        network.stop();
    }

    TEST_CASE("Game network preserves remote hash order under retransmit and detects mismatch", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        commands.registerPlayer(PlayerId(0)); // local human
        commands.registerPlayer(PlayerId(1)); // remote human
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));

        auto updateWithHashes = [&](unsigned int firstHash, std::initializer_list<unsigned int> hashes) {
            proto::NetworkMessage message;
            auto& update = *message.mutable_game_update();
            update.set_player_id(1);
            update.set_next_game_hash_to_send(firstHash);
            for (const auto hash : hashes)
            {
                update.add_game_hashes(hash);
            }
            peer.send(std::move(message), network.getLocalPort());
        };

        updateWithHashes(0, {10u, 11u});
        REQUIRE(eventually([&]() { return commands.bufferedHashCount(PlayerId(1)) == 2; }));

        updateWithHashes(0, {10u, 11u}); // duplicated packet
        updateWithHashes(5, {99u});      // gapped stream position -- dropped
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE(commands.bufferedHashCount(PlayerId(1)) == 2);

        updateWithHashes(2, {12u, 13u});
        REQUIRE(eventually([&]() { return commands.bufferedHashCount(PlayerId(1)) == 4; }));

        for (unsigned int h : {10u, 11u, 12u, 13u})
        {
            commands.pushHash(PlayerId(0), GameHash(h));
        }
        REQUIRE(commands.checkHashes());
        REQUIRE(commands.bufferedHashCount(PlayerId(1)) == 0);

        // a divergent remote hash is caught on the next aligned row
        updateWithHashes(4, {500u});
        commands.pushHash(PlayerId(0), GameHash(14));
        REQUIRE(eventually([&]() { return commands.bufferedHashCount(PlayerId(1)) == 1; }));
        REQUIRE_FALSE(commands.checkHashes());

        network.stop();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);
    }

    TEST_CASE("Game network drops submissions after stop without reviving the executor", "[network][followup]")
    {
        Peer peer;
        PlayerCommandService commands;
        GameNetworkService network(PlayerId(0), 0, endpointFor(peer), &commands);
        network.start();
        REQUIRE(eventually([&]() { return network.getStatus() == GameNetworkService::Status::Running; }));
        network.stop();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);

        for (unsigned int i = 0; i < 100; ++i)
        {
            network.submitCommands(SceneTime(i), {PlayerUnitCommand(UnitId(i), PlayerUnitCommand::Stop{})});
            network.submitGameHash(GameHash(i));
        }
        assertResponsive(network);
        network.start();
        REQUIRE(network.getStatus() == GameNetworkService::Status::Stopped);

        // any datagram sent before the stop has had ample time to land;
        // after draining, a stopped service must transmit nothing further
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::array<char, 2048> buffer;
        asio::ip::udp::endpoint sender;
        asio::error_code error;
        while (peer.socket.receive_from(asio::buffer(buffer), sender, 0, error) != 0 || (!error))
        {
            error.clear();
        }
        REQUIRE((error == asio::error::would_block || error == asio::error::try_again));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto n = peer.socket.receive_from(asio::buffer(buffer), sender, 0, error);
        REQUIRE(error);
        REQUIRE(n == 0);
    }
}
