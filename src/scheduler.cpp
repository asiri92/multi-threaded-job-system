#include "job_system/scheduler.h"

#include <algorithm>
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
                        uint32_t cost_hint,
                        Priority priority,
                        std::chrono::steady_clock::time_point deadline) {
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
    job.priority = priority;
    job.deadline = deadline;

    const uint64_t job_id_snapshot = job.job_id;
    const size_t prio_idx = static_cast<size_t>(priority);

    {
        std::unique_lock client_lock(client->mutex);
        if (client->max_queue_depth > 0) {
            switch (client->overflow_strategy) {
            case OverflowStrategy::REJECT:
                if (client->total_queued() >= client->max_queue_depth) {
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                    throw QueueFullException("Queue full for client: " +
                                             client_id);
                }
                break;
            case OverflowStrategy::BLOCK:
                client->submit_cv_.wait(client_lock, [&] {
                    return client->total_queued() < client->max_queue_depth;
                });
                break;
            case OverflowStrategy::DROP_OLDEST:
                if (client->total_queued() >= client->max_queue_depth) {
                    // Drop oldest job from lowest non-empty priority level
                    for (auto& q : client->queues) {
                        if (!q.empty()) {
                            q.pop_front();
                            break;
                        }
                    }
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                }
                break;
            case OverflowStrategy::DROP_NEWEST:
                if (client->total_queued() >= client->max_queue_depth) {
                    client->overflow_count.fetch_add(1,
                                                     std::memory_order_relaxed);
                    return; // job silently discarded
                }
                break;
            }
        }
        client->queues[prio_idx].push_back(std::move(job));
    }
    client->submitted_count.fetch_add(1, std::memory_order_relaxed);

    if (auto obs = observer_.load(std::memory_order_acquire)) {
        obs->on_job_submitted(client_id, job_id_snapshot);
    }
}

std::optional<Job> Scheduler::select_next_job() {
    std::shared_lock registry_lock(registry_mutex_);
    if (client_order_.empty()) return std::nullopt;

    while (true) {
        std::optional<Job> maybe_job;
        {
            std::lock_guard rr_lock(rr_mutex_);
            maybe_job = policy_->select_next_job(client_order_, clients_);
        }
        if (!maybe_job.has_value()) return std::nullopt;

        Job job = std::move(*maybe_job);
        if (job.is_expired()) {
            auto it = clients_.find(job.client_id);
            if (it != clients_.end()) {
                it->second->expired_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (auto obs = observer_.load(std::memory_order_acquire)) {
                obs->on_job_expired(job.client_id, job.job_id);
            }
            continue;
        }
        return job;
    }
}

bool Scheduler::cancel_job(uint64_t job_id) {
    std::shared_lock registry_lock(registry_mutex_);
    for (const auto& cid : client_order_) {
        auto& client = clients_.at(cid);
        std::lock_guard client_lock(client->mutex);
        for (auto& q : client->queues) {
            for (auto it = q.begin(); it != q.end(); ++it) {
                if (it->job_id == job_id) {
                    const uint64_t jid = it->job_id;
                    q.erase(it);
                    client->submit_cv_.notify_one();
                    if (auto obs = observer_.load(std::memory_order_acquire)) {
                        obs->on_job_cancelled(cid, jid);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t Scheduler::drain_client(const std::string& client_id) {
    std::shared_ptr<ClientState> client;
    {
        std::shared_lock registry_lock(registry_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            throw std::runtime_error("Unknown client: " + client_id);
        }
        client = it->second;
    }

    std::lock_guard client_lock(client->mutex);
    uint64_t count = 0;
    for (auto& q : client->queues) {
        count += static_cast<uint64_t>(q.size());
        q.clear();
    }
    client->submit_cv_.notify_all();
    return count;
}

void Scheduler::drain_all_clients() {
    std::vector<std::string> ids;
    {
        std::shared_lock lock(registry_mutex_);
        ids = client_order_;
    }
    for (const auto& id : ids) {
        try { drain_client(id); } catch (const std::runtime_error&) {}
    }
}

void Scheduler::set_observer(std::shared_ptr<IMetricsObserver> observer) {
    observer_.store(std::move(observer), std::memory_order_release);
}

void Scheduler::update_client_weight(const std::string& client_id,
                                      size_t new_weight) {
    if (new_weight == 0) {
        throw std::invalid_argument("Client weight must be >= 1: " + client_id);
    }
    std::shared_lock registry_lock(registry_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + client_id);
    }
    auto& client = it->second;
    std::lock_guard rr_lock(rr_mutex_);
    client->weight = new_weight;
    policy_->on_client_weight_updated(client_id, new_weight);
}

uint64_t Scheduler::unregister_client(const std::string& client_id) {
    std::unique_lock registry_lock(registry_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        throw std::runtime_error("Unknown client: " + client_id);
    }

    auto& client = it->second;
    uint64_t count = 0;
    {
        std::lock_guard client_lock(client->mutex);
        for (auto& q : client->queues) {
            count += static_cast<uint64_t>(q.size());
            q.clear();
        }
        client->submit_cv_.notify_all();
    }

    {
        std::lock_guard rr_lock(rr_mutex_);
        policy_->on_client_unregistered(client_id);
    }

    clients_.erase(it);
    auto order_it = std::find(client_order_.begin(), client_order_.end(), client_id);
    if (order_it != client_order_.end()) {
        client_order_.erase(order_it);
    }

    return count;
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
        metrics.queue_depth = client->total_queued();
    }
    metrics.weight         = client->weight;
    metrics.overflow_count =
        client->overflow_count.load(std::memory_order_relaxed);
    metrics.expired_count =
        client->expired_count.load(std::memory_order_relaxed);
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
                                  uint64_t job_id,
                                  std::chrono::microseconds duration) {
    std::shared_lock lock(registry_mutex_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;

    it->second->executed_count.fetch_add(1, std::memory_order_relaxed);
    it->second->total_execution_time_us.fetch_add(duration.count(),
                                                   std::memory_order_relaxed);
    total_processed_.fetch_add(1, std::memory_order_relaxed);

    if (auto obs = observer_.load(std::memory_order_acquire)) {
        obs->on_job_executed(client_id, job_id, duration);
    }
}

bool Scheduler::has_pending_jobs() const {
    std::shared_lock lock(registry_mutex_);
    for (const auto& [_, client] : clients_) {
        std::lock_guard client_lock(client->mutex);
        if (client->any_queued()) return true;
    }
    return false;
}

} // namespace job_system
