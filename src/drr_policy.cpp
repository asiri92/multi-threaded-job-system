#include "job_system/drr_policy.h"

#include <mutex>

namespace job_system {

DeficitRoundRobinPolicy::DeficitRoundRobinPolicy(uint32_t base_quantum)
    : base_quantum_(base_quantum) {}

void DeficitRoundRobinPolicy::on_client_registered(
    const std::string& client_id, size_t /*weight*/) {
    deficit_[client_id] = 0;
}

std::optional<Job> DeficitRoundRobinPolicy::select_next_job(
    const std::vector<std::string>& client_order,
    const ClientMap& clients) {
    const size_t n = client_order.size();

    for (size_t scanned = 0; scanned < n; ++scanned) {
        const std::string& current = client_order[drr_index_];
        auto& client = clients.at(current);

        std::lock_guard client_lock(client->mutex);

        if (client->queue.empty()) {
            // No carry for idle clients — reset deficit
            deficit_[current] = 0;
            drr_index_ = (drr_index_ + 1) % n;
            continue;
        }

        if (deficit_[current] <= 0) {
            // Refill: weight × base_quantum credits
            deficit_[current] +=
                static_cast<int64_t>(client->weight) *
                static_cast<int64_t>(base_quantum_);
        }

        Job job = std::move(client->queue.front());
        client->queue.pop_front();
        deficit_[current] -= static_cast<int64_t>(job.cost_hint);

        if (deficit_[current] <= 0) {
            // Quota spent — next call starts at next client
            drr_index_ = (drr_index_ + 1) % n;
        }

        client->submit_cv_.notify_one();
        return job;
    }

    return std::nullopt;
}

} // namespace job_system
