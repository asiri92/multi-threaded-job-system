// scaling_bench.cpp — Worker scaling throughput + execution latency benchmark
//
// Section 1: Throughput Scaling
//   4 clients × 10,000 jobs = 40,000 total
//   Each job spins ~1µs to simulate compute work
//   Runs with 1, 2, 4, 8, 16 workers and prints formatted table
//
// Section 2: Execution Time Stats via Observer
//   LatencyObserver accumulates on_job_executed durations (min/avg/max)
//   Scheduling latency (enqueue_time → dequeue) is available on the Job struct
//   but requires worker instrumentation; this benchmark measures execution time.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "job_system/metrics_observer.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Spin for ~1µs to simulate real compute work
static void spin_1us() {
    auto end = steady_clock::now() + microseconds(1);
    while (steady_clock::now() < end) {
        // busy wait
    }
}

// Run all 40,000 jobs through a pool with `num_workers` threads.
// Returns wall-clock duration in microseconds.
static microseconds run_throughput(size_t num_workers) {
    Scheduler sched;
    sched.register_client("A");
    sched.register_client("B");
    sched.register_client("C");
    sched.register_client("D");

    std::atomic<int> completed{0};
    constexpr int JOBS_PER_CLIENT = 10'000;

    // Submit all jobs before starting pool to avoid warm-up noise
    for (int i = 0; i < JOBS_PER_CLIENT; ++i) {
        sched.submit("A", [&completed] { spin_1us(); completed.fetch_add(1, std::memory_order_relaxed); });
        sched.submit("B", [&completed] { spin_1us(); completed.fetch_add(1, std::memory_order_relaxed); });
        sched.submit("C", [&completed] { spin_1us(); completed.fetch_add(1, std::memory_order_relaxed); });
        sched.submit("D", [&completed] { spin_1us(); completed.fetch_add(1, std::memory_order_relaxed); });
    }

    auto wall_start = steady_clock::now();
    {
        ThreadPool pool(sched, num_workers);
        pool.shutdown(ShutdownMode::GRACEFUL);
    }
    auto wall_end = steady_clock::now();

    return duration_cast<microseconds>(wall_end - wall_start);
}

// ---------------------------------------------------------------------------
// LatencyObserver — records execution durations with atomics
// ---------------------------------------------------------------------------
class LatencyObserver : public IMetricsObserver {
public:
    std::atomic<uint64_t> count{0};
    std::atomic<int64_t>  total_us{0};
    // min/max tracked via compare-exchange
    std::atomic<int64_t>  min_us{INT64_MAX};
    std::atomic<int64_t>  max_us{INT64_MIN};

    void on_job_executed(const std::string& /*client_id*/,
                         uint64_t /*job_id*/,
                         microseconds duration) override {
        int64_t us = duration.count();
        count.fetch_add(1, std::memory_order_relaxed);
        total_us.fetch_add(us, std::memory_order_relaxed);

        int64_t old_min = min_us.load(std::memory_order_relaxed);
        while (us < old_min &&
               !min_us.compare_exchange_weak(old_min, us,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {}

        int64_t old_max = max_us.load(std::memory_order_relaxed);
        while (us > old_max &&
               !max_us.compare_exchange_weak(old_max, us,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {}
    }

    double avg_us() const {
        uint64_t n = count.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(total_us.load(std::memory_order_relaxed)) /
               static_cast<double>(n);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    constexpr int    TOTAL_JOBS = 40'000;
    constexpr size_t LATENCY_WORKERS = 4;

    // -----------------------------------------------------------------------
    // Section 1 — Throughput Scaling
    // -----------------------------------------------------------------------
    std::cout << "\n=== Throughput Scaling (4 clients x 10,000 jobs = 40,000 total) ===\n\n";
    std::cout << std::setw(10) << "Workers"
              << std::setw(14) << "Wall (ms)"
              << std::setw(16) << "Jobs/sec"
              << std::setw(18) << "µs/job"
              << std::setw(20) << "Scaling Eff."
              << "\n";
    std::cout << std::string(78, '-') << "\n";

    double baseline_jps = 0.0;
    const std::vector<size_t> worker_counts = {1, 2, 4, 8, 16};

    for (size_t workers : worker_counts) {
        auto elapsed = run_throughput(workers);
        double wall_ms  = static_cast<double>(elapsed.count()) / 1000.0;
        double jps      = static_cast<double>(TOTAL_JOBS) /
                          (static_cast<double>(elapsed.count()) / 1e6);
        double us_per_j = static_cast<double>(elapsed.count()) /
                          static_cast<double>(TOTAL_JOBS);

        if (baseline_jps == 0.0) baseline_jps = jps;
        double eff = (jps / (baseline_jps * static_cast<double>(workers))) * 100.0;

        std::cout << std::setw(10) << workers
                  << std::setw(14) << std::fixed << std::setprecision(1) << wall_ms
                  << std::setw(16) << std::fixed << std::setprecision(0) << jps
                  << std::setw(18) << std::fixed << std::setprecision(2) << us_per_j
                  << std::setw(19) << std::fixed << std::setprecision(1) << eff << "%"
                  << "\n";
    }

    // -----------------------------------------------------------------------
    // Section 2 — Execution Time Distribution via Observer
    // -----------------------------------------------------------------------
    std::cout << "\n=== Execution Time Distribution (" << LATENCY_WORKERS
              << " workers, 40,000 jobs) ===\n\n";

    auto obs = std::make_shared<LatencyObserver>();
    {
        Scheduler sched;
        sched.set_observer(obs);
        sched.register_client("A");
        sched.register_client("B");
        sched.register_client("C");
        sched.register_client("D");

        for (int i = 0; i < 10'000; ++i) {
            sched.submit("A", spin_1us);
            sched.submit("B", spin_1us);
            sched.submit("C", spin_1us);
            sched.submit("D", spin_1us);
        }

        ThreadPool pool(sched, LATENCY_WORKERS);
        pool.shutdown(ShutdownMode::GRACEFUL);
    }

    std::cout << "  Jobs observed : " << obs->count.load() << "\n";
    std::cout << "  Min exec time : " << obs->min_us.load() << " µs\n";
    std::cout << "  Avg exec time : " << std::fixed << std::setprecision(2)
              << obs->avg_us() << " µs\n";
    std::cout << "  Max exec time : " << obs->max_us.load() << " µs\n\n";

    // Scheduling latency (enqueue→dequeue) is recorded on job.enqueue_time
    // but requires worker instrumentation to measure. See docs/ARCHITECTURE.md.
    std::cout << "  Note: scheduling latency (enqueue→dequeue) is measurable via\n"
              << "  job.enqueue_time captured inside the worker loop before execution.\n\n";

    return 0;
}
