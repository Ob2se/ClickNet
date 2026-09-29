#include "ClickNetWire.h"
#include "ClickNetTrace.h"
#include "ClickNetInputChannel.h"
#include <gamenetworkingsockets/steam/steamnetworkingsockets.h>
#include <gamenetworkingsockets/steam/isteamnetworkingsockets.h>
#include <gamenetworkingsockets/steam/isteamnetworkingutils.h>
#include <gamenetworkingsockets/steam/steamnetworkingtypes.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto TickInterval = std::chrono::microseconds(16667);
constexpr float Pi = 3.14159265358979323846f;
std::atomic<bool> stopRequested{false};

void StopOnSignal(int) { stopRequested.store(true, std::memory_order_relaxed); }

struct Options
{
    std::string host = "127.0.0.1";
    uint16_t port = 27015;
    uint32_t bots = 100;
    uint32_t rampPerSecond = 20;
    uint32_t durationSeconds = 120;
    std::string pattern = "spread";
    float spreadSeconds = 12.0f;
    float turnSeconds = 3.0f;
    float jumpEverySeconds = 0.0f;
    float targetX = 0.0f, targetY = 0.0f;
    std::filesystem::path mapPath;
    std::filesystem::path csvPath = "load-results.csv";
    bool dryRun = false;
};

bool ParseUInt(const char* text, uint32_t& out)
{
    if (!text || !*text || *text == '-') return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (*end || value > std::numeric_limits<uint32_t>::max()) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool ParseFloat(const char* text, float& out)
{
    if (!text) return false;
    char* end = nullptr;
    out = std::strtof(text, &end);
    return end != text && *end == '\0' && std::isfinite(out);
}

void PrintUsage()
{
    std::cout << "ClickNet headless load generator (wire protocol v"
              << static_cast<int>(clicknet::wire::Version) << ")\n"
              << "Usage: ClickNetLoadTest.exe [options]\n"
              << "  --host IPv4-or-IPv6    default 127.0.0.1\n"
              << "  --port N               default 27015\n"
              << "  --bots N               default 100, max 100000\n"
              << "  --ramp N               new connections per second, default 20\n"
              << "  --duration N           total run seconds including ramp, default 120\n"
              << "  --pattern idle|steady|spread|cluster|wander|target  default spread\n"
              << "  --spread-seconds N     travel time before stopping, default 12\n"
              << "  --turn-seconds N       direction change period, default 3\n"
              << "  --jump-every N        jump when grounded about every N seconds; 0 disables\n"
              << "  --target X Y           world-space meters, used by target pattern\n"
              << "  --map path.box3d       verify server level hash\n"
              << "  --csv path             output, default load-results.csv\n"
              << "  --dry-run              validate arguments without networking\n"
              << "  --help                 show this help\n";
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help") { PrintUsage(); return false; }
        if (arg == "--dry-run") { options.dryRun = true; continue; }
        if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << '\n'; return false; }
        const char* value = argv[++i];
        uint32_t number = 0;
        if (arg == "--host") options.host = value;
        else if (arg == "--port" && ParseUInt(value, number) && number > 0 && number <= 65535)
            options.port = static_cast<uint16_t>(number);
        else if (arg == "--bots" && ParseUInt(value, number) && number > 0 && number <= 100000)
            options.bots = number;
        else if (arg == "--ramp" && ParseUInt(value, number) && number > 0 && number <= 10000)
            options.rampPerSecond = number;
        else if (arg == "--duration" && ParseUInt(value, number) && number > 0 && number <= 86400)
            options.durationSeconds = number;
        else if (arg == "--pattern") options.pattern = value;
        else if (arg == "--spread-seconds" && ParseFloat(value, options.spreadSeconds)
            && options.spreadSeconds >= 0.0f && options.spreadSeconds <= 3600.0f) {}
        else if (arg == "--turn-seconds" && ParseFloat(value, options.turnSeconds)
            && options.turnSeconds >= 0.2f && options.turnSeconds <= 3600.0f) {}
        else if (arg == "--jump-every" && ParseFloat(value, options.jumpEverySeconds)
            && options.jumpEverySeconds >= 0.0f && options.jumpEverySeconds <= 60.0f) {}
        else if (arg == "--target" && i + 1 < argc
            && ParseFloat(value, options.targetX) && ParseFloat(argv[i + 1], options.targetY)) ++i;
        else if (arg == "--map") options.mapPath = value;
        else if (arg == "--csv") options.csvPath = value;
        else { std::cerr << "Invalid option or value: " << arg << '\n'; return false; }
    }
    if (options.pattern != "idle" && options.pattern != "steady" && options.pattern != "spread"
        && options.pattern != "cluster" && options.pattern != "wander" && options.pattern != "target")
    {
        std::cerr << "Unknown pattern: " << options.pattern << '\n';
        return false;
    }
    SteamNetworkingIPAddr address{};
    if (!address.ParseString(options.host.c_str()))
    {
        std::cerr << "--host requires a numeric IPv4 or IPv6 address\n";
        return false;
    }
    return true;
}

