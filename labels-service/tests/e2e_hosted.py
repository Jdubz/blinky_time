#!/usr/bin/env python3
"""End to end against a running hosted labels service (CI runs it against the container image):

    python tests/e2e_hosted.py --api http://127.0.0.1:8766 --origin https://site.example.invalid \\
        --host api.example.invalid --remote /path/to/bare.git

--remote is the bare repository the service clones from (the stand-in for blinky-corpus). Two pages
edit one track and see each other's edits; the export offers the result; doing what blinky-corpus's
labels-sync workflow does (merging the export into main) makes the service report it published.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def check(cond: bool, what: str) -> None:
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--api", required=True)
    ap.add_argument("--origin", required=True)
    ap.add_argument("--host")
    ap.add_argument("--remote", required=True)
    ap.add_argument("--wait", type=float, default=180)
    args = ap.parse_args()

    def call(method: str, path: str, body: Any = None, origin: str | None = args.origin,
             extra: dict[str, str] | None = None) -> tuple[int, dict[str, str], Any]:
        h = {**({"Host": args.host} if args.host else {}), **({"Origin": origin} if origin else {}), **(extra or {})}
        data = json.dumps(body).encode() if body is not None else None
        if data:
            h["Content-Type"] = "application/json"
        req = urllib.request.Request(args.api + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw, st, hd = r.read(), r.status, dict(r.headers)
        except urllib.error.HTTPError as e:
            raw, st, hd = e.read(), e.code, dict(e.headers)
        return st, hd, (json.loads(raw) if raw and hd.get("Content-Type", "").startswith("application/json") else raw)

    def git(*a: str) -> str:
        return subprocess.run(["git", "-C", args.remote, *a], capture_output=True, text=True, check=True).stdout

    def until(pred, what: str, timeout: float = 60) -> None:
        end = time.monotonic() + timeout
        while not pred():
            if time.monotonic() > end:
                check(False, what)
            time.sleep(1)
        check(True, what)

    until(lambda: _ok(lambda: call("GET", "/healthz", origin=None)[0] == 200), "server is up", args.wait)
    st, _, cfg = call("GET", "/api/config")
    check(st == 200 and cfg["hosted"] and cfg["publishing"] == "pulled" and cfg["version"] != "dev",
          f"hosted, published by the corpus pulling, version {cfg.get('version', '')[:12]}")
    st, h, _ = call("OPTIONS", "/api/ops?stem=x", extra={"Access-Control-Request-Method": "POST",
                                                         "Access-Control-Request-Headers": "content-type"})
    check(st == 204 and h.get("Access-Control-Allow-Origin") == args.origin, "CORS preflight for the site")
    check(call("OPTIONS", "/api/ops?stem=x", origin="https://evil.example",
               extra={"Access-Control-Request-Method": "POST"})[0] == 403, "CORS refused for another origin")

    tracks = call("GET", "/api/tracks")[2]["tracks"]
    stem = next(t["stem"] for t in tracks if not t["has_human"])
    t = call("GET", f"/api/track?stem={stem}")[2]
    base = {"audio_sha256": t["audio_sha256"], "duration": t["manifest_duration"]}
    h1, h2 = t["onsets"][0]["id"], t["onsets"][1]["id"]
    # page A and page B on the same track
    st, _, a = call("POST", f"/api/ops?stem={stem}", {**base, "tab": "page-aaaa", "ops": [
        {"op": "move", "id": h1, "time": round(t["onsets"][0]["time"] + 0.002, 4)},
        {"op": "add", "id": "page-aaaa-1", "time": 1.2345, "type": "snare"}]})
    check(st == 200 and a["rev"] == t["rev"] + 2, "page A's edits are saved")
    pulled = call("GET", f"/api/ops?stem={stem}&since={t['rev']}")[2]
    check([op["op"] for _, by, op in pulled["ops"] if by == "page-aaaa"] == ["move", "add"], "page B pulls page A's edits")
    st, _, b = call("POST", f"/api/ops?stem={stem}", {**base, "tab": "page-bbbb", "ops": [{"op": "del", "id": h2}]})
    pulled = call("GET", f"/api/ops?stem={stem}&since={a['rev']}")[2]
    check([(by, op["op"]) for _, by, op in pulled["ops"]] == [("page-bbbb", "del")], "page A pulls page B's edit")
    view = call("GET", f"/api/track?stem={stem}")[2]
    ids = {o["id"] for o in view["onsets"]}
    check("page-aaaa-1" in ids and h2 not in ids and view["unpublished"], "both edits are in the shared labels; publishing soon")

    path = f"edm/drums_human/{stem}.drums.json"
    until(lambda: call("GET", "/api/export", origin=None)[2]["ready"], "the export is ready once editing goes quiet", 60)
    exp = call("GET", "/api/export", origin=None)[2]
    check(list(exp["tracks"]) == [path], "the export offers the edited track")
    doc = json.loads(exp["tracks"][path])
    check(len(doc["onsets"]) == len(t["onsets"]) and doc["complete"] is False, "with both pages' edits (partial progress)")
    # What labels-sync.yml does: the file, committed on main (here straight onto the stand-in's main).
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(["git", "clone", "-q", args.remote, d], check=True)
        dest = Path(d) / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(exp["tracks"][path])
        for cmd in (["add", "-A"], ["-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qm", "labels: autosave"],
                    ["push", "-q", "origin", "HEAD:main"]):
            subprocess.run(["git", "-C", d, *cmd], check=True)
    until(lambda: not call("GET", f"/api/track?stem={stem}")[2]["unpublished"], "the service sees it on main: published", 120)
    check(call("GET", "/api/export", origin=None)[2]["tracks"] == {}, "nothing left to offer")
    print("e2e: all checks passed")


def _ok(f) -> bool:
    try:
        return f()
    except OSError:
        return False


if __name__ == "__main__":
    main()
