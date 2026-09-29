#include "ClicknetServer.h"
#include "RemoteMotion.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <unordered_map>
#include <sstream>
#include <thread>
#include <windows.h>

namespace
{
double CpuMilliseconds(bool process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = process ? GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
        : GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    if (!ok) return -1.0;
    const uint64_t ticks = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime)
        + (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
    return ticks / 10000.0;
}
constexpr float TickSeconds = 1.0f / 60.0f;
constexpr uint32_t InputTimeoutTicks = 180;
constexpr float MoveSpeed = 5.0f;
constexpr uint64_t PawnCategory = 1ull << 2;
// Spread visibility scans across ticks so a crowded server does not pause
// every client while it checks every nearby pair in one frame.
constexpr uint32_t RemoteScanSlices = 18;
constexpr size_t MinimumRemoteRecipientsPerTick = 1;

std::string UtcNow()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_s(&utc, &now);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream text;
    for (const unsigned char byte : value)
    {
        if (byte == '"' || byte == '\\') text << '\\' << static_cast<char>(byte);
        else if (byte < 0x20) text << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byte) << std::dec;
        else text << static_cast<char>(byte);
    }
    return text.str();
}

std::string CsvEscape(const std::string& value)
{
    std::string result = "\"";
    for (char ch : value)
    {
        if (ch == '"') result += '"';
        result += ch;
    }
    return result + '"';
}

float NonNegativeRate(float value)
{
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

b3Capsule PlayerCapsule()
{
    b3Capsule capsule{};
    capsule.center1 = b3Vec3{0.0f, 0.0f, -0.55f};
    capsule.center2 = b3Vec3{0.0f, 0.0f, 0.55f};
    capsule.radius = 0.35f;
    return capsule;
}

bool IsNewer(uint32_t tick, uint32_t previous)
{
    return static_cast<int32_t>(tick - previous) > 0;
}

constexpr float InterestCellMeters = 20.0f;
uint64_t CellKey(int32_t x, int32_t y)
{
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
}
int32_t CellCoord(float value)
{
    return static_cast<int32_t>(std::floor(value / InterestCellMeters));
}
float DistanceSquared(float ax, float ay, float az, float bx, float by, float bz)
{
    const float dx = ax - bx, dy = ay - by, dz = az - bz;
    return dx * dx + dy * dy + dz * dz;
}
}

ClicknetServer* ClicknetServer::s_pCallbackInstance = nullptr;

bool ClicknetServer::Init(const std::string& mapPath, b3Pos spawn, const std::filesystem::path& statsPath, float interestRadius,
    unsigned replicationWorkers)
{
    m_interestRadius = interestRadius;
    m_statsPath = statsPath;
    m_historyPath = m_statsPath;
    m_historyPath.replace_extension(".csv");
    std::error_code directoryError;
    std::filesystem::create_directories(m_statsPath.parent_path(), directoryError);
    if (directoryError)
    {
        std::cerr << "Could not create bandwidth stats directory: " << directoryError.message() << '\n';
        return false;
    }
    if (!m_timingTrace.Open(m_statsPath.parent_path(), "server"))
        std::cerr << "Could not open server timing trace\n";
    std::string error;
    if (!m_world.LoadFile(mapPath, error))
    {
        std::cerr << "Failed to load Box3D map '" << mapPath << "': " << error << '\n';
        return false;
    }
    m_spawn = spawn;
    std::cout << "Loaded " << m_world.Shapes().size() << " Box3D shapes from " << mapPath
              << " (hash " << m_world.LevelHash() << ")\n";

    SteamDatagramErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg))
    {
        std::cerr << "GameNetworkingSockets_Init failed: " << errMsg << '\n';
        return false;
    }
    m_bGNSInitialized = true;
    m_pInterface = SteamNetworkingSockets();
    if (!m_pInterface)
    {
        std::cerr << "Failed to get SteamNetworkingSockets interface\n";
        return false;
    }

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = 27015;
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
    s_pCallbackInstance = this;
    m_hListenSock = m_pInterface->CreateListenSocketIP(addr, 1, &option);
    if (m_hListenSock == k_HSteamListenSocket_Invalid)
    {
        std::cerr << "Failed to listen on port 27015\n";
        return false;
    }
    m_hPollGroup = m_pInterface->CreatePollGroup();
    if (m_hPollGroup == k_HSteamNetPollGroup_Invalid)
    {
        std::cerr << "Failed to create poll group\n";
        return false;
    }
    try { m_replicationWorkers.Start((std::min)(replicationWorkers, 16u)); }
    catch (const std::exception& error)
    {
        std::cerr << "Could not start replication workers: " << error.what() << '\n';
        return false;
    }
    std::cout << "Replication: " << m_replicationWorkers.Count() << " workers plus main thread\n";
    try { m_networkControl.Start([this] { m_pInterface->RunCallbacks(); }); }
    catch (const std::exception& error)
    {
        std::cerr << "Could not start network control thread: " << error.what() << '\n';
        return false;
    }
    return true;
}

