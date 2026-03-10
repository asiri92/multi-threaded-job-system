#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "job_system/scheduler.h"

namespace job_system {

enum class ShutdownMode { GRACEFUL, IMMEDIATE };

class ThreadPool {
public:
    explicit ThreadPool(Scheduler& scheduler, size_t worker_count);
    ~ThreadPool();

    // Graceful shutdown: drain all queues, then stop workers.
    // IMMEDIATE shutdown: drain queues atomically then kill workers.
    void shutdown(ShutdownMode mode = ShutdownMode::GRACEFUL);

    bool is_running() const;
    size_t worker_count() const;

    // Called by Scheduler::submit to wake a worker
    void notify_workers();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    void worker_loop(std::stop_token stop_token);

    Scheduler& scheduler_;
    std::vector<std::jthread> workers_;

    std::atomic<bool> running_{true};
    std::atomic<bool> draining_{false}; // shutdown requested, drain remaining

    std::mutex cv_mutex_;
    std::condition_variable_any cv_;
};

} // namespace job_system
