# Baton: System Design

> Living version (with diagram and comments): the "Baton: System Design" doc in the project.
> This copy lives with the code. Update both when the design changes.

## Overview

Baton is a single-node, crash-safe coordination server that lets several AI coding agents work on one repo at the same time without colliding. Agents connect through MCP, claim tasks, lock the files they will edit, leave progress notes, and mark work done. If an agent dies, its task and locks return to the pool and the next agent resumes from its notes.

**Goals**

- No task is ever held by two agents at once, and no completed task is ever lost, even across `kill -9`
- A crashed agent's task and file locks are reclaimed automatically within one lease period
- Two agents can never hold overlapping file paths (locking `src/` blocks `src/parser/lexer.cpp` and the reverse)
- The next agent on a task sees every note left by earlier attempts
- Every error tells the agent what to do next, in plain language

**Non-goals for v1**

- Replication or multi-node failover (one server; durability comes from the log)
- Authentication and multi-tenant isolation (trusted local network only)
- Log compaction and snapshots (the log is replayed in full on startup)
- A web dashboard (the README and a demo GIF are the front end)

## Typical use case

Four Claude Code agents add a live-game API to PreSnap in parallel, and one crashes halfway without losing work or causing an edit conflict.

**Setup.** You run `baton-server` locally and add the Baton MCP shim to each agent's config. A planner agent (or you) breaks the feature into tasks:

| Task | Paths it will touch |
| --- | --- |
| T1: Add play-by-play ingest endpoint | `api/ingest/` |
| T2: Add live feature builder | `features/live/` |
| T3: Add inference route | `api/predict/`, `models/loader.py` |
| T4: Update README and API docs | `README.md`, `docs/` |

**What happens**

1. Agents A, B, C and D each call `list_tasks`, then `claim_task`. Each claim names the paths the task needs. Baton grants T1 to A, T2 to B, T3 to C and T4 to D, and locks each set of paths.
2. Agent B realizes it also needs `models/loader.py` and calls `lock_paths`. Baton refuses: C holds it, with about 40 seconds left on its current lease. B adds a note ("needs loader change after T3") and works on the rest of T2.
3. Each agent's shim sends a heartbeat every 20 seconds while the agent works. Each heartbeat renews the lease on its task and its locks.
4. Agent C finishes the loader change, calls `add_note` ("loader refactored; route handler half done, tests in tests/test_predict.py failing on empty payload"), and then its process crashes. Its shim dies with it, so heartbeats stop.
5. No heartbeat arrives for 60 seconds. Baton's lease timer fires: T3 goes back to *open*, and C's locks on `api/predict/` and `models/loader.py` are released.
6. Agent A finishes T1 and calls `complete_task`. It then claims the next open task, which is T3. The claim response includes C's note, so A continues from the failing test instead of starting over.
7. B retries its lock on `models/loader.py`. A now holds it as part of T3, so Baton again says who holds it and for how long. B keeps working on its other files.
8. A completes T3, which releases its locks. B takes the loader lock, finishes T2 and completes it. D completes T4.

**Result.** All four tasks are done, no two agents ever edited the same file, and the crash cost one lease period (60 s) instead of C's half-finished work. Every step above was written to the log, so restarting the server at any point would replay the same state.

## Architecture

One C++ process owns all state, and the Python shims are thin translators, so every hard problem lives in a single thread you can reason about.

```
 Agent A        Agent B        Agent C        Agent D
    | MCP over stdio  |             |             |
 MCP shim       MCP shim       MCP shim       MCP shim      (Python, one per agent,
    | NDJSON over TCP |             |             |          auto-heartbeats)
 +--v--------------v-------------v-------------v---------------------------+
 |  baton-server (C++20, one thread)                                        |
 |                                                                          |
 |  epoll event loop  -->  Request handler  -->  WAL writer ----------------+--> baton.wal
 |  (sockets, reads,       (parse, prepare        (append; one fsync        |    (disk, CRC
 |   lease timers)          a Mutation)            per loop iteration)      |     per record)
 |                              |                                           |        |
 |                              | apply after fsync                         |        |
 |              +---------------+---------------+                           |        |
 |              v               v               v                           |        |
 |         Task table      Lease heap       Path trie   <-------------------+--------+
 |         (tasks+notes)   (next expiry)    (file locks)    replay on startup
 +--------------------------------------------------------------------------+
```

