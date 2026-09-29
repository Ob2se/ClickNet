#include "ReplicationWorkers.h"
#include <cassert>
#include <chrono>
#include <stdexcept>

int main()
{
    ReplicationWorkers workers;
    workers.Start(3);
    // Force all four participants into the same batch to verify real parallelism.
    std::atomic<unsigned> entered{0};
    workers.Run(1000, [&](size_t index)
    {
        if (index >= 4) return;
        entered.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (entered.load() < 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        assert(entered.load() == 4);
    });
    // Successive batches must join completely and execute each recipient once.
    std::vector<unsigned> values(1000, 0);
    for (unsigned tick = 1; tick <= 200; ++tick)
    {
        workers.Run(values.size(), [&](size_t i) { assert(values[i] == tick - 1); ++values[i]; });
        for (unsigned value : values) assert(value == tick);
    }
    workers.Run(0, [](size_t) { assert(false); });
    workers.Run(1, [&](size_t i) { assert(i == 0); });
    bool caught = false;
    try { workers.Run(1000, [](size_t i) { if (i == 40) throw std::runtime_error("test"); }); }
    catch (const std::runtime_error&) { caught = true; }
    assert(caught);
    workers.Run(values.size(), [&](size_t i) { ++values[i]; });
    for (unsigned value : values) assert(value == 201);
    workers.Stop();
    workers.Start(0);
    workers.Run(values.size(), [&](size_t i) { ++values[i]; });
    for (unsigned value : values) assert(value == 202);
    workers.Start(2);
    workers.Run(values.size(), [&](size_t i) { ++values[i]; });
    for (unsigned value : values) assert(value == 203);
}
