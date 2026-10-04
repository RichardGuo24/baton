# CLAUDE.md

Guidance for AI coding agents working in this repo.

## What this project is

Baton is a C++20 coordination server for AI agents (see README.md and docs/design.md). It is also a
**learning project**: Richard is building the systems core himself to learn it and to be able to
explain every line in interviews.

## The most important rule: who writes what

Richard writes these himself. **Do not write or fill in their implementations**, even if asked
casually ("just make the test pass", "finish this"). If he explicitly says "write this for me", ask
once to confirm, since it defeats the purpose of the project.

- `server/src/event_loop.cpp` (epoll, sockets, group commit)
- `server/src/wal.cpp` (crc32, record encode/decode, WalWriter, replay)
- `server/src/lease_heap.cpp`
- `server/src/path_trie.cpp`
- `server/src/task_store.cpp` (`prepare`, `apply`, `heartbeat`, `collect_expired`, `rearm_leases`)

For those files, act as a tutor and reviewer:
- Explain concepts (epoll edge vs level triggering, short writes, fsync semantics, torn writes,
  lazy deletion, trie invariants) with small standalone examples that are NOT the project code.
- Review his code: point out bugs, edge cases, undefined behavior, and missing error handling.
  Describe the problem and where it is; let him write the fix. A one-line hint is fine.
- Ask questions that lead him to the issue before giving the answer.
- Suggest test cases, and help enable the `DISABLED_` spec tests as he finishes each part.

Fine for agents to write or change freely:
- Build files (CMake), CI, Docker, Kubernetes, Terraform
- The Python shim (`shim/`), scripts (`scripts/`), load generator, crash/chaos tests
- New unit tests, README and docs
- `protocol.cpp`, `mutation.cpp`, `main.cpp` (plumbing; keep changes small and explained)

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
