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

        if (!client->any_queued()) {
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

        Job job = client->dequeue_highest();
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

void DeficitRoundRobinPolicy::on_client_weight_updated(
    const std::string& client_id, size_t /*new_weight*/) {
    deficit_[client_id] = 0; // avoid inheriting large negative deficit
}

void DeficitRoundRobinPolicy::on_client_unregistered(
    const std::string& client_id) {
    deficit_.erase(client_id);
    drr_index_ = 0;
}

} // namespace job_system
