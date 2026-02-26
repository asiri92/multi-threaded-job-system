#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;

// ── 1. Default weight=1 is backward-compatible ────────────────────────────────
TEST(WRR, DefaultWeightBackwardCompat) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("A");
    scheduler.register_client("B");

    std::atomic<int> count_a{0}, count_b{0};
    for (int i = 0; i < 10; ++i) scheduler.submit("A", [&]{ ++count_a; });
    for (int i = 0; i < 10; ++i) scheduler.submit("B", [&]{ ++count_b; });

    pool.shutdown();

    EXPECT_EQ(count_a.load(), 10);
    EXPECT_EQ(count_b.load(), 10);
    EXPECT_EQ(scheduler.get_client_metrics("A").weight, 1u);
    EXPECT_EQ(scheduler.get_client_metrics("B").weight, 1u);
}

// ── 2. Weight is exposed in metrics ───────────────────────────────────────────
TEST(WRR, WeightExposedInMetrics) {
    Scheduler scheduler;
    scheduler.register_client("light", 1);
    scheduler.register_client("medium", 3);
    scheduler.register_client("heavy", 7);

    EXPECT_EQ(scheduler.get_client_metrics("light").weight,  1u);
    EXPECT_EQ(scheduler.get_client_metrics("medium").weight, 3u);
    EXPECT_EQ(scheduler.get_client_metrics("heavy").weight,  7u);
}

// ── 3. Zero weight throws std::invalid_argument ───────────────────────────────
TEST(WRR, ZeroWeightThrows) {
    Scheduler scheduler;
    EXPECT_THROW(scheduler.register_client("bad", 0), std::invalid_argument);
}

// ── 4. Key sequence test: single worker → exact A,A,A,B,C,C order ─────────────
TEST(WRR, WRRExecutionSequence) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 1); // single worker = deterministic order

    scheduler.register_client("A", 3);
    scheduler.register_client("B", 1);
    scheduler.register_client("C", 2);

    std::atomic<int> seq{0};
    std::array<std::string, 6> order;

    // Submit all jobs before workers can drain — jobs arrive atomically from
    // the scheduler's perspective because the worker is blocked until notify.
    for (int i = 0; i < 3; ++i) scheduler.submit("A", [&]{ order[seq++] = "A"; });
    for (int i = 0; i < 1; ++i) scheduler.submit("B", [&]{ order[seq++] = "B"; });
    for (int i = 0; i < 2; ++i) scheduler.submit("C", [&]{ order[seq++] = "C"; });

    pool.shutdown(); // drains all queues then stops

    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "A");
    EXPECT_EQ(order[2], "A");
    EXPECT_EQ(order[3], "B");
    EXPECT_EQ(order[4], "C");
    EXPECT_EQ(order[5], "C");
}

// ── 5. Work-conserving: empty client is skipped, others drain fully ────────────
TEST(WRR, WorkConserving_SkipsEmptyClient) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("A", 1);
    scheduler.register_client("B", 3); // high weight but no jobs
    scheduler.register_client("C", 1);

    std::atomic<int> count_a{0}, count_c{0};
    for (int i = 0; i < 20; ++i) scheduler.submit("A", [&]{ ++count_a; });
    for (int i = 0; i < 20; ++i) scheduler.submit("C", [&]{ ++count_c; });
    // "B" gets no jobs at all

    pool.shutdown();

    EXPECT_EQ(count_a.load(), 20);
    EXPECT_EQ(count_c.load(), 20);
}

// ── 6. Single high-weight client with idle neighbour drains completely ─────────
TEST(WRR, SingleHighWeightClient) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("heavy", 10);
    scheduler.register_client("idle",  1);

    std::atomic<int> done{0};
    for (int i = 0; i < 50; ++i) scheduler.submit("heavy", [&]{ ++done; });

    pool.shutdown();

    EXPECT_EQ(done.load(), 50);
}

// ── 7. Concurrent submission — all jobs complete regardless of weights ─────────
TEST(WRR, ConcurrentSubmissionWeighted) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 4);

    scheduler.register_client("fast", 4);
    scheduler.register_client("slow", 1);

    constexpr int N = 200;
    std::atomic<int> fast_done{0}, slow_done{0};

    // Submit from background threads to stress concurrent submission
    std::thread t1([&]{
        for (int i = 0; i < N; ++i) scheduler.submit("fast", [&]{ ++fast_done; });
    });
    std::thread t2([&]{
        for (int i = 0; i < N; ++i) scheduler.submit("slow", [&]{ ++slow_done; });
    });
    t1.join();
    t2.join();

    pool.shutdown();

    EXPECT_EQ(fast_done.load(), N);
    EXPECT_EQ(slow_done.load(), N);
}
