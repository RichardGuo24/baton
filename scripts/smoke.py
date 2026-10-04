#!/usr/bin/env python3
"""Talk to baton-server directly over TCP, without MCP. Handy while building the server.

  python3 scripts/smoke.py                      # run the scripted demo below
  python3 scripts/smoke.py '{"op":"list"}'      # send one raw request
"""

import json
import socket
import sys

HOST, PORT = "127.0.0.1", 7000

DEMO = [
    {"op": "create", "title": "Add inference route"},
    {"op": "create", "title": "Update README"},
    {"op": "claim", "agent": "agent-a", "paths": ["api/predict/", "models/loader.py"]},
    {"op": "claim", "agent": "agent-b", "task": 2, "paths": ["models/"]},  # expect PATH_LOCKED
    {"op": "note", "agent": "agent-a", "task": 1, "text": "loader refactored; route half done"},
    {"op": "list"},
    {"op": "complete", "agent": "agent-a", "task": 1, "text": "route added"},
    {"op": "claim", "agent": "agent-b", "task": 2, "paths": ["models/"]},  # now succeeds
]


def main() -> None:
    requests = [json.loads(a) for a in sys.argv[1:]] or DEMO
    with socket.create_connection((HOST, PORT)) as s:
        f = s.makefile("rwb")
        for i, req in enumerate(requests, 1):
            req.setdefault("req_id", i)
            f.write((json.dumps(req) + "\n").encode())
            f.flush()
            resp = json.loads(f.readline())
            print(">>", json.dumps(req))
            print("<<", json.dumps(resp, indent=2))


if __name__ == "__main__":
    main()