bool HashFile(const std::filesystem::path& path, uint64_t& hash)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    hash = 14695981039346656037ull;
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0)
        for (std::streamsize i = 0; i < input.gcount(); ++i)
        {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    return !input.bad();
}

struct Bot
{
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    bool connected = false;
    bool welcomed = false;
    bool failed = false;
    uint32_t playerId = 0;
    uint32_t lastOwnServerTick = 0;
    uint32_t inputTick = 0;
    uint32_t acknowledgedInputTick = 0;
    uint32_t commandLeadTicks = 4;
    uint32_t lastSendTick = 0;
    clicknet::InputSender inputSender;
    float lastX = 0, lastY = 0, lastYaw = 0;
    bool sentInput = false;
    float x = 0, y = 0;
    bool hasPosition = false;
    bool grounded = false;
    float nextJumpAt = 0.0f;
    float targetHeading = 0;
    bool hasTargetHeading = false;
    Clock::time_point connectedAt{}, lastOwnStateAt{}, lastAnyMessageAt{};
    uint64_t payloadIn = 0, payloadOut = 0;
    uint64_t messagesIn = 0, messagesOut = 0;
};

class LoadTest
{
public:
    explicit LoadTest(Options options, uint64_t expectedHash)
        : options_(std::move(options)), expectedHash_(expectedHash), bots_(options_.bots) {}

    bool Init()
    {
        SteamDatagramErrMsg error{};
        if (!GameNetworkingSockets_Init(nullptr, error))
        {
            std::cerr << "GameNetworkingSockets_Init: " << error << '\n';
            return false;
        }
        initialized_ = true;
        sockets_ = SteamNetworkingSockets();
        if (!sockets_) return false;
        pollGroup_ = sockets_->CreatePollGroup();
        if (pollGroup_ == k_HSteamNetPollGroup_Invalid) return false;
        address_.Clear();
        if (!address_.ParseString(options_.host.c_str())) return false;
        address_.m_port = options_.port;
        csv_.open(options_.csvPath, std::ios::binary | std::ios::trunc);
        if (!timingTrace_.Open(std::filesystem::path(options_.csvPath).parent_path(), "bots"))
            std::cerr << "Could not open bot timing trace\n";
        if (!csv_) { std::cerr << "Cannot write " << options_.csvPath << '\n'; return false; }
        csv_ << "elapsed_s,attempted,connected,welcomed,failed,stale_own_states,"
                "payload_in_Bps,payload_out_Bps,transport_in_Bps,transport_out_Bps,"
                "average_ping_ms,jumps_sent,bot_late_ticks,send_errors,protocol_errors,expired_commands\n";
        instance_ = this;
        return true;
    }

