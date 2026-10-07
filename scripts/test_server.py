#!/usr/bin/env python3
"""End-to-end tests for baton-server's event loop, over real TCP sockets.

Each test starts a fresh `baton-server --no-wal --port 0` (the kernel picks a free port) and talks
to it the way a client would, including the awkward ways: one byte at a time, many requests in one
write, never reading replies, hanging up mid-request.

  python3 scripts/test_server.py                         # uses build/server/baton-server
  BATON_SERVER=/path/to/baton-server python3 scripts/test_server.py -v

Linux only (the server uses epoll). Standard library only.
"""

from __future__ import annotations

import json
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SERVER = os.environ.get("BATON_SERVER", str(REPO / "build" / "server" / "baton-server"))

# In sanitizer builds, keep every AddressSanitizer check except "stack use after return". With GCC 13
# on arm64 that one check makes each 1 KB JSON parse take ~50 ms instead of ~0.1 ms (measured in
# nlohmann::json alone, no Baton code involved), which turns these tests into a multi-minute crawl.
# Set ASAN_OPTIONS yourself to override.
SERVER_ENV = {**os.environ}
SERVER_ENV.setdefault("ASAN_OPTIONS", "detect_stack_use_after_return=0")


class Server:
    """A baton-server child process on a free port."""

    def __init__(self) -> None:
        self.proc = subprocess.Popen(
            [SERVER, "--host", "127.0.0.1", "--port", "0", "--no-wal"],
            stderr=subprocess.PIPE,
            text=True,
            env=SERVER_ENV,
        )
        assert self.proc.stderr is not None
        line = self.proc.stderr.readline()  # "listening on 127.0.0.1:PORT ..."
        m = re.search(r"listening on [\d.]+:(\d+)", line)
        if not m:
            self.proc.kill()
            raise RuntimeError(f"server did not start: {line!r}")
        self.port = int(m.group(1))
        # Keep draining stderr so the server can never block on a full pipe.
        self.log: list[str] = []
        self.clients: list[Client] = []
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self) -> None:
        assert self.proc.stderr is not None
        for line in self.proc.stderr:
            self.log.append(line)

    def connect(self) -> Client:
        c = Client(self.port)
        self.clients.append(c)
        return c

    def stop(self) -> int:
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        code = self.proc.wait(timeout=10)
        for c in self.clients:
            c.close()
        return code


class Client:
    def __init__(self, port: int) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""
        self.next_id = 1

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def send(self, op: str, **fields) -> int:
        req_id = self.next_id
        self.next_id += 1
        self.send_raw((json.dumps({"req_id": req_id, "op": op, **fields}) + "\n").encode())
        return req_id

    def recv(self) -> dict:
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def call(self, op: str, **fields) -> dict:
        req_id = self.send(op, **fields)
        resp = self.recv()
        assert resp["req_id"] == req_id, resp
        return resp

    def at_eof(self, timeout: float = 5.0) -> bool:
        """True if the server closes this connection (after any buffered replies)."""
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1) == b""
        except ConnectionResetError:
            return True
        except socket.timeout:
            return False

    def close(self) -> None:
        self.sock.close()


class ServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = Server()

    def tearDown(self) -> None:
        code = self.server.stop()
        log = "".join(self.server.log)
        # AddressSanitizer reports go to stderr; a clean exit must not have any.
        self.assertNotIn("ERROR: AddressSanitizer", log, log)
        self.assertNotIn("runtime error:", log, log)  # UBSan
        self.assertEqual(code, 0, f"server exited with {code}\n{log}")

    def assert_alive(self) -> None:
        """The server still answers a fresh client."""
        c = self.server.connect()
        self.assertTrue(c.call("list")["ok"])
        c.close()

    # ---- the basics ----

    def test_create_claim_complete(self) -> None:
        c = self.server.connect()
        self.assertEqual(c.call("create", title="t")["task"]["id"], 1)
        claim = c.call("claim", agent="a")
        self.assertTrue(claim["ok"], claim)
        self.assertEqual(claim["task"]["holder"], "a")
        self.assertTrue(c.call("complete", agent="a", task=1, text="done")["ok"])
        tasks = c.call("list", state="done")["tasks"]
        self.assertEqual([t["id"] for t in tasks], [1])

    # ---- TCP is a byte stream: requests can be split or merged arbitrarily ----

    def test_request_split_into_single_bytes(self) -> None:
        c = self.server.connect()
        c.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # really send tiny segments
        for b in b'{"req_id": 7, "op": "create", "title": "slow"}\n':
            c.send_raw(bytes([b]))
            time.sleep(0.001)
        resp = c.recv()
        self.assertEqual(resp["req_id"], 7)
        self.assertTrue(resp["ok"])

    def test_pipelined_requests_answered_in_order(self) -> None:
        c = self.server.connect()
        batch = b"".join(
            (json.dumps({"req_id": i, "op": "create", "title": f"t{i}"}) + "\n").encode()
            for i in range(1, 201)
        )
        batch += b'{"req_id": 201, "op": "list"}\n'
        c.send_raw(batch)  # 201 requests in one write
        for i in range(1, 201):
            resp = c.recv()
            self.assertEqual(resp["req_id"], i)
            self.assertEqual(resp["task"]["id"], i)
        # The list was sent after the creates on the same connection, so it must see all of them.
        self.assertEqual(len(c.recv()["tasks"]), 200)

    def test_crlf_and_blank_lines_are_tolerated(self) -> None:
        c = self.server.connect()
        c.send_raw(b'\r\n\n{"req_id": 1, "op": "list"}\r\n')
        self.assertEqual(c.recv()["req_id"], 1)

    # ---- the group-commit ordering fix ----

    def test_two_claims_in_one_batch_cannot_both_win(self) -> None:
        # Both lines arrive in a single read(), so they are handled in the same loop iteration.
        # If the server validated the whole batch before applying any of it, both would succeed.
        c = self.server.connect()
        c.call("create", title="only one")
        c.send_raw(
            b'{"req_id": 10, "op": "claim", "agent": "a", "task": 1}\n'
            b'{"req_id": 11, "op": "claim", "agent": "b", "task": 1}\n'
        )
        first, second = c.recv(), c.recv()
        self.assertTrue(first["ok"], first)
        self.assertEqual(second["code"], "TASK_TAKEN", second)

    def test_many_clients_race_for_one_task(self) -> None:
        setup = self.server.connect()
        setup.call("create", title="contested")
        clients = [self.server.connect() for _ in range(50)]
        lines = [
            (json.dumps({"req_id": 1, "op": "claim", "agent": f"agent-{i}", "task": 1}) + "\n").encode()
            for i in range(50)
        ]
        for c, line in zip(clients, lines):  # fire all claims before reading any reply
            c.send_raw(line)
        results = [c.recv() for c in clients]
        winners = [r for r in results if r["ok"]]
        self.assertEqual(len(winners), 1, results)
        self.assertTrue(all(r["code"] == "TASK_TAKEN" for r in results if not r["ok"]))
        holder = setup.call("list")["tasks"][0]["holder"]
        self.assertEqual(holder, winners[0]["task"]["holder"])

    # ---- bad input never kills the server or the connection ----

    def test_malformed_json_gets_error_and_connection_stays_usable(self) -> None:
        c = self.server.connect()
        c.send_raw(b"this is not json\n")
        err = c.recv()
        self.assertFalse(err["ok"])
        self.assertEqual(err["code"], "BAD_REQUEST")
        self.assertTrue(c.call("list")["ok"])

    def test_protocol_errors_keep_req_id(self) -> None:
        c = self.server.connect()
        c.send_raw(b'{"req_id": 5, "op": "complete", "agent": "a"}\n')  # no task
        err = c.recv()
        self.assertEqual(err["req_id"], 5)
        self.assertEqual(err["code"], "BAD_REQUEST")

    def test_overlong_line_is_rejected_and_connection_closed(self) -> None:
        c = self.server.connect()
        try:
            c.send_raw(b"x" * (2 << 20))  # 2 MiB, no newline
        except (BrokenPipeError, ConnectionResetError):
            pass  # the server may hang up before we finish sending; that is fine too
        try:
            err = c.recv()
            self.assertEqual(err["code"], "BAD_REQUEST")
            self.assertIn("1 MiB", err["message"])
        except (ConnectionError, OSError):
            pass  # RST raced ahead of the error reply
        self.assert_alive()

    # ---- clients that go away ----

    def test_disconnect_mid_line(self) -> None:
        c = self.server.connect()
        c.send_raw(b'{"req_id": 1, "op": "create", "ti')
        c.close()
        time.sleep(0.1)
        self.assert_alive()
        # The half-sent create must not have happened.
        self.assertEqual(self.server.connect().call("list")["tasks"], [])

    def test_half_close_still_gets_replies(self) -> None:
        # Like `printf '...' | nc`: send requests, then shutdown(SHUT_WR). The server must still
        # answer what it received, then close its side.
        c = self.server.connect()
        c.send_raw(b'{"req_id": 1, "op": "create", "title": "a"}\n{"req_id": 2, "op": "list"}\n')
        c.sock.shutdown(socket.SHUT_WR)
        self.assertEqual(c.recv()["req_id"], 1)
        self.assertEqual(c.recv()["req_id"], 2)
        self.assertTrue(c.at_eof())

    def test_reset_with_pending_output(self) -> None:
        c = self.server.connect()
        for i in range(300):
            c.call("create", title="x" * 1000)
        c.send_raw(b'{"req_id": 9999, "op": "list"}\n')  # big reply we never read
        time.sleep(0.05)
        # SO_LINGER with timeout 0 makes close() send an RST instead of a FIN.
        c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        c.close()
        time.sleep(0.1)
        self.assert_alive()

    # ---- partial writes ----

    def test_slow_reader_gets_complete_large_reply(self) -> None:
        # A ~3 MB list reply is far bigger than a socket send buffer, so the server's send() must
        # return short writes and it has to finish the rest from EPOLLOUT events.
        setup = self.server.connect()
        for i in range(3000):
            setup.send("create", title=f"task-{i}-" + "y" * 1000)
        for _ in range(3000):
            setup.recv()

        slow = self.server.connect()
        slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.send("list")
        time.sleep(0.3)  # let the server fill both socket buffers and hit EAGAIN

        # While the big reply is stuck, other clients must still be served promptly.
        start = time.monotonic()
        self.assertTrue(setup.call("create", title="meanwhile")["ok"])
        self.assertLess(time.monotonic() - start, 1.0)

        # Now read it slowly, in small pieces.
        data = b""
        while not data.endswith(b"\n"):
            chunk = slow.sock.recv(8192)
            self.assertTrue(chunk, "server closed mid-reply")
            data += chunk
        reply = json.loads(data)
        self.assertGreater(len(data), 3_000_000)
        self.assertEqual(len(reply["tasks"]), 3000)  # "meanwhile" was created after the list

    # ---- shutdown ----

    def test_sigterm_exits_cleanly_with_clients_connected(self) -> None:
        clients = [self.server.connect() for _ in range(5)]
        clients[0].call("list")
        # tearDown sends SIGTERM and checks the exit code is 0 with no sanitizer reports.
        del clients


if __name__ == "__main__":
    if not Path(SERVER).exists():
        sys.exit(f"server binary not found at {SERVER}; build it or set BATON_SERVER")
    unittest.main()
