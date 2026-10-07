# Baton

A crash-safe coordination server that lets several AI coding agents work on one repo at the same time without stepping on each other.

Agents connect over [MCP](https://modelcontextprotocol.io). They claim tasks, **lock the files they are about to edit**, leave **handoff notes** as they go, and mark work done. If an agent crashes, its task and file locks are reclaimed after one lease period, and the next agent picks up from its notes instead of starting over.

The server is written from scratch in C++20: a single-threaded `epoll` event loop, a write-ahead log with replay-based crash recovery, a lease heap, and a path trie for hierarchical file locks.

> **Status:** early development. The protocol, mutation codec, MCP shim, epoll event loop and in-memory create/claim/complete work (`--no-wal`); the WAL, leases and locks are being built now. See [Roadmap](#roadmap).

## How it works

```
 Agent A ... Agent D                (Claude Code, etc.)
    | MCP (stdio)
 Python MCP shim, one per agent     (auto-heartbeats every 20 s)
    | newline-delimited JSON over TCP
 baton-server (C++20, one thread)
    epoll loop -> handler -> WAL append + fsync -> apply -> reply
    state: task table, lease heap, path trie
    baton.wal replayed on startup
```

- **Atomic claims.** One thread applies requests in order, so two agents can never hold the same task.
- **Durable before "ok".** Every change is appended to the log and `fsync`'d before the agent hears back. Requests in the same loop iteration share one `fsync` (group commit).
- **Leases.** A claim lives 60 s past its last heartbeat. Dead agent, no heartbeats, task and locks go back to the pool.
- **Hierarchical file locks.** Locking `src/` blocks `src/parser/lexer.cpp` and the reverse; siblings never conflict.
- **Handoff notes.** The next agent to claim a task gets every note from earlier attempts.

Full design: [docs/design.md](docs/design.md).

## Layout

```
server/include/baton/   headers: types, protocol, mutation, task_store, path_trie, lease_heap, wal, event_loop
server/src/             implementations + main.cpp
tests/                  GoogleTest unit tests (spec tests for unbuilt parts are DISABLED_)
shim/                   Python MCP shim (pip install -e shim)
scripts/                smoke test client; later: load generator, crash test
deploy/                 later: Docker, kind, GKE + Terraform
docs/                   design doc
```

## Build and test

Requires CMake 3.20+ and a C++20 compiler. nlohmann/json and GoogleTest are used from the system if installed (`apt install nlohmann-json3-dev libgtest-dev`, `brew install nlohmann-json googletest`), otherwise downloaded.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBATON_SANITIZE=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run the server and poke it directly:

```bash
./build/server/baton-server --port 7000 --wal baton.wal   # durable (from week 2)
./build/server/baton-server --port 7000 --no-wal          # in memory only
python3 scripts/smoke.py
```

### Developing on macOS

The server uses `epoll`, so it only builds on Linux. On macOS, CMake builds the core library and
unit tests and skips `baton-server`. Build the Mac side without sanitizers: with recent macOS and
Apple clang, AddressSanitizer binaries hang at startup inside `malloc` initialization.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBATON_SANITIZE=OFF
```

To build and run the server, use a Linux VM such as [Multipass](https://multipass.run) that mounts
the repo. Keep the Linux build directory on the VM's own disk, not in the mounted folder: it is much
faster, and a mount that drops mid-link can leave a truncated binary that `make` thinks is fresh.

```bash
multipass exec primary --working-directory /home/ubuntu -- bash -c "cd /path/in/vm/to/baton && \
  cmake -S . -B ~/baton-build -DCMAKE_BUILD_TYPE=Debug -DBATON_SANITIZE=ON && \
  cmake --build ~/baton-build -j && ctest --test-dir ~/baton-build --output-on-failure"
```

If the repo lives under `~/Desktop` or `~/Documents`, give `multipassd` Full Disk Access
(System Settings → Privacy & Security) or the VM sees those folders as empty.

## Use it from an agent

```bash
pip install -e shim
```

Add to the repo's `.mcp.json` (one per agent; give each a distinct id):

```json
{
  "mcpServers": {
    "baton": {
      "command": "baton-shim",
      "env": { "BATON_HOST": "127.0.0.1", "BATON_PORT": "7000", "BATON_AGENT_ID": "agent-a" }
    }
  }
}
```

Tools the agent gets: `create_task`, `list_tasks`, `claim_task`, `lock_paths`, `add_note`, `complete_task`, `release_task`.

## Roadmap

- [x] Wire protocol, mutation codec, MCP shim, CI
- [x] Week 1: epoll event loop; create, list, claim, complete in memory
- [ ] Week 2: write-ahead log, replay, `kill -9` crash test
- [ ] Week 3: leases with expiry, handoff notes
- [ ] Week 4: hierarchical path locks
- [ ] Week 5: load generator, benchmarks, `perf`, dogfood with real agents
- [ ] Week 6: Kubernetes chaos test (kind, then GKE via Terraform)

## Benchmarks

Coming in week 5: claims/sec, p50/p99 latency, group commit on vs off, recovery time, lease reclaim time.
