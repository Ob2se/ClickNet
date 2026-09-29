#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace clicknet
{
inline std::int64_t TraceMonoUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool TraceSample(std::uint32_t id) { return id != 0 && id % 64 == 0; }

// Single-threaded, buffered diagnostics. Clock domains are explicitly separate:
// wall_us can correlate processes on the SAME machine; mono_us measures local
// intervals. Cross-machine wall-time differences require synchronized clocks.
class TimingTrace
{
public:
    bool Open(const std::filesystem::path& directory, const char* role)
    {
        std::error_code error;
        if (!directory.empty()) std::filesystem::create_directories(directory, error);
        if (error) return false;
        const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        file.open(directory / (std::string("timing-") + role + "-" + std::to_string(wall) + ".csv"),
            std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << "wall_us,mono_us,event,player,peer,command_tick,server_tick,ack_tick,received_tick,value_us,lead_ticks\n";
        lastFlush = TraceMonoUs();
        return true;
    }
    void Row(const char* event, std::uint32_t player, std::uint32_t peer,
        std::uint32_t command, std::uint32_t tick, std::uint32_t ack,
        std::uint32_t received, std::int64_t value = 0, std::int64_t lead = 0)
    {
        if (!file || bytes >= 64 * 1024 * 1024) return;
        const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream row;
        row << wall << ',' << TraceMonoUs() << ',' << event << ',' << player << ',' << peer
            << ',' << command << ',' << tick << ',' << ack << ',' << received << ',' << value << ',' << lead << '\n';
        pending += row.str();
    }
    void Flush(bool force = false)
    {
        const auto now = TraceMonoUs();
        if (!force && now - lastFlush < 1000000) return;
        if (!pending.empty()) { file << pending; file.flush(); bytes += pending.size(); pending.clear(); }
        lastFlush = now;
    }
    ~TimingTrace() { Flush(true); }
private:
    std::ofstream file;
    std::string pending;
    std::int64_t lastFlush = 0;
    std::size_t bytes = 0;
};
}
