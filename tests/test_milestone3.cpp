#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "job_system/drr_policy.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"
#include "job_system/wrr_policy.h"

using namespace job_system;
using namespace std::chrono_literals;

// ── 1. PolicyRefactor: default constructor uses WRR ───────────────────────────
TEST(PolicyRefactor, DefaultConstructorUsesWRR) {
    Scheduler scheduler; // no-arg → WeightedRoundRobinPolicy
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("A", 2);
    scheduler.register_client("B", 1);

    std::atomic<int> a_done{0}, b_done{0};
    for (int i = 0; i < 10; ++i) scheduler.submit("A", [&] { ++a_done; });
    for (int i = 0; i < 10; ++i) scheduler.submit("B", [&] { ++b_done; });

    pool.shutdown();

    EXPECT_EQ(a_done.load(), 10);
    EXPECT_EQ(b_done.load(), 10);
}

// ── 2. PolicyRefactor: explicit WRR policy == default ─────────────────────────
TEST(PolicyRefactor, ExplicitWRRPolicyMatchesDefault) {
    Scheduler scheduler(std::make_unique<WeightedRoundRobinPolicy>());
    ThreadPool pool(scheduler, 1);

    scheduler.register_client("A", 3);
    scheduler.register_client("B", 1);
    scheduler.register_client("C", 2);

    std::atomic<int> seq{0};
    std::array<std::string, 6> order;

    for (int i = 0; i < 3; ++i)
        scheduler.submit("A", [&] { order[seq++] = "A"; });
    for (int i = 0; i < 1; ++i)
        scheduler.submit("B", [&] { order[seq++] = "B"; });
    for (int i = 0; i < 2; ++i)
        scheduler.submit("C", [&] { order[seq++] = "C"; });

    pool.shutdown();

    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "A");
    EXPECT_EQ(order[2], "A");
    EXPECT_EQ(order[3], "B");
    EXPECT_EQ(order[4], "C");
    EXPECT_EQ(order[5], "C");
}

// ── 3. DRR: basic execution — all jobs complete ───────────────────────────────
TEST(DRR, BasicExecution) {
    Scheduler scheduler(std::make_unique<DeficitRoundRobinPolicy>());
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("X");
    scheduler.register_client("Y");

    std::atomic<int> x_done{0}, y_done{0};
    for (int i = 0; i < 20; ++i) scheduler.submit("X", [&] { ++x_done; });
    for (int i = 0; i < 20; ++i) scheduler.submit("Y", [&] { ++y_done; });

    pool.shutdown();

    EXPECT_EQ(x_done.load(), 20);
    EXPECT_EQ(y_done.load(), 20);
}

// ── 4. DRR: unit cost degrades to WRR sequence ───────────────────────────────
// Single worker + cost_hint=1 + equal weights → round-robin 1-by-1 each cycle
TEST(DRR, UnitCostDegradesToWRR) {
    // Use base_quantum=1 so each job (cost=1) exactly exhausts quota
    Scheduler scheduler(std::make_unique<DeficitRoundRobinPolicy>(1));
    ThreadPool pool(scheduler, 1);

    scheduler.register_client("A", 1);
    scheduler.register_client("B", 1);

    std::atomic<int> seq{0};
    std::array<std::string, 4> order;

    for (int i = 0; i < 2; ++i)
        scheduler.submit("A", [&] { order[seq++] = "A"; });
    for (int i = 0; i < 2; ++i)
        scheduler.submit("B", [&] { order[seq++] = "B"; });

    pool.shutdown();

    // With base_quantum=1 weight=1 each client gets exactly 1 job per cycle
    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "B");
    EXPECT_EQ(order[2], "A");
    EXPECT_EQ(order[3], "B");
}

// ── 5. DRR: weight ratio respected ───────────────────────────────────────────
// A(weight=1, cost=1) vs B(weight=3, cost=1); base_quantum=1
// B gets 3× the jobs per cycle
TEST(DRR, CostHintSequence) {
    Scheduler scheduler(std::make_unique<DeficitRoundRobinPolicy>(1));
    ThreadPool pool(scheduler, 1);

    scheduler.register_client("A", 1);
    scheduler.register_client("B", 3);

    std::atomic<int> a_done{0}, b_done{0};
    for (int i = 0; i < 20; ++i) scheduler.submit("A", [&] { ++a_done; });
    for (int i = 0; i < 60; ++i) scheduler.submit("B", [&] { ++b_done; });

    pool.shutdown();

    EXPECT_EQ(a_done.load(), 20);
    EXPECT_EQ(b_done.load(), 60);
}

// ── 6. DRR: work-conserving — empty client skipped, deficit reset ─────────────
TEST(DRR, WorkConservingSkip) {
    Scheduler scheduler(std::make_unique<DeficitRoundRobinPolicy>());
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("active");
    scheduler.register_client("idle"); // never gets any jobs

    std::atomic<int> done{0};
    for (int i = 0; i < 30; ++i)
        scheduler.submit("active", [&] { ++done; });

    pool.shutdown();

    EXPECT_EQ(done.load(), 30);
}

