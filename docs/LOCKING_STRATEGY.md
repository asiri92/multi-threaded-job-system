# Locking Strategy

## Lock Hierarchy

Locks must always be acquired in this order to prevent deadlock:

```
registry_mutex_  (shared_mutex)     — outermost
  └─ rr_mutex_   (mutex)            — policy state
       └─ client->mutex (mutex)     — innermost: queue ops
            └─ submit_cv_           — condition variable (BLOCK strategy)

cv_mutex_                           — independent (worker sleep)
observer_                           — atomic<shared_ptr>, no lock needed
```

| Lock | Type | Protects | Held By |
|------|------|----------|---------|
| `registry_mutex_` | `shared_mutex` | `clients_`, `client_order_` | All public methods |
| `rr_mutex_` | `mutex` | Policy state (`rr_remaining_`, deficit map, etc.) | `select_next_job()`, `update_client_weight()`, `unregister_client()` |
| `client->mutex` | `mutex` | Per-client `queues[]`, backpressure CV | `submit()`, policy `select_next_job()`, `drain_client()`, `cancel_job()` |
| `cv_mutex_` | `mutex` | `cv_` condition variable | Worker sleep/wake |

## Key Invariants

1. **Workers execute jobs outside all locks.** `job->task()` is called after releasing every lock. This means the task can safely call `submit()` or read metrics without deadlock.

2. **`registry_mutex_` is never held across blocking operations.** `drain_client()` acquires a shared lock to get the `ClientState` pointer, releases it, then acquires `client->mutex`. The pointer stays alive via `shared_ptr` reference counting.

3. **`drain_all_clients()` snapshots `client_order_` first.** It takes a brief shared lock, copies the vector, releases, then calls `drain_client()` for each ID. This avoids holding the registry lock across multiple `drain_client()` calls that each re-acquire it.

4. **Observer callbacks are outside all scheduler locks.** Observer is loaded with `memory_order_acquire`, then called after the scheduler lock has been released (or not held). Exception: `on_job_cancelled` is fired while `client->mutex` is held in `cancel_job()`. Therefore, observers **must not call `submit()`** (which acquires `client->mutex`).

## Observer Re-entrancy Constraint

`on_job_cancelled` is the only callback that fires while `client->mutex` is held. Observer implementations must not call back into Scheduler methods that acquire `client->mutex` (`submit()`, `drain_client()`) from within `on_job_cancelled`. All other callbacks (`on_job_submitted`, `on_job_executed`, `on_job_expired`) are safe to call any public Scheduler method.

## Contention Analysis

**At low worker counts (1–2):** `rr_mutex_` is uncontested. Per-client mutexes are briefly held during dequeue (~ns). Throughput is near-linear with worker count.

**At high worker counts (4+) for short jobs (1µs):** `rr_mutex_` becomes a bottleneck — all workers contend on it each time they call `select_next_job()`. For a 1µs job, scheduling overhead (~1µs lock acquire/release) is comparable to execution time, causing super-linear slowdown.

**Mitigation paths (future work):**
- Replace `rr_mutex_` with a ready-queue of non-empty clients (lock-free or per-shard). Workers dequeue a client, drain one job, re-enqueue if non-empty.
- Per-worker client assignment (work stealing) to reduce cross-worker contention.
- Batch dequeue: worker dequeues K jobs per lock acquisition.

**For jobs longer than ~10µs**, scaling efficiency is near-linear up to physical core count, as lock contention is negligible relative to execution time.
