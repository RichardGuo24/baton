"""MCP server exposing Baton's tools to an AI agent over stdio.

Each agent launches its own shim. The shim keeps a TCP connection to baton-server and sends
heartbeats in the background for every task this agent holds, so the model never has to remember
to. If the agent process dies, the shim dies with it, heartbeats stop, and the server reclaims the
task after one lease period.

Config (environment variables):
  BATON_HOST       default 127.0.0.1
  BATON_PORT       default 7000
  BATON_AGENT_ID   default <hostname>-<pid>
  BATON_HEARTBEAT_S default 20
"""

from __future__ import annotations

import logging
import os
import socket
import sys
import threading
from typing import Any

from mcp.server.fastmcp import FastMCP

from baton_shim.client import BatonClient, BatonError

log = logging.getLogger("baton_shim")

HOST = os.environ.get("BATON_HOST", "127.0.0.1")
PORT = int(os.environ.get("BATON_PORT", "7000"))
AGENT_ID = os.environ.get("BATON_AGENT_ID", f"{socket.gethostname()}-{os.getpid()}")
HEARTBEAT_S = float(os.environ.get("BATON_HEARTBEAT_S", "20"))

mcp = FastMCP("baton")
client = BatonClient(HOST, PORT)

_held: set[int] = set()
_held_lock = threading.Lock()


def _call(op: str, **fields: Any) -> dict[str, Any]:
    """Call the server; turn connection failures into a normal error dict the model can read."""
    try:
        return client.call(op, **fields)
    except BatonError as e:
        return {"ok": False, "code": "UNAVAILABLE", "message": str(e)}


def _heartbeat_loop() -> None:
    stop = threading.Event()
    while not stop.wait(HEARTBEAT_S):
        with _held_lock:
            tasks = list(_held)
        for task_id in tasks:
            resp = _call("heartbeat", agent=AGENT_ID, task=task_id)
            if not resp.get("ok") and resp.get("code") in ("NOT_HOLDER", "NOT_FOUND"):
                with _held_lock:
                    _held.discard(task_id)
                log.warning("lost task %s: %s", task_id, resp.get("message"))


@mcp.tool()
def create_task(title: str, description: str = "") -> dict[str, Any]:
    """Add a new task to the shared board. Use this to break a feature into pieces other agents
    can pick up. Returns the new task id."""
    return _call("create", title=title, description=description)


@mcp.tool()
def list_tasks(state: str | None = None) -> dict[str, Any]:
    """List tasks on the shared board. Optional state filter: "open", "claimed" or "done".
    Claimed tasks show which agent holds them and how long their lease has left."""
    return _call("list", state=state)


@mcp.tool()
def claim_task(task_id: int | None = None, paths: list[str] | None = None) -> dict[str, Any]:
    """Claim a task before working on it. Omit task_id to take the oldest open task.

    List every file or directory you expect to edit in `paths` (for example ["src/parser/",
    "README.md"]); they are locked for you so no other agent edits them. Locking a directory
    covers everything inside it.

    The response includes notes left by agents who worked on this task before you. Read them and
    continue from where they stopped instead of starting over. Your claim stays alive
    automatically while you work."""
    resp = _call("claim", agent=AGENT_ID, task=task_id, paths=paths or [])
    if resp.get("ok"):
        task = resp.get("task", {})
        if isinstance(task, dict) and "id" in task:
            with _held_lock:
                _held.add(int(task["id"]))
    return resp


@mcp.tool()
def lock_paths(task_id: int, paths: list[str]) -> dict[str, Any]:
    """Lock more files or directories under a task you already hold, when you discover you need
    to edit something you did not list at claim time. If another agent holds a path, the response
    says who and for how long; work on something else and try again later."""
    return _call("lock", agent=AGENT_ID, task=task_id, paths=paths)


@mcp.tool()
def add_note(task_id: int, text: str) -> dict[str, Any]:
    """Save a progress note on a task you hold: what is done, what is left, what is failing.
    If you stop or crash, the next agent to claim the task will see it. Add a note after each
    meaningful step."""
    return _call("note", agent=AGENT_ID, task=task_id, text=text)


@mcp.tool()
def complete_task(task_id: int, summary: str = "") -> dict[str, Any]:
    """Mark a task you hold as done and release its file locks. Include a short summary of what
    changed."""
    resp = _call("complete", agent=AGENT_ID, task=task_id, text=summary)
    if resp.get("ok"):
        with _held_lock:
            _held.discard(task_id)
    return resp


@mcp.tool()
def release_task(task_id: int, reason: str = "") -> dict[str, Any]:
    """Give up a task you hold without finishing it, so another agent can take it. Add a note
    first describing where you stopped, then release with a short reason."""
    resp = _call("release", agent=AGENT_ID, task=task_id, text=reason)
    if resp.get("ok"):
        with _held_lock:
            _held.discard(task_id)
    return resp


def main() -> None:
    # stdout carries the MCP protocol, so logs must go to stderr.
    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="%(name)s: %(message)s")
    log.info("agent %s -> baton-server %s:%s", AGENT_ID, HOST, PORT)
    threading.Thread(target=_heartbeat_loop, name="baton-heartbeat", daemon=True).start()
    mcp.run()  # stdio transport


if __name__ == "__main__":
    main()