// ── 7. Backpressure: REJECT throws QueueFullException ─────────────────────────
TEST(Backpressure, RejectThrowsQueueFullException) {
    Scheduler scheduler;
    scheduler.register_client("limited", 1, /*max_depth=*/3,
                              OverflowStrategy::REJECT);
    ThreadPool pool(scheduler, 1);

    scheduler.submit("limited", [] {});
    scheduler.submit("limited", [] {});
    scheduler.submit("limited", [] {});

    EXPECT_THROW(scheduler.submit("limited", [] {}), QueueFullException);
}

// ── 8. Backpressure: DROP_OLDEST — oldest job is evicted ──────────────────────
TEST(Backpressure, DropOldest) {
    Scheduler scheduler;
    scheduler.register_client("q", 1, /*max_depth=*/2,
                              OverflowStrategy::DROP_OLDEST);

    std::atomic<int> seq{0};
    std::array<int, 2> executed{-1, -1};

    // Fill the queue: jobs 0 and 1 sit waiting
    scheduler.submit("q", [&] { executed[seq++] = 0; });
    scheduler.submit("q", [&] { executed[seq++] = 1; });
    // Third submit: drops job 0, enqueues job 2
    scheduler.submit("q", [&] { executed[seq++] = 2; });

    ThreadPool pool(scheduler, 1);
    pool.shutdown();

    // Only jobs 1 and 2 should have run
    EXPECT_EQ(seq.load(), 2);
    EXPECT_EQ(executed[0], 1);
    EXPECT_EQ(executed[1], 2);
}

// ── 9. Backpressure: DROP_NEWEST — incoming job silently discarded ─────────────
TEST(Backpressure, DropNewest) {
    Scheduler scheduler;
    scheduler.register_client("q", 1, /*max_depth=*/2,
                              OverflowStrategy::DROP_NEWEST);

    std::atomic<int> done{0};

    scheduler.submit("q", [&] { ++done; });
    scheduler.submit("q", [&] { ++done; });
    // Third job is silently dropped
    scheduler.submit("q", [&] { ++done; });

    ThreadPool pool(scheduler, 1);
    pool.shutdown();

    EXPECT_EQ(done.load(), 2);
}

// ── 10. Backpressure: BLOCK unblocks when worker drains ───────────────────────
TEST(Backpressure, BlockUnblocksWhenDrained) {
    Scheduler scheduler;
    scheduler.register_client("q", 1, /*max_depth=*/2,
                              OverflowStrategy::BLOCK);

    std::atomic<int> done{0};

    // Pre-fill: 2 jobs fill the queue
    scheduler.submit("q", [&] { ++done; });
    scheduler.submit("q", [&] { ++done; });

    // Start a thread that will block on the 3rd submit until the pool drains
    std::thread submitter([&] {
        scheduler.submit("q", [&] { ++done; }); // blocks here until room
    });

    // Start pool after the blocking submitter is launched
    ThreadPool pool(scheduler, 1);
    pool.shutdown(); // drains all 3 jobs

    submitter.join();
    EXPECT_EQ(done.load(), 3);
}

// ── 11. Backpressure: overflow_count in ClientMetrics ─────────────────────────
TEST(Backpressure, OverflowCountMetric) {
    Scheduler scheduler;
    scheduler.register_client("q", 1, /*max_depth=*/1,
                              OverflowStrategy::DROP_NEWEST);

    scheduler.submit("q", [] {}); // accepted
    scheduler.submit("q", [] {}); // dropped (overflow)
    scheduler.submit("q", [] {}); // dropped (overflow)

    auto metrics = scheduler.get_client_metrics("q");
    EXPECT_EQ(metrics.overflow_count, 2u);
}

// ── 12. Metrics: Jain Fairness Index ≈ 1.0 for equal throughput ───────────────
TEST(Metrics, JainFairnessIndexEqual) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 4);

    scheduler.register_client("A");
    scheduler.register_client("B");
    scheduler.register_client("C");

    std::atomic<int> done{0};
    constexpr int N = 30;
    for (int i = 0; i < N; ++i) scheduler.submit("A", [&] { ++done; });
    for (int i = 0; i < N; ++i) scheduler.submit("B", [&] { ++done; });
    for (int i = 0; i < N; ++i) scheduler.submit("C", [&] { ++done; });

    pool.shutdown();

    auto gm = scheduler.get_global_metrics();
    EXPECT_EQ(gm.total_processed, static_cast<uint64_t>(3 * N));
    EXPECT_EQ(gm.active_clients, 3u);
    // Equal throughput → Jain ≈ 1.0 (allow small floating-point tolerance)
    EXPECT_NEAR(gm.jain_fairness_index, 1.0, 0.01);
}
