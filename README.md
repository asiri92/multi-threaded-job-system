# Multithreaded Job System

A production-grade multithreaded job system with centralized fair scheduling, pluggable policies, deadline scheduling, backpressure, priorities, cancellation, and live observability. Written in Modern C++20.

---

## Features

| Feature | Milestone |
|---------|-----------|
| Thread pool, round-robin scheduler, per-client metrics, graceful shutdown | M1 |
| Weighted Round Robin (WRR) — per-client throughput quotas | M2 |
| Pluggable scheduling policies (WRR + DRR), backpressure, Jain fairness | M3 |
| Job priorities (LOW/NORMAL/HIGH/CRITICAL), cancellation, dynamic reconfiguration | M4 |
| Deadline scheduling, IMMEDIATE shutdown, `IMetricsObserver` interface | M5 |

---

## Build

```bash
# Set environment (Windows/Clang)
export PATH="$PATH:/c/Program Files/CMake/bin:/c/Program Files/LLVM/bin"
export INCLUDE="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/include;C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/ucrt;C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/um;C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/shared"
export LIB="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/lib/x64;C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/um/x64;C:/Program Files (x86)/Windows Kits/10/Lib/10.0.19041.0/ucrt/x64"

# Configure
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++

# Build
cmake --build build

# Test (49/49)
ctest --test-dir build --output-on-failure

# Benchmarks
./build/benchmarks/mixed_workload_bench.exe
./build/benchmarks/scaling_bench.exe
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Client Threads                      │
│         submit(client_id, task, cost, priority,          │
│                deadline)                                 │
└───────────────────────┬─────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│                      Scheduler                           │
│  ┌──────────────────────────────────────────────────┐   │
│  │  Client Registry  (shared_mutex)                 │   │
│  │  ┌──────────────┐ ┌──────────────┐               │   │
│  │  │ ClientState A│ │ ClientState B│ ...           │   │
│  │  │ queues[4]    │ │ queues[4]    │               │   │
│  │  │ (per-mutex)  │ │ (per-mutex)  │               │   │
│  │  └──────────────┘ └──────────────┘               │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │  ISchedulingPolicy  (rr_mutex)                   │   │
│  │  WeightedRoundRobinPolicy | DeficitRoundRobin     │   │
│  └──────────────────────────────────────────────────┘   │
│                                                          │
│  observer_ (atomic<shared_ptr<IMetricsObserver>>)        │
└──────────┬──────────────────────────────────────────────┘
           │  select_next_job() / record_execution()
           ▼
┌─────────────────────────────────────────────────────────┐
│                      ThreadPool                          │
│  worker_loop × N  (std::jthread)                         │
│  execute job → record_execution → notify_workers         │
│  shutdown(GRACEFUL | IMMEDIATE)                          │
└─────────────────────────────────────────────────────────┘
```

---

## API Quick Reference

### Setup
```cpp
Scheduler sched;                          // default WRR policy
ThreadPool pool(sched, 4);               // 4 worker threads

sched.register_client("A");              // default weight=1
sched.register_client("B", 3);           // 3x throughput weight
sched.register_client("C", 1, 100,       // max 100 queued jobs,
                      OverflowStrategy::DROP_OLDEST);
```

### Submit
```cpp
// Basic
sched.submit("A", []{ /* work */ });

// With priority
sched.submit("A", task, 1, Priority::CRITICAL);

// With deadline (job skipped if expired when dequeued)
auto deadline = std::chrono::steady_clock::now() + 500ms;
sched.submit("A", task, 1, Priority::NORMAL, deadline);
```

### Cancellation & Drain
```cpp
uint64_t job_id = /* captured from observer */;
sched.cancel_job(job_id);         // returns true if still pending
sched.drain_client("A");          // clear all pending jobs for A
```

### Metrics
```cpp
auto m = sched.get_client_metrics("A");
// m.submitted, m.executed, m.avg_execution_time_us
// m.queue_depth, m.weight, m.overflow_count, m.expired_count

auto gm = sched.get_global_metrics();
// gm.total_processed, gm.active_clients, gm.jain_fairness_index
```

### Observer
```cpp
struct MyObserver : public job_system::IMetricsObserver {
    void on_job_submitted(const std::string& cid, uint64_t jid) override { ... }
    void on_job_executed(const std::string& cid, uint64_t jid,
                         std::chrono::microseconds dur) override { ... }
    void on_job_expired(const std::string& cid, uint64_t jid) override { ... }
    void on_job_cancelled(const std::string& cid, uint64_t jid) override { ... }
};

sched.set_observer(std::make_shared<MyObserver>());
```

### Shutdown
```cpp
pool.shutdown();                          // GRACEFUL (default): drain then stop
pool.shutdown(ShutdownMode::IMMEDIATE);   // drain queues atomically, stop workers
```

---

## Benchmark Results

Machine: Windows 11, AMD Ryzen (12-core), Clang 21.1.8
Workload: 4 clients × 10,000 jobs (40,000 total), each job ~1µs compute

### Throughput Scaling

| Workers | Wall (ms) | Jobs/sec | µs/job | Scaling Eff. |
|---------|-----------|----------|--------|--------------|
| 1 | 150.9 | 265,148 | 3.77 | 100.0% |
| 2 | 117.5 | 340,336 | 2.94 | 64.2% |
| 4 | 173.6 | 230,463 | 4.34 | 21.7% |
| 8 | 216.2 | 185,012 | 5.41 | 8.7% |
| 16 | 201.9 | 198,166 | 5.05 | 4.7% |

Sub-linear scaling is expected for 1µs jobs — the `rr_mutex_` policy lock and per-client mutexes become contention bottlenecks relative to the tiny execution time. For longer-running jobs (>100µs), worker count scaling approaches linear. See `docs/LOCKING_STRATEGY.md` for analysis.

### Execution Time Distribution (4 workers, 40,000 jobs)

| Metric | Value |
|--------|-------|
| Min | 1 µs |
| Avg | 1.01 µs |
| Max | 177 µs |

---

## Directory Structure

```
include/job_system/   — Public headers
src/                  — Implementations
tests/                — GoogleTest suites (49 tests)
examples/             — Demo programs
benchmarks/           — Throughput + latency benchmarks
docs/                 — Architecture and locking documentation
```
