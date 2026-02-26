#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "job_system/client_state.h"
#include "job_system/job.h"
#include "job_system/scheduling_policy.h"

namespace job_system {

// Thrown by submit() when a client queue is full and strategy == REJECT
class QueueFullException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Scheduler {
public:
    struct ClientMetrics {
        uint64_t submitted{0};
        uint64_t executed{0};
        double   avg_execution_time_us{0.0};
        size_t   queue_depth{0};
        size_t   weight{1};
        uint64_t overflow_count{0};
    };

    struct GlobalMetrics {
        uint64_t total_processed{0};
        size_t   active_clients{0};
        double   jain_fairness_index{1.0}; // [1/n, 1.0]; 1.0 = perfectly fair
    };

    // Default constructor — uses WeightedRoundRobinPolicy
    Scheduler();

    // Policy constructor — caller supplies any ISchedulingPolicy
    explicit Scheduler(std::unique_ptr<ISchedulingPolicy> policy);

    ~Scheduler();

    // Client management
    void register_client(const std::string& client_id,
                         size_t weight = 1,
                         size_t max_queue_depth = 0,
                         OverflowStrategy strategy = OverflowStrategy::REJECT);

    // Job submission — called by client threads
    void submit(const std::string& client_id, std::function<void()> task,
                uint32_t cost_hint = 1);

    // Job selection — called by worker threads
    // Returns nullopt if no jobs available (caller should wait on CV)
    std::optional<Job> select_next_job();

    // Metrics
    ClientMetrics  get_client_metrics(const std::string& client_id) const;
    GlobalMetrics  get_global_metrics() const;
    uint64_t       total_jobs_processed() const; // backward compat

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
    std::vector<std::string> client_order_; // stable iteration order

    mutable std::mutex rr_mutex_; // protects policy state
    std::unique_ptr<ISchedulingPolicy> policy_;

    std::atomic<uint64_t> next_job_id_{1};
    std::atomic<uint64_t> total_processed_{0};
};

} // namespace job_system