    void Run()
    {
        const auto start = Clock::now();
        auto nextTick = start;
        auto nextReport = start + std::chrono::seconds(1);
        uint64_t previousIn = 0, previousOut = 0;
        auto previousReportAt = start;
        std::cout << "Generating " << options_.bots << " connections at " << options_.rampPerSecond
                  << "/s to " << options_.host << ':' << options_.port << ", pattern=" << options_.pattern
                  << ". Press Ctrl+C to stop.\n";
        while (!stopRequested.load(std::memory_order_relaxed))
        {
            const auto now = Clock::now();
            const float elapsed = std::chrono::duration<float>(now - start).count();
            if (elapsed >= options_.durationSeconds) break;
            sockets_->RunCallbacks();
            ReceiveAll();
            timingTrace_.Row("bot_receive_work", 0, 0, 0, 0, 0, 0,
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
            timingTrace_.Flush();
            const uint32_t desired = std::min(options_.bots,
                static_cast<uint32_t>(elapsed * options_.rampPerSecond));
            for (uint32_t added = 0; attempted_ < desired && added < 32; ++added)
                ConnectBot(attempted_++);
            if (now >= nextTick)
            {
                if (now - nextTick > TickInterval) ++lateLoopTicks_;
                // Preserve elapsed command time when receive/report work skips
                // a generator frame. Send the current command once, not a burst
                // of commands stamped progressively further behind the server.
                const uint32_t elapsedTicks = static_cast<uint32_t>((now - nextTick) / TickInterval) + 1;
                for (uint32_t i = 0; i < attempted_; ++i)
                    if (bots_[i].connected && bots_[i].welcomed)
                        SendInput(i, std::chrono::duration<float>(now - bots_[i].connectedAt).count(), elapsedTicks);
                nextTick += TickInterval * elapsedTicks;
            }
            if (now >= nextReport)
            {
                Report(elapsed, previousIn, previousOut, previousReportAt);
                nextReport = now + std::chrono::seconds(1);
            }
            timingTrace_.Row("bot_loop_wall", 0, 0, 0, 0, 0, 0,
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
            std::this_thread::sleep_until(std::min(nextTick, nextReport));
        }
        const float elapsed = std::chrono::duration<float>(Clock::now() - start).count();
        Report(elapsed, previousIn, previousOut, previousReportAt);
        std::cout << "Done. CSV: " << std::filesystem::absolute(options_.csvPath).string() << '\n';
    }

    void Shutdown()
    {
        if (sockets_)
        {
            for (auto& bot : bots_)
                if (bot.connection != k_HSteamNetConnection_Invalid)
                    sockets_->CloseConnection(bot.connection, 0, "Load test finished", false);
            if (pollGroup_ != k_HSteamNetPollGroup_Invalid)
                sockets_->DestroyPollGroup(pollGroup_);
        }
        instance_ = nullptr;
        sockets_ = nullptr;
        if (initialized_) GameNetworkingSockets_Kill();
        initialized_ = false;
    }

private:
    static void StatusCallback(SteamNetConnectionStatusChangedCallback_t* info)
    {
        if (instance_) instance_->OnStatus(info);
    }

    void OnStatus(SteamNetConnectionStatusChangedCallback_t* info)
    {
        const auto found = byConnection_.find(info->m_hConn);
        if (found == byConnection_.end()) return;
        Bot& bot = bots_[found->second];
        switch (info->m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_Connected:
            if (!sockets_->SetConnectionPollGroup(info->m_hConn, pollGroup_))
            {
                bot.failed = true;
                sockets_->CloseConnection(info->m_hConn, 0, "Poll group failed", false);
                bot.connection = k_HSteamNetConnection_Invalid;
                byConnection_.erase(found);
                break;
            }
            bot.connected = true;
            bot.connectedAt = Clock::now();
            bot.lastAnyMessageAt = bot.connectedAt;
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            bot.connected = false;
            bot.failed = true;
            bot.connection = k_HSteamNetConnection_Invalid;
            sockets_->CloseConnection(info->m_hConn, 0, nullptr, false);
            byConnection_.erase(found);
            break;
        default: break;
        }
    }

    void ConnectBot(uint32_t index)
    {
        SteamNetworkingConfigValue_t callback{};
        callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            reinterpret_cast<void*>(StatusCallback));
        Bot& bot = bots_[index];
        bot.connection = sockets_->ConnectByIPAddress(address_, 1, &callback);
        if (bot.connection == k_HSteamNetConnection_Invalid) bot.failed = true;
        else byConnection_[bot.connection] = index;
    }

    void ReceiveAll()
    {
        ISteamNetworkingMessage* messages[256]{};
        int count = 0;
        for (int batch = 0; batch < 64; ++batch)
        {
            count = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, messages, 256);
            if (count <= 0) break;
            for (int i = 0; i < count; ++i)
            {
                ISteamNetworkingMessage* message = messages[i];
                const auto found = byConnection_.find(message->m_conn);
                if (found != byConnection_.end())
                {
                    Bot& bot = bots_[found->second];
                    bot.payloadIn += message->m_cbSize;
                    ++bot.messagesIn;
                    bot.lastAnyMessageAt = Clock::now();
                    const auto type = clicknet::wire::MessageType(message->m_pData, message->m_cbSize);
                    if (type == clicknet::wire::Type::Welcome)
                    {
                        clicknet::wire::Welcome welcome{};
                        if (!clicknet::wire::Decode(message->m_pData, message->m_cbSize, welcome)
                            || (expectedHash_ && welcome.levelHash != expectedHash_))
                        {
                            ++protocolErrors_;
                            bot.failed = true;
                            bot.connected = false;
                            sockets_->CloseConnection(bot.connection, 0, "Invalid welcome or level", false);
                            bot.connection = k_HSteamNetConnection_Invalid;
                            byConnection_.erase(found);
                        }
                        else if (!bot.welcomed)
                        {
                            bot.welcomed = true;
                            bot.playerId = welcome.playerId;
                            bot.lastOwnStateAt = Clock::now();
                            const std::string name = "LoadBot-" + std::to_string(found->second + 1);
                            Send(bot, clicknet::wire::Encode(clicknet::wire::PlayerName{name}),
                                k_nSteamNetworkingSend_Reliable);
                        }
                    }
                    else if (type == clicknet::wire::Type::PlayerState
                        || type == clicknet::wire::Type::PlayerStateBatch)
                    {
                        clicknet::wire::PlayerStateBatch states;
                        if (type == clicknet::wire::Type::PlayerState)
                        {
                            clicknet::wire::PlayerState state{};
                            if (clicknet::wire::Decode(message->m_pData, message->m_cbSize, state))
                                states.states.push_back(state);
                            else ++protocolErrors_;
                        }
                        else if (!clicknet::wire::Decode(message->m_pData, message->m_cbSize, states))
                            ++protocolErrors_;
                        if (bot.welcomed)
                        for (const clicknet::wire::PlayerState& state : states.states)
                        {
                            if (state.playerId == bot.playerId
                                && state.serverTick > bot.lastOwnServerTick)
                            {
                                if (clicknet::TraceSample(bot.playerId))
                                    timingTrace_.Row("snapshot_receive", bot.playerId, bot.playerId, 0,
                                        state.serverTick, state.acknowledgedInputTick, state.receivedInputTick,
                                        SteamNetworkingUtils()->GetLocalTimestamp() - message->m_usecTimeReceived,
                                        int64_t(bot.inputTick) - state.acknowledgedInputTick);
                                bot.lastOwnServerTick = state.serverTick;
                                bot.inputSender.Acknowledge(state.inputAckSequence, state.inputAckBits);
                                // Bots have no local prediction to replay. Keep
                                // their command clock near the server timeline
                                // when the generator itself runs a loop late.
                                bot.acknowledgedInputTick = state.acknowledgedInputTick;
                                bot.inputTick = std::max(bot.inputTick, state.acknowledgedInputTick + bot.commandLeadTicks);
                                bot.x = state.px; bot.y = state.py;
                                bot.hasPosition = true;
                                bot.grounded = state.grounded;
                                bot.lastOwnStateAt = Clock::now();
                                if (!bot.hasTargetHeading)
                                {
                                    bot.targetHeading = std::atan2(options_.targetY - bot.y,
                                        options_.targetX - bot.x);
                                    bot.hasTargetHeading = true;
                                }
                            }
                        }
                    }
                    else if (type == clicknet::wire::Type::BodyState)
                    {
                        clicknet::wire::BodyState state{};
                        if (!clicknet::wire::Decode(message->m_pData, message->m_cbSize, state)) ++protocolErrors_;
                    }
                    else if (type == clicknet::wire::Type::DespawnPlayer)
                    {
                        clicknet::wire::DespawnPlayer state{};
                        if (!clicknet::wire::Decode(message->m_pData, message->m_cbSize, state)) ++protocolErrors_;
                    }
                    else ++protocolErrors_;
                }
                message->Release();
            }
        }
    }

