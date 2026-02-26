#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "job_system/drr_policy.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"
#include "job_system/wrr_policy.h"

using namespace job_system;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct RunResult {
    std::string policy_name;
    std::string client_name;
    uint64_t    submitted{0};
    uint64_t    executed{0};
    double      avg_us{0.0};
};

static std::vector<RunResult> run_bench(const std::string& policy_name,
                                         std::unique_ptr<ISchedulingPolicy> policy,
                                         uint32_t wrr_cost) {
    constexpr int FAST_N   = 300;
    constexpr int MEDIUM_N = 300;
    constexpr int SLOW_N   = 300;

    Scheduler scheduler(std::move(policy));
    scheduler.register_client("fast",   1);
    scheduler.register_client("medium", 2);
    scheduler.register_client("slow",   4);

    std::atomic<uint64_t> fast_ns{0}, medium_ns{0}, slow_ns{0};

    auto make_jobs = [&](const std::string& name, int n, uint32_t cost,
                         std::atomic<uint64_t>& ns_acc) {
        for (int i = 0; i < n; ++i) {
            scheduler.submit(name, [&ns_acc, cost] {
                auto t0 = steady_clock::now();
                // Simulate proportional work
                volatile uint64_t x = 1;
                for (uint32_t k = 0; k < cost * 100u; ++k) x += k;
                (void)x;
                auto dt = duration_cast<nanoseconds>(steady_clock::now() - t0);
                ns_acc.fetch_add(static_cast<uint64_t>(dt.count()),
                                 std::memory_order_relaxed);
            }, cost);
        }
    };

    make_jobs("fast",   FAST_N,   wrr_cost,       fast_ns);
    make_jobs("medium", MEDIUM_N, wrr_cost * 10,  medium_ns);
    make_jobs("slow",   SLOW_N,   wrr_cost * 100, slow_ns);

    {
        ThreadPool pool(scheduler, 4);
        pool.shutdown();
    }

    auto to_result = [&](const std::string& cname,
                         int n, std::atomic<uint64_t>& ns_acc) {
        RunResult r;
        r.policy_name  = policy_name;
        r.client_name  = cname;
        r.submitted    = static_cast<uint64_t>(n);
        r.executed     = scheduler.get_client_metrics(cname).executed;
        uint64_t total = ns_acc.load(std::memory_order_relaxed);
        r.avg_us       = r.executed > 0
                             ? static_cast<double>(total) /
                                   (1000.0 * static_cast<double>(r.executed))
                             : 0.0;
        return r;
    };

    return {
        to_result("fast",   FAST_N,   fast_ns),
        to_result("medium", MEDIUM_N, medium_ns),
        to_result("slow",   SLOW_N,   slow_ns),
    };
}

int main() {
    std::cout << "\n=== Mixed Workload Benchmark: WRR vs DRR ===\n\n";

    std::vector<RunResult> all;

    // Run A: WRR with cost_hint=1 for all (WRR ignores cost_hint)
    auto wrr_results = run_bench("WRR",
                                 std::make_unique<WeightedRoundRobinPolicy>(),
                                 /*wrr_cost=*/1);
    for (auto& r : wrr_results) all.push_back(r);

    // Run B: DRR with cost_hint matching simulated sizes
    auto drr_results = run_bench("DRR",
                                 std::make_unique<DeficitRoundRobinPolicy>(50),
                                 /*wrr_cost=*/1);
    for (auto& r : drr_results) all.push_back(r);

    // Table header
    std::cout << std::left
              << std::setw(8)  << "Policy"
              << std::setw(10) << "Client"
              << std::setw(12) << "Submitted"
              << std::setw(12) << "Executed"
              << std::setw(12) << "Avg(us)"
              << "\n";
    std::cout << std::string(54, '-') << "\n";

    for (const auto& r : all) {
        std::cout << std::left
                  << std::setw(8)  << r.policy_name
                  << std::setw(10) << r.client_name
                  << std::setw(12) << r.submitted
                  << std::setw(12) << r.executed
                  << std::setw(12) << std::fixed << std::setprecision(2)
                                   << r.avg_us
                  << "\n";
    }

    return 0;
}
