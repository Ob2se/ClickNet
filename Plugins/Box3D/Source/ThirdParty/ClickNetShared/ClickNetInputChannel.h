#pragma once
#include "ClickNetWire.h"
#include <algorithm>
#include <deque>

namespace clicknet
{
struct InputReceiptWindow
{
    std::uint32_t sequence = 0;
    std::uint64_t bits = 0;
    bool Accept(std::uint32_t incoming)
    {
        if (incoming == 0) return false;
        if (incoming > sequence)
        {
            const auto distance = incoming - sequence;
            bits = distance >= 64 ? 1 : (bits << distance) | 1;
            sequence = incoming;
            return true;
        }
        const auto distance = sequence - incoming;
        if (distance >= 64 || (bits & (std::uint64_t(1) << distance))) return false;
        bits |= std::uint64_t(1) << distance;
        return true;
    }
};

class InputSender
{
public:
    wire::Input Queue(wire::Input input)
    {
        input.sequence = ++sequence;
        if (pending.size() == 64) { pending.pop_front(); ++expired; }
        pending.push_back(input);
        return input;
    }
    void Acknowledge(std::uint32_t ack, std::uint64_t bits)
    {
        for (auto it = pending.begin(); it != pending.end();)
        {
            if (it->sequence > ack) { ++it; continue; }
            const auto distance = ack - it->sequence;
            if (distance >= 64) { ++expired; it = pending.erase(it); }
            else if (bits & (std::uint64_t(1) << distance)) it = pending.erase(it);
            else ++it;
        }
    }
    wire::InputPacket Packet() const
    {
        wire::InputPacket packet;
        if (pending.empty()) return packet;
        // Oldest missing commands recover gaps; newest input always gets through.
        const auto count = (std::min)(pending.size(), wire::MaxInputsPerPacket);
        for (std::size_t i = 0; i + 1 < count; ++i) packet.commands.push_back(pending[i]);
        packet.commands.push_back(pending.back());
        return packet;
    }
    bool RetryDue(std::int64_t nowUs) const { return !pending.empty() && nowUs - lastAttempt >= 50000; }
    void Attempted(std::int64_t nowUs) { lastAttempt = nowUs; }
    std::uint64_t Expired() const { return expired; }
    bool PendingThrough(std::uint32_t tick) const
    {
        for (const auto& input : pending) if (input.tick <= tick) return true;
        return false;
    }
private:
    std::deque<wire::Input> pending;
    std::uint32_t sequence = 0;
    std::int64_t lastAttempt = 0;
    std::uint64_t expired = 0;
};
}