    bool Send(Bot& bot, const std::vector<uint8_t>& bytes, int flags)
    {
        if (bytes.empty() || bot.connection == k_HSteamNetConnection_Invalid) return false;
        if (sockets_->SendMessageToConnection(bot.connection, bytes.data(),
            static_cast<uint32_t>(bytes.size()), flags, nullptr) == k_EResultOK)
        {
            bot.payloadOut += bytes.size();
            ++bot.messagesOut;
            return true;
        }
        ++sendErrors_;
        return false;
    }

    void SendInput(uint32_t index, float botAge, uint32_t elapsedTicks)
    {
        Bot& bot = bots_[index];
        bot.inputTick += elapsedTicks;
        if (bot.lastOwnServerTick != 0)
        {
            const auto sinceSnapshot = Clock::now() - bot.lastOwnStateAt;
            const uint32_t snapshotAgeTicks = static_cast<uint32_t>(sinceSnapshot / TickInterval);
            // The acknowledgement is already one network trip old. Allow
            // another trip for this command plus two scheduling ticks.
            // Re-anchor from each owner update rather than accumulating a
            // permanent lead if the server briefly runs below 60 Hz.
            bot.inputTick = std::max(bot.lastSendTick + 1,
                bot.acknowledgedInputTick + snapshotAgeTicks + bot.commandLeadTicks);
        }
        const float phase = (index * 0.61803398875f - std::floor(index * 0.61803398875f)) * 2.0f * Pi;
        float angle = phase;
        float speed = 1.0f;
        if (options_.pattern == "idle") speed = 0.0f;
        else if (options_.pattern == "steady") angle = 0.0f;
        else if (options_.pattern == "spread")
        {
            if (botAge > options_.spreadSeconds) speed = 0.0f;
        }
        else if (options_.pattern == "cluster")
        {
            if (static_cast<int>(botAge / options_.turnSeconds) % 2) angle += Pi;
        }
        else if (options_.pattern == "wander")
        {
            angle += static_cast<float>(static_cast<int>(botAge / options_.turnSeconds)) * 1.618f;
        }
        else if (options_.pattern == "target" && bot.hasPosition)
        {
            const float dx = options_.targetX - bot.x;
            const float dy = options_.targetY - bot.y;
            if (dx * dx + dy * dy > 4.0f) angle = std::atan2(dy, dx);
            else angle = bot.targetHeading;
        }
        const float x = speed * std::cos(angle), y = speed * std::sin(angle);
        const float yaw = speed > 0.0f ? angle : bot.lastYaw;
        bool jump = false;
        if (options_.jumpEverySeconds > 0.0f)
        {
            if (bot.nextJumpAt <= 0.0f)
                bot.nextJumpAt = options_.jumpEverySeconds *
                    (0.5f + 0.5f * (index * 0.61803398875f
                        - std::floor(index * 0.61803398875f)));
            jump = bot.grounded && botAge >= bot.nextJumpAt;
        }
        const bool changed = !bot.sentInput || std::abs(x - bot.lastX) > 0.005f
            || std::abs(y - bot.lastY) > 0.005f
            || std::abs(std::remainder(yaw - bot.lastYaw, 2.0f * Pi)) > 0.02f || jump;
        const bool newCommand = changed || bot.inputTick - bot.lastSendTick >= 60;
        const auto nowUs = clicknet::TraceMonoUs();
        if (!newCommand && !bot.inputSender.RetryDue(nowUs)) return;
        clicknet::wire::Input input{};
        input.tick = bot.inputTick;
        input.moveX = x; input.moveY = y; input.yaw = yaw;
        input.jump = jump;
        if (newCommand) input = bot.inputSender.Queue(input);
        const auto packet = bot.inputSender.Packet();
        bot.inputSender.Attempted(nowUs);
        const bool sent = !packet.commands.empty()
            && Send(bot, clicknet::wire::Encode(packet), k_nSteamNetworkingSend_UnreliableNoNagle);
        if (sent && newCommand && clicknet::TraceSample(bot.playerId))
            timingTrace_.Row("command_send", bot.playerId, 0, input.tick, bot.lastOwnServerTick,
                bot.acknowledgedInputTick, 0, 0, bot.commandLeadTicks);
        if (newCommand && jump)
        {
            ++jumpsSent_;
            bot.grounded = false;
            bot.nextJumpAt = botAge + options_.jumpEverySeconds;
        }
        if (newCommand)
        {
            bot.lastX = x; bot.lastY = y; bot.lastYaw = yaw;
            bot.lastSendTick = bot.inputTick;
            bot.sentInput = true;
        }
    }

