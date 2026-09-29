#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Connection lifecycle APIs may wait on GNS's global lock. Keep that wait off
// the simulation thread. The pump queues value events; it never edits players.
class NetworkControlWorker
{
public:
    ~NetworkControlWorker() { Stop(); }
    void Start(std::function<void()> pump)
    {
        Stop();
        m_stopping = false;
        m_thread = std::thread([this, pump = std::move(pump)]
        {
            for (;;)
            {
                std::vector<std::function<void()>> jobs;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_ready.wait_for(lock, std::chrono::milliseconds(2),
                        [this] { return m_stopping || !m_jobs.empty(); });
                    if (m_stopping) return;
                    jobs.swap(m_jobs);
                }
                for (auto& job : jobs) job();
                pump();
            }
        });
    }
    bool Post(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) return false;
            m_jobs.push_back(std::move(job));
        }
        m_ready.notify_one();
        return true;
    }
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_one();
        if (m_thread.joinable()) m_thread.join();
        m_jobs.clear();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::vector<std::function<void()>> m_jobs;
    std::thread m_thread;
    bool m_stopping = true;
};
