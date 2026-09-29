#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// A synchronous batch over persistent threads. The caller participates and
// returns only after every worker has stopped accessing the batch's data.
class ReplicationWorkers
{
public:
    ~ReplicationWorkers() { Stop(); }
    void Start(unsigned count)
    {
        Stop();
        m_stopping = false;
        m_generation = 0;
        try
        {
            for (unsigned i = 0; i < count; ++i)
                m_threads.emplace_back([this] { Worker(); });
        }
        catch (...) { Stop(); throw; }
    }
    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_ready.notify_all();
        for (auto& thread : m_threads) thread.join();
        m_threads.clear();
    }
    unsigned Count() const { return static_cast<unsigned>(m_threads.size()); }
    void Run(size_t count, std::function<void(size_t)> job)
    {
        if (m_threads.empty() || count < 32)
        {
            for (size_t i = 0; i < count; ++i) job(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_job = std::move(job);
            m_count = count;
            m_next.store(0, std::memory_order_relaxed);
            m_remaining = m_threads.size();
            m_exception = nullptr;
            ++m_generation;
        }
        m_ready.notify_all();
        Drain();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_remaining == 0; });
        m_job = {};
        if (m_exception) std::rethrow_exception(m_exception);
    }
private:
    void Drain()
    {
        for (;;)
        {
            const size_t index = m_next.fetch_add(1, std::memory_order_relaxed);
            if (index >= m_count) return;
            try { m_job(index); }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_exception) m_exception = std::current_exception();
            }
        }
    }
    void Worker()
    {
        size_t generation = 0;
        for (;;)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock, [&] { return m_stopping || m_generation != generation; });
            if (m_stopping) return;
            generation = m_generation;
            lock.unlock();
            Drain();
            lock.lock();
            if (--m_remaining == 0) m_done.notify_one();
        }
    }
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_ready, m_done;
    std::function<void(size_t)> m_job;
    std::atomic<size_t> m_next{0};
    size_t m_count = 0, m_remaining = 0, m_generation = 0;
    bool m_stopping = false;
    std::exception_ptr m_exception;
};
