#include "job_system/scheduler.h"

#include <cmath>
#include <stdexcept>

#include "job_system/wrr_policy.h"

namespace job_system {

Scheduler::Scheduler()
    : Scheduler(std::make_unique<WeightedRoundRobinPolicy>()) {}

Scheduler::Scheduler(std::unique_ptr<ISchedulingPolicy> policy)
    : policy_(std::move(policy)) {}

Scheduler::~Scheduler() = default;

void Scheduler::register_client(const std::string& client_id,
                                 size_t weight,
                                 size_t max_queue_depth,
                                 OverflowStrategy strategy) {
    if (weight == 0) {
        throw std::invalid_argument("Client weight must be >= 1: " + client_id);
    }
    std::unique_lock lock(registry_mutex_);
    if (clients_.contains(client_id)) {
        throw std::runtime_error("Client already registered: " + client_id);
    }
    clients_.emplace(client_id,
                     std::make_shared<ClientState>(client_id, weight,
                                                   max_queue_depth, strategy));
    client_order_.push_back(client_id);
    policy_->on_client_registered(client_id, weight);
}

void Scheduler::submit(const std::string& client_id,
                        std::function<void()> task,
                        uint32_t cost_hint) {
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
    job.cost_hint = cost_hint;

    {
        std::unique_lock client_lock(client->mutex);
        if (client->max_queue_depth > 0) {
            switch (client->overflow_strategy) {
            case OverflowStrategy::REJECT:
                if (client->queue.size() >= client->max_queue_depth) {
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                    throw QueueFullException("Queue full for client: " +
                                             client_id);
                }
                break;
            case OverflowStrategy::BLOCK:
                client->submit_cv_.wait(client_lock, [&] {
                    return client->queue.size() < client->max_queue_depth;
                });
                break;
            case OverflowStrategy::DROP_OLDEST:
                if (client->queue.size() >= client->max_queue_depth) {
                    client->queue.pop_front();
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                }
                break;
            case OverflowStrategy::DROP_NEWEST:
                if (client->queue.size() >= client->max_queue_depth) {
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                    return; // job silently discarded
                }
                break;
            }
        }
        client->queue.push_back(std::move(job));
    }
    client->submitted_count.fetch_add(1, std::memory_order_relaxed);
}

std::optional<Job> Scheduler::select_next_job() {
    std::shared_lock registry_lock(registry_mutex_);
    if (client_order_.empty()) return std::nullopt;
    std::lock_guard rr_lock(rr_mutex_);
    return policy_->select_next_job(client_order_, clients_);
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
    metrics.submitted =
        client->submitted_count.load(std::memory_order_relaxed);
    metrics.executed  =
        client->executed_count.load(std::memory_order_relaxed);

    auto total_us =
        client->total_execution_time_us.load(std::memory_order_relaxed);
    metrics.avg_execution_time_us =
        metrics.executed > 0
            ? static_cast<double>(total_us) /
                  static_cast<double>(metrics.executed)
            : 0.0;

    {
        std::lock_guard client_lock(client->mutex);
        metrics.queue_depth = client->queue.size();
    }
    metrics.weight         = client->weight;
    metrics.overflow_count =
        client->overflow_count.load(std::memory_order_relaxed);
    return metrics;
}

Scheduler::GlobalMetrics Scheduler::get_global_metrics() const {
    // J = (Σxᵢ)² / (n × Σxᵢ²), where xᵢ = executed_count per client
    std::shared_lock lock(registry_mutex_);

    GlobalMetrics gm;
    gm.total_processed = total_processed_.load(std::memory_order_relaxed);
    gm.active_clients  = clients_.size();

    if (clients_.size() < 2) {
        gm.jain_fairness_index = 1.0;
        return gm;
    }

    double sum   = 0.0;
    double sum_sq = 0.0;
    for (const auto& [_, client] : clients_) {
        double x = static_cast<double>(
            client->executed_count.load(std::memory_order_relaxed));
        sum    += x;
        sum_sq += x * x;
    }

    if (sum_sq == 0.0) {
        gm.jain_fairness_index = 1.0;
    } else {
        double n = static_cast<double>(clients_.size());
        gm.jain_fairness_index = (sum * sum) / (n * sum_sq);
    }
    return gm;
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
    it->second->total_execution_time_us.fetch_add(duration.count(),
                                                   std::memory_order_relaxed);
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