void ClicknetServer::Run(std::atomic<bool>& shutdownRequested)
{
    using clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::microseconds(16667);
    auto nextTick = clock::now();
    auto lastPhysicsTime = nextTick - interval;
    double physicsAccumulator = 0.0;
    auto nextStats = clock::now();
    auto nextHistory = clock::now() + std::chrono::seconds(1);
    WriteBandwidthSnapshot();
    while (!shutdownRequested.load(std::memory_order_relaxed))
    {
        const auto tickStart = clock::now();
        const double threadCpuStart = CpuMilliseconds(false);
        const double processCpuStart = CpuMilliseconds(true);
        ProcessConnectionEvents();
        const auto callbacksEnd = clock::now();
        PollIncomingMessages();
        const auto receiveEnd = clock::now();
        CollectBandwidthSamples();
        const auto networkEnd = clock::now();
        physicsAccumulator = (std::min)(0.1, physicsAccumulator
            + std::chrono::duration<double>(tickStart - lastPhysicsTime).count());
        lastPhysicsTime = tickStart;
        m_lastMoverWorkMs = 0.0f;
        m_lastWorldStepMs = 0.0f;
        m_lastInputReplaySteps = 0;
        int physicsSteps = 0;
        while (physicsAccumulator >= TickSeconds && physicsSteps < 3)
        {
            TickPhysics(TickSeconds);
            physicsAccumulator -= TickSeconds;
            ++physicsSteps;
        }
        if (physicsSteps == 0) m_physicsEnd = networkEnd;
        else
        {
            // Leave headroom before the next 16.7 ms step. The remote budget
            // shrinks automatically when physics or input work takes longer.
            m_replicationDeadline = tickStart + std::chrono::milliseconds(14);
            SendStateUpdates();
        }
        const auto replicationEnd = clock::now();
        const auto sampleTime = clock::now();
        if (sampleTime >= nextStats)
        {
            WriteBandwidthSnapshot();
            nextStats = sampleTime + std::chrono::milliseconds(250);
        }
        if (sampleTime >= nextHistory)
        {
            AppendBandwidthHistory();
            nextHistory = sampleTime + std::chrono::seconds(1);
        }
        m_timingTrace.Flush();
        const auto workEnd = clock::now();
        const double threadCpuEnd = CpuMilliseconds(false);
        const double processCpuEnd = CpuMilliseconds(true);
        m_lastThreadCpuMs = threadCpuStart < 0 || threadCpuEnd < 0 ? -1 : threadCpuEnd - threadCpuStart;
        m_lastProcessCpuMs = processCpuStart < 0 || processCpuEnd < 0 ? -1 : processCpuEnd - processCpuStart;
        m_lastNetworkWorkMs = std::chrono::duration<float, std::milli>(networkEnd - tickStart).count();
        m_lastReplicationWorkMs = std::chrono::duration<float, std::milli>(replicationEnd - m_physicsEnd).count();
        m_lastStatsWorkMs = std::chrono::duration<float, std::milli>(workEnd - sampleTime).count();
        m_lastTickWorkMs = std::chrono::duration<float, std::milli>(workEnd - tickStart).count();
        m_timingTrace.Row("loop_wall", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastTickWorkMs * 1000));
        m_timingTrace.Row("wake_delay", 0, 0, 0, m_serverTick, 0, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(tickStart - nextTick).count());
        m_timingTrace.Row("loop_thread_cpu", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastThreadCpuMs * 1000));
        m_timingTrace.Row("loop_process_cpu", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastProcessCpuMs * 1000));
        // These phase samples describe this exact loop, including rare stalls.
        m_timingTrace.Row("phase_network", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastNetworkWorkMs * 1000));
        m_timingTrace.Row("phase_callbacks", 0, 0, 0, m_serverTick, 0, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(callbacksEnd - tickStart).count());
        m_timingTrace.Row("phase_receive", 0, 0, 0, m_serverTick, 0, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(receiveEnd - callbacksEnd).count());
        m_timingTrace.Row("phase_bandwidth_queries", 0, 0, 0, m_serverTick, 0, 0,
            std::chrono::duration_cast<std::chrono::microseconds>(networkEnd - receiveEnd).count());
        const auto controlUs = m_lastConnectionControlUs.exchange(0);
        if (controlUs) m_timingTrace.Row("connection_control", 0, 0, 0, m_serverTick, 0, 0, controlUs);
        m_timingTrace.Row("phase_mover", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastMoverWorkMs * 1000));
        m_timingTrace.Row("phase_world", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastWorldStepMs * 1000));
        m_timingTrace.Row("phase_replication", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastReplicationWorkMs * 1000));
        m_timingTrace.Row("phase_stats", 0, 0, 0, m_serverTick, physicsSteps, 0, int64_t(m_lastStatsWorkMs * 1000));
        if (m_lastTickWorkMs > m_peakTickWorkMs) m_peakTickWorkMs = m_lastTickWorkMs;
        nextTick += interval;
        if (nextTick < workEnd)
        {
            ++m_lateTickCount;
            nextTick = workEnd;
        }
        std::this_thread::sleep_until(nextTick);
    }
}

void ClicknetServer::Shutdown()
{
    m_networkControl.Stop();
    m_replicationWorkers.Stop();
    AppendBandwidthHistory();
    WriteBandwidthSnapshot(false);
    m_players.clear(); // Proxy bodies must die before their world.
    m_statsConnections.clear();
    m_statsCursor = 0;
    m_replicationCursor = 0;
    m_motionSequence = 0;
    if (m_pInterface)
    {
        if (m_hPollGroup != k_HSteamNetPollGroup_Invalid)
        {
            m_pInterface->DestroyPollGroup(m_hPollGroup);
            m_hPollGroup = k_HSteamNetPollGroup_Invalid;
        }
        if (m_hListenSock != k_HSteamListenSocket_Invalid)
        {
            m_pInterface->CloseListenSocket(m_hListenSock);
            m_hListenSock = k_HSteamListenSocket_Invalid;
        }
    }
    s_pCallbackInstance = nullptr;
    m_pInterface = nullptr;
    if (m_bGNSInitialized)
    {
        GameNetworkingSockets_Kill();
        m_bGNSInitialized = false;
    }
}

