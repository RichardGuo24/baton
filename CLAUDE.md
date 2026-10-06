# CLAUDE.md

Guidance for AI coding agents working in this repo.

## What this project is

Baton is a C++20 coordination server for AI agents (see README.md and docs/design.md). It is also a
**learning project**: Richard must be able to explain every line in interviews.

## How this project is built

Agents build the whole system, one week at a time. Richard is quizzed after each week, so every
piece must be written to be learned: plain, well-commented code that explains *why* at each
syscall, at every partial read/write, and at every durability or ordering decision. Favor clarity
over cleverness.

Follow docs/design.md. If you need to deviate from it, say so and update the doc in the same commit.

### Weekly plan

Work in small steps and commit after each one that builds and passes tests. Keep CI green.

1. **Week 1: event loop + in-memory core.** `event_loop.cpp` (epoll, non-blocking sockets, line
   buffering, partial writes) and `task_store.cpp` create, list, claim, complete. Add a `--no-wal`
   flag so the server can run fully in memory, but keep the group-commit point in the loop
   (prepare, apply and append each request; one `sync()`; then send the held replies). Done: `scripts/smoke.py` runs end to end.
2. **Week 2: durability.** `wal.cpp` (crc32, encode/decode, WalWriter with short-write handling,
   replay with torn-tail truncation). Wire group commit. Add `scripts/crash_test.py`: load +
   `kill -9` at random points, restart, check the invariants in docs/design.md. Done: all `Wal`
   tests enabled and passing; crash test passes 100+ kills locally and a short run in CI.
3. **Week 3: leases + notes.** `lease_heap.cpp`, heartbeat, `collect_expired`, `rearm_leases`,
   note, release, `NOT_HOLDER` handling. Done: `LeaseHeap` and lease/notes `TaskStore` tests pass.
4. **Week 4: path locks.** `path_trie.cpp`, locks on claim and `lock`, release on
   complete/release/expire, `PATH_LOCKED` errors with holder details and open-task suggestions.
   Done: all `PathTrie` and `TaskStore` tests enabled and passing.
5. **Week 5: prove it.** `scripts/loadgen.py`, benchmarks (claims/sec, p50/p99, group commit on vs
   off, recovery time, reclaim time), a `perf` pass, results table in README. Never invent
   numbers: only report what was measured, with the machine described.
6. **Week 6: Kubernetes.** Dockerfiles, kind manifests (server StatefulSet + PVC, headless
   Service, agent Deployment, chaos Job, invariant-check Job), then Terraform for a one-off GKE
   run. Do not create cloud resources or spend money without Richard's explicit go-ahead.

### After each week

Write `docs/walkthrough-weekN.md`: how a request flows through the new code, each design decision
and its alternative, the bugs or edge cases hit along the way, and 10 to 15 interview-style
questions (no answers). Then stop and offer to quiz Richard, one question at a time, no hints up
front, before starting the next week.

Also in scope:
- Build files (CMake), CI, Docker, Kubernetes, Terraform
- The Python shim (`shim/`), scripts (`scripts/`), load generator, crash/chaos tests
- New unit tests, README and docs
- `protocol.cpp`, `mutation.cpp`, `main.cpp`

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBATON_SANITIZE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Spec tests for unbuilt parts are prefixed `DISABLED_`. Remove the prefix when the part is
implemented. CI must stay green: never leave an enabled test failing on `main`.

## Conventions

- C++20, `.clang-format` (Google base, 4-space indent, 100 cols). No exceptions across the epoll
  loop boundary: catch per request and turn errors into error responses.
- No new dependencies without asking. Linux-only syscalls are fine (epoll, fsync, ftruncate).
- Time always comes from a `Clock` (tests use `FakeClock`). Never call the system clock in
  `TaskStore`.
- `TaskStore::apply` must be deterministic; replay depends on it.
- Error messages to agents are full sentences that say what to do next.
- Commit messages: short imperative subject; explain the why in the body when it is not obvious.
