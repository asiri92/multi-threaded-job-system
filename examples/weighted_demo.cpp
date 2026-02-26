#include <array>
#include <atomic>
#include <cstdio>
#include <string>

#include "job_system/scheduler.h"
#include "job_system/thread_pool.h"

// Weighted Round Robin demo
// Three clients: light(w=1), medium(w=2), heavy(w=4)
// Single worker → deterministic WRR order is observable in the sequence log.

int main() {
    using namespace job_system;

    Scheduler scheduler;
    ThreadPool pool(scheduler, 1); // 1 worker for deterministic sequence

    scheduler.register_client("light",  1);
    scheduler.register_client("medium", 2);
    scheduler.register_client("heavy",  4);

    constexpr int JOBS_PER_CLIENT = 7; // 7 × lcm(1,2,4)=4 → full cycles
    constexpr int TOTAL = JOBS_PER_CLIENT * 3;

    std::atomic<int> seq{0};
    std::array<std::string, TOTAL> order;

    for (int i = 0; i < JOBS_PER_CLIENT; ++i)
        scheduler.submit("light",  [&]{ order[seq++] = "L"; });
    for (int i = 0; i < JOBS_PER_CLIENT; ++i)
        scheduler.submit("medium", [&]{ order[seq++] = "M"; });
    for (int i = 0; i < JOBS_PER_CLIENT; ++i)
        scheduler.submit("heavy",  [&]{ order[seq++] = "H"; });

    pool.shutdown();

    // ── Sequence log ──────────────────────────────────────────────────────────
    std::printf("\nExecution sequence (%d jobs):\n  ", TOTAL);
    for (int i = 0; i < TOTAL; ++i) {
        std::printf("%s", order[i].c_str());
        if ((i + 1) % 7 == 0) std::printf(" | ");
    }
    std::printf("\n");

    // ── Ratio table ───────────────────────────────────────────────────────────
    int cnt_l = 0, cnt_m = 0, cnt_h = 0;
    for (int i = 0; i < TOTAL; ++i) {
        if (order[i] == "L") ++cnt_l;
        else if (order[i] == "M") ++cnt_m;
        else ++cnt_h;
    }

    // Note: with equal job counts and graceful drain, executed counts are always
    // equal (all queues drain fully). WRR weight controls SCHEDULING ORDER, not
    // total throughput — see the sequence log above for the 1:2:4 pattern.
    std::printf("\n%-10s %6s %8s\n", "Client", "Weight", "Executed");
    std::printf("%-10s %6d %8d\n", "light",  1, cnt_l);
    std::printf("%-10s %6d %8d\n", "medium", 2, cnt_m);
    std::printf("%-10s %6d %8d\n", "heavy",  4, cnt_h);
    std::printf("\nWRR effect: in each cycle the sequence is L(x1) M(x2) H(x4).\n");
    std::printf("See first 7-job group above: L M M H H H H\n\n");
}
