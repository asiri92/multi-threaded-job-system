#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "job_system/client_state.h"
#include "job_system/job.h"

namespace job_system {

using ClientMap = std::unordered_map<std::string, std::shared_ptr<ClientState>>;

class ISchedulingPolicy {
public:
    virtual ~ISchedulingPolicy() = default;

    // Called under registry write lock when a client is registered
    virtual void on_client_registered(const std::string& client_id,
                                      size_t weight) = 0;

    // Called while Scheduler holds rr_mutex_; policy's internal state is
    // protected by that same lock — no additional synchronization needed
    virtual std::optional<Job> select_next_job(
        const std::vector<std::string>& client_order,
        const ClientMap& clients) = 0;

    // Default no-op — override for time-aware policies
    virtual void on_job_executed(const std::string& /*client_id*/,
                                 std::chrono::microseconds /*duration*/) {}
};

} // namespace job_system
