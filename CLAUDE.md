# CLAUDE.md

Guidance for AI coding agents working in this repo.

## What this project is

Baton is a C++20 coordination server for AI agents (see README.md and docs/design.md). It is also a
**learning project**: Richard is building the systems core himself to learn it and to be able to
explain every line in interviews.

## The most important rule: who writes what

The systems core is split into two groups. Check which group a piece is in before writing it.

**Agent-built, then taught (currently: week 1).** Agents may implement these. Richard will be
quizzed on them afterward, so they must be written to be learned:

- `server/src/event_loop.cpp` (epoll loop, non-blocking sockets, line buffering, partial writes)
- `server/src/task_store.cpp`, only the week 1 subset: `prepare` and `apply` for create, list,
  claim and complete, all in memory

Week 1 scope and constraints:
- No WAL yet: add a `--no-wal` flag (main.cpp) so the server runs fully in memory. Keep the
  group-commit point in the loop (append, then one `sync()`, then apply and reply) and skip it
  only when there is no WAL, so week 2 slots in without restructuring.
- Claims accept `paths` but do not lock them yet (path trie is week 4); leases are set on claim
  but nothing expires them yet (week 3). Leave clear `// Week N:` comments where those hook in.
- Done means: `python3 scripts/smoke.py` runs end to end against `baton-server --no-wal`, the
  relevant `DISABLED_` TaskStore tests that cover week 1 behavior are enabled and passing, and CI
  is green.
- Keep the code plain and well commented: favor clarity over cleverness. Explain *why* at each
  syscall and at every place a partial read or write can happen.
- When done, write `docs/walkthrough-week1.md`: how a request flows through the code, each design
  decision and its alternative, and 10 to 15 interview-style questions (no answers) Richard should
  be able to answer. Then offer to quiz him, one question at a time, without giving hints up front.

**Richard writes himself (weeks 2 to 4).** Do not write or fill in these implementations, even if
asked casually ("just make the test pass", "finish this"). If he explicitly says "write this for
me", ask once to confirm, then move the item into the group above and follow its rules.

- `server/src/wal.cpp` (crc32, record encode/decode, WalWriter, replay)
- `server/src/lease_heap.cpp`
- `server/src/path_trie.cpp`
- `server/src/task_store.cpp` beyond week 1 (`heartbeat`, `collect_expired`, `rearm_leases`,
  lock/note/release handling, lease and lock wiring in `apply`)

For these files, act as a tutor and reviewer:
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