void ClicknetServer::PollIncomingMessages()
{
    if (!m_pInterface || m_hPollGroup == k_HSteamNetPollGroup_Invalid) return;
    ISteamNetworkingMessage* messages[32]{};
    int count;
    while ((count = m_pInterface->ReceiveMessagesOnPollGroup(m_hPollGroup, messages, 32)) > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            ISteamNetworkingMessage* message = messages[i];
            auto found = m_players.find(message->m_conn);
            if (found != m_players.end())
            {
                found->second.payloadBytesIn += static_cast<uint64_t>(message->m_cbSize);
                ++found->second.messagesIn;



                clicknet::wire::PlayerName playerName;
                if (clicknet::wire::Decode(message->m_pData, static_cast<size_t>(message->m_cbSize), playerName))
                {
                    if (!playerName.value.empty()) found->second.name = playerName.value;
                    message->Release();
                    continue;
                }

                clicknet::wire::PlayerAppearance appearance;
                if (clicknet::wire::Decode(message->m_pData, static_cast<size_t>(message->m_cbSize), appearance))
                {
                    const uint8_t requested = appearance.value <= MaxAppearanceId
                        ? static_cast<uint8_t>(appearance.value) : 0;   // validate: never trust the client
                    Player& player = found->second;
                    if (player.appearanceId != requested)
                    {
                        player.appearanceId = requested;
                        player.remoteStateDirty = true;
                        player.appearanceVersion++;      // lets replication know to re-announce
                    }
                    message->Release();
                    continue;
                }


                clicknet::wire::InputPacket packet;
                if (clicknet::wire::Decode(message->m_pData, static_cast<size_t>(message->m_cbSize), packet))
                for (auto input : packet.commands)
                {
                    Player& player = found->second;
                    if (!player.inputReceipts.Accept(input.sequence)) continue;
                    const bool newer = input.tick > player.lastReceivedInputTick;
                    const uint32_t originalTick = input.tick;
                    const bool trace = clicknet::TraceSample(player.id) || player.name.rfind("LoadBot-", 0) != 0;
                    const auto receivedAt = clicknet::TraceMonoUs();
                    if (trace)
                        m_timingTrace.Row("command_receive", player.id, 0, input.tick, m_serverTick,
                            player.acknowledgedInputTick, player.lastReceivedInputTick,
                            SteamNetworkingUtils()->GetLocalTimestamp() - message->m_usecTimeReceived,
                            int64_t(input.tick) - player.acknowledgedInputTick);
                    player.lastReceivedInputTick = (std::max)(player.lastReceivedInputTick, input.tick);
                    if (!player.receivedInput || player.ticksSinceInput > InputTimeoutTicks)
                    {
                        player.acknowledgedInputTick = packet.commands.back().tick - 1;
                        player.inputTimeline = InputTimeline{};
                        player.inputHistory.clear();
                        player.rewindInputTick = 0;
                    }
                    player.ticksSinceInput = 0;
                    player.receivedInput = true;
                    if (input.tick <= player.acknowledgedInputTick)
                    {
                        ++m_lateInputCommands;
                        if (player.inputHistory.empty() || input.tick < player.inputHistory.front().tick)
                        {
                            // A paused client can resume outside the bounded
                            // replay window. Apply its latest command next step.
                            ++m_inputHistoryMisses;
                            if (!newer)
                            {
                                if (!input.jump) continue;
                                const auto held = player.inputTimeline.At(player.acknowledgedInputTick + 1);
                                input.moveX = held.moveX; input.moveY = held.moveY; input.yaw = held.yaw;
                            }
                            input.tick = player.acknowledgedInputTick + 1;
                        }
                        else if (player.rewindInputTick == 0 || input.tick < player.rewindInputTick)
                            player.rewindInputTick = input.tick;
                    }
                    player.inputTimeline.Add(input);
                    if (trace)
                    {
                        if (player.tracedCommands.size() >= 64) player.tracedCommands.clear();
                        player.tracedCommands[input.tick] = {originalTick, receivedAt};
                    }
                }

                

            }
            message->Release();


        }
    }
}

void ClicknetServer::CollectBandwidthSamples()
{
    if (!m_pInterface || m_statsConnections.empty()) return;
    // Refresh every connection over about 15 server loops, instead of making
    // one loop query all connections when the monitor snapshot is written.
    const size_t count = (m_statsConnections.size() + 14) / 15;
    for (size_t i = 0; i < count; ++i)
    {
        if (m_statsCursor >= m_statsConnections.size()) m_statsCursor = 0;
        const HSteamNetConnection connection = m_statsConnections[m_statsCursor++];
        const auto found = m_players.find(connection);
        if (found == m_players.end()) continue;
        Player& player = found->second;
        SteamNetConnectionRealTimeStatus_t status{};
        if (m_pInterface->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) == k_EResultOK)
        {
            player.pingMs = status.m_nPing;
            player.inBytesPerSecond = NonNegativeRate(status.m_flInBytesPerSec);
            player.outBytesPerSecond = NonNegativeRate(status.m_flOutBytesPerSec);
            player.inPacketsPerSecond = NonNegativeRate(status.m_flInPacketsPerSec);
            player.outPacketsPerSecond = NonNegativeRate(status.m_flOutPacketsPerSec);
            player.qualityLocal = NonNegativeRate(status.m_flConnectionQualityLocal);
            player.qualityRemote = NonNegativeRate(status.m_flConnectionQualityRemote);
            player.pendingReliableBytes = status.m_cbPendingReliable;
            player.pendingUnreliableBytes = status.m_cbPendingUnreliable;
            player.sendQueueUs = status.m_usecQueueTime;
            if (clicknet::TraceSample(player.id) || player.name.rfind("LoadBot-", 0) != 0)
            {
                m_timingTrace.Row("server_send_queue", player.id, 0, 0, m_serverTick, 0, 0, status.m_usecQueueTime);
                m_timingTrace.Row("server_pending_reliable", player.id, 0, 0, m_serverTick, 0, 0, status.m_cbPendingReliable);
            }
        }
        else
        {
            player.pingMs = -1;
            player.inBytesPerSecond = player.outBytesPerSecond = 0.0f;
            player.inPacketsPerSecond = player.outPacketsPerSecond = 0.0f;
            player.qualityLocal = player.qualityRemote = 0.0f;
        }
    }
}

