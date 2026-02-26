#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "job_system/scheduling_policy.h"

namespace job_system {

class WeightedRoundRobinPolicy : public ISchedulingPolicy {
public:
    WeightedRoundRobinPolicy() = default;

    void on_client_registered(const std::string& client_id,
                               size_t weight) override;

    std::optional<Job> select_next_job(
        const std::vector<std::string>& client_order,
        const ClientMap& clients) override;

private:
    size_t rr_index_{0};
    size_t rr_remaining_{0};
};

} // namespace job_system
