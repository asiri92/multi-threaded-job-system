#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "job_system/scheduling_policy.h"

namespace job_system {

class DeficitRoundRobinPolicy : public ISchedulingPolicy {
public:
    // base_quantum: credits added per round, scaled by client weight
    explicit DeficitRoundRobinPolicy(uint32_t base_quantum = 100);

    void on_client_registered(const std::string& client_id,
                               size_t weight) override;

    std::optional<Job> select_next_job(
        const std::vector<std::string>& client_order,
        const ClientMap& clients) override;

private:
    uint32_t base_quantum_;
    size_t   drr_index_{0};
    std::unordered_map<std::string, int64_t> deficit_; // credit counters
};

} // namespace job_system
