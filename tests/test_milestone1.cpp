#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;

// ---------------------------------------------------------------------------
// Basic functionality
// ---------------------------------------------------------------------------

TEST(JobSystem, SingleClientSingleJob) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("alice");

    std::atomic<bool> executed{false};
    scheduler.submit("alice", [&] { executed.store(true); });
    pool.notify_workers();

    pool.shutdown();
    EXPECT_TRUE(executed.load());

    auto m = scheduler.get_client_metrics("alice");
    EXPECT_EQ(m.submitted, 1u);
    EXPECT_EQ(m.executed, 1u);
    EXPECT_EQ(m.queue_depth, 0u);
    EXPECT_EQ(scheduler.total_jobs_processed(), 1u);
}

TEST(JobSystem, MultipleJobsSingleClient) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("bob");

    constexpr int N = 100;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i) {
        scheduler.submit("bob", [&] { counter.fetch_add(1); });
    }
    pool.notify_workers();

    pool.shutdown();
    EXPECT_EQ(counter.load(), N);

    auto m = scheduler.get_client_metrics("bob");
    EXPECT_EQ(m.submitted, static_cast<uint64_t>(N));
    EXPECT_EQ(m.executed, static_cast<uint64_t>(N));
}

// ---------------------------------------------------------------------------
// Multi-client fairness
// ---------------------------------------------------------------------------

TEST(JobSystem, MultiClientFairness) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 4);

    const int num_clients = 3;
    const int jobs_per_client = 300;

    std::vector<std::atomic<int>> counters(num_clients);
    for (auto& c : counters) c.store(0);

    for (int c = 0; c < num_clients; ++c) {
        scheduler.register_client("client_" + std::to_string(c));
    }

    // Submit all jobs for all clients
    for (int c = 0; c < num_clients; ++c) {
        for (int j = 0; j < jobs_per_client; ++j) {
            scheduler.submit("client_" + std::to_string(c),
                             [&counters, c] { counters[c].fetch_add(1); });
        }
    }
    pool.notify_workers();

    pool.shutdown();

    // All jobs must have executed
    int total = 0;
    for (int c = 0; c < num_clients; ++c) {
        EXPECT_EQ(counters[c].load(), jobs_per_client)
            << "Client " << c << " did not execute all jobs";
        total += counters[c].load();
    }
    EXPECT_EQ(total, num_clients * jobs_per_client);

    // Verify metrics consistency
    for (int c = 0; c < num_clients; ++c) {
        auto m = scheduler.get_client_metrics("client_" + std::to_string(c));
        EXPECT_EQ(m.submitted, static_cast<uint64_t>(jobs_per_client));
        EXPECT_EQ(m.executed, static_cast<uint64_t>(jobs_per_client));
        EXPECT_EQ(m.queue_depth, 0u);
    }
    EXPECT_EQ(scheduler.total_jobs_processed(),
              static_cast<uint64_t>(num_clients * jobs_per_client));
}

// ---------------------------------------------------------------------------
// Shutdown semantics
// ---------------------------------------------------------------------------

TEST(JobSystem, ShutdownDrainsAllJobs) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 2);

    scheduler.register_client("drain_test");

    constexpr int N = 500;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i) {
        scheduler.submit("drain_test", [&] {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            counter.fetch_add(1);
        });
    }
    pool.notify_workers();

    // Shutdown should wait until all jobs are done
    pool.shutdown();
    EXPECT_EQ(counter.load(), N);
}

TEST(JobSystem, EmptyShutdown) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 4);

    scheduler.register_client("empty");
    // No jobs submitted — shutdown should complete cleanly
    pool.shutdown();

    auto m = scheduler.get_client_metrics("empty");
    EXPECT_EQ(m.submitted, 0u);
    EXPECT_EQ(m.executed, 0u);
}

// ---------------------------------------------------------------------------
// Concurrent submission
// ---------------------------------------------------------------------------

TEST(JobSystem, ConcurrentSubmission) {
    Scheduler scheduler;
    ThreadPool pool(scheduler, 4);

    const int num_submitters = 4;
    const int jobs_per_submitter = 200;

    for (int s = 0; s < num_submitters; ++s) {
        scheduler.register_client("sub_" + std::to_string(s));
    }

    std::atomic<int> total_executed{0};
    std::vector<std::thread> submitters;

    for (int s = 0; s < num_submitters; ++s) {
        submitters.emplace_back([&, s] {
            for (int j = 0; j < jobs_per_submitter; ++j) {
                scheduler.submit("sub_" + std::to_string(s),
                                 [&] { total_executed.fetch_add(1); });
                pool.notify_workers();
            }
        });
    }

    for (auto& t : submitters) t.join();
    pool.shutdown();

    EXPECT_EQ(total_executed.load(), num_submitters * jobs_per_submitter);
    EXPECT_EQ(scheduler.total_jobs_processed(),
              static_cast<uint64_t>(num_submitters * jobs_per_submitter));
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(JobSystem, SubmitToUnregisteredClientThrows) {
    Scheduler scheduler;
    EXPECT_THROW(scheduler.submit("nobody", [] {}), std::runtime_error);
}

TEST(JobSystem, DuplicateRegistrationThrows) {
    Scheduler scheduler;
    scheduler.register_client("dup");
    EXPECT_THROW(scheduler.register_client("dup"), std::runtime_error);
}