void ClicknetServer::TickPhysics(float dt)
{
    const auto moverStart = std::chrono::steady_clock::now();
    ++m_serverTick;
    for (auto& [connection, player] : m_players)
    {
        const bool previouslyGrounded = player.mover->State().grounded;
        ++player.ticksSinceInput;
        const bool fresh = player.receivedInput && player.ticksSinceInput <= InputTimeoutTicks;
        const auto simulateInput = [&](const clicknet::wire::Input& input)
        {
            const float length = std::sqrt(input.moveX * input.moveX + input.moveY * input.moveY);
            const float scale = length > 1.0f ? MoveSpeed / length : MoveSpeed;
            player.mover->Simulate(b3Vec3{input.moveX * scale, input.moveY * scale, 0.0f}, input.jump, dt);
            if (auto trace = player.tracedCommands.find(input.tick); trace != player.tracedCommands.end())
            {
                player.lastTracedAppliedTick = trace->second.originalTick;
                player.lastTracedAppliedAt = clicknet::TraceMonoUs();
                m_timingTrace.Row("command_apply", player.id, 0, trace->second.originalTick,
                    m_serverTick, player.acknowledgedInputTick, player.lastReceivedInputTick,
                    player.lastTracedAppliedAt - trace->second.receivedAt,
                    int64_t(input.tick) - trace->second.originalTick);
                player.tracedCommands.erase(trace);
            }
        };
        if (fresh && player.rewindInputTick != 0)
        {
            auto first = std::find_if(player.inputHistory.begin(), player.inputHistory.end(),
                [&](const Player::InputStep& step) { return step.tick == player.rewindInputTick; });
            if (first != player.inputHistory.end())
            {
                player.mover->ResetState(first->before);
                for (auto step = first; step != player.inputHistory.end(); ++step)
                {
                    step->before = player.mover->State();
                    simulateInput(player.inputTimeline.At(step->tick));
                    ++m_lastInputReplaySteps;
                }
                // Only the current world step advances dynamic bodies. Restore
                // the proxy once after replay, without repeating world impacts.
                const auto corrected = player.mover->State();
                player.mover->ResetState(corrected);
                player.remoteStateDirty = true;
            }
            player.rewindInputTick = 0;
        }
        if (fresh)
        {
            const uint32_t tick = player.acknowledgedInputTick + 1;
            player.inputHistory.push_back({tick, player.mover->State()});
            const auto activeInput = player.inputTimeline.At(tick);
            player.remoteStateDirty |= std::abs(activeInput.moveX - player.lastInput.moveX) > 0.005f
                || std::abs(activeInput.moveY - player.lastInput.moveY) > 0.005f
                || std::abs(std::remainder(activeInput.yaw - player.lastInput.yaw, 6.2831853f)) > 0.02f
                || activeInput.jump;
            player.lastInput = activeInput;
            simulateInput(player.lastInput);
            player.remoteStateDirty |= previouslyGrounded != player.mover->State().grounded;
            player.acknowledgedInputTick = tick;
            if (player.inputHistory.size() > 32) player.inputHistory.pop_front();
            player.inputTimeline.DiscardBefore(player.inputHistory.front().tick);
            continue;
        }
        player.inputHistory.clear();
        player.rewindInputTick = 0;
        player.mover->Simulate(b3Vec3{0.0f, 0.0f, 0.0f}, false, dt);
        player.remoteStateDirty |= previouslyGrounded != player.mover->State().grounded;
    }
    const auto moverEnd = std::chrono::steady_clock::now();
    m_world.Step(dt);
    m_physicsEnd = std::chrono::steady_clock::now();
    m_lastMoverWorkMs += std::chrono::duration<float, std::milli>(moverEnd - moverStart).count();
    m_lastWorldStepMs += std::chrono::duration<float, std::milli>(m_physicsEnd - moverEnd).count();
}

bool ClicknetServer::SendTo(HSteamNetConnection connection, const std::vector<uint8_t>& bytes, int flags)
{
    // Only called on the simulation thread, outside the recipient worker batch.
    const EResult result = m_pInterface->SendMessageToConnection(connection,
        bytes.data(), static_cast<uint32_t>(bytes.size()), flags, nullptr);
    if (result != k_EResultOK && result != k_EResultLimitExceeded)
        std::cerr << "SendMessageToConnection failed: " << static_cast<int>(result) << '\n';
    if (result == k_EResultOK)
    {
        if (auto found = m_players.find(connection); found != m_players.end())
        {
            found->second.payloadBytesOut += bytes.size();
            ++found->second.messagesOut;
        }
    }
    return result == k_EResultOK;
}

bool ClicknetServer::SendBatchTo(HSteamNetConnection connection, ReplicationOutbox& outbox)
{
    if (outbox.packets.empty()) return true;
    std::vector<SteamNetworkingMessage_t*> messages;
    messages.reserve(outbox.packets.size());
    std::vector<int64_t> results(outbox.packets.size(), 0);
    {
        // GNS locks each connection internally. Each recipient belongs to one
        // job, and callbacks/owner sends happen outside this joined batch.
        // Different recipients can therefore submit concurrently.
        for (const auto& packet : outbox.packets)
        {
            auto* message = SteamNetworkingUtils()->AllocateMessage(static_cast<int>(packet.bytes.size()));
            if (!message)
            {
                for (auto* allocated : messages) allocated->Release();
                return outbox.Commit(results);
            }
            std::memcpy(message->m_pData, packet.bytes.data(), packet.bytes.size());
            message->m_conn = connection;
            message->m_nFlags = packet.flags;
            messages.push_back(message);
        }
        // The library takes ownership of all messages, including failed ones.
        m_pInterface->SendMessages(static_cast<int>(messages.size()), messages.data(), results.data(), true);
        Player& target = m_players.at(connection);
        for (size_t i = 0; i < results.size(); ++i)
            if (results[i] > 0)
            {
                target.payloadBytesOut += outbox.packets[i].bytes.size();
                ++target.messagesOut;
            }
    }
    // Each worker exclusively owns its target caches and traffic counters.
    return outbox.Commit(results);
}

