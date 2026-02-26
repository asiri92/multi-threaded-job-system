#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "job_system/job.h"

namespace job_system {

enum class OverflowStrategy {
    REJECT,      // throw QueueFullException
    BLOCK,       // caller blocks until space is available
    DROP_OLDEST, // evict front of queue to make room
    DROP_NEWEST  // silently discard the incoming job
};

struct ClientState {
    std::string client_id;
    const size_t weight;
    std::deque<Job> queue;
    mutable std::mutex mutex;
    std::condition_variable submit_cv_; // for BLOCK strategy

    // Atomic metrics — readable without locking
    std::atomic<uint64_t> submitted_count{0};
    std::atomic<uint64_t> executed_count{0};
    std::atomic<int64_t>  total_execution_time_us{0}; // microseconds
    std::atomic<uint64_t> overflow_count{0};

    // Backpressure config — set at registration time, const thereafter
    size_t max_queue_depth{0};                         // 0 = unlimited
    OverflowStrategy overflow_strategy{OverflowStrategy::REJECT};

    explicit ClientState(std::string id, size_t w = 1,
                         size_t max_depth = 0,
                         OverflowStrategy strategy = OverflowStrategy::REJECT)
        : client_id(std::move(id))
        , weight(w)
        , max_queue_depth(max_depth)
        , overflow_strategy(strategy) {}

    // Non-copyable, non-movable (contains mutex)
    ClientState(const ClientState&) = delete;
    ClientState& operator=(const ClientState&) = delete;
    ClientState(ClientState&&) = delete;
    ClientState& operator=(ClientState&&) = delete;
};

} // namespace job_system