    void Report(float elapsed, uint64_t& previousIn, uint64_t& previousOut,
        Clock::time_point& previousReportAt)
    {
        uint32_t connected = 0, welcomed = 0, failed = 0, stale = 0;
        uint64_t payloadIn = 0, payloadOut = 0;
        uint64_t expiredCommands = 0;
        float transportIn = 0, transportOut = 0, pingSum = 0;
        uint32_t pingCount = 0;
        const auto now = Clock::now();
        for (uint32_t i = 0; i < attempted_; ++i)
        {
            Bot& bot = bots_[i];
            connected += bot.connected ? 1 : 0;
            welcomed += bot.welcomed && bot.connected ? 1 : 0;
            failed += bot.failed ? 1 : 0;
            payloadIn += bot.payloadIn;
            payloadOut += bot.payloadOut;
            expiredCommands += bot.inputSender.Expired();
            if (bot.welcomed && bot.connected
                && std::chrono::duration<float>(now - bot.lastOwnStateAt).count() > 2.0f) ++stale;
            if (bot.connected)
            {
                SteamNetConnectionRealTimeStatus_t status{};
                if (sockets_->GetConnectionRealTimeStatus(bot.connection, &status, 0, nullptr) == k_EResultOK)
                {
                    transportIn += status.m_flInBytesPerSec > 0 ? status.m_flInBytesPerSec : 0;
                    transportOut += status.m_flOutBytesPerSec > 0 ? status.m_flOutBytesPerSec : 0;
                    if (status.m_nPing > 0) { pingSum += status.m_nPing; ++pingCount; }
                    bot.commandLeadTicks = std::clamp(
                        static_cast<uint32_t>(std::max(0, status.m_nPing) * 60 + 999) / 1000 + 2u,
                        4u, 30u);
                    if (clicknet::TraceSample(bot.playerId))
                    {
                        timingTrace_.Row("send_queue", bot.playerId, 0, 0, 0, 0, 0, status.m_usecQueueTime);
                        timingTrace_.Row("pending_reliable", bot.playerId, 0, 0, 0, 0, 0, status.m_cbPendingReliable);
                    }
                }
            }
        }
        const float rawReportSeconds = std::chrono::duration<float>(now - previousReportAt).count();
        const float reportSeconds = rawReportSeconds > 0.001f ? rawReportSeconds : 0.001f;
        const float rateIn = (payloadIn - previousIn) / reportSeconds;
        const float rateOut = (payloadOut - previousOut) / reportSeconds;
        previousIn = payloadIn; previousOut = payloadOut;
        previousReportAt = now;
        const float averagePing = pingCount ? pingSum / pingCount : 0.0f;
        std::cout << std::fixed << std::setprecision(1) << elapsed << "s  "
                  << "attempted=" << attempted_ << " connected=" << connected
                  << " welcomed=" << welcomed << " failed=" << failed << " stale=" << stale
                  << "  payload=" << rateIn / 1024.0f << "/" << rateOut / 1024.0f
                  << " KiB/s  wire=" << transportIn / 1024.0f << "/" << transportOut / 1024.0f
                  << " KiB/s  ping=" << averagePing << "ms  jumps=" << jumpsSent_
                  << "  botLate=" << lateLoopTicks_ << " expiredCommands=" << expiredCommands << '\n';
        csv_ << std::fixed << std::setprecision(2) << elapsed << ',' << attempted_ << ','
             << connected << ',' << welcomed << ',' << failed << ',' << stale << ','
             << rateIn << ',' << rateOut << ',' << transportIn << ',' << transportOut << ','
             << averagePing << ',' << jumpsSent_ << ',' << lateLoopTicks_ << ','
             << sendErrors_ << ',' << protocolErrors_ << ',' << expiredCommands << '\n';
        csv_.flush();
    }

