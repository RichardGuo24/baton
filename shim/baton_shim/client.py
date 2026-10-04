"""Blocking TCP client for baton-server's newline-delimited JSON protocol."""

from __future__ import annotations

import json
import socket
import threading
from typing import Any


class BatonError(Exception):
    """Raised when the server cannot be reached at all."""


class BatonClient:
    """One persistent connection. Thread-safe: requests are serialized with a lock, so each
    request's response is the next line read (the server answers each connection in order)."""

    def __init__(self, host: str, port: int, timeout_s: float = 10.0) -> None:
        self._addr = (host, port)
        self._timeout_s = timeout_s
        self._lock = threading.Lock()
        self._sock: socket.socket | None = None
        self._buf = b""
        self._next_id = 1

    def _connect(self) -> None:
        try:
            self._sock = socket.create_connection(self._addr, timeout=self._timeout_s)
        except OSError as e:
            raise BatonError(
                f"cannot reach baton-server at {self._addr[0]}:{self._addr[1]} ({e}). "
                "Is the server running?"
            ) from e
        self._buf = b""

    def _close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
                self._buf = b""

    def _read_line(self) -> bytes:
        assert self._sock is not None
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line

    def call(self, op: str, **fields: Any) -> dict[str, Any]:
        """Send one request and return the decoded response dict.

        Retries once on a broken connection. Note: a retried write could be applied twice if the
        first attempt reached the server (see "Known limit" in docs/design.md)."""
        with self._lock:
            req_id = self._next_id
            self._next_id += 1
            payload = {"req_id": req_id, "op": op}
            payload.update({k: v for k, v in fields.items() if v is not None})
            line = (json.dumps(payload) + "\n").encode()

            for attempt in range(2):
                try:
                    if self._sock is None:
                        self._connect()
                    assert self._sock is not None
                    self._sock.sendall(line)
                    resp = json.loads(self._read_line())
                    if resp.get("req_id") != req_id:
                        raise ConnectionError(f"response for req {resp.get('req_id')}, expected {req_id}")
                    return resp
                except (OSError, ConnectionError, json.JSONDecodeError):
                    self._close()
                    if attempt == 1:
                        raise BatonError("lost connection to baton-server mid-request; try again")
            raise AssertionError("unreachable")
