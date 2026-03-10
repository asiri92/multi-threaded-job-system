#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "job_system/job.h"
#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

using namespace job_system;
using namespace std::chrono_literals;

// ── Helper to print section headers ──────────────────────────────────────────
static void section(const char* title) {
    std::cout << "\n=== " << title << " ===\n";
}

int main() {
    // ── 1. Priority ordering ──────────────────────────────────────────────────
    section("Priority Ordering");
    {
        Scheduler sched;
        sched.register_client("demo");

        // Hold the worker so jobs queue up
        std::atomic<bool> gate{false};
        sched.submit("demo", [&]() {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        std::vector<std::string> order;
        std::mutex order_mu;

        auto add = [&](const char* label, Priority prio) {
            sched.submit("demo", [&, label]() {
                std::lock_guard lk(order_mu);
                order.push_back(label);
            }, 1, prio);
        };

        add("LOW",      Priority::LOW);
        add("NORMAL",   Priority::NORMAL);
        add("HIGH",     Priority::HIGH);
        add("CRITICAL", Priority::CRITICAL);

        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        pool.shutdown();

        std::cout << "Execution order: ";
        for (const auto& s : order) std::cout << s << " ";
        std::cout << '\n';
        // Expected: CRITICAL HIGH NORMAL LOW
    }

    // ── 2. cancel_job ─────────────────────────────────────────────────────────
    section("cancel_job");
    {
        Scheduler sched;
        sched.register_client("demo");

        // Blocker holds the worker
        std::atomic<bool> gate{false};
        sched.submit("demo", [&]() {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        std::atomic<bool> low_ran{false};
        sched.submit("demo", [&]() { low_ran.store(true); }, 1, Priority::LOW);

        // Try to cancel job IDs 1-5 — one of them is the LOW job
        bool cancelled = false;
        for (uint64_t id = 1; id <= 5; ++id) {
            if (sched.cancel_job(id)) {
                std::cout << "Cancelled job id=" << id << '\n';
                cancelled = true;
                break;
            }
        }
        std::cout << "Cancel succeeded: " << (cancelled ? "yes" : "no") << '\n';

        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        pool.shutdown();

        std::cout << "LOW job ran: " << (low_ran.load() ? "yes" : "no") << '\n';
    }

    // ── 3. drain_client ───────────────────────────────────────────────────────
    section("drain_client");
    {
        Scheduler sched;
        sched.register_client("demo");

        // Blocker
        std::atomic<bool> gate{false};
        sched.submit("demo", [&]() {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        std::atomic<int> ran{0};
        for (int i = 0; i < 5; ++i) {
            sched.submit("demo", [&]() { ran.fetch_add(1); });
        }

        uint64_t drained = sched.drain_client("demo");
        std::cout << "Drained " << drained << " jobs\n"; // 5

        ThreadPool pool(sched, 1);
        gate.store(true, std::memory_order_release);
        pool.shutdown();

        std::cout << "Jobs that ran after drain: " << ran.load() << '\n'; // 0
    }

    // ── 4. update_client_weight ───────────────────────────────────────────────
    section("update_client_weight");
    {
        Scheduler sched;
        sched.register_client("A", 1);
        sched.register_client("B", 1);

        std::cout << "A weight before: " << sched.get_client_metrics("A").weight << '\n';
        sched.update_client_weight("A", 10);
        std::cout << "A weight after:  " << sched.get_client_metrics("A").weight << '\n';

        // Run some work to show A gets more CPU time with higher weight
        std::atomic<int> a_done{0}, b_done{0};
        for (int i = 0; i < 30; ++i) sched.submit("A", [&]() { ++a_done; });
        for (int i = 0; i < 30; ++i) sched.submit("B", [&]() { ++b_done; });

        ThreadPool pool(sched, 1);
        pool.shutdown();

        std::cout << "A ran: " << a_done.load() << ", B ran: " << b_done.load() << '\n';
    }

    // ── 5. unregister_client ──────────────────────────────────────────────────
    section("unregister_client");
    {
        Scheduler sched;
        sched.register_client("temp");

        // Blocker
        std::atomic<bool> gate{false};
        sched.submit("temp", [&]() {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        for (int i = 0; i < 3; ++i) sched.submit("temp", []() {});

        uint64_t drained = sched.unregister_client("temp");
        std::cout << "Unregistered 'temp', drained " << drained << " pending jobs\n"; // 4

        // Submitting to unregistered client throws
        try {
            sched.submit("temp", []() {});
        } catch (const std::runtime_error& e) {
            std::cout << "submit after unregister threw: " << e.what() << '\n';
        }

        // Pool shuts down cleanly with no 'temp' client
        sched.register_client("active");
        std::atomic<int> done{0};
        for (int i = 0; i < 5; ++i) sched.submit("active", [&]() { ++done; });

        ThreadPool pool(sched, 2);
        pool.shutdown();
        std::cout << "Active client ran " << done.load() << " jobs\n"; // 5
    }

    std::cout << "\nAll demos complete.\n";
    return 0;
}
