#pragma once

#include <asio.hpp>
#include <chrono>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <deque>
#include <network.pb.h>
#include <random>
#include <rwe/game/PlayerCommand.h>
#include <rwe/game/PlayerCommandService.h>
#include <rwe/rwe_time.h>
#include <rwe/sim/GameHash.h>
#include <rwe/sim/GameTime.h>
#include <rwe/sim/PlayerId.h>
#include <rwe/util/OpaqueId.h>
#include <rwe/util/OpaqueUnit.h>

namespace rwe
{
    struct SequenceNumberTag;
    using SequenceNumber = OpaqueUnit<unsigned int, SequenceNumberTag>;

    class GameNetworkService
    {
    public:
        enum class Status { NotStarted, Starting, Running, Stopped, Failed };
        using CommandSet = std::vector<PlayerCommand>;
        struct EndpointInfo
        {
            PlayerId playerId;
            asio::ip::udp::endpoint endpoint;

            SequenceNumber nextCommandToSend{0};
            SequenceNumber nextCommandToReceive{0};

            GameTime nextHashToSend{0};
            GameTime nextHashToReceive{0};

            /**
             * The time at which the last relevant update packet
             * was received from the remote peer.
             * An update packet is relevant
             * if it contains new commands that we haven't seen before.
             */
            std::optional<Timestamp> lastReceiveTime;

            /**
             * The last reported scene time from this peer,
             * adjusted for RTT.
             */
            std::optional<std::pair<SceneTime, Timestamp>> lastKnownSceneTime;

            std::deque<CommandSet> sendBuffer;

            std::deque<GameHash> hashSendBuffer;

            /**
             * Records the time at which we first sent a packet
             * finishing at the given sequence number.
             * This is used for measuring RTT when we receive acks.
             */
            std::deque<std::pair<SequenceNumber, Timestamp>> sendTimes;

            /**
             * Exponential moving average of round trip time
             * for communication between us and the remote peer.
             */
            float averageRoundTripTime{0};

            EndpointInfo(const PlayerId& playerId, const asio::ip::udp::endpoint& endpoint)
                : playerId(playerId), endpoint(endpoint)
            {
            }
        };

    private:
        std::random_device rd;
        std::default_random_engine gen{rd()};
        std::uniform_int_distribution<int> uniform_dist{};
        PlayerId localPlayerId;
        int port;

        std::thread networkThread;
        // Lifecycle calls are serialized; the I/O thread never takes this lock.
        std::mutex lifecycleMutex;
        std::atomic<Status> status{Status::NotStarted};
        std::atomic<unsigned short> localPort{0};
        struct TimingSnapshot
        {
            std::vector<std::pair<SceneTime, Timestamp>> peerSceneTimes;
            float maxRttMillis{0};
        };
        std::shared_ptr<const TimingSnapshot> timingSnapshot{std::make_shared<TimingSnapshot>()};
        std::shared_ptr<const std::string> failureReason{std::make_shared<std::string>()};

        asio::io_context ioContext;
        asio::ip::udp::resolver resolver;
        asio::ip::udp::socket socket;
        asio::steady_timer sendTimer;

        std::vector<EndpointInfo> endpoints;

        // Complete command sets remain atomic. Large selections may need IP
        // fragmentation, but never exceed the portable UDP payload limit.
        static constexpr std::size_t MaxDatagramSize = 65507;
        std::array<char, MaxDatagramSize> sendBuffer;
        std::array<char, MaxDatagramSize> receiveBuffer;
        asio::ip::udp::endpoint currentRemoteEndpoint;

        PlayerCommandService* const playerCommandService;

        SceneTime currentSceneTime{0};

    public:
        GameNetworkService(PlayerId localPlayerId, int port, const std::vector<EndpointInfo>& endpoints, PlayerCommandService* playerCommandService);

        virtual ~GameNetworkService();

        // One-shot lifecycle: repeated start/stop calls are safe; stop is final.
        void start();
        void stop();
        Status getStatus() const { return status.load(); }
        unsigned short getLocalPort() const { return localPort.load(); }
        std::string getFailureReason() const { return *std::atomic_load(&failureReason); }

        /**
         * Submit new information to be sent on the network.
         * @param currentSceneTime The scene time we are currently simulating.
         *                         This is used to inform peers/synchronise simulation speed.
         * @param commands The latest set of player commands.
         *                 These are not related to the current scene time.
         *                 They will be queued up to be sent over the network
         *                 after all the previously submitted commands.
         */
        void submitCommands(SceneTime currentSceneTime, const CommandSet& commands);

        void submitGameHash(GameHash hash);

        SceneTime estimateAvergeSceneTime(SceneTime localSceneTime);

        float getMaxAverageRttMillis();

    private:
        void run();
        void publishTimingSnapshot();
        bool acceptsSubmissions() const;

        void listenForNextMessage();

        void sendLoop();

        void sendToAll();

        void send(EndpointInfo& endpoint);

        void receive(const asio::error_code& error, std::size_t receivedBytes);
    };
}