void ClicknetServer::WriteBandwidthSnapshot(bool serverRunning)
{
    if (m_statsPath.empty()) return;
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::fixed << std::setprecision(1);
    json << "{\"schemaVersion\":1,\"updatedUtc\":\"" << UtcNow()
         << "\",\"serverRunning\":" << (serverRunning ? "true" : "false")
         << ",\"serverTick\":" << m_serverTick
         << ",\"tickWorkMs\":" << m_lastTickWorkMs
         << ",\"mainThreadCpuMs\":" << m_lastThreadCpuMs
         << ",\"processCpuMs\":" << m_lastProcessCpuMs
         << ",\"peakTickWorkMs\":" << m_peakTickWorkMs
         << ",\"networkWorkMs\":" << m_lastNetworkWorkMs
         << ",\"moverWorkMs\":" << m_lastMoverWorkMs
         << ",\"worldStepMs\":" << m_lastWorldStepMs
         << ",\"replicationWorkMs\":" << m_lastReplicationWorkMs
         << ",\"replicationWorkers\":" << m_replicationWorkers.Count()
         << ",\"remoteRecipients\":" << m_lastRemoteRecipients
         << ",\"replicationBuildWorkerMs\":" << m_lastReplicationBuildMs
         << ",\"replicationSendWorkerMs\":" << m_lastReplicationSendMs
         << ",\"maxRemoteServiceGapTicks\":" << m_lastMaxRemoteServiceGapTicks
         << ",\"statsWorkMs\":" << m_lastStatsWorkMs
         << ",\"lateTickCount\":" << m_lateTickCount
         << ",\"lateInputCommands\":" << m_lateInputCommands
         << ",\"inputReplaySteps\":" << m_lastInputReplaySteps
         << ",\"inputHistoryMisses\":" << m_inputHistoryMisses
         << ",\"playerCount\":" << m_players.size() << ",\"players\":[";
    bool first = true;
    for (const auto& [connection, player] : m_players)
    {
        if (!first) json << ',';
        first = false;
        json << "{\"id\":" << player.id << ",\"name\":\"" << JsonEscape(player.name)
             << "\",\"connection\":" << connection
             << ",\"pingMs\":" << player.pingMs
             << ",\"pendingReliableBytes\":" << player.pendingReliableBytes
             << ",\"pendingUnreliableBytes\":" << player.pendingUnreliableBytes
             << ",\"sendQueueUs\":" << player.sendQueueUs
             << ",\"inBytesPerSecond\":" << player.inBytesPerSecond
             << ",\"outBytesPerSecond\":" << player.outBytesPerSecond
             << ",\"inPacketsPerSecond\":" << player.inPacketsPerSecond
             << ",\"outPacketsPerSecond\":" << player.outPacketsPerSecond
             << ",\"qualityLocal\":" << player.qualityLocal
             << ",\"qualityRemote\":" << player.qualityRemote
             << ",\"payloadBytesIn\":" << player.payloadBytesIn
             << ",\"payloadBytesOut\":" << player.payloadBytesOut
             << ",\"messagesIn\":" << player.messagesIn
             << ",\"messagesOut\":" << player.messagesOut << '}';
    }
    json << "]}\n";

    std::filesystem::path temporary = m_statsPath;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << json.str();
        if (!output)
        {
            if (!m_statsWriteWarningShown) std::cerr << "Could not write bandwidth snapshot: " << temporary << '\n';
            m_statsWriteWarningShown = true;
            return;
        }
    }
    // This is a live, replaceable telemetry snapshot. Forcing durable disk
    // writes on the simulation thread can stall a busy server every sample.
    if (!MoveFileExW(temporary.c_str(), m_statsPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        if (!m_statsWriteWarningShown)
            std::cerr << "Could not publish bandwidth snapshot (Windows error " << GetLastError() << "): "
                      << m_statsPath << '\n';
        m_statsWriteWarningShown = true;
        return;
    }
    m_statsWriteWarningShown = false;
    m_peakTickWorkMs = 0.0f;
}

void ClicknetServer::AppendBandwidthHistory()
{
    if (m_historyPath.empty() || m_players.empty()) return;
    std::error_code error;
    const bool needsHeader = !std::filesystem::exists(m_historyPath, error)
        || std::filesystem::file_size(m_historyPath, error) == 0;
    std::ofstream output(m_historyPath, std::ios::binary | std::ios::app);
    if (!output) return;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(1);
    if (needsHeader)
        output << "utc,player_id,name,connection,ping_ms,in_bytes_per_sec,out_bytes_per_sec,"
                  "payload_bytes_in,payload_bytes_out\n";
    const std::string timestamp = UtcNow();
    for (const auto& [connection, player] : m_players)
    {
        output << timestamp << ',' << player.id << ',' << CsvEscape(player.name) << ',' << connection << ','
               << player.pingMs << ','
               << player.inBytesPerSecond << ','
               << player.outBytesPerSecond << ','
               << player.payloadBytesIn << ',' << player.payloadBytesOut << '\n';
    }
}

