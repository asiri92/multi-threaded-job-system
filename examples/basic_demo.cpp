#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

int main() {
    using namespace job_system;

    constexpr size_t NUM_WORKERS = 4;
    constexpr int JOBS_PER_CLIENT = 50;
    const std::string clients[] = {"alice", "bob", "charlie"};

    std::printf("=== Multithreaded Job System — Milestone 1 Demo ===\n");
    std::printf("Workers: %zu | Clients: %zu | Jobs per client: %d\n\n",
                NUM_WORKERS, std::size(clients), JOBS_PER_CLIENT);

    Scheduler scheduler;
    ThreadPool pool(scheduler, NUM_WORKERS);

    for (const auto& name : clients) {
        scheduler.register_client(name);
    }

    // Submit jobs: each client does simulated work
    for (const auto& name : clients) {
        for (int i = 0; i < JOBS_PER_CLIENT; ++i) {
            scheduler.submit(name, [i] {
                // Simulate variable workload
                std::this_thread::sleep_for(
                    std::chrono::microseconds(50 + (i % 10) * 10));
            });
        }
    }
    pool.notify_workers();

    std::printf("All jobs submitted. Shutting down (draining queues)...\n\n");
    pool.shutdown();

    // Print metrics
    std::printf("%-12s %10s %10s %12s %12s\n", "Client", "Submitted",
                "Executed", "Avg Time(us)", "Queue Depth");
    std::printf("%-12s %10s %10s %12s %12s\n", "------", "---------",
                "--------", "------------", "-----------");

    for (const auto& name : clients) {
        auto m = scheduler.get_client_metrics(name);
        std::printf("%-12s %10llu %10llu %12.1f %12zu\n", name.c_str(),
                    static_cast<unsigned long long>(m.submitted),
                    static_cast<unsigned long long>(m.executed),
                    m.avg_execution_time_us, m.queue_depth);
    }

    std::printf("\nTotal jobs processed: %llu\n",
                static_cast<unsigned long long>(scheduler.total_jobs_processed()));
    std::printf("=== Demo Complete ===\n");

    return 0;
}
