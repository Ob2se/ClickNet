#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

class MotionChangeLog
{
public:
    struct Change { std::uint64_t sequence; std::uint32_t playerId; };
    explicit MotionChangeLog(std::size_t capacity = 65536) : limit(capacity) {}
    std::uint64_t Record(std::uint32_t playerId)
    {
        changes.push_back({++latest, playerId});
        return latest;
    }
    auto FirstAfter(std::uint64_t cursor) const
    {
        return std::upper_bound(changes.begin(), changes.end(), cursor,
            [](std::uint64_t sequence, const Change& change) { return sequence < change.sequence; });
    }
    auto End() const { return changes.end(); }
    bool NeedsRescan(std::uint64_t cursor) const
    {
        return !changes.empty() && cursor + 1 < changes.front().sequence;
    }
    void RetireThrough(std::uint64_t completed)
    {
        while (!changes.empty() && (changes.front().sequence <= completed || changes.size() > limit))
            changes.pop_front();
    }
    void Clear() { changes.clear(); latest = 0; }
private:
    std::deque<Change> changes;
    std::uint64_t latest = 0;
    std::size_t limit;
};