Each request flows down: the event loop reads a full line, the handler validates it and builds a `Mutation`, the WAL writer appends it and fsyncs once per batch, and only then is the mutation applied to the task table, lease heap and path trie and the reply sent. The event loop also acts as the lease timer: its `epoll_wait` timeout is the time until the heap's next expiry. On startup, the log is replayed into the same three structures before any connection is accepted.

### Code map

| Piece | Files | Notes |
| --- | --- | --- |
| Types, clock | `types.h`, `clock.h` | `FakeClock` lets tests control time |
| Wire protocol | `protocol.h/.cpp` | Request parsing, response building (done) |
| Mutations | `mutation.h/.cpp` | The 7 state changes and their WAL encoding (done) |
| State + rules | `task_store.h/.cpp` | `prepare()` validates, `apply()` changes state |
| Path locks | `path_trie.h/.cpp` | Hierarchical trie |
| Leases | `lease_heap.h/.cpp` | Min-heap with lazy deletion |
| Durability | `wal.h/.cpp` | Record format, writer, replay |
| Networking | `event_loop.h/.cpp` | epoll loop, group commit |
| Entry point | `main.cpp` | Flags, replay, serve (done) |

**prepare / apply split.** `TaskStore::prepare(req, now)` checks a request against current state and returns either a `Mutation` or an error, without changing anything. `TaskStore::apply(mutation)` changes state and must be deterministic. The live server and WAL replay both go through `apply()`, so recovery runs exactly the code that built the state in the first place.

## Data model

All state lives in memory in four structures, and every change to them is written to the log first. See `server/include/baton/types.h` for `Task` and `Note`.

| Structure | Type | Purpose |
| --- | --- | --- |
| Task table | `unordered_map<TaskId, Task>` | O(1) lookup by task id |
| Open queue | `std::set<TaskId>` of open task ids | Ordered, so agents get the oldest open task first |
| Lease heap | `priority_queue<{expiry, task, gen}>`, min on expiry | Finds the next lease to expire in O(log n); stale entries are skipped when popped |
| Path trie | One node per path segment | Detects overlapping path locks in O(path depth) |

Locks belong to the **task**, not the agent. When a task's lease expires or the task completes, its paths are released in one step.

## API

Agents see seven MCP tools. The shim maps each to one request on the TCP protocol, so the C++ server never deals with MCP directly. Heartbeats are not a tool: the shim sends them automatically every 20 s for every task the agent holds, because models cannot be trusted to remember.

| MCP tool | Wire op | Arguments | Returns | Logged |
| --- | --- | --- | --- | --- |
| `create_task` | `create` | title, description | task id | Yes |
| `list_tasks` | `list` | state filter (optional) | tasks with state, holder, lease time left | No |
| `claim_task` | `claim` | task_id (optional, else oldest open), paths | task with all prior notes, lease expiry | Yes |
| (automatic) | `heartbeat` | task | new lease expiry | No |
| `lock_paths` | `lock` | task_id, paths | ok, or who holds what | Yes |
| `add_note` | `note` | task_id, text | ok | Yes |
| `complete_task` | `complete` | task_id, summary (`text`) | ok; locks released | Yes |
| `release_task` | `release` | task_id, reason (`text`) | ok; task back to open | Yes |

**Wire protocol.** Newline-delimited JSON over TCP, one request per line, one response per line, matched by `req_id`. The shim fills in `agent`.

```json
{"req_id": 17, "op": "claim", "agent": "agent-c", "task": 3, "paths": ["api/predict/", "models/loader.py"]}
{"req_id": 17, "ok": true, "task": {"id": 3, "title": "Add inference route", "notes": []}, "lease_expiry": 1759600060000}
```

**Errors written for agents.** Every failure carries a machine code plus a sentence the agent can act on:

```json
{"req_id": 22, "ok": false, "code": "PATH_LOCKED",
 "message": "models/loader.py is locked by agent-c (task 3); its lease has about 40 more seconds unless renewed. Open tasks you could take instead: 4, 6.",
 "held_by": {"agent": "agent-c", "task": 3, "expires_in_ms": 41000}}
```

Codes: `BAD_REQUEST`, `NOT_FOUND`, `TASK_TAKEN`, `NOT_HOLDER` (acting on a task you no longer hold, for example after your lease expired), `NO_OPEN_TASKS`, `PATH_LOCKED`, `INTERNAL`. The shim adds `UNAVAILABLE` when it cannot reach the server.

## Leases and path locks

