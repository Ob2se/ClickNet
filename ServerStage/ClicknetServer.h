#pragma once

#include "ClickNetShared.h"
#include "ClickNetWire.h"
#include "InputTimeline.h"
#include "ReplicationOutbox.h"
#include "ClickNetTrace.h"
#include "ClickNetInputChannel.h"
#include "ReplicationWorkers.h"
#include "NetworkControlWorker.h"
#include <gamenetworkingsockets/steam/steamnetworkingsockets.h>
#include <gamenetworkingsockets/steam/isteamnetworkingsockets.h>
#include <gamenetworkingsockets/steam/isteamnetworkingutils.h>
#include <gamenetworkingsockets/steam/steamnetworkingtypes.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <vector>

class ClicknetServer
{
public:
    bool Init(const std::string& mapPath, b3Pos spawn, const std::filesystem::path& statsPath, float interestRadius,
        unsigned replicationWorkers = 3);
    void Run(std::atomic<bool>& shutdownRequested);
    void Shutdown();

private:
    struct Player
    {
        struct SentPlayer { clicknet::wire::PlayerState state{}; uint32_t sentTick = 0; uint32_t seenTick = 0; uint64_t motionSequence = 0; };
        struct SentBody { clicknet::wire::BodyState state{}; uint32_t sentTick = 0; uint32_t seenTick = 0; };
        uint32_t id = 0;
        uint8_t appearanceId = 0;
        uint32_t appearanceVersion = 1;
        std::unordered_map<uint32_t, uint32_t> sentAppearance;
        std::unique_ptr<clicknet::Mover> mover;
        clicknet::wire::Input lastInput{};
        struct InputStep { uint32_t tick; clicknet::MoverState before; };
        InputTimeline inputTimeline;
        std::deque<InputStep> inputHistory;
        uint32_t rewindInputTick = 0;
        uint32_t acknowledgedInputTick = 0;
        uint32_t lastReceivedInputTick = 0;
        bool receivedInput = false;
        clicknet::InputReceiptWindow inputReceipts;
        uint32_t ticksSinceInput = 0;
        bool remoteStateDirty = true;
        clicknet::wire::PlayerState remoteMotionBaseline{};
        uint64_t motionSequence = 0;
        uint64_t remoteChangeCursor = 0;
        uint32_t lastRemoteScanTick = 0;
        uint32_t lastRemoteServiceTick = 0;
        struct TracedCommand { uint32_t originalTick; int64_t receivedAt; };
        std::unordered_map<uint32_t, TracedCommand> tracedCommands;
        uint32_t lastTracedAppliedTick = 0;
        int64_t lastTracedAppliedAt = 0;
        int pendingReliableBytes = 0, pendingUnreliableBytes = 0;
        int64_t sendQueueUs = 0;
        std::unordered_map<uint32_t, SentPlayer> sentPlayers;
        std::unordered_map<size_t, SentBody> sentBodies;
        std::string name;
        uint64_t payloadBytesIn = 0;
        uint64_t payloadBytesOut = 0;
        uint64_t messagesIn = 0;
        uint64_t messagesOut = 0;
        int pingMs = -1;
        float inBytesPerSecond = 0.0f;
        float outBytesPerSecond = 0.0f;
        float inPacketsPerSecond = 0.0f;
        float outPacketsPerSecond = 0.0f;
        float qualityLocal = 0.0f;
        float qualityRemote = 0.0f;
    };

    static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* info);
    void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void QueueConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void ProcessConnectionEvents();
    void PollIncomingMessages();
    void CollectBandwidthSamples();
    void TickPhysics(float dt);
    void SendStateUpdates();
    bool SendTo(HSteamNetConnection connection, const std::vector<uint8_t>& bytes, int flags);
    bool SendBatchTo(HSteamNetConnection connection, ReplicationOutbox& outbox);
    void WriteBandwidthSnapshot(bool serverRunning = true);
    void AppendBandwidthHistory();

    HSteamListenSocket m_hListenSock = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup m_hPollGroup = k_HSteamNetPollGroup_Invalid;
    ISteamNetworkingSockets* m_pInterface = nullptr;
    clicknet::World m_world;
    b3Pos m_spawn{};
    std::unordered_map<HSteamNetConnection, Player> m_players;
    std::vector<HSteamNetConnection> m_statsConnections;
    size_t m_statsCursor = 0;
    size_t m_replicationCursor = 0;
    clicknet::TimingTrace m_timingTrace;
    ReplicationWorkers m_replicationWorkers;
    std::mutex m_connectionEventMutex;
    std::vector<SteamNetConnectionStatusChangedCallback_t> m_connectionEvents;
    std::atomic<int64_t> m_lastConnectionControlUs{0};
    NetworkControlWorker m_networkControl;
    std::mutex m_replicationTraceMutex;
    size_t m_lastRemoteRecipients = 0;
    float m_lastReplicationBuildMs = 0.0f;
    float m_lastReplicationSendMs = 0.0f;
    uint32_t m_lastMaxRemoteServiceGapTicks = 0;
    double m_lastThreadCpuMs = 0, m_lastProcessCpuMs = 0;
    uint64_t m_motionSequence = 0;
    uint32_t m_nextPlayerId = 1;
    uint32_t m_serverTick = 0;
    uint32_t m_replicationTick = 0;
    float m_interestRadius = 40.0f;
    float m_lastTickWorkMs = 0.0f;
    float m_peakTickWorkMs = 0.0f;
    float m_lastNetworkWorkMs = 0.0f;
    float m_lastMoverWorkMs = 0.0f;
    float m_lastWorldStepMs = 0.0f;
    float m_lastReplicationWorkMs = 0.0f;
    float m_lastStatsWorkMs = 0.0f;
    std::chrono::steady_clock::time_point m_physicsEnd{};
    std::chrono::steady_clock::time_point m_replicationDeadline{};
    uint64_t m_lateTickCount = 0;
    uint64_t m_lateInputCommands = 0;
    uint64_t m_inputHistoryMisses = 0;
    uint32_t m_lastInputReplaySteps = 0;
    std::filesystem::path m_statsPath;
    std::filesystem::path m_historyPath;
    bool m_statsWriteWarningShown = false;
    bool m_bGNSInitialized = false;

    static ClicknetServer* s_pCallbackInstance;

    static constexpr std::uint8_t MaxAppearanceId = 1;

};