void ClicknetServer::SendStateUpdates()
{
    m_lastRemoteRecipients = 0;
    m_lastReplicationBuildMs = m_lastReplicationSendMs = 0.0f;
    m_lastMaxRemoteServiceGapTicks = 0;
    if (m_players.empty()) return;
    ++m_replicationTick;
    using PlayerState = clicknet::wire::PlayerState;
    using BodyState = clicknet::wire::BodyState;
    struct PlayerView { HSteamNetConnection connection; PlayerState state; uint64_t motionSequence; uint8_t appearanceId; uint32_t appearanceVersion; };
    struct BodyView { size_t shapeIndex; BodyState state; };
    const auto makePlayerState = [this](const Player& player)
    {
        const auto& state = player.mover->State();
        PlayerState packet{};
        packet.serverTick = m_serverTick;
        packet.acknowledgedInputTick = player.acknowledgedInputTick;
        packet.receivedInputTick = player.lastReceivedInputTick;
        packet.inputAckSequence = player.inputReceipts.sequence;
        packet.inputAckBits = player.inputReceipts.bits;
        packet.playerId = player.id;
        packet.px = state.position.x; packet.py = state.position.y; packet.pz = state.position.z;
        packet.vx = state.velocity.x; packet.vy = state.velocity.y; packet.vz = state.velocity.z;
        packet.yaw = player.lastInput.yaw;
        packet.grounded = state.grounded;

        return packet;
    };
    std::vector<PlayerView> players;
    players.reserve(m_players.size());
    std::unordered_map<uint32_t, size_t> playerById;
    std::unordered_map<uint64_t, std::vector<size_t>> playerCells;
    for (auto& [connection, player] : m_players)
    {
        PlayerState packet = makePlayerState(player);
        if (player.remoteStateDirty || RemoteMotionNeedsUpdate(player.remoteMotionBaseline, packet))
        {
            player.motionSequence = ++m_motionSequence;
            player.remoteStateDirty = false;
            player.remoteMotionBaseline = packet;
        }
        playerCells[CellKey(CellCoord(packet.px), CellCoord(packet.py))].push_back(players.size());
        playerById.emplace(player.id, players.size());
        players.push_back({connection, packet, player.motionSequence, player.appearanceId, player.appearanceVersion });
    }

    // Bodies are sampled at 20 Hz. A client continues its local Box3D simulation
    // between packets, so only prediction errors and periodic baselines are sent.
    const bool sampleBodies = m_replicationTick % 3 == 0;
    std::vector<BodyView> bodies;
    std::unordered_map<uint64_t, std::vector<size_t>> bodyCells;
    if (sampleBodies)
    {
        const auto& shapes = m_world.Shapes();
        for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex)
        {
            const auto& shape = shapes[shapeIndex];
            if (!shape.dynamic || !b3Body_IsValid(shape.bodyId)) continue;
            const b3Pos position = b3Body_GetPosition(shape.bodyId);
            const b3Quat rotation = b3Body_GetRotation(shape.bodyId);
            const b3Vec3 velocity = b3Body_GetLinearVelocity(shape.bodyId);
            const b3Vec3 angularVelocity = b3Body_GetAngularVelocity(shape.bodyId);
            BodyState packet{};
            packet.serverTick = m_serverTick;
            packet.exportId = shape.exportId;
            packet.px = position.x; packet.py = position.y; packet.pz = position.z;
            packet.qx = rotation.v.x; packet.qy = rotation.v.y; packet.qz = rotation.v.z; packet.qw = rotation.s;
            packet.vx = velocity.x; packet.vy = velocity.y; packet.vz = velocity.z;
            packet.wx = angularVelocity.x; packet.wy = angularVelocity.y; packet.wz = angularVelocity.z;
            bodyCells[CellKey(CellCoord(packet.px), CellCoord(packet.py))].push_back(bodies.size());
            bodies.push_back({shapeIndex, packet});
        }
    }

    const int cellRange = static_cast<int>(std::ceil(m_interestRadius / InterestCellMeters));
    const float radiusSquared = m_interestRadius * m_interestRadius;
    const auto shouldSendPlayerState = [this](Player& target,
        const PlayerState& packet, bool own, bool urgent)
    {
        auto found = target.sentPlayers.find(packet.playerId);
        if (found != target.sentPlayers.end()) found->second.seenTick = m_serverTick;
        if (urgent || found == target.sentPlayers.end()) return true;
        const auto& old = found->second.state;
        const uint32_t age = m_serverTick - found->second.sentTick;
        const float dt = age * TickSeconds;
        const auto predicted = clicknet::wire::PredictPlayerState(old, dt);
        const float positionError = DistanceSquared(packet.px, packet.py, packet.pz,
            predicted.px, predicted.py, predicted.pz);
        const float velocityError = DistanceSquared(packet.vx, packet.vy, packet.vz,
            predicted.vx, predicted.vy, predicted.vz);
        return age >= (own ? 30u : (old.grounded ? 300u : 60u))
            || (age >= 3 && ((own && (old.inputAckSequence != packet.inputAckSequence || old.inputAckBits != packet.inputAckBits))
                || positionError > (own ? 0.03f * 0.03f : 0.12f * 0.12f)
                || velocityError > (own ? 0.15f * 0.15f : 0.4f * 0.4f)
                || old.grounded != packet.grounded
                || std::abs(std::remainder(packet.yaw - old.yaw, 6.2831853f)) > 0.04f));
    };
    // Owner acknowledgements must not wait behind a recipient's remote batch.
    for (auto& [connection, target] : m_players)
    {
        const PlayerState packet = makePlayerState(target);
        if (!shouldSendPlayerState(target, packet, true, false)) continue;
        const bool firstState = target.sentPlayers.find(target.id) == target.sentPlayers.end();
        if (SendTo(connection, clicknet::wire::Encode(packet), firstState
            ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle))
        {
            target.sentPlayers[target.id] = {packet, m_serverTick, m_serverTick, target.motionSequence};
            if (clicknet::TraceSample(target.id) || target.name.rfind("LoadBot-", 0) != 0)
                m_timingTrace.Row("snapshot_send", target.id, target.id, target.lastTracedAppliedTick,
                    packet.serverTick, packet.acknowledgedInputTick, packet.receivedInputTick,
                    target.lastTracedAppliedAt ? clicknet::TraceMonoUs() - target.lastTracedAppliedAt : -1);
        }
    }
    // Rotate recipients so a crowded loop cannot repeatedly favor the same players.
    const auto remoteDeadline = m_replicationDeadline;
    const size_t targetCount = m_statsConnections.size();
    if (m_replicationCursor >= targetCount) m_replicationCursor = 0;
    const size_t startCursor = m_replicationCursor;
    std::vector<uint8_t> remoteProcessed(targetCount, 0);
    std::vector<float> buildTimes(targetCount, 0.0f), sendTimes(targetCount, 0.0f);
    std::vector<uint32_t> serviceGaps(targetCount, 0);
    // Each job exclusively owns one recipient's replication caches. Player and
    // body snapshots, spatial grids and change log remain immutable until join.
    m_replicationWorkers.Run(targetCount, [&](size_t offset)
    {
        const auto buildStart = std::chrono::steady_clock::now();
        const size_t cursor = (startCursor + offset) % targetCount;
        const HSteamNetConnection targetConnection = m_statsConnections[cursor];
        auto targetIt = m_players.find(targetConnection);
        if (targetIt == m_players.end()) return;
        Player& target = targetIt->second;
        const bool remoteAllowed = offset < MinimumRemoteRecipientsPerTick
            || std::chrono::steady_clock::now() < remoteDeadline;
        remoteProcessed[offset] = remoteAllowed ? 1 : 0;
        if (!remoteAllowed && !sampleBodies) return;
        if (remoteAllowed)
        {
            serviceGaps[offset] = target.lastRemoteServiceTick ? m_serverTick - target.lastRemoteServiceTick : 0;
            target.lastRemoteServiceTick = m_serverTick;
        }
        ReplicationOutbox outbox;
        clicknet::wire::PlayerStateBatch reliableBatch, unreliableBatch;
        if (remoteAllowed)
        {
            reliableBatch.states.reserve(clicknet::wire::MaxPlayerStatesPerBatch);
            unreliableBatch.states.reserve(clicknet::wire::MaxPlayerStatesPerBatch);
        }
        const auto flush = [&](clicknet::wire::PlayerStateBatch& batch, int flags)
        {
            if (batch.states.empty()) return;
            auto bytes = clicknet::wire::Encode(batch);
            outbox.Add(std::move(bytes), flags, [&, states = std::move(batch.states)]
            {
                for (const PlayerState& state : states)
                {
                    target.sentPlayers[state.playerId] = {state, m_serverTick, m_serverTick,
                        players[playerById.at(state.playerId)].motionSequence};
                    if (clicknet::TraceSample(state.playerId) && target.name.rfind("LoadBot-", 0) != 0)
                    {
                        std::lock_guard<std::mutex> traceLock(m_replicationTraceMutex);
                        const auto& source = m_players.at(players[playerById.at(state.playerId)].connection);
                        m_timingTrace.Row("snapshot_send", state.playerId, target.id, source.lastTracedAppliedTick,
                            state.serverTick, state.acknowledgedInputTick, state.receivedInputTick,
                            source.lastTracedAppliedAt ? clicknet::TraceMonoUs() - source.lastTracedAppliedAt : -1);
                    }
                }
            });
            batch.states.clear();
            batch.states.reserve(clicknet::wire::MaxPlayerStatesPerBatch);
        };
        const auto sendPlayerState = [&](const PlayerView& source)
        {
            const PlayerState& packet = source.state;
            const uint64_t sequence = source.motionSequence;

            uint32_t& sentVersion = target.sentAppearance[packet.playerId];   // 0 if new
            if (sentVersion != source.appearanceVersion)
                outbox.Add(clicknet::wire::Encode(clicknet::wire::PlayerAppearance{ m_serverTick, packet.playerId, source.appearanceId }),
                    k_nSteamNetworkingSend_Reliable, [&target, id = packet.playerId, version = source.appearanceVersion]
                    { target.sentAppearance[id] = version; });

            auto found = target.sentPlayers.find(packet.playerId);
            const bool urgent = found == target.sentPlayers.end() || found->second.motionSequence < sequence;
            if (!shouldSendPlayerState(target, packet, false, urgent)) return;
            // A new player needs a reliable baseline. Later motion snapshots
            // supersede older ones, so keep them out of the reliable queue.
            auto& batch = found == target.sentPlayers.end() ? reliableBatch : unreliableBatch;
            batch.states.push_back(packet);
            if (batch.states.size() == clicknet::wire::MaxPlayerStatesPerBatch)
                flush(batch, &batch == &reliableBatch ? k_nSteamNetworkingSend_Reliable
                    : k_nSteamNetworkingSend_Unreliable);
        };
        const bool scanRemote = remoteAllowed &&
            (target.lastRemoteScanTick == 0
                || m_replicationTick % RemoteScanSlices == target.id % RemoteScanSlices
                || m_replicationTick - target.lastRemoteScanTick >= RemoteScanSlices);
        if (scanRemote) target.lastRemoteScanTick = m_replicationTick;
        const b3Pos center = target.mover->State().position;
        const int cx = CellCoord(center.x), cy = CellCoord(center.y);
        for (int dx = -cellRange; dx <= cellRange; ++dx)
        for (int dy = -cellRange; dy <= cellRange; ++dy)
        {
            const uint64_t key = CellKey(cx + dx, cy + dy);
            if (remoteAllowed)
            if (const auto cell = playerCells.find(key); cell != playerCells.end())
            for (size_t index : cell->second)
            {
                const PlayerView& source = players[index];
                const PlayerState& packet = source.state;
                if (source.connection == targetConnection) continue;
                // Only the latest source revision matters; no historical-event
                // replay or per-recipient duplicate set is needed.
                if (!scanRemote && source.motionSequence <= target.remoteChangeCursor) continue;
                if (DistanceSquared(center.x, center.y, center.z,
                    packet.px, packet.py, packet.pz) > radiusSquared) continue;
                sendPlayerState(source);
            }
            if (sampleBodies)
            if (const auto cell = bodyCells.find(key); cell != bodyCells.end())
            for (size_t index : cell->second)
            {
                const BodyView& body = bodies[index];
                const BodyState& packet = body.state;
                if (DistanceSquared(center.x, center.y, center.z,
                    packet.px, packet.py, packet.pz) > radiusSquared) continue;
                auto found = target.sentBodies.find(body.shapeIndex);
                if (found != target.sentBodies.end()) found->second.seenTick = m_serverTick;
                bool send = found == target.sentBodies.end();
                if (!send)
                {
                    const auto& old = found->second.state;
                    const uint32_t age = m_serverTick - found->second.sentTick;
                    const float dt = age * TickSeconds;
                    const float positionError = DistanceSquared(packet.px, packet.py, packet.pz,
                        old.px + old.vx * dt, old.py + old.vy * dt, old.pz + old.vz * dt);
                    const float velocityError = DistanceSquared(packet.vx, packet.vy, packet.vz,
                        old.vx, old.vy, old.vz);
                    const float angularVelocityError = DistanceSquared(packet.wx, packet.wy, packet.wz,
                        old.wx, old.wy, old.wz);
                    // Predict angular motion as well as linear motion. Box3D
                    // reports world-space angular velocity in radians/second.
                    const float angularSpeed = std::sqrt(old.wx * old.wx + old.wy * old.wy + old.wz * old.wz);
                    float predictedQx = old.qx, predictedQy = old.qy;
                    float predictedQz = old.qz, predictedQw = old.qw;
                    if (angularSpeed > 0.0001f)
                    {
                        const float half = angularSpeed * dt * 0.5f;
                        const float scale = std::sin(half) / angularSpeed;
                        const float dx = old.wx * scale, dy = old.wy * scale, dz = old.wz * scale;
                        const float dw = std::cos(half);
                        predictedQx = dw * old.qx + dx * old.qw + dy * old.qz - dz * old.qy;
                        predictedQy = dw * old.qy - dx * old.qz + dy * old.qw + dz * old.qx;
                        predictedQz = dw * old.qz + dx * old.qy - dy * old.qx + dz * old.qw;
                        predictedQw = dw * old.qw - dx * old.qx - dy * old.qy - dz * old.qz;
                    }
                    const float rotationDot = std::abs(packet.qx * predictedQx + packet.qy * predictedQy
                        + packet.qz * predictedQz + packet.qw * predictedQw);
                    send = age >= 120
                        || (age >= 3 && (positionError > 0.06f * 0.06f
                            || velocityError > 0.3f * 0.3f
                            || angularVelocityError > 0.4f * 0.4f
                            || rotationDot < 0.998f));
                    // Keep impacts responsive for the player touching a body,
                    // even when the server body's velocity remains predictable.
                    if (DistanceSquared(center.x, center.y, center.z,
                        packet.px, packet.py, packet.pz) <= 3.0f * 3.0f && age >= 6)
                        send = true;
                }
                if (send)
                    outbox.Add(clicknet::wire::Encode(packet), k_nSteamNetworkingSend_Unreliable,
                        [&, index = body.shapeIndex, state = packet]
                        { target.sentBodies[index] = {state, m_serverTick, m_serverTick}; });
            }
        }
        flush(reliableBatch, k_nSteamNetworkingSend_Reliable);
        flush(unreliableBatch, k_nSteamNetworkingSend_Unreliable);
        if (scanRemote)
        for (auto it = target.sentPlayers.begin(); it != target.sentPlayers.end();)
        {
            if (it->first != target.id && it->second.seenTick != m_serverTick)
            {
                outbox.Add(clicknet::wire::Encode(clicknet::wire::DespawnPlayer{it->first, m_serverTick}),
                    k_nSteamNetworkingSend_Reliable, [&target, id = it->first]
                    { target.sentAppearance.erase(id); target.sentPlayers.erase(id); });
                ++it;
            }
            else ++it;
        }
        const auto sendStart = std::chrono::steady_clock::now();
        buildTimes[offset] = std::chrono::duration<float, std::milli>(sendStart - buildStart).count();
        const bool allAccepted = SendBatchTo(targetConnection, outbox);
        sendTimes[offset] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - sendStart).count();
        if (remoteAllowed && allAccepted) target.remoteChangeCursor = m_motionSequence;
        if (sampleBodies)
        for (auto it = target.sentBodies.begin(); it != target.sentBodies.end();)
        {
            if (it->second.seenTick != m_serverTick) it = target.sentBodies.erase(it);
            else ++it;
        }
    }); // No worker may access replication data beyond this join.
    m_lastRemoteRecipients = 0;
    size_t firstSkipped = targetCount;
    for (size_t offset = 0; offset < targetCount; ++offset)
    {
        m_lastRemoteRecipients += remoteProcessed[offset];
        m_lastReplicationBuildMs += buildTimes[offset];
        m_lastReplicationSendMs += sendTimes[offset];
        m_lastMaxRemoteServiceGapTicks = (std::max)(m_lastMaxRemoteServiceGapTicks, serviceGaps[offset]);
        if (!remoteProcessed[offset] && firstSkipped == targetCount) firstSkipped = offset;
    }
    if (targetCount) m_replicationCursor = (startCursor + (firstSkipped == targetCount ? 1 : firstSkipped)) % targetCount;
}