A claim is only valid while its lease is renewed; the event loop itself is the timer, so no extra threads are needed.

**Leases**

- Lease length: 60 s (`kLeaseMs`). Shims heartbeat every 20 s, so two missed heartbeats are tolerated.
- On claim or heartbeat: set `lease_expiry = now + 60s` and push `{expiry, task, lease_gen}` onto the heap.
- On claim only: increment `lease_gen`. Any older heap entry for that task now has a stale generation.
- Expiry check: each loop iteration calls `epoll_wait` with a timeout equal to the time until the heap's earliest expiry. After it returns, pop every entry whose expiry has passed. Skip it if the task's `lease_gen` or `lease_expiry` no longer match (a later heartbeat or claim superseded it). Otherwise expire the task: log an `ExpireM`, set it to Open, release its paths.
- Heap growth: each heartbeat adds an entry, so the heap holds about (active tasks × heartbeats per lease). Stale entries drain as they reach the top. That is fine at this scale; a timer wheel is the upgrade if it ever shows up in `perf`.

**Path locks (hierarchical)**

Paths are split on `/` and stored in the trie. Locking path P for task T succeeds only if:

1. No **ancestor** of P is locked by another task. Walking down the trie from the root checks this in O(depth).
2. P itself is not locked by another task.
3. No **descendant** of P is locked by another task. If `locked_below` at P is 0, this is O(1). If not, a DFS of P's subtree finds the holders, which is also how the error message names them.

If every path in a request passes, all are locked together; if any fails, none are (all or nothing). Locking increments `locked_below` on every node from the root to P's parent; unlocking decrements it and prunes empty nodes.

| Held | Requested | Result |
| --- | --- | --- |
| `src/` by T1 | `src/parser/lexer.cpp` by T2 | Refused: ancestor locked |
| `src/parser/lexer.cpp` by T1 | `src/` by T2 | Refused: descendant locked |
| `src/parser/` by T1 | `src/api/` by T2 | Granted: siblings |
| `README.md` by T1 | `README.md` by T1 | Granted: same task |

## Durability

The rule is: write the change to the log, `fsync`, then apply it in memory, then reply. An agent never hears "ok" for something that could be lost.

**Record format (little-endian)**

| Field | Size | Notes |
| --- | --- | --- |
| `len` | 4 bytes | Length of `type` + `payload` |
| `crc32` | 4 bytes | CRC-32 (IEEE) of `type` + `payload` |
| `type` | 1 byte | Create, Claim, Lock, Note, Complete, Release, Expire |
| `payload` | `len - 1` bytes | The mutation's fields as JSON (v1; easy to inspect with `strings`) |

**Group commit.** Requests that arrive in the same event-loop iteration are appended together and covered by one `fsync` before any of them is answered. Under load this turns many fsyncs into one, and it is the main lever in the benchmarks.

**Heartbeats are not logged.** Logging them would multiply writes by about 3 for no benefit. Instead, on recovery every task that replays as Claimed gets a fresh lease of `now + 60s` (`rearm_leases`). A live agent keeps it with its next heartbeat; a dead one loses it one lease later.

**Recovery on startup**

1. Open the log and read records in order.
2. For each record, check `len` and `crc32`. A short or corrupt record at the end is a torn write from the crash: truncate the file at that point and stop.
3. Decode each valid record into a `Mutation` and `apply()` it, the same function the live server uses.
4. Give every Claimed task a fresh lease, rebuild the heap, then start accepting connections.

A corrupt record in the middle of the file (not the tail) means real disk damage; the server refuses to start rather than guess.

## Failure scenarios

| Failure | What Baton does | Mechanism |
| --- | --- | --- |
| Agent crashes mid-task | Task and its locks return to open after 60 s; next claimer gets its notes | Lease expiry, notes on task |
| Agent hangs but its process stays alive | The shim keeps heartbeating, so the task stays held until the agent releases it or exits | Known gap; a max-lease-age cap is future work |
| Agent is slow, lease expires, then it calls `complete_task` | Refused with `NOT_HOLDER`; told someone else holds the task now | Holder check on every write |
| Two agents claim the same task at once | Exactly one wins; the other gets `TASK_TAKEN` | Single-threaded loop: requests are applied one at a time |
| Server killed between log append and `fsync` | Unsynced records may be lost, but no client was told "ok" for them | Reply only after `fsync` |
| Server killed mid-write, leaving a torn record | Truncated on recovery; everything before it replays | Length + CRC per record |
| Server restarts while agents hold tasks | Claims survive; each gets a fresh 60 s lease | Replay, then re-arm leases |
| Client disconnects mid-request | Request either fully applied or not at all; client retries | Apply happens only after a full line is parsed |
| Disk full | Server stops accepting writes and returns an error; reads still work | Check `write`/`fsync` return values |

