# Week 1 walkthrough: event loop + in-memory core

What got built: `TaskStore` create/claim/complete (`server/src/task_store.cpp`), the epoll server
(`server/src/event_loop.cpp`), a `--no-wal` flag (`server/src/main.cpp`), and end-to-end tests over
real sockets (`scripts/test_server.py`). The WAL, leases, notes and path locks are still stubs.

## How one request flows through the code

Say agent A sends `{"req_id": 3, "op": "claim", "agent": "a"}\n`.

1. **The kernel queues the bytes** on A's socket. `epoll_wait` in `Server::run` returns with A's
   fd marked `EPOLLIN`.
2. **`on_readable` does one `read()`** of up to 64 KiB into a stack buffer and appends it to
   `in_buf`. TCP is a byte stream, so this read might contain half the line, exactly the line, or
   five lines and the start of a sixth.
3. **Line splitting.** It scans `in_buf` for `'\n'`. Each complete line goes to `handle_line`; the
   bytes after the last newline stay in `in_buf` for next time. All handled lines are erased in one
   `erase` at the end, not one per line.
4. **`handle_line` parses** with `parse_request` (throws `ProtocolError` on bad input; caught and
   turned into a `BAD_REQUEST` reply carrying the request's `req_id`).
5. **`TaskStore::prepare`** checks the claim against current state, without changing anything. With
   no task id it picks `*open_.begin()`, the oldest open task. It returns a `ClaimM` that already
   contains every decision: task id, agent, paths, and `expiry = now + 60s`.
6. **Append, then apply.** If there is a WAL, `to_record(m)` is appended to its in-memory buffer
   (no disk I/O yet). Then `TaskStore::apply` carries out the `ClaimM`: state, holder,
   `lease_gen++`, `attempts++`, remove from `open_`. It returns the response body.
7. **The reply is held.** `handle_line` pushes `{fd, reply line}` onto `held_`. Nothing is sent.
8. **After every event in this `epoll_wait` batch is handled**, `commit_and_release_replies` runs:
   one `wal_->sync()` (skipped with `--no-wal`), then each held reply is appended to its
   connection's `out_buf`, in arrival order, and `flush` tries to `send()` it right away.
9. **Partial writes.** If `send()` accepts only part of `out_buf` (socket buffer full), the rest
   stays queued, `update_interest` turns on `EPOLLOUT` for that fd, and a later iteration finishes
   the job when epoll says the socket is writable. `EPOLLOUT` is turned off again once drained.
10. **Closing** happens last, in `close_marked`, for clients that hung up or errored this iteration.

## Design decisions and their alternatives

| Decision | Chosen | Alternative | Why |
| --- | --- | --- | --- |
| Commit ordering | Prepare + apply each request immediately; hold *all* replies until the iteration's one `sync()` | Prepare the whole batch, `sync()`, then apply all (the original doc) | The original lets two claims for one task in the same batch both validate against unchanged state and both win. Holding every reply (reads and errors too) keeps "ok" meaning "on disk" and keeps replies in request order. Cost: if `sync()` fails, memory is ahead of disk, so the server must exit and replay. |
| epoll mode | Level-triggered | Edge-triggered (`EPOLLET`) | With LT, reading part of what is available is safe: epoll reports the fd again. With ET you must drain every socket until `EAGAIN` or that client stalls silently. |
| Reads per wakeup | One `read()` per client per iteration | Read until `EAGAIN` | Fairness: a client blasting data cannot starve the others. LT makes it safe. It also means the read that sees EOF never shares an iteration with that client's other requests. |
| `EPOLLOUT` | Registered only while output is pending | Always registered | A socket is writable almost all the time; with LT an always-on `EPOLLOUT` wakes the loop constantly (busy loop). |
| Closing fds | Deferred to the end of the iteration | `close()` the moment an error is seen | The kernel hands out the lowest free fd number. Closing fd 7 mid-iteration and then accepting a new client as fd 7 would let a stale event or held reply for the old client hit the new one. |
| Signals | `signalfd` in the epoll set | `sigaction` handler setting a flag | A handler runs at an arbitrary instruction and may only call async-signal-safe functions. `signalfd` turns the signal into an ordinary readable fd handled at a normal point in the loop. |
| `SIGPIPE` | `send(..., MSG_NOSIGNAL)` | Default behavior | Writing to a socket the peer has closed raises `SIGPIPE`, which kills the process by default. With the flag it is just an `EPIPE` error for that one connection. |
| Nagle | `TCP_NODELAY` on every client socket | Default | Small request/reply traffic plus Nagle plus the client's delayed ACK can add ~40 ms per request. |
| Memory bounds | 1 MiB max line; stop reading a client with 16 MiB of unread replies | Unbounded buffers | One misbehaving client must not be able to grow server memory without limit. |
| Ids and lease expiry | Chosen in `prepare`, stored in the Mutation | Chosen in `apply` (e.g. `next_id_++`, `clock.now()`) | `apply` must be deterministic so replay rebuilds identical state. Anything decided from current state or time is decided once and logged. |
| `--no-wal` | `Server` takes a nullable `WalWriter*` | A fake "null WAL" class | Smallest change; the loop runs the same steps either way. |
| Week-1 stubs | `note`/`lock`/`release`/`heartbeat` answer with a clear error; the epoll timeout is `-1` | Let them hit `todo()` and throw | The shim sends heartbeats automatically; they must get a normal error, not an `INTERNAL`. |

## Bugs and edge cases hit along the way

- **The double claim in the original design.** Found while planning, before any code: "prepare
  everything, fsync, then apply" validates every request against the same stale state. Fixed by
  applying each request before preparing the next. `test_two_claims_in_one_batch_cannot_both_win`
  sends both claims in one write so they land in one `read()` and one iteration.
- **fd reuse.** Deciding where to close connections showed the stale-event / held-reply risk above.
  Hence `to_close_` and `close_marked()`.
- **Quadratic output flushing.** `flush` first did `out_buf.erase(0, n)` after every `send()`. With
  a 3 MB `list` reply going to a slow reader a few KB at a time, every partial send shifts the
  whole remaining buffer: quadratic copying. Spotted while investigating why the slow-reader test
  dominated the suite's runtime. It now advances `out_off` and only compacts once more than half the
  buffer has been sent, so each byte is copied a constant number of times. (Not measured before and
  after: the VM went down first. The fix stands on the analysis; the suite passes with it in CI.)
- **AddressSanitizer pathologies (tooling, not Baton).** On macOS with recent Apple clang, ASan test
  binaries hang inside `__malloc_init` before `main` runs: the Mac build now uses
  `-DBATON_SANITIZE=OFF`. In the arm64 Linux VM with GCC 13, ASan's "stack use after return" check
  made a single 1 KB `nlohmann::json::parse` take ~50 ms (versus 0.08 ms without it), measured with
  no Baton code involved. The end-to-end tests turn just that check off via `ASAN_OPTIONS`.
- **The dev VM itself.** Restarting the Multipass daemon while the VM ran orphaned the old QEMU
  process, and the daemon then started a second copy on the same disk image. Symptoms looked like
  flaky mounts and networking. Lesson: stop VMs before restarting their supervisor, and check for
  duplicate processes when infrastructure behaves strangely. Week 1 was verified in CI instead.
- **Building on a shared folder.** A Multipass mount that dropped mid-link left a 0-byte
  `baton_tests` that `make` considered up to date. The Linux build directory now lives on the VM's
  own disk.

## Interview questions

1. Walk through what happens, syscall by syscall, from a client's `connect()` to receiving the reply
   to its first `claim`.
2. Why does the server read into a per-connection `in_buf` instead of parsing whatever one
   `read()` returns? Give two concrete byte sequences that would break the naive version.
3. Level-triggered vs edge-triggered epoll: what does each guarantee, and what bug is easy to write
   with edge-triggered? Why does reading only once per wakeup require level-triggered mode?
4. Why is `EPOLLOUT` only registered while there is unsent output? What happens if it is always on?
5. What is a short write? Where can it happen in this server, and what does the code do with the
   bytes that were not written?
6. Two agents send `claim task 1` and their requests are read in the same loop iteration. Explain
   why "prepare all, fsync, apply all" breaks, and how the current ordering prevents it.
7. Every reply waits for the `fsync`, even a `list` that changes nothing. Why?
8. If `fsync` fails, why can't the server just send errors for that batch and keep running?
9. Why are connections closed at the end of the iteration instead of immediately? Describe the
   exact failure the alternative allows.
10. What is `SIGPIPE`, when would this server receive one, and what are two ways to avoid dying
    from it?
11. Why does `prepare` choose the task id and lease expiry instead of `apply`? What would break in
    replay otherwise?
12. What does `SO_REUSEADDR` fix, and why does it matter for a server that is restarted by crash
    tests?
13. What is `TCP_NODELAY`, and how can Nagle's algorithm interact with delayed ACKs to add latency?
14. How does the server protect itself from a client that sends an endless line, and from one that
    sends many requests but never reads the replies?
15. `out_buf.erase(0, n)` after each `send()` turned out to be quadratic. Explain why, and why
    compacting only once more than half is consumed makes the total copying linear.
