#!/usr/bin/env python3
"""A fake of the one GitHub call the hosted drum reviewer makes: the public, unauthenticated
pull-request lookup by head branch (tools/drum-reviewer/hosted.py PublicGitHub). The pushes go to a
local bare git repository instead of GitHub, and the test stands in for the corpus workflow
(.github/workflows/labels-pr.yml) that opens the pull request:

    python tests/fake_github.py --port 9999 --repo OWNER/NAME

Test hooks: GET /_fake/state, POST /_fake/pr {"head": branch} (open a PR, as the workflow would),
POST /_fake/pr/<n> {fields} (e.g. {"state": "closed"} or {"merged_at": "..."}), POST /_fake/fail
{"status": 403} (fail the next lookup, e.g. a rate limit).
"""

from __future__ import annotations

import argparse
import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse


class FakeGitHub:
    def __init__(self, repo: str = "owner/corpus"):
        self.repo = repo
        self.lock = threading.Lock()
        self.prs: dict[int, dict[str, Any]] = {}
        self.calls: list[str] = []
        self.auth_seen: list[str] = []
        self.fail_next: int | None = None
        self.server: ThreadingHTTPServer | None = None

    def open_pr(self, head: str) -> dict[str, Any]:
        with self.lock:
            n = len(self.prs) + 1
            self.prs[n] = {"number": n, "html_url": f"https://github.com/{self.repo}/pull/{n}",
                           "head": {"ref": head}, "state": "open", "merged_at": None}
            return self.prs[n]

    def state(self) -> dict[str, Any]:
        with self.lock:
            return {"prs": self.prs, "calls": self.calls, "auth_seen": self.auth_seen}

    def start(self, port: int = 0) -> str:
        fake = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a: Any) -> None:
                pass

            def _send(self, status: int, body: Any = None) -> None:
                raw = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def _body(self) -> Any:
                n = int(self.headers.get("Content-Length") or 0)
                return json.loads(self.rfile.read(n)) if n else {}

            def do_GET(self) -> None:
                url = urlparse(self.path)
                if url.path == "/_fake/state":
                    return self._send(200, fake.state())
                with fake.lock:
                    fake.calls.append(f"GET {self.path}")
                    if self.headers.get("Authorization"):
                        fake.auth_seen.append(self.headers["Authorization"][:10])
                    if fake.fail_next:
                        st, fake.fail_next = fake.fail_next, None
                        return self._send(st, {"message": "API rate limit exceeded"})
                    if url.path != f"/repos/{fake.repo}/pulls":
                        return self._send(404, {"message": "Not Found"})
                    head = unquote((parse_qs(url.query).get("head") or [""])[0])
                    owner = fake.repo.split("/")[0]
                    branch = head.removeprefix(owner + ":")
                    found = [p for p in fake.prs.values() if p["head"]["ref"] == branch]
                self._send(200, sorted(found, key=lambda p: -p["number"]))

            def do_POST(self) -> None:
                url, body = urlparse(self.path), self._body()
                if url.path == "/_fake/pr":
                    return self._send(201, fake.open_pr(body["head"]))
                m = re.fullmatch(r"/_fake/pr/(\d+)", url.path)
                if m:
                    with fake.lock:
                        fake.prs[int(m.group(1))].update(body)
                    return self._send(200, {"ok": True})
                if url.path == "/_fake/fail":
                    with fake.lock:
                        fake.fail_next = int(body.get("status", 403))
                    return self._send(200, {"ok": True})
                self._send(404, {"message": "Not Found"})

        self.server = ThreadingHTTPServer(("127.0.0.1", port), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def stop(self) -> None:
        if self.server:
            self.server.shutdown()
            self.server.server_close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=9999)
    ap.add_argument("--repo", default="owner/corpus")
    args = ap.parse_args()
    fake = FakeGitHub(args.repo)
    print(f"fake GitHub for {args.repo} on {fake.start(args.port)}", flush=True)
    threading.Event().wait()


if __name__ == "__main__":
    main()
