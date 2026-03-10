# Architecture

## Components

### `Scheduler`
Central coordinator. Owns the client registry (`clients_` map + `client_order_` vector) and the scheduling policy. Exposes `submit()`, `select_next_job()`, `record_execution()`, `cancel_job()`, `drain_client()`, and observer management.

### `ThreadPool`
Owns `N` `std::jthread` workers. Each runs `worker_loop()`: calls `select_next_job()`, executes the task outside any lock, then calls `record_execution()`. Supports GRACEFUL (drain then stop) and IMMEDIATE (drain atomically then kill) shutdown modes.

### `ClientState` (CCB — Client Control Block)
Per-client state: four priority queues (`queues[4]`), a `std::mutex` for queue access, `std::condition_variable` for BLOCK-strategy backpressure, and atomic metrics (`submitted_count`, `executed_count`, `expired_count`, `overflow_count`).

### `ISchedulingPolicy`
Abstract interface for job selection. Called inside `rr_mutex_` with read-locked registry. Implementations:
- `WeightedRoundRobinPolicy` — WRR with per-client weight and `rr_remaining_` counter
- `DeficitRoundRobinPolicy` — DRR with per-client deficit accumulation and `base_quantum`

### `IMetricsObserver`
Event interface for out-of-band observability. Installed via `set_observer()` using `std::atomic<std::shared_ptr<IMetricsObserver>>` (C++20 lock-free). Callbacks: `on_job_submitted`, `on_job_executed`, `on_job_expired`, `on_job_cancelled`. Must be non-blocking and must not call back into Scheduler write paths.

---

## Data Flow

```
Client thread
    │
    ▼
Scheduler::submit(client_id, task, cost, priority, deadline)
    ├─ shared_lock(registry_mutex_)    — find ClientState
    ├─ unique_lock(client->mutex)      — backpressure check + enqueue
    ├─ client->submitted_count++
    └─ observer->on_job_submitted()    — load(acquire), call outside locks

Worker thread
    │
    ▼
Scheduler::select_next_job()
    ├─ shared_lock(registry_mutex_)
    └─ loop:
        ├─ lock_guard(rr_mutex_) → policy->select_next_job()
        │     └─ unique_lock(client->mutex) inside policy
        ├─ job.is_expired()? → expired_count++, observer->on_job_expired()
        └─ return job (or nullopt)

Worker thread (outside all locks)
    ├─ job.task()
    └─ Scheduler::record_execution(cid, jid, duration)
        ├─ shared_lock(registry_mutex_)
        ├─ executed_count++, total_execution_time_us++
        └─ observer->on_job_executed()
```

---

## Design Decisions

**Pluggable policy**: `ISchedulingPolicy` lets callers inject WRR, DRR, or custom policies without changing `Scheduler`. The policy is called inside `rr_mutex_` which serializes access to mutable policy state.

**Per-client mutex**: Each `ClientState` has its own `std::mutex`. Workers only hold it during the brief dequeue operation (inside the policy). This allows N workers to drain N different clients simultaneously.

**`std::shared_mutex` on registry**: Registration is infrequent; `select_next_job()` and `submit()` take shared locks so multiple workers/submitters proceed in parallel.

**`std::atomic<std::shared_ptr<IMetricsObserver>>`**: Zero-contention on the hot path. Observer reads use `memory_order_acquire`; writes use `memory_order_release`. C++20 required. The observer pointer is loaded once per event; the callback runs after all scheduler locks are released to avoid re-entrancy.

**Deadline field on Job**: `is_expired()` is checked by `select_next_job()` after dequeue. Expired jobs increment `expired_count` and fire `on_job_expired()`, then the policy loop continues — no job is lost silently.

**Priority queues**: Four `std::deque<Job>` per client (indexed by `Priority` enum). `dequeue_highest()` scans from CRITICAL down; FIFO within each level.
