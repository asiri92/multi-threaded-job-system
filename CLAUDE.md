# Multithreaded Job System - Project Guide

## Project Overview
Production-grade multithreaded job system with centralized fair scheduling, observability, and benchmarking. Written in Modern C++20.

## Build Commands
```bash
# Set PATH (needed in this shell)
export PATH="$PATH:/c/Program Files/CMake/bin:/c/Program Files/LLVM/bin:/c/Users/asiri/AppData/Local/Microsoft/WinGet/Links"

# Configure (from project root)
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++

# Build
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run demo
./build/examples/basic_demo
```

## Architecture
- **Scheduler** — Central coordinator: manages client registry, round-robin job selection, worker wake-up
- **ThreadPool** — Owns `std::jthread` workers, fetch-execute loop, shutdown semantics
- **ClientState (CCB)** — Per-client queue + mutex + atomic metrics
- **Job** — Move-only struct: client_id, task callable, enqueue timestamp, job ID

## Directory Structure
```
include/job_system/   — Public headers (job.h, client_state.h, scheduler.h, thread_pool.h)
src/                  — Implementation files (scheduler.cpp, thread_pool.cpp)
tests/                — GoogleTest test files
examples/             — Demo programs
docs/                 — Design documentation
```

## Code Conventions
- **C++20** standard, target Clang primary, GCC + MSVC CI
- **Namespace:** `job_system`
- **Header guards:** `#pragma once`
- **Naming:** `snake_case` for functions/variables, `PascalCase` for types/classes, `snake_case_` for private members (trailing underscore)
- **Includes:** `<system>` first, then `"project"` headers
- **Threading:** `std::jthread` for workers, `std::mutex`/`std::shared_mutex` for synchronization, `std::atomic` for metrics
- **No raw `new`/`delete`** — use `std::shared_ptr`, `std::unique_ptr`
- **Move semantics** over copy for Job objects
- Warnings: `-Wall -Wextra -Wpedantic`

## Locking Strategy
1. `std::shared_mutex` on client registry (read-heavy: workers read, registration writes)
2. `std::mutex` per-client on their job queue (workers only hold during dequeue)
3. `std::mutex` + `std::condition_variable` for worker wake-up / shutdown signaling
4. Workers execute jobs **outside** any lock

## Git Workflow (SOP)
- **Never push directly to `main`** — branch protection rejects merge commits
- Always work on a feature branch: `git checkout -b <branch> origin/main`
- Cherry-pick or commit work onto the branch, push, then open a PR via `gh pr create`
- Branch naming convention: `milestone-N` or `feature/<short-description>`

## Current Milestone
**Milestone 2** — Weighted Round Robin Scheduler (complete, PR #4)
- Per-client weight controls consecutive job slots per WRR cycle
- `rr_remaining_` counter piggybacked on `rr_mutex_` (no new locks)
- Default weight=1 preserves full Milestone 1 backward compatibility
- 15 tests passing (8 M1 + 7 M2)
