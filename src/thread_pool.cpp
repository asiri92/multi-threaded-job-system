#include "job_system/thread_pool.h"

#include <chrono>

namespace job_system {

ThreadPool::ThreadPool(Scheduler& scheduler, size_t worker_count)
    : scheduler_(scheduler) {
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
    }
}

ThreadPool::~ThreadPool() {
    if (running_.load(std::memory_order_relaxed)) {
        shutdown();
    }
}

void ThreadPool::shutdown(ShutdownMode mode) {
    if (mode == ShutdownMode::IMMEDIATE) {
        // Drain all pending jobs atomically, then stop workers immediately
        scheduler_.drain_all_clients();
        running_.store(false, std::memory_order_release);
        draining_.store(true, std::memory_order_release);
        cv_.notify_all();
        for (auto& w : workers_) w.request_stop();
        workers_.clear();
        return;
    }

    // GRACEFUL — existing logic: drain queues then stop
    draining_.store(true, std::memory_order_release);
    cv_.notify_all();

    // Spin until all jobs are drained
    while (scheduler_.has_pending_jobs()) {
        cv_.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // All queues empty — tell workers to stop
    running_.store(false, std::memory_order_release);
    cv_.notify_all();

    // Request stop on all jthreads and let destructors join
    for (auto& w : workers_) {
        w.request_stop();
    }
    // jthread destructor calls request_stop + join automatically
    workers_.clear();
}

bool ThreadPool::is_running() const {
    return running_.load(std::memory_order_acquire);
}

size_t ThreadPool::worker_count() const { return workers_.size(); }

void ThreadPool::notify_workers() { cv_.notify_one(); }

void ThreadPool::worker_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        auto job = scheduler_.select_next_job();

        if (!job.has_value()) {
            // No work available
            if (draining_.load(std::memory_order_acquire) &&
                !scheduler_.has_pending_jobs()) {
                // Sleep briefly to allow any BLOCK-strategy submitters that were
                // just notified to push their queued jobs before we exit.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (!scheduler_.has_pending_jobs()) {
                    return; // Truly done
                }
                continue; // Jobs appeared — keep processing
            }

            // Wait for new work or shutdown signal
            std::unique_lock lock(cv_mutex_);
            cv_.wait(lock, stop_token, [this] {
                return draining_.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire);
            });
            continue;
        }

        // Execute the job outside any scheduler/client lock
        const std::string cid = job->client_id;
        const uint64_t jid = job->job_id;

        auto start = std::chrono::steady_clock::now();
        if (job->task) {
            job->task();
        }
        auto end = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        scheduler_.record_execution(cid, jid, duration);
    }
}

} // namespace job_system
