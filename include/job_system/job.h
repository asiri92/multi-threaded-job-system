#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace job_system {

struct Job {
    std::string client_id;
    std::function<void()> task;
    std::chrono::steady_clock::time_point enqueue_time;
    uint64_t job_id{0};
    uint32_t cost_hint{1}; // DRR cost unit; default 1 = unit cost (WRR-equivalent)

    Job() = default;

    Job(std::string cid, std::function<void()> t)
        : client_id(std::move(cid))
        , task(std::move(t))
        , enqueue_time(std::chrono::steady_clock::now()) {}

    // Move-only
    Job(Job&&) = default;
    Job& operator=(Job&&) = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
};

} // namespace job_system
