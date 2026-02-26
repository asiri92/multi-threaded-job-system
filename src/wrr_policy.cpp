#include "job_system/wrr_policy.h"

namespace job_system {

void WeightedRoundRobinPolicy::on_client_registered(
    const std::string& /*client_id*/, size_t /*weight*/) {
    // WRR reads weight directly from ClientState — nothing to initialise
}

std::optional<Job> WeightedRoundRobinPolicy::select_next_job(
    const std::vector<std::string>& client_order,
    const ClientMap& clients) {
    const size_t n = client_order.size();

    for (size_t scanned = 0; scanned < n; ++scanned) {
        auto& client = clients.at(client_order[rr_index_]);

        // Lazy init / refill quota when we arrive at a new client
        if (rr_remaining_ == 0) {
            rr_remaining_ = client->weight;
        }

        std::lock_guard client_lock(client->mutex);
        if (!client->queue.empty()) {
            Job job = std::move(client->queue.front());
            client->queue.pop_front();
            --rr_remaining_;
            if (rr_remaining_ == 0) {
                rr_index_ = (rr_index_ + 1) % n; // quota exhausted → rotate
            }
            client->submit_cv_.notify_one();
            return job;
        }

        // Client empty — work-conserving skip
        rr_remaining_ = 0;
        rr_index_ = (rr_index_ + 1) % n;
    }

    return std::nullopt;
}

} // namespace job_system
