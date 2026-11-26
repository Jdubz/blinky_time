#!/usr/bin/env python3
"""Two browsers on one track, in the production shape: the page from one origin (the site), the API
on another (CORS). CI runs it with Playwright's Chromium:

    python tests/browser_hosted.py --page http://127.0.0.1:8000/ --api http://127.0.0.1:8767

Each person's edit shows up on the other's page within a few seconds, nothing has a Save button,
and the service's export offers the result to the corpus. Fails on any page error.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request

from playwright.sync_api import sync_playwright

HELP_SEEN = "drumReviewer.helpSeen.v1"


def check(cond: bool, what: str) -> None:
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--page", required=True)
    ap.add_argument("--api", required=True)
    args = ap.parse_args()
    deadline = time.monotonic() + 180  # the server clones the corpus before it listens
    while True:
        try:
            with urllib.request.urlopen(args.api + "/api/tracks", timeout=10) as r:
                tracks = json.loads(r.read())["tracks"]
            break
        except OSError:
            if time.monotonic() > deadline:
                check(False, "the API answered within 180 s")
            time.sleep(2)
    stem = next(t["stem"] for t in tracks if not t["has_human"])

    errors: list[str] = []
    with sync_playwright() as p:
        browser = p.chromium.launch()

        def person(name: str):
            ctx = browser.new_context()
            ctx.add_init_script(f"localStorage.setItem({HELP_SEEN!r}, '1');")
            page = ctx.new_page()
            page.on("pageerror", lambda e: errors.append(f"{name}: {e}"))
            page.on("console", lambda m: m.type == "error" and not m.text.startswith(
                "Failed to load resource: the server responded with a status of 4") and errors.append(f"{name}: {m.text}"))
            page.goto(f"{args.page}#{stem}")
            page.wait_for_function("typeof S !== 'undefined' && S.meta !== null && S.duration > 0", timeout=120000)
            return page

        a, b = person("A"), person("B")
        check(a.locator("#btn-save").count() == 0, "there's no Save button")
        n0 = a.evaluate("S.onsets.length")
        check(b.evaluate("S.onsets.length") == n0, f"both pages show the same {n0} hits")

        a.evaluate("edit(() => { S.onsets.push({ id: newId(), time: 1.2345, type: 'snare' }); })")
        a.wait_for_function("!syncPending() && /saved/.test(document.getElementById('status').textContent)", timeout=15000)
        check(True, "A's added hit saves by itself")
        b.wait_for_function(f"S.onsets.length === {n0 + 1}", timeout=15000)
        check(True, "B sees A's hit within seconds")

        victim = b.evaluate("S.onsets[0].id")
        b.evaluate(f"edit(() => {{ S.onsets = S.onsets.filter(o => o.id !== {json.dumps(victim)}); }})")
        a.wait_for_function(f"!S.onsets.some(o => o.id === {json.dumps(victim)})", timeout=15000)
        check(True, "A sees B's delete within seconds")
        b.evaluate("edit(() => addRange(0, 5))")
        a.wait_for_function("S.reviewed.length && S.reviewed[0][1] >= 4.99", timeout=15000)
        check(True, "A sees B mark a range reviewed")
        check(a.evaluate("S.onsets.length") == b.evaluate("S.onsets.length") == n0, "the two pages agree")
        browser.close()

    deadline = time.monotonic() + 60
    exp = {}
    while time.monotonic() < deadline and not exp.get("ready"):
        with urllib.request.urlopen(args.api + "/api/export", timeout=10) as r:
            exp = json.loads(r.read())
        time.sleep(2)
    check(list(exp.get("tracks", {})) == [f"edm/drums_human/{stem}.drums.json"], "the export offers the edited track to the corpus")
    check(not errors, "no page errors" + ("" if not errors else ": " + " | ".join(errors[:5])))
    print("browser: all checks passed")


if __name__ == "__main__":
    main()
