#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

// Commit replication baselines only for messages accepted by the transport.
// A failed or skipped message must leave its state eligible for retry.
class ReplicationOutbox
{
public:
    struct Packet
    {
        std::vector<uint8_t> bytes;
        int flags;
        std::function<void()> accepted;
    };
    void Add(std::vector<uint8_t> bytes, int flags, std::function<void()> accepted)
    {
        packets.push_back({std::move(bytes), flags, std::move(accepted)});
    }
    bool Commit(const std::vector<int64_t>& results)
    {
        bool allAccepted = results.size() == packets.size();
        for (size_t i = 0; i < packets.size(); ++i)
        {
            if (i < results.size() && results[i] > 0) packets[i].accepted();
            else allAccepted = false;
        }
        packets.clear();
        return allAccepted;
    }
    std::vector<Packet> packets;
};