    Options options_;
    uint64_t expectedHash_ = 0;
    std::vector<Bot> bots_;
    std::unordered_map<HSteamNetConnection, uint32_t> byConnection_;
    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    SteamNetworkingIPAddr address_{};
    std::ofstream csv_;
    clicknet::TimingTrace timingTrace_;
    uint32_t attempted_ = 0;
    uint64_t sendErrors_ = 0, protocolErrors_ = 0;
    uint64_t lateLoopTicks_ = 0;
    uint64_t jumpsSent_ = 0;
    bool initialized_ = false;
    static LoadTest* instance_;
};
LoadTest* LoadTest::instance_ = nullptr;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--help") { PrintUsage(); return 0; }
    Options options;
    if (!ParseOptions(argc, argv, options)) return 2;
    uint64_t expectedHash = 0;
    if (!options.mapPath.empty() && !HashFile(options.mapPath, expectedHash))
    {
        std::cerr << "Cannot hash map: " << options.mapPath << '\n';
        return 2;
    }
    if (options.dryRun)
    {
        std::cout << "Ready: " << options.bots << " bots at " << options.rampPerSecond
                  << "/s for " << options.durationSeconds << "s; pattern=" << options.pattern << '\n';
        return 0;
    }
    std::signal(SIGINT, StopOnSignal);
    std::signal(SIGTERM, StopOnSignal);
    LoadTest test(std::move(options), expectedHash);
    if (!test.Init()) { test.Shutdown(); return 1; }
    test.Run();
    test.Shutdown();
    return 0;
}
