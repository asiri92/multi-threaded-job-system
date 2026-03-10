#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "job_system/job.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;
using namespace std::chrono_literals;

// ============================================================
// Priority Suite
// ============================================================

TEST(Priority, CriticalBeforeLow) {
    Scheduler sched;
    sched.register_client("A");

    // Hold the single worker so all subsequent jobs queue up before any run
    std::atomic<bool> gate{false};
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::vector<int> order;
    std::mutex order_mu;
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(0); }, 1, Priority::LOW);
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(3); }, 1, Priority::CRITICAL);

    {
        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        std::this_thread::sleep_for(100ms);
    } // pool destructor joins threads

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 3); // CRITICAL first
    EXPECT_EQ(order[1], 0); // LOW second
}

TEST(Priority, FIFOWithinSameLevel) {
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> gate{false};
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::vector<int> order;
    std::mutex order_mu;
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(1); }, 1, Priority::HIGH);
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(2); }, 1, Priority::HIGH);

    {
        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        std::this_thread::sleep_for(100ms);
    }

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST(Priority, AllFourLevels) {
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> gate{false};
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::vector<int> order;
    std::mutex order_mu;
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(0); }, 1, Priority::LOW);
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(1); }, 1, Priority::NORMAL);
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(2); }, 1, Priority::HIGH);
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(3); }, 1, Priority::CRITICAL);

    {
        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        std::this_thread::sleep_for(100ms);
    }

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], 3); // CRITICAL
    EXPECT_EQ(order[1], 2); // HIGH
    EXPECT_EQ(order[2], 1); // NORMAL
    EXPECT_EQ(order[3], 0); // LOW
}

TEST(Priority, DefaultIsNormal) {
    // Submit without priority arg should queue at NORMAL level.
    // Verify by submitting both default and CRITICAL and observing order.
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> gate{false};
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::vector<int> order;
    std::mutex order_mu;
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(1); }); // default = NORMAL
    sched.submit("A", [&]() { std::lock_guard lk(order_mu); order.push_back(2); }, 1, Priority::CRITICAL);

    {
        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        std::this_thread::sleep_for(100ms);
    }

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2); // CRITICAL runs first
    EXPECT_EQ(order[1], 1); // NORMAL (default) runs second
}

// ============================================================
// Cancellation Suite
// ============================================================

TEST(Cancellation, CancelPendingJob) {
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> gate{false};
    // Blocker job: job_id = 1
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::atomic<bool> ran{false};
    // Target job: job_id = 2
    sched.submit("A", [&]() { ran.store(true); });

    // Cancel by id=2 specifically (scheduler starts at next_job_id_=1)
    EXPECT_TRUE(sched.cancel_job(2));
    EXPECT_EQ(sched.get_client_metrics("A").queue_depth, 1u); // only blocker remains

    {
        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_FALSE(ran.load()); // cancelled job never ran
}

TEST(Cancellation, CancelAlreadyDequeuedReturnsFalse) {
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> started{false};
    std::atomic<bool> finish{false};
    // Job id = 1
    sched.submit("A", [&]() {
        started.store(true, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    {
        ThreadPool pool(sched, 1);
        // Wait until the job is executing (dequeued from queue)
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Job is in-flight — cancel should return false (not in queue)
        EXPECT_FALSE(sched.cancel_job(1));
        finish.store(true, std::memory_order_release);
    }
}

TEST(Cancellation, CancelUnknownIdReturnsFalse) {
    Scheduler sched;
    sched.register_client("A");
    EXPECT_FALSE(sched.cancel_job(99999));
}

TEST(Cancellation, DrainClientClearsAll) {
    Scheduler sched;
    sched.register_client("A");

    std::atomic<bool> gate{false};
    // Blocker — will be drained too
    sched.submit("A", [&]() {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::atomic<int> ran{0};
    sched.submit("A", [&]() { ran.fetch_add(1); });
    sched.submit("A", [&]() { ran.fetch_add(1); }, 1, Priority::HIGH);
    sched.submit("A", [&]() { ran.fetch_add(1); }, 1, Priority::CRITICAL);

    // Drain all 4 queued jobs (including blocker) before starting pool
    uint64_t drained = sched.drain_client("A");
    EXPECT_EQ(drained, 4u);
    EXPECT_EQ(sched.get_client_metrics("A").queue_depth, 0u);

    // Release gate — nobody is waiting, just cleanup
    gate.store(true, std::memory_order_release);

    {
        ThreadPool pool(sched, 1);
        pool.shutdown();
    }
    EXPECT_EQ(ran.load(), 0); // no jobs ran
}

// ============================================================
// DynamicWeight Suite
// ============================================================

TEST(DynamicWeight, UpdateWeightReflectedInMetrics) {
    Scheduler sched;
    sched.register_client("A", 1);
    EXPECT_EQ(sched.get_client_metrics("A").weight, 1u);
    sched.update_client_weight("A", 5);
    EXPECT_EQ(sched.get_client_metrics("A").weight, 5u);
}

TEST(DynamicWeight, UpdateWeightZeroThrows) {
    Scheduler sched;
    sched.register_client("A");
    EXPECT_THROW(sched.update_client_weight("A", 0), std::invalid_argument);
}

// ============================================================
// Unregister Suite
// ============================================================

TEST(Unregister, UnregisterDrainsAndRemoves) {
    Scheduler sched;
    sched.register_client("A");

    // 3 jobs in the queue (no pool is running)
    sched.submit("A", []() {});
    sched.submit("A", []() {});
    sched.submit("A", []() {}, 1, Priority::HIGH);

    uint64_t drained = sched.unregister_client("A");
    EXPECT_EQ(drained, 3u);

    // Subsequent submit should throw (client removed)
    EXPECT_THROW(sched.submit("A", []() {}), std::runtime_error);
}

TEST(Unregister, UnregisterUnknownThrows) {
    Scheduler sched;
    EXPECT_THROW(sched.unregister_client("nonexistent"), std::runtime_error);
}