void ClicknetServer::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* info)
{
    if (s_pCallbackInstance) s_pCallbackInstance->QueueConnectionStatusChanged(info);
}

void ClicknetServer::QueueConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    // Executed only by the control thread. GNS lifecycle calls may block on
    // its global lock, but player state is never touched here.
    const auto started = clicknet::TraceMonoUs();
    const HSteamNetConnection connection = info->m_hConn;
    if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting)
    {
        if (m_pInterface->AcceptConnection(connection) != k_EResultOK
            || !m_pInterface->SetConnectionPollGroup(connection, m_hPollGroup))
            m_pInterface->CloseConnection(connection, 0, nullptr, false);
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(m_connectionEventMutex);
            m_connectionEvents.push_back(*info);
        }
        if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer
            || info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
            m_pInterface->CloseConnection(connection, 0, nullptr, false);
    }
    const int64_t elapsed = clicknet::TraceMonoUs() - started;
    auto previous = m_lastConnectionControlUs.load();
    while (previous < elapsed && !m_lastConnectionControlUs.compare_exchange_weak(previous, elapsed)) {}
}

void ClicknetServer::ProcessConnectionEvents()
{
    std::vector<SteamNetConnectionStatusChangedCallback_t> events;
    {
        std::lock_guard<std::mutex> lock(m_connectionEventMutex);
        events.swap(m_connectionEvents);
    }
    for (auto& event : events) OnConnectionStatusChanged(&event);
}

