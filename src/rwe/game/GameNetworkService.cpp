#include "GameNetworkService.h"
#include <algorithm>
#include <rwe/network_util.h>
#include <rwe/proto/serialization.h>
#include <rwe/sim/GameHash.h>
#include <rwe/sim/SimTicksPerSecond.h>
#include <rwe/util/Index.h>
#include <rwe/util/OpaqueId_io.h>
#include <rwe/util/range_util.h>
#include <rwe/util/SimpleLogger.h>
#include <thread>

namespace rwe
{
    GameNetworkService::GameNetworkService(
        PlayerId localPlayerId,
        int port,
        const std::vector<GameNetworkService::EndpointInfo>& endpoints,
        PlayerCommandService* playerCommandService)
        : localPlayerId(localPlayerId),
          port(port),
          resolver(ioContext),
          socket(ioContext),
          sendTimer(ioContext),
          endpoints(endpoints),
          playerCommandService(playerCommandService)
    {
    }

    GameNetworkService::~GameNetworkService()
    {
        stop();
    }

    void GameNetworkService::start()
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (status.load() != Status::NotStarted)
        {
            return;
        }
        if (endpoints.empty())
        {
            // Single-player does not need a socket or an I/O thread. In
            // particular, an unrelated process owning the UDP port must not
            // prevent local simulation from starting.
            status.store(Status::Running);
            return;
        }
        status.store(Status::Starting);
        try
        {
            networkThread = std::thread(&GameNetworkService::run, this);
        }
        catch (...)
        {
            status.store(Status::Failed);
            throw;
        }
    }

    void GameNetworkService::stop()
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        // stop() is thread-safe and wakes run(); no posted handler is needed.
        if (status.load() != Status::Failed)
        {
            status.store(Status::Stopped);
        }
        ioContext.stop();
        if (networkThread.joinable())
        {
            networkThread.join();
        }
        asio::error_code ignored;
        socket.close(ignored);
        // The timer is destroyed with the service after the executor stopped;
        // no cancel handler needs dispatching during shutdown.
    }

    bool GameNetworkService::acceptsSubmissions() const
    {
        const auto state = status.load();
        return state == Status::NotStarted || state == Status::Starting || state == Status::Running;
    }

    void GameNetworkService::submitCommands(SceneTime currentSceneTime, const GameNetworkService::CommandSet& commands)
    {
        // no peers to send to -- skip the post entirely; with endpoints
        // empty the posted lambda only writes fields nothing reads
        if (endpoints.empty() || !acceptsSubmissions())
        {
            return;
        }

        asio::post(ioContext,[this, currentSceneTime, commands]() {
            this->currentSceneTime = currentSceneTime;
            for (auto& e : endpoints)
            {
                e.sendBuffer.push_back(commands);
            }
        });
    }

    void GameNetworkService::submitGameHash(GameHash hash)
    {
        // no peers -- nothing consumes hashSendBuffer
        if (endpoints.empty() || !acceptsSubmissions())
        {
            return;
        }

        asio::post(ioContext,[this, hash]() {
            for (auto& e : endpoints)
            {
                e.hashSendBuffer.push_back(hash);
            }
        });
    }

    SceneTime GameNetworkService::estimateAvergeSceneTime(SceneTime localSceneTime)
    {
        // Never wait for the executor: it may be busy, stopped, or failed.
        if (status.load() != Status::Running)
        {
            return localSceneTime;
        }
        const auto snapshot = std::atomic_load(&timingSnapshot);
        return SceneTime(estimateAverageSceneTimeStatic(localSceneTime, snapshot->peerSceneTimes, getTimestamp()));
    }

    float GameNetworkService::getMaxAverageRttMillis()
    {
        if (status.load() != Status::Running)
        {
            return 0.0f;
        }
        return std::atomic_load(&timingSnapshot)->maxRttMillis;
    }

    void GameNetworkService::publishTimingSnapshot()
    {
        auto snapshot = std::make_shared<TimingSnapshot>();
        for (const auto& e : endpoints)
        {
            if (e.lastKnownSceneTime)
            {
                snapshot->peerSceneTimes.push_back(*e.lastKnownSceneTime);
            }
            snapshot->maxRttMillis = std::max(snapshot->maxRttMillis, e.averageRoundTripTime);
        }
        std::atomic_store(&timingSnapshot, std::shared_ptr<const TimingSnapshot>(std::move(snapshot)));
    }

    void GameNetworkService::run()
    {
        try
        {
            auto endpoint = asio::ip::udp::endpoint(asio::ip::udp::v6(), port);
            socket.open(endpoint.protocol());
            socket.bind(endpoint);
            // BSD/macOS defaults can reject a complete large selection even
            // below the UDP payload maximum unless SO_SNDBUF is enlarged.
            socket.set_option(asio::socket_base::send_buffer_size(MaxDatagramSize));
            socket.set_option(asio::socket_base::receive_buffer_size(MaxDatagramSize * 4));
            // UDP writes must not pin shutdown behind a full kernel buffer.
            socket.non_blocking(true);
            localPort.store(socket.local_endpoint().port());
            auto expected = Status::Starting;
            if (!status.compare_exchange_strong(expected, Status::Running))
            {
                return;
            }

            listenForNextMessage();

            sendLoop();

            ioContext.run();
        }
        catch (const std::exception& e)
        {
            std::atomic_store(&failureReason, std::make_shared<const std::string>(e.what()));
            status.store(Status::Failed);
            LOG_ERROR << "Network thread died with error: " << e.what();
        }
        catch (...)
        {
            std::atomic_store(&failureReason, std::make_shared<const std::string>("Unknown network thread exception"));
            status.store(Status::Failed);
        }
        auto expected = Status::Running;
        status.compare_exchange_strong(expected, Status::Stopped);
    }

    void GameNetworkService::listenForNextMessage()
    {
        socket.async_receive_from(
            asio::buffer(receiveBuffer.data(), receiveBuffer.size()),
            currentRemoteEndpoint,
            [this](const auto& error, const auto& bytesTransferred) {
                if (error == asio::error::operation_aborted || status.load() != Status::Running)
                {
                    return;
                }
                receive(error, bytesTransferred);
                listenForNextMessage();
            });
    }

    proto::NetworkMessage createProtoMessage(
        int packetId,
        PlayerId playerId,
        SceneTime currentSceneTime,
        SequenceNumber nextCommandToSend,
        SequenceNumber nextCommandToReceive,
        GameTime nextHashToSend,
        GameTime nextHashToReceive,
        std::chrono::milliseconds ackDelay,
        const std::deque<GameNetworkService::CommandSet>& sendBuffer,
        const std::deque<GameHash>& gameHashBuffer)
    {
        proto::NetworkMessage outerMessage;
        auto& m = *outerMessage.mutable_game_update();
        m.set_packet_id(packetId);
        m.set_player_id(playerId.value);
        m.set_current_scene_time(currentSceneTime.value);
        m.set_next_command_set_to_send(nextCommandToSend.value);
        m.set_next_command_set_to_receive(nextCommandToReceive.value);
        m.set_next_game_hash_to_send(nextHashToSend.value);
        m.set_next_game_hash_to_receive(nextHashToReceive.value);
        m.set_ack_delay(ackDelay.count());

        constexpr std::size_t maxMessageSize = 65507 - 4;
        constexpr std::size_t normalMessageSize = 1500 - 4;
        for (const auto& set : sendBuffer)
        {
            auto& setMessage = *m.add_command_set();

            for (const auto& cmd : set)
            {
                auto& cmdMessage = *setMessage.add_command();
                serializePlayerCommand(cmd, cmdMessage);
            }
            const auto size = outerMessage.ByteSizeLong();
            if (size > maxMessageSize || (m.command_set_size() > 1 && size > normalMessageSize))
            {
                m.mutable_command_set()->RemoveLast();
                if (m.command_set_size() == 0)
                {
                    throw std::runtime_error("One command set exceeds the UDP payload limit");
                }
                break;
            }
        }

        const auto hashPacketLimit = std::max(normalMessageSize, outerMessage.ByteSizeLong());
        for (const auto& hash : gameHashBuffer)
        {
            m.add_game_hashes(hash.value);
            if (outerMessage.ByteSizeLong() > std::min(maxMessageSize, hashPacketLimit))
            {
                m.mutable_game_hashes()->RemoveLast();
                break;
            }
        }

        return outerMessage;
    }

    void GameNetworkService::sendLoop()
    {
        sendToAll();
        sendTimer.expires_after(std::chrono::milliseconds(100));
        sendTimer.async_wait([this](const asio::error_code& error) {
            if (error)
            {
                LOG_ERROR << "Error while waiting on timer: " << error.message();
                return;
            }

            sendLoop();
        });
    }

    void GameNetworkService::sendToAll()
    {
        for (auto& e : endpoints)
        {
            send(e);
        }
    }

    void GameNetworkService::send(GameNetworkService::EndpointInfo& endpoint)
    {
        auto packetId = uniform_dist(gen);
        LOG_DEBUG << "Sending packet ID " << packetId << " to endpoint: " << endpoint.endpoint.address().to_string() << ":" << endpoint.endpoint.port();
        std::chrono::milliseconds delay(0);
        auto sendTime = getTimestamp();
        if (endpoint.lastReceiveTime)
        {
            delay = std::chrono::duration_cast<std::chrono::milliseconds>(sendTime - *endpoint.lastReceiveTime);
        }

        auto message = createProtoMessage(packetId, localPlayerId, currentSceneTime, endpoint.nextCommandToSend, endpoint.nextCommandToReceive, endpoint.nextHashToSend, endpoint.nextHashToReceive, delay, endpoint.sendBuffer, endpoint.hashSendBuffer);
        auto messageSize = message.ByteSizeLong();
        if (messageSize > getSize(sendBuffer) - 4)
        {
            throw std::runtime_error("Message to be sent was bigger than buffer size");
        }
        if (!message.SerializeToArray(sendBuffer.data(), sendBuffer.size()))
        {
            throw std::runtime_error("Failed to serialize message to buffer");
        }

        // throw in a CRC to verify the message
        writeInt(&sendBuffer[messageSize], computeCrc(sendBuffer.data(), messageSize));

        asio::error_code error;
        socket.send_to(asio::buffer(sendBuffer.data(), messageSize + 4), endpoint.endpoint, 0, error);
        if (error)
        {
            if (error == asio::error::message_size || error == asio::error::bad_descriptor ||
                error == asio::error::invalid_argument || error == asio::error::operation_not_supported)
            {
                throw std::runtime_error("Cannot send game update: " + error.message());
            }
            // Keep queued commands and retry next tick. A transient UDP error
            // must not kill the executor or lose a command set.
            LOG_WARN << "Could not send game update: " << error.message();
            return;
        }

        auto nextSequenceNumber = SequenceNumber(endpoint.nextCommandToSend.value + message.game_update().command_set_size());
        if (endpoint.sendTimes.empty() || endpoint.sendTimes.back().first < nextSequenceNumber)
        {
            endpoint.sendTimes.emplace_back(nextSequenceNumber, sendTime);
        }
    }

    void GameNetworkService::receive(const asio::error_code& error, std::size_t receivedBytes)
    {
        if (error)
        {
            LOG_ERROR << "Error on receive: " << error.message();
            return;
        }

        auto receiveTime = getTimestamp();
        LOG_DEBUG << "Received " << receivedBytes << " bytes from endpoint: " << currentRemoteEndpoint.address().to_string() << ":" << currentRemoteEndpoint.port();

        if (receivedBytes == receiveBuffer.size())
        {
            LOG_WARN << "Received " << receivedBytes << " bytes, which filled the entire message buffer!!";
        }

        if (receivedBytes < 4)
        {
            LOG_ERROR << "Received message is too short (" << receivedBytes << " bytes), ignoring";
            return;
        }

        auto endpointIt = std::find_if(endpoints.begin(), endpoints.end(), [this](const auto& e) { return currentRemoteEndpoint == e.endpoint; });
        if (endpointIt == endpoints.end())
        {
            // message was from some unknown address, ignore it
            LOG_DEBUG << "Unknown address, ignoring";
            return;
        }

        auto receivedCrc = readInt(&receiveBuffer[receivedBytes - 4]);
        auto computedCrc = computeCrc(receiveBuffer.data(), receivedBytes - 4);
        if (receivedCrc != computedCrc)
        {
            LOG_ERROR << "Message CRC incorrect, ignoring";
            return;
        }

        proto::NetworkMessage outerMessage;
        if (!outerMessage.ParseFromArray(receiveBuffer.data(), receivedBytes - 4) || !outerMessage.has_game_update())
        {
            // message wasn't a game update, ignore it
            LOG_DEBUG << "Not game update, ignoring";
            return;
        }

        EndpointInfo& endpoint = *endpointIt;

        const auto& message = outerMessage.game_update();

        LOG_DEBUG << "Packet received with ID " << message.packet_id();
        if (message.player_id() != endpoint.playerId.value)
        {
            LOG_ERROR << "Player " << endpoint.playerId.value << " endpoint sent wrong player ID: " << message.player_id();
            return;
        }

        LOG_DEBUG << "Received ack to " << message.next_command_set_to_receive() << " and " << message.command_set_size() << " commands starting at " << message.next_command_set_to_send();

        SequenceNumber newNextCommandToSend(message.next_command_set_to_receive());
        if (newNextCommandToSend.value > endpoint.nextCommandToSend.value + endpoint.sendBuffer.size())
        {
            LOG_ERROR << "Remote acked up to " << newNextCommandToSend.value << ", but we are at " << endpoint.nextCommandToSend.value << " and command buffer contains " << endpoint.sendBuffer.size() << " elements";
        }
        while (newNextCommandToSend > endpoint.nextCommandToSend && !endpoint.sendBuffer.empty())
        {
            endpoint.sendBuffer.pop_front();
            endpoint.nextCommandToSend = SequenceNumber(endpoint.nextCommandToSend.value + 1);
        }

        while (!endpoint.sendTimes.empty() && endpoint.nextCommandToSend > endpoint.sendTimes.front().first)
        {
            // skip older send time measurements
            endpoint.sendTimes.pop_front();
        }
        if (!endpoint.sendTimes.empty() && endpoint.nextCommandToSend == endpoint.sendTimes.front().first)
        {
            auto roundTripTime = receiveTime - endpoint.sendTimes.front().second;
            auto ackDelay = std::chrono::milliseconds(message.ack_delay());
            roundTripTime = roundTripTime > ackDelay ? roundTripTime - ackDelay : std::chrono::milliseconds(0);
            auto rttMillis = std::chrono::duration_cast<std::chrono::milliseconds>(roundTripTime).count();
            endpoint.averageRoundTripTime = ema(rttMillis, endpoint.averageRoundTripTime, 0.1f);
            // Consume each measurement once. Repeated idle ACKs otherwise
            // measure from the first ever send and inflate RTT indefinitely.
            endpoint.sendTimes.pop_front();
            LOG_DEBUG << "Average RTT: " << endpoint.averageRoundTripTime << "ms";
        }

        auto extraFrames = static_cast<unsigned int>((endpoint.averageRoundTripTime / 2.0f) * SimTicksPerSecond / 1000.0f);
        endpoint.lastKnownSceneTime = std::make_pair(SceneTime(message.current_scene_time() + extraFrames), receiveTime);
        publishTimingSnapshot();
        LOG_DEBUG << "Estimated peer scene time: " << endpoint.lastKnownSceneTime->first.value;

        SequenceNumber firstCommandNumber(message.next_command_set_to_send());
        if (firstCommandNumber > endpoint.nextCommandToReceive)
        {
            // message starts with commands too far in the future, ignore it.
            // FIXME: this should probably be an error as it shouldn't ever happen
            LOG_ERROR << "First command number in message was too high! Expecting no more than " << endpoint.nextCommandToReceive.value << ", received " << firstCommandNumber.value;
            return;
        }

        auto firstRelevantCommandIndex = (endpoint.nextCommandToReceive - firstCommandNumber).value;

        // if the packet is relevant (contains new information), process it
        if (firstRelevantCommandIndex < static_cast<unsigned int>(message.command_set_size()))
        {
            endpoint.lastReceiveTime = receiveTime;

            for (int i = firstRelevantCommandIndex; i < message.command_set_size(); ++i)
            {
                auto commandSet = deserializeCommandSet(message.command_set(i));
                playerCommandService->pushCommands(endpoint.playerId, commandSet);
                endpoint.nextCommandToReceive = SequenceNumber(endpoint.nextCommandToReceive.value + 1);
            }
        }

        GameTime newNextHashToSend(message.next_game_hash_to_receive());
        if (newNextHashToSend > endpoint.nextHashToSend + GameTime(endpoint.hashSendBuffer.size()))
        {
            LOG_ERROR << "Remote acked up to " << newNextHashToSend.value << ", but we are at " << endpoint.nextHashToSend.value << " and hash buffer contains " << endpoint.hashSendBuffer.size() << " elements";
        }
        while (newNextHashToSend > endpoint.nextHashToSend && !endpoint.hashSendBuffer.empty())
        {
            endpoint.hashSendBuffer.pop_front();
            endpoint.nextHashToSend += GameTime(1);
        }

        GameTime firstGameHashTime(message.next_game_hash_to_send());
        if (firstGameHashTime > endpoint.nextHashToReceive)
        {
            // message starts with hashes too far in the future, ignore it.
            // FIXME: this should probably be an error as it shouldn't ever happen
            LOG_ERROR << "First game hash time in message was too high! Expecting no more than " << endpoint.nextHashToReceive.value << ", received " << firstGameHashTime.value;
            return;
        }

        auto firstRelevantGameHashIndex = (endpoint.nextHashToReceive - firstGameHashTime).value;
        for (int i = firstRelevantGameHashIndex; i < message.game_hashes_size(); ++i)
        {
            playerCommandService->pushHash(endpoint.playerId, GameHash(message.game_hashes(i)));
            endpoint.nextHashToReceive += GameTime(1);
        }
    }
}
