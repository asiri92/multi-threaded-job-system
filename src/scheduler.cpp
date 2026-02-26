#include "job_system/scheduler.h"

#include <stdexcept>

namespace job_system {

Scheduler::Scheduler() = default;
Scheduler::~Scheduler() = default;

void Scheduler::register_client(const std::string& client_id, size_t weight) {
    if (weight == 0) {
        throw std::invalid_argument("Client weight must be >= 1: " + client_id);
    }
    std::unique_lock lock(registry_mutex_);
    if (clients_.contains(client_id)) {
        throw std::runtime_error("Client already registered: " + client_id);
    }
    clients_.emplace(client_id, std::make_shared<ClientState>(client_id, weight));
    client_order_.push_back(client_id);
}

void Scheduler::submit(const std::string& client_id,
                        std::function<void()> task) {
    std::shared_ptr<ClientState> client;
    {
        std::shared_lock lock(registry_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            throw std::runtime_error("Unknown client: " + client_id);
        }
        client = it->second;
    }

    Job job(client_id, std::move(task));
    job.job_id = next_job_id_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard lock(client->mutex);
        client->queue.push_back(std::move(job));
    }
    client->submitted_count.fetch_add(1, std::memory_order_relaxed);
}

std::optional<Job> Scheduler::select_next_job() {
    std::shared_lock registry_lock(registry_mutex_);
    const size_t n = client_order_.size();
    if (n == 0) return std::nullopt;

    std::lock_guard rr_lock(rr_mutex_);

    for (size_t scanned = 0; scanned < n; ++scanned) {
        auto& client = clients_.at(client_order_[rr_index_]);

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
            return job;
        }

        // Client empty — work-conserving skip
        rr_remaining_ = 0;
        rr_index_ = (rr_index_ + 1) % n;
    }

    return std::nullopt;
}

Scheduler::ClientMetrics Scheduler::get_client_metrics(
    const std::string& client_id) const {
    std::shared_lock lock(registry_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + client_id);
    }

    const auto& client = it->second;
    ClientMetrics metrics;
    metrics.submitted = client->submitted_count.load(std::memory_order_relaxed);
    metrics.executed = client->executed_count.load(std::memory_order_relaxed);

    auto total_us =
        client->total_execution_time_us.load(std::memory_order_relaxed);
    metrics.avg_execution_time_us =
        metrics.executed > 0
            ? static_cast<double>(total_us) / static_cast<double>(metrics.executed)
            : 0.0;

    {
        std::lock_guard client_lock(client->mutex);
        metrics.queue_depth = client->queue.size();
    }
    metrics.weight = client->weight;
    return metrics;
}

uint64_t Scheduler::total_jobs_processed() const {
    return total_processed_.load(std::memory_order_relaxed);
}

void Scheduler::record_execution(const std::string& client_id,
                                  std::chrono::microseconds duration) {
    std::shared_lock lock(registry_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;

    it->second->executed_count.fetch_add(1, std::memory_order_relaxed);
    it->second->total_execution_time_us.fetch_add(
        duration.count(), std::memory_order_relaxed);
    total_processed_.fetch_add(1, std::memory_order_relaxed);
}

bool Scheduler::has_pending_jobs() const {
    std::shared_lock lock(registry_mutex_);
    for (const auto& [_, client] : clients_) {
        std::lock_guard client_lock(client->mutex);
        if (!client->queue.empty()) return true;
    }
    return false;
}

} // namespace job_system