**Known limit.** A retry after a lost reply can repeat a non-idempotent call (for example `add_note` twice). Idempotency keys on writes fix this; see Future work.

## Deployment

Kubernetes is used to prove fault tolerance at scale, not to scale the server: one server pod, many agent pods, and a chaos job that keeps killing them.

| Piece | Kubernetes object | Notes |
| --- | --- | --- |
| Server | StatefulSet, 1 replica | Stable name and identity; restarts in place after a kill |
| Log storage | PersistentVolumeClaim | `baton.wal` lives here, so a restarted pod replays the same log |
| Server address | Headless Service | Agents connect to `baton-0.baton:7000` |
| Simulated agents | Deployment, scaled to 100+ | Python load generator that claims, locks, notes, completes, and sometimes "crashes" on purpose |
| Chaos | Job running a script | Deletes random server and agent pods during the run |
| Invariant check | Job, run after the test | Reads final state and agent logs; fails if any invariant broke |

1. **Local:** a `kind` cluster for day-to-day chaos testing.
2. **Cloud, once:** a small GKE cluster provisioned with Terraform. Run the full chaos test at 100+ agents, record results and a demo, then `terraform destroy`.

## Testing and benchmarks

**Invariants** (checked after every crash/chaos run):

1. No task was ever held by two agents at the same time.
2. Every `complete_task` that got "ok" is still Done after any number of restarts.
3. No two tasks ever held overlapping paths at the same time.
4. Every task held by a killed agent returned to Open within one lease period (+ 1 s slack).
5. Notes are never lost: the count after recovery equals the count acknowledged.

**Test layers**

- **Unit (GoogleTest):** `tests/`. Spec tests for the parts still to build are `DISABLED_`; enable them as you go.
- **Crash test (Python):** load generator + `kill -9` at random points 100+ times, invariants after each run.
- **Chaos test (Kubernetes):** same invariants, with server and agent pods killed under 100+ agent replicas.

**Benchmarks to report**

| Metric | How it is measured |
| --- | --- |
| Claims per second | Load generator with N connections, each claim-then-complete in a loop |
| p50 and p99 claim latency (µs) | Client-side timing per request |
| Throughput with vs without group commit | `fsync` per request vs per loop iteration |
| Recovery time | Process start to accepting connections, for logs of 10k, 100k and 1M records |
| Lease reclaim time | Agent kill to task back in Open |

## Design tradeoffs

| Decision | Chosen | Alternative | Why |
| --- | --- | --- | --- |
| Concurrency | Single-threaded epoll loop | Thread pool with locks | No data races; `fsync`, not CPU, is the bottleneck; same model as Redis |
| Persistence | Write-ahead log, full replay | Snapshots + compaction | Far less code; replay of 1M records should still take seconds (to be measured) |
| Lease timers | Min-heap with lazy deletion | Timer wheel | About 30 lines, O(log n); swap only if `perf` shows it |
| Transport | NDJSON over TCP + Python MCP shim | HTTP in C++ | Keeps C++ focused on systems work; the MCP SDK handles protocol details |
| Heartbeats | Sent by the shim, not logged | Agent calls a tool; log every one | Models forget; logging them triples writes for no correctness gain |
| Lock scope | Locks owned by tasks | Locks owned by agents | One expiry path frees both the task and its files |
| WAL payload | JSON | Hand-rolled binary | Debuggable; switch only if encoding shows up in `perf` |

**Why not Redis?** Redis could store the same data, and in production that is the sensible starting point. Baton exists to (1) learn durability and crash recovery by building them, and (2) put agent-specific behavior in the server itself: path locks tied to leases, handoff notes on reclaim, and errors written for LLMs.

## Future work

- [ ] Idempotency keys on writes, so retries after lost replies are safe
- [ ] Max lease age, so a hung-but-alive agent cannot hold a task forever
- [ ] Snapshots and log compaction
- [ ] Task dependencies, with cycle detection
- [ ] Terminal UI (FTXUI) showing tasks, holders and locks live
- [ ] Primary-backup replication for failover