void ClicknetServer::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    if (!m_pInterface) return;
    const HSteamNetConnection connection = info->m_hConn;
    switch (info->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connected:
        if (m_players.find(connection) == m_players.end())
        {
            Player player;
            player.id = m_nextPlayerId++;
            player.remoteChangeCursor = m_motionSequence;
            player.name = "Player " + std::to_string(player.id);
            player.mover = std::make_unique<clicknet::Mover>(m_world, PlayerCapsule(), m_spawn, PawnCategory);
            if (!b3Body_IsValid(player.mover->ProxyBodyId()))
            {
                m_networkControl.Post([this, connection]
                    { m_pInterface->CloseConnection(connection, 0, "Could not spawn player", false); });
                break;
            }
            const uint32_t id = player.id;
            m_players.emplace(connection, std::move(player));
            m_statsConnections.push_back(connection);
            clicknet::wire::Welcome welcome{};
            welcome.playerId = id;
            welcome.levelHash = m_world.LevelHash();
            SendTo(connection, clicknet::wire::Encode(welcome), k_nSteamNetworkingSend_Reliable);
            std::cout << "Player " << id << " connected (" << m_players.size() << " total)\n";
        }
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        m_players.erase(connection);
        if (const auto it = std::find(m_statsConnections.begin(), m_statsConnections.end(), connection);
            it != m_statsConnections.end())
        {
            const size_t index = static_cast<size_t>(it - m_statsConnections.begin());
            m_statsConnections.erase(it);
            if (index < m_statsCursor) --m_statsCursor;
            if (m_statsCursor >= m_statsConnections.size()) m_statsCursor = 0;
            if (index < m_replicationCursor) --m_replicationCursor;
            if (m_replicationCursor >= m_statsConnections.size()) m_replicationCursor = 0;
        }
        break;
    default:
        break;
    }
}
