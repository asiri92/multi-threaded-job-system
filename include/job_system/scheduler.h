#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "job_system/client_state.h"
#include "job_system/job.h"

namespace job_system {

class Scheduler {
public:
    struct ClientMetrics {
        uint64_t submitted{0};
        uint64_t executed{0};
        double avg_execution_time_us{0.0};
        size_t queue_depth{0};
    };

    Scheduler();
    ~Scheduler();

    // Client management
    void register_client(const std::string& client_id);

    // Job submission — called by client threads
    void submit(const std::string& client_id, std::function<void()> task);

    // Job selection — called by worker threads
    // Returns nullopt if no jobs available (caller should wait on CV)
    std::optional<Job> select_next_job();

    // Metrics
    ClientMetrics get_client_metrics(const std::string& client_id) const;
    uint64_t total_jobs_processed() const;

    // Record that a job finished executing (called by workers)
    void record_execution(const std::string& client_id,
                          std::chrono::microseconds duration);

    // State
    bool has_pending_jobs() const;

    // Non-copyable, non-movable
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

private:
    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ClientState>> clients_;
    std::vector<std::string> client_order_; // stable iteration order for RR

    mutable std::mutex rr_mutex_;
    size_t rr_index_{0};

    std::atomic<uint64_t> next_job_id_{1};
    std::atomic<uint64_t> total_processed_{0};
};

} // namespace job_system
