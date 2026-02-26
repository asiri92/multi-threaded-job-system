#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "job_system/job.h"

namespace job_system {

struct ClientState {
    std::string client_id;
    const size_t weight;
    std::deque<Job> queue;
    mutable std::mutex mutex;

    // Atomic metrics — readable without locking
    std::atomic<uint64_t> submitted_count{0};
    std::atomic<uint64_t> executed_count{0};
    std::atomic<int64_t> total_execution_time_us{0}; // microseconds

    explicit ClientState(std::string id, size_t w = 1)
        : client_id(std::move(id)), weight(w) {}

    // Non-copyable, non-movable (contains mutex)
    ClientState(const ClientState&) = delete;
    ClientState& operator=(const ClientState&) = delete;
    ClientState(ClientState&&) = delete;
    ClientState& operator=(ClientState&&) = delete;
};

} // namespace job_system
