#!/usr/bin/env python3
"""Two browsers on one track, in the production shape: the page from one origin (the site), the API
on another (CORS). CI runs it with Playwright's Chromium:

    python tests/browser_hosted.py --page http://127.0.0.1:8000/ --api http://127.0.0.1:8767

Each person's edit shows up on the other's page within a few seconds, nothing has a Save button,
and the service's export offers the result to the corpus. The suspect strip, Next suspect, the
"suspect first" review order and the expected-hit markers work (a clicked marker becomes a label on
both pages; markers are never saved), and the page still works against an API that doesn't send
the suspect map yet. Fails on any page error.
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

        def person(name: str, old_api: bool = False):
            ctx = browser.new_context()
            ctx.add_init_script(f"localStorage.setItem({HELP_SEEN!r}, '1');")
            if old_api:  # an API from before the suspect map: /api/track and /api/tracks without it
                def strip(route):
                    r = route.fetch()
                    body = r.json()
                    body.pop("suspect", None)
                    for t in body.get("tracks", []):
                        t.pop("suspect_s", None)
                        t.pop("suspect_left", None)
                    route.fulfill(response=r, json=body)
                ctx.route(lambda url: "/api/track?" in url or url.endswith("/api/tracks"), strip)
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

        # --- the suspect map (the corpus's edm/drums_suspect/, schema 2)
        nbars = a.evaluate("susBars().length")
        check(nbars > 0 and a.evaluate("expHits().length") > 0, f"the track has {nbars} suspect bars and expected hits")
        check(a.locator("#btn-suspect").is_visible(), "there's a visible Next suspect button")
        a.click("#btn-suspect")
        bar = a.evaluate("susBars().find(b => Math.abs(b.start - S.cursor) < 1e-6) || null")
        check(bar is not None and not a.evaluate(f"barReviewed(susBars().find(b => b.start === {bar['start']}))"),
              "Next suspect jumps to an unreviewed suspect bar")
        check(a.evaluate("S.view.start <= S.cursor && S.cursor < S.view.start + S.view.dur"), "and shows it")
        a.mouse.move(0, 0)
        box = a.locator("#overlay").bounding_box()
        why = a.evaluate(f"suspectWhy(susBars().find(b => b.start === {bar['start']}))")
        check(why.startswith("Check ") and len(why) > 15, f"its reason in plain words: {why!r}")

        # Review order "suspect first": Reviewed → next goes to the next unreviewed suspect bar.
        a.click("#btn-settings")
        a.select_option("#review-order", "suspect")
        a.click("#btn-settings-close")
        a.click("#btn-next")
        check(a.evaluate("susBars().some(b => Math.abs(b.start - S.cursor) < 1e-6 && !barReviewed(b))"),
              "with suspect first, Reviewed → next lands on an unreviewed suspect bar")
        a.wait_for_function("!syncPending()", timeout=15000)

        # An expected-hit marker becomes a real label with one click, on both pages.
        ex = a.evaluate("(() => { const e = expHits().find(e => expShown(e)); return e ? { ...e } : null; })()")
        check(ex is not None, "an expected-hit marker is shown")
        a.evaluate(f"setView({ex['time']} - 0.5, 1)")
        x = box["x"] + a.evaluate(f"t2x({ex['time']})")
        y = box["y"] + a.evaluate(f"LANE[{json.dumps(ex['type'])}].y + 16")
        n1 = a.evaluate("S.onsets.length")
        a.mouse.click(x, y)
        near = f"S.onsets.some(o => o.type === {json.dumps(ex['type'])} && Math.abs(o.time - {ex['time']}) <= 0.03)"
        check(a.evaluate(f"S.onsets.length === {n1 + 1} && {near}"), f"clicking the {ex['type']} marker at {ex['time']} s adds a label")
        check(not a.evaluate(f"expShown(expHits().find(e => e.time === {ex['time']} && e.type === {json.dumps(ex['type'])}))"),
              "and the marker hides")
        a.wait_for_function("!syncPending()", timeout=15000)
        b.wait_for_function(near, timeout=15000)
        check(True, "B sees the label within seconds")
        n_final = a.evaluate("S.onsets.length")

        # The page against an API that doesn't send the suspect map yet.
        c = person("C (old API)", old_api=True)
        check(c.evaluate("susBars().length === 0 && expHits().length === 0"), "old API: no suspect bars, no markers")
        c.click("#btn-suspect")
        check("no suspect map" in c.inner_text("#toast"), "old API: Next suspect says there's no suspect map")
        c.evaluate("setView(20, 6); S.cursor = 20")
        c.click("#btn-settings")
        c.select_option("#review-order", "suspect")
        c.click("#btn-settings-close")
        c.click("#btn-next")
        check(abs(c.evaluate("S.view.start") - (20 + 0.9 * 6)) < 0.01, "old API: suspect first falls back to the next screen")
        check("in order" in c.inner_text("#toast"), "and says so")
        c.wait_for_function("!syncPending()", timeout=15000)
        a.wait_for_function("S.reviewed.some(([x, y]) => x <= 21 && y >= 25)", timeout=15000)
        check(True, "A sees C's review from the old-API page")
        check(c.evaluate("S.onsets.length") == n_final, "C sees the same labels")
        browser.close()

    deadline = time.monotonic() + 60
    exp = {}
    while time.monotonic() < deadline and not exp.get("ready"):
        with urllib.request.urlopen(args.api + "/api/export", timeout=10) as r:
            exp = json.loads(r.read())
        time.sleep(2)
    check(list(exp.get("tracks", {})) == [f"edm/drums_human/{stem}.drums.json"], "the export offers the edited track to the corpus")
    doc = json.loads(next(iter(exp["tracks"].values())))
    check(len(doc["onsets"]) == n_final and "suspect" not in doc and "expected" not in doc,
          f"the published file has the {n_final} labels and no markers or suspect data")
    check(not errors, "no page errors" + ("" if not errors else ": " + " | ".join(errors[:5])))
    print("browser: all checks passed")


if __name__ == "__main__":
    main()
