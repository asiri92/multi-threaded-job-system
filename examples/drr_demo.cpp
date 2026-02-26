#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "job_system/drr_policy.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== DRR Demo: cost-aware scheduling ===\n\n";

    // Three clients with different weights and cost hints
    //   fast:   weight=1, cost_hint=1   (cheap jobs)
    //   medium: weight=1, cost_hint=10  (moderate jobs)
    //   slow:   weight=1, cost_hint=100 (expensive jobs)
    // With DRR and equal weights, each client gets the same byte-credit per
    // round, so 'fast' runs 100× more jobs than 'slow' per cycle.

    Scheduler scheduler(std::make_unique<DeficitRoundRobinPolicy>(/*base_quantum=*/100));
    scheduler.register_client("fast",   1);
    scheduler.register_client("medium", 1);
    scheduler.register_client("slow",   1);

    std::atomic<int> fast_done{0}, medium_done{0}, slow_done{0};

    constexpr int FAST_JOBS   = 100;
    constexpr int MEDIUM_JOBS = 10;
    constexpr int SLOW_JOBS   = 1;

    for (int i = 0; i < FAST_JOBS; ++i)
        scheduler.submit("fast",   [&] { ++fast_done;   }, /*cost_hint=*/1);
    for (int i = 0; i < MEDIUM_JOBS; ++i)
        scheduler.submit("medium", [&] { ++medium_done; }, /*cost_hint=*/10);
    for (int i = 0; i < SLOW_JOBS; ++i)
        scheduler.submit("slow",   [&] { ++slow_done;   }, /*cost_hint=*/100);

    {
        ThreadPool pool(scheduler, 2);
        pool.shutdown();
    }

    std::cout << "Jobs completed:\n";
    std::cout << "  fast   : " << fast_done.load()   << " / " << FAST_JOBS   << "\n";
    std::cout << "  medium : " << medium_done.load() << " / " << MEDIUM_JOBS << "\n";
    std::cout << "  slow   : " << slow_done.load()   << " / " << SLOW_JOBS   << "\n\n";

    auto gm = scheduler.get_global_metrics();
    std::cout << "Global metrics:\n";
    std::cout << "  total_processed    : " << gm.total_processed << "\n";
    std::cout << "  active_clients     : " << gm.active_clients  << "\n";
    std::cout << "  jain_fairness_index: " << gm.jain_fairness_index << "\n";

    return 0;
}
