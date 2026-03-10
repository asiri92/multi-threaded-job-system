#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "job_system/metrics_observer.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// TestObserver — accumulates observer callbacks for assertions
// ---------------------------------------------------------------------------
struct TestObserver : public IMetricsObserver {
    std::atomic<int>      submitted_calls{0};
    std::atomic<int>      executed_calls{0};
    std::atomic<int>      expired_calls{0};
    std::atomic<int>      cancelled_calls{0};
    std::atomic<uint64_t> last_job_id{0};

    void on_job_submitted(const std::string& /*client_id*/,
                          uint64_t job_id) override {
        submitted_calls.fetch_add(1, std::memory_order_relaxed);
        last_job_id.store(job_id, std::memory_order_relaxed);
    }
    void on_job_executed(const std::string& /*client_id*/,
                         uint64_t /*job_id*/,
                         std::chrono::microseconds /*duration*/) override {
        executed_calls.fetch_add(1, std::memory_order_relaxed);
    }
    void on_job_expired(const std::string& /*client_id*/,
                        uint64_t /*job_id*/) override {
        expired_calls.fetch_add(1, std::memory_order_relaxed);
    }
    void on_job_cancelled(const std::string& /*client_id*/,
                          uint64_t /*job_id*/) override {
        cancelled_calls.fetch_add(1, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Shutdown tests
// ---------------------------------------------------------------------------

// GracefulModeEnum: shutdown(GRACEFUL) with explicit enum still drains all jobs
TEST(Shutdown, GracefulModeEnum) {
    Scheduler sched;
    ThreadPool pool(sched, 2);
    sched.register_client("A");

    std::atomic<int> ran{0};
    for (int i = 0; i < 10; ++i) {
        sched.submit("A", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown(ShutdownMode::GRACEFUL);
    EXPECT_EQ(ran.load(), 10);
}

// ImmediateAbandonsAllPending: blocker keeps worker busy while IMMEDIATE
// drains pending jobs — those pending jobs must never execute.
TEST(Shutdown, ImmediateAbandonsAllPending) {
    Scheduler sched;
    ThreadPool pool(sched, 1); // single worker — deterministic
    sched.register_client("A");

    // Blocker: spins until gate=true, keeping the single worker busy
    std::atomic<bool> gate{false};
    sched.submit("A", [&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    // Pending jobs — these must NOT run
    std::atomic<int> ran{0};
    for (int i = 0; i < 5; ++i) {
        sched.submit("A", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    // Give worker time to dequeue and start executing the blocker
    std::this_thread::sleep_for(10ms);

    // Spawn a releaser that sets gate AFTER drain_all_clients() has run.
    // shutdown(IMMEDIATE) calls drain_all_clients() synchronously as its FIRST
    // action (sub-microsecond), then blocks joining workers until gate=true.
    // 5ms delay >> drain time, so drain is guaranteed to complete first.
    std::thread releaser([&gate] {
        std::this_thread::sleep_for(5ms);
        gate.store(true, std::memory_order_release);
    });

    // drain_all_clients() clears the 5 pending jobs before any worker picks them up
    pool.shutdown(ShutdownMode::IMMEDIATE);
    releaser.join();

    EXPECT_EQ(ran.load(), 0);
}

// ---------------------------------------------------------------------------
// Deadline tests
// ---------------------------------------------------------------------------

// ExpiredJobIsSkipped: a past-deadline job never executes
TEST(Deadline, ExpiredJobIsSkipped) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    std::atomic<bool> ran{false};
    // Deadline already in the past
    auto past = std::chrono::steady_clock::now() - 1s;
    sched.submit("A", [&ran] { ran.store(true, std::memory_order_relaxed); },
                 /*cost_hint=*/1, Priority::NORMAL, past);

    pool.shutdown(ShutdownMode::GRACEFUL);
    EXPECT_FALSE(ran.load());
}

// ValidDeadlineExecutes: future deadline → job runs normally
TEST(Deadline, ValidDeadlineExecutes) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    std::atomic<bool> ran{false};
    auto future = std::chrono::steady_clock::now() + 60s;
    sched.submit("A", [&ran] { ran.store(true, std::memory_order_relaxed); },
                 /*cost_hint=*/1, Priority::NORMAL, future);

    pool.shutdown(ShutdownMode::GRACEFUL);
    EXPECT_TRUE(ran.load());
}

// ExpiredCountInMetrics: expired_count increments when job deadline passes
TEST(Deadline, ExpiredCountInMetrics) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    auto past = std::chrono::steady_clock::now() - 1s;
    sched.submit("A", [] {}, /*cost_hint=*/1, Priority::NORMAL, past);

    pool.shutdown(ShutdownMode::GRACEFUL);

    auto metrics = sched.get_client_metrics("A");
    EXPECT_EQ(metrics.expired_count, 1u);
}

// NoDeadlineExecutesNormally: default {} deadline → job runs (regression guard)
TEST(Deadline, NoDeadlineExecutesNormally) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    std::atomic<bool> ran{false};
    // Default deadline — no deadline parameter
    sched.submit("A", [&ran] { ran.store(true, std::memory_order_relaxed); });

    pool.shutdown(ShutdownMode::GRACEFUL);
    EXPECT_TRUE(ran.load());
}

// ---------------------------------------------------------------------------
// Observer tests
// ---------------------------------------------------------------------------

// OnJobSubmittedCalled: on_job_submitted fires with correct job_id
TEST(Observer, OnJobSubmittedCalled) {
    Scheduler sched;
    sched.register_client("A");

    auto obs = std::make_shared<TestObserver>();
    sched.set_observer(obs);

    sched.submit("A", [] {});
    EXPECT_EQ(obs->submitted_calls.load(), 1);
    EXPECT_GT(obs->last_job_id.load(), 0u);
}

// OnJobExecutedCalled: on_job_executed fires after job runs
TEST(Observer, OnJobExecutedCalled) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    auto obs = std::make_shared<TestObserver>();
    sched.set_observer(obs);

    sched.submit("A", [] {});
    pool.shutdown(ShutdownMode::GRACEFUL);

    EXPECT_EQ(obs->executed_calls.load(), 1);
}

// OnJobExpiredCalled: expired job → on_job_expired fires
TEST(Observer, OnJobExpiredCalled) {
    Scheduler sched;
    ThreadPool pool(sched, 1);
    sched.register_client("A");

    auto obs = std::make_shared<TestObserver>();
    sched.set_observer(obs);

    auto past = std::chrono::steady_clock::now() - 1s;
    sched.submit("A", [] {}, /*cost_hint=*/1, Priority::NORMAL, past);
    pool.shutdown(ShutdownMode::GRACEFUL);

    EXPECT_EQ(obs->expired_calls.load(), 1);
}

// OnJobCancelledCalled: cancel_job → on_job_cancelled fires
TEST(Observer, OnJobCancelledCalled) {
    Scheduler sched;
    sched.register_client("A");

    auto obs = std::make_shared<TestObserver>();
    sched.set_observer(obs);

    // Submit two jobs — cancel the second one (higher job_id)
    sched.submit("A", [] {});
    sched.submit("A", [] {});

    uint64_t target_id = obs->last_job_id.load(std::memory_order_relaxed);
    bool cancelled = sched.cancel_job(target_id);
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(obs->cancelled_calls.load(), 1);

    // Drain remaining job so no worker needed
    sched.drain_client("A");
}
