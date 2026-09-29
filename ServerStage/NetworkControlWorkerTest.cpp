#include "NetworkControlWorker.h"
#include <atomic>
#include <cassert>
#include <future>

int main()
{
    NetworkControlWorker worker;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<bool> first{true};
    worker.Start([&]
    {
        if (first.exchange(false))
        {
            entered.set_value();
            gate.wait(); // Simulate a lifecycle API stuck on the GNS global lock.
        }
    });
    assert(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    std::vector<int> order;
    std::promise<void> completed;
    const auto simulationThread = std::this_thread::get_id();
    assert(worker.Post([&] { assert(std::this_thread::get_id() != simulationThread); order.push_back(1); }));
    assert(worker.Post([&] { order.push_back(2); completed.set_value(); }));
    auto done = completed.get_future();
    assert(done.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
    // The caller remained free to queue work and progress while the pump blocked.
    release.set_value();
    assert(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    worker.Stop();
    assert((order == std::vector<int>{1, 2}));
    assert(!worker.Post([] {}));
    worker.Start([] {});
    std::promise<void> restarted;
    assert(worker.Post([&] { restarted.set_value(); }));
    assert(restarted.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    worker.Stop();
}
