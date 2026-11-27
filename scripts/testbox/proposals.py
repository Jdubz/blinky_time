#!/usr/bin/env python3
"""Score-ranked stable proposals (docs/DEV_LOOP.md, "Stable candidates from scores"; phase 3).

When blinky-firmware's newest release candidate leads the current stable release on the box's
quality-suite scores, with no health problem, the box opens ONE blinky_time issue, "Propose bN
for stable", with the comparison, and keeps it up to date. The owner decides; promotion stays
blinky-firmware's ``release_promote.py``. Nothing here promotes, tags or deploys a release to
anything but the box's own test chip.

**The scores** are the staging quality suite's (blinky-server testing/staging.py), one line per
run in ``staging-scores.jsonl`` in the server's data dir: per track and chip, drum F, kick and
snare recall (±50 ms), on-drum share, the octave-tolerant tempo error (testing/tempo.py) and
where the scorer's lag came from. repS (repetition consistency) is a metric of the comparison
too, but the box doesn't measure it yet: it shows as "not measured" and counts neither way.

**Health and smoothness are metrics too**, so a pure smoothness win can qualify: the longest
frame, the lowest fps and the beat-pace error. Each
is one value per run, the strict rule's worst (testing/strict_rules.py; the worst chip's), from
the line's ``health`` record. A line without it (before phase 3, or a firmware that doesn't
report the field) is "not measured" for it; the staging report isn't used for these, as it is
one file per commit and a second run of it overwrites the first's. Not metrics: audio overrun
growth and stream lines dropped (counters that are almost always 0, so their measured noise is
0 and a single count from a rig hiccup would read as beyond it; overrun_growth blocks at 0
anyway), music level and onsets (the rig's volume), and detection latency (a drift from the
rig's calibration, not better or worse by direction).

**What is compared with what.** A candidate ``bN`` is a merge of a staging commit into master;
its scores are that staging commit's (its tag's second parent, with the same tree). The stable
release's are the suite run on the release itself (``release`` in the line). Only lines with
the same corpus pin (the server's ``corpus/`` submodule, as deployed now) and the same test setup
(chips and limits) are compared, and per track only scores whose lags are comparable: the same
``lagSource``, and if it isn't ``measured``, neither ambiguous (DEV_LOOP.md, "Lag"). Tracks left
out are listed with the reason.

**Noise.** Each metric's run-to-run noise is measured, not assumed: from builds the box ran more
than once (same build, same pin and setup; a ``--retest``, or the extra baseline run below), the
pooled within-build variance per track (per run for a health metric), over every pin (the noise
is the rig's, not the pin's). A metric's difference is the mean over the tracks (the runs) of
candidate minus stable; it is beyond its noise when it exceeds K standard errors (K = 2: about
95% for a real difference). With too little repeat data for a metric (under MIN_NOISE_DOF
degrees of freedom), it can't be judged.

**Qualifies** when no metric is worse beyond its noise, at least one is better beyond it, every
measured score metric could be judged, and the candidate is healthy: its staging commit's
``Hardware test (staging)`` is ``success``, and no run of it rebooted or failed a strict rule
that blocks now (testing/strict_rules.py, as the server has it). A health metric whose noise
can't be judged yet counts neither way: it gets one degree of freedom per repeat run (a score
metric gets one per track), so waiting for it would hold every proposal for weeks. No
composite score: each metric is shown.

**Runs it asks for**, queued behind every other box job and never preempting one (the staging
runner submits them only when it has nothing of its own to run, in queue mode, with nothing of
the PR queue waiting): the candidate's staging commit when it has no scores under the current
pin, the stable release when it has none, and the stable release once more while the noise is
unmeasured (at most MAX_BASELINE_RUNS a pin). A full suite takes about 20 minutes; the baseline
runs again only when the pin, the setup or the stable release changes.

**The issue:** one per candidate, updated in place (its body changes only when the comparison
does), never a second one (the state and a title search). A newer candidate's proposal closes
the older one, pointing to it. A proposal that stops qualifying is updated and closed, and
reopened if it qualifies again; one whose candidate became stable is closed.

The staging runner (staging.py) runs this when it has nothing of its own to do, at most every
RECHECK_S unless the scores or the pin changed. Run by hand, it only prints: the comparison it
would post and the run it would ask for (``--status``: the state). Stdlib only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FW_REPO = "Jdubz/blinky-firmware"
TOOLING_REPO = "Jdubz/blinky_time"
HW_CONTEXT = "Hardware test (staging)"
TITLE = "Propose {} for stable"
K = 2.0  # standard errors: "beyond its noise"
MIN_NOISE_DOF = 12  # repeat-run degrees of freedom a metric needs before it is judged
MAX_BASELINE_RUNS = 3  # stable-release runs a pin, the noise run(s) included
MAX_FAILURES = 2  # a requested run that ended without scores this often is given up on
RECHECK_S = 30 * 60  # re-evaluate at most this often when nothing changed
DATA = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "blinky-server"

Gh = Callable[..., str]


@dataclass(frozen=True)
class Metric:
    key: str  # the score line's track field, or for a health metric its strict rule
    name: str
    higher: bool  # True: higher is better
    fmt: str = "{:.3f}"
    health: bool = False  # one value per run: the strict rule's worst, in the line's health


RUN = ("the run", "worst chip")  # a health metric's one row per line


METRICS: tuple[Metric, ...] = (
    Metric("drumF", "drum F", True),
    Metric("kickR50", "kick recall (±50 ms)", True),
    Metric("snareR50", "snare recall (±50 ms)", True),
    Metric("onDrum", "on-drum share", True),
    Metric("repS", "repS (repetition consistency)", True),
    Metric("tempoErr", "tempo error (octave-tolerant; lower is better)", False, "{:.4f}"),
    Metric("frame_time", "longest frame (ms; lower is better)", False, "{:.1f}", True),
    Metric("fps", "lowest fps", True, "{:.1f}", True),
    Metric("beat_pace", "beat-pace error (lower is better)", False, "{:.3f}", True),
)


def log(msg: str) -> None:
    print(msg, flush=True)


# -- the score lines ---------------------------------------------------------------------------


def load_records(path: Path | None = None) -> list[dict[str, Any]]:
    path = path or DATA / "staging-scores.jsonl"
    if not path.exists():
        return []
    out = []
    for ln in path.read_text().splitlines():
        if ln.strip():
            try:
                out.append(json.loads(ln))
            except ValueError:
                log(f"ERROR: unreadable line in {path}: {ln[:80]}")
    return out


def setup_key(rec: dict[str, Any]) -> str:
    return json.dumps(
        {"devices": sorted(rec.get("devices") or []), "limits": rec.get("limits") or {}},
        sort_keys=True,
    )


def build_key(rec: dict[str, Any]) -> str:
    """The firmware build a line is of: the release, the PR head whose tree it has, the sha."""
    return str(rec.get("release") or rec.get("build") or rec.get("sha"))


def _num(v: Any) -> float | None:
    if isinstance(v, bool) or not isinstance(v, (int, float)) or math.isnan(float(v)):
        return None
    return float(v)


def rows(rec: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    """The line's tracks, by (track, chip)."""
    return {(str(t.get("track")), str(t.get("device"))): t for t in rec.get("tracks") or []}


def cells(rec: dict[str, Any], m: Metric) -> dict[tuple[str, str], dict[str, Any]]:
    """The line's rows for a metric, by (track, chip): its tracks, or for a health metric one
    row for the run holding the strict rule's worst value (None: not measured)."""
    if not m.health:
        return rows(rec)
    r = (((rec.get("health") or {}).get("strict") or {}).get(m.key)) or {}
    return {RUN: {m.key: r.get("worst")}}


def lag_comparable(group: list[dict[str, Any]]) -> str | None:
    """None if these rows' lags are comparable, else why not."""
    sources = {r.get("lagSource") for r in group}
    if None in sources:
        return "no lag source recorded"
    if len(sources) > 1:
        return "lag sources differ (" + ", ".join(sorted(str(s) for s in sources)) + ")"
    if sources != {"measured"} and any(r.get("lagAmbiguous") for r in group):
        return f"ambiguous {sources.pop()} lag"
    return None


@dataclass
class Noise:
    sd: float | None  # one run's standard deviation on one track
    dof: int
    builds: int  # builds with repeat runs that contributed


def noise(records: list[dict[str, Any]]) -> dict[str, Noise]:
    """Each metric's run-to-run noise: the pooled within-build variance per track (per run for
    a health metric), over every group of runs of one build with the same pin and setup."""
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for rec in records:
        key = (build_key(rec), str(rec.get("corpus")), setup_key(rec))
        groups.setdefault(key, []).append(rec)
    out: dict[str, Noise] = {}
    for m in METRICS:
        ss, dof, builds = 0.0, 0, 0
        for recs in groups.values():
            if len(recs) < 2:
                continue
            used = False
            keys = set().union(*(cells(r, m).keys() for r in recs))
            for k in keys:
                group = [cells(r, m)[k] for r in recs if k in cells(r, m)]
                vals = [v for v in (_num(g.get(m.key)) for g in group) if v is not None]
                if len(vals) < 2 or (not m.health and lag_comparable(group)):
                    continue
                mean = sum(vals) / len(vals)
                ss += sum((v - mean) ** 2 for v in vals)
                dof += len(vals) - 1
                used = True
            builds += used
        out[m.key] = Noise(math.sqrt(ss / dof) if dof else None, dof, builds)
    return out


@dataclass
class Result:
    metric: Metric
    stable: float | None = None
    candidate: float | None = None
    diff: float | None = None
    margin: float | None = None  # K standard errors of the difference
    tracks: int = 0
    verdict: str = "not measured"  # better | worse | within noise | noise unknown | not measured
    left_out: dict[str, str] = field(default_factory=dict)


def compare(
    cand: list[dict[str, Any]], base: list[dict[str, Any]], noises: dict[str, Noise]
) -> list[Result]:
    """Each metric: the mean over comparable tracks of candidate minus stable, against its
    noise. Several runs of one side are averaged per track first (a health metric: its one
    value per run, averaged)."""
    results = []
    for m in METRICS:
        res = Result(m)
        keys = sorted(set().union(*(cells(r, m).keys() for r in cand + base)))
        diffs, inv = [], 0.0
        cs, ss = [], []
        for k in keys:
            c = [cells(r, m)[k] for r in cand if k in cells(r, m)]
            s = [cells(r, m)[k] for r in base if k in cells(r, m)]
            cv = [v for v in (_num(g.get(m.key)) for g in c) if v is not None]
            sv = [v for v in (_num(g.get(m.key)) for g in s) if v is not None]
            if not cv and not sv:
                continue
            name = k[0]
            if not cv or not sv:
                what = "value" if m.health else "score"
                res.left_out[name] = f"no {what} from the {'candidate' if not cv else 'stable'}"
                continue
            why = None if m.health else lag_comparable(c + s)
            if why:
                res.left_out[name] = why
                continue
            cm, sm = sum(cv) / len(cv), sum(sv) / len(sv)
            cs.append(cm)
            ss.append(sm)
            diffs.append(cm - sm)
            inv += 1 / len(cv) + 1 / len(sv)
        res.tracks = len(diffs)
        if not diffs:
            results.append(res)
            continue
        n = len(diffs)
        res.candidate, res.stable = sum(cs) / n, sum(ss) / n
        res.diff = sum(diffs) / n
        nz = noises.get(m.key)
        if nz is None or nz.sd is None or nz.dof < MIN_NOISE_DOF:
            res.verdict = "noise unknown"
        else:
            res.margin = K * nz.sd * math.sqrt(inv) / n
            gain = res.diff if m.higher else -res.diff
            if abs(res.diff) <= res.margin:
                res.verdict = "within noise"
            else:
                res.verdict = "better" if gain > 0 else "worse"
        results.append(res)
    return results


# -- health ------------------------------------------------------------------------------------


def health_from_report(report: dict[str, Any]) -> dict[str, Any]:
    """A report's health as blinky-server's staging.health_record makes it (for lines written
    before the record carried it)."""
    rebooted = any(
        (h.get("stability") or {}).get("rebooted") for h in (report.get("health") or {}).values()
    ) or any(
        (f.get("no_reboot") or {}).get("pass") is False
        for f in (report.get("final") or {}).values()
    )
    return {
        "passed": report.get("passed"),
        "rebooted": bool(rebooted),
        "strict": {
            r["rule"]: {"status": r.get("status"), "worst": r.get("worst"), "mode": r.get("mode")}
            for r in (report.get("strict") or {}).get("rules") or []
            if r.get("rule")
        },
    }


def health_of(rec: dict[str, Any], reports: Path | None = None) -> dict[str, Any] | None:
    if rec.get("health"):
        return dict(rec["health"])
    path = (reports or DATA / "staging-reports") / f"{rec.get('sha')}.json"
    try:
        return health_from_report(json.loads(path.read_text()))
    except (OSError, ValueError):
        return None


def health_problems(
    runs: list[dict[str, Any]], hw_state: str | None, blocking: set[str]
) -> list[str]:
    """Why a candidate can never be proposed: its hardware checks, a reboot, or a strict rule
    that blocks now failing in any run (each run's health, from health_of)."""
    out = []
    if hw_state != "success":
        out.append(f"`{HW_CONTEXT}` is {hw_state or 'missing'} on its staging commit")
    for h in runs:
        if h.get("rebooted"):
            out.append(f"a run rebooted ({h.get('version') or h.get('run') or '?'})")
        for rule, r in (h.get("strict") or {}).items():
            if rule in blocking and r.get("status") in ("warn", "fail"):
                out.append(f"strict rule `{rule}` (blocking) failed: worst {r.get('worst')}")
    return sorted(set(out))


@dataclass
class Decision:
    qualifies: bool
    why: str


def decide(results: list[Result], problems: list[str]) -> Decision:
    if problems:
        return Decision(False, "health: " + "; ".join(problems))
    worse = [r.metric.name for r in results if r.verdict == "worse"]
    # A health metric's unknown noise counts neither way (module docstring, "Qualifies").
    unknown = [r.metric.name for r in results if r.verdict == "noise unknown"]
    blocking = [
        r.metric.name for r in results if r.verdict == "noise unknown" and not r.metric.health
    ]
    better = [r.metric.name for r in results if r.verdict == "better"]
    if worse:
        return Decision(False, "worse beyond its noise: " + ", ".join(worse))
    if blocking:
        return Decision(False, "noise not measured yet for: " + ", ".join(blocking))
    if not better:
        why = "no metric better beyond its noise"
        if unknown:
            why += " (noise not measured yet for: " + ", ".join(unknown) + ")"
        return Decision(False, why)
    return Decision(True, "better beyond its noise: " + ", ".join(better))


# -- what to compare ---------------------------------------------------------------------------


def number(tag: str) -> int:
    m = re.fullmatch(r"b(\d+)", tag)
    return int(m.group(1)) if m else -1


def releases(gh_: Gh) -> tuple[str | None, str | None]:
    """(the current stable tag, the newest candidate tag newer than it)."""
    rels = json.loads(gh_("api", f"repos/{FW_REPO}/releases?per_page=50") or "[]")
    rels = [r for r in rels if not r.get("draft") and number(str(r.get("tag_name"))) >= 0]
    stable = max(
        (str(r["tag_name"]) for r in rels if not r.get("prerelease")), key=number, default=None
    )
    newer = [
        str(r["tag_name"])
        for r in rels
        if r.get("prerelease") and (stable is None or number(str(r["tag_name"])) > number(stable))
    ]
    return stable, max(newer, key=number, default=None)


def commit(gh_: Gh, ref: str) -> dict[str, Any]:
    c = json.loads(gh_("api", f"repos/{FW_REPO}/commits/{ref}"))
    return {
        "sha": c["sha"],
        "tree": c["commit"]["tree"]["sha"],
        "parents": [p["sha"] for p in c.get("parents") or []],
    }


def staging_commit(gh_: Gh, tag: str) -> tuple[str | None, str]:
    """The staging commit a candidate was cut from: its tag's second parent (the staging ->
    master merge), when the trees are the same. (sha, why)."""
    c = commit(gh_, tag)
    if len(c["parents"]) < 2:
        return None, f"{tag} isn't a merge from staging"
    p = commit(gh_, c["parents"][1])
    if p["tree"] != c["tree"]:
        return None, f"{tag}'s tree isn't its staging parent's {p['sha'][:12]}"
    return p["sha"], "ok"


def hw_state(gh_: Gh, sha: str) -> str | None:
    st = json.loads(gh_("api", f"repos/{FW_REPO}/commits/{sha}/status") or "{}")
    for s in st.get("statuses") or []:  # newest first
        if s.get("context") == HW_CONTEXT:
            return str(s.get("state"))
    return None


def corpus_pin(corpus: Path) -> str | None:
    r = subprocess.run(
        ["git", "-C", str(corpus), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )
    out = r.stdout.strip()
    return out if r.returncode == 0 and len(out) == 40 else None


@dataclass
class Plan:
    """What the comparison has, and the next run it needs (if any)."""

    stable: str
    candidate: str
    pin: str
    cand_sha: str | None = None
    stable_sha: str | None = None
    cand: list[dict[str, Any]] = field(default_factory=list)
    base: list[dict[str, Any]] = field(default_factory=list)
    need: dict[str, Any] | None = None  # the run to request: {key, what, body}
    blocked: str | None = None  # why nothing can be compared or run


def plan(
    records: list[dict[str, Any]],
    stable: str,
    stable_sha: str,
    candidate: str,
    cand_sha: str,
    pin: str,
    noises: dict[str, Noise],
    failed: dict[str, int],
) -> Plan:
    p = Plan(stable, candidate, pin, cand_sha=cand_sha, stable_sha=stable_sha)
    cand_all = [r for r in records if r.get("sha") == cand_sha and not r.get("release")]
    p.cand = [r for r in cand_all if r.get("corpus") == pin]

    def need(key: str, what: str, body: dict[str, Any]) -> None:
        if failed.get(key, 0) >= MAX_FAILURES:
            p.blocked = f"{what}: given up after {failed[key]} runs without scores"
        else:
            p.need = {"key": key, "what": what, "body": body}

    if not p.cand:
        build_of = next((r.get("build") for r in cand_all if r.get("build")), None)
        body: dict[str, Any] = {"sha": cand_sha}
        if build_of and build_of != cand_sha:
            body["build_of"] = build_of
        need(f"cand:{cand_sha}:{pin}", f"{candidate}'s staging commit under corpus {pin[:8]}", body)
        return p
    setup = setup_key(p.cand[-1])
    p.cand = [r for r in p.cand if setup_key(r) == setup]
    base_all = [r for r in records if r.get("release") == stable and r.get("corpus") == pin]
    p.base = [r for r in base_all if setup_key(r) == setup]
    base_body = {"sha": stable_sha, "release": stable}
    if not p.base:
        need(f"base:{stable}:{pin}:1", f"the stable {stable} under corpus {pin[:8]}", base_body)
        return p

    def has(recs: list[dict[str, Any]], m: Metric) -> bool:
        return any(_num(t.get(m.key)) is not None for r in recs for t in cells(r, m).values())

    # Only metrics both sides measured need noise: one the stable firmware doesn't report
    # (an older build without the field) is "not measured" and never waits for repeats.
    # Health metrics count here, so the stable's repeat runs (up to the cap) measure theirs.
    measured = [m.key for m in METRICS if has(p.cand, m) and has(p.base, m)]
    short = [k for k in measured if noises[k].dof < MIN_NOISE_DOF]
    if short:
        n = len(base_all)
        if n < MAX_BASELINE_RUNS:
            need(
                f"base:{stable}:{pin}:{n + 1}",
                f"the stable {stable} again, for the run-to-run noise ({', '.join(short)})",
                base_body,
            )
    return p


# -- the issue ---------------------------------------------------------------------------------


def _fmt(m: Metric, v: float | None) -> str:
    return "—" if v is None else m.fmt.format(v)


def _signed(m: Metric, v: float | None) -> str:
    return "—" if v is None else ("+" if v >= 0 else "") + m.fmt.format(v)


MARK = {
    "better": "✅ better",
    "worse": "❌ worse",
    "within noise": "≈ within noise",
    "noise unknown": "? noise unknown",
    "not measured": "— not measured",
}


def runs_line(recs: list[dict[str, Any]]) -> str:
    return ", ".join(
        f"`{r.get('version')}` ({time.strftime('%Y-%m-%d', time.gmtime(float(r.get('at') or 0)))})"
        for r in recs
    )


def body(
    p: Plan,
    results: list[Result],
    noises: dict[str, Noise],
    d: Decision,
    healths: list[dict[str, Any]],
) -> str:
    head = (
        f"**{p.candidate} leads the stable {p.stable}** on the test box's quality suite, "
        "beyond the box's run-to-run noise, with no health problem."
        if d.qualifies
        else f"**{p.candidate} doesn't qualify now**: {d.why}."
    )
    lines = [
        head,
        "",
        f"Corpus pin `{p.pin[:12]}`; {p.candidate} = staging `{(p.cand_sha or '')[:12]}` "
        f"(runs: {runs_line(p.cand)}); {p.stable} = `{(p.stable_sha or '')[:12]}` "
        f"(runs: {runs_line(p.base)}). Same chips and limits on both sides.",
        "",
        f"| metric | {p.stable} | {p.candidate} | difference | noise (±{K:g} SE) | tracks | |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in results:
        m = r.metric
        lines.append(
            f"| {m.name} | {_fmt(m, r.stable)} | {_fmt(m, r.candidate)} | {_signed(m, r.diff)} | "
            f"{_fmt(m, r.margin)} | {('per run' if m.health else r.tracks) if r.tracks else '—'} | "
            f"{MARK[r.verdict]} |"
        )
    lines += [
        "",
        "Each value is the mean over the tracks both sides scored with comparable lags "
        '(DEV_LOOP.md, "Lag"); for the health metrics (longest frame to beat-pace error), over '
        "the runs, each run's value the strict rule's worst on the worst chip. A health metric "
        "whose noise isn't measured yet counts neither way. "
        "The noise is measured from builds the box ran more than once: "
        + ", ".join(
            f"{m.key} sd {noises[m.key].sd:.4f} ({noises[m.key].dof} dof)"
            for m in METRICS
            if noises[m.key].sd is not None
        )
        + ". repS isn't measured by the box yet. No composite score: each metric is judged on "
        "its own.",
    ]
    # Per metric: a track one metric leaves out (no tempo from an older firmware) still counts
    # for the others.
    left = [
        f"{r.metric.name}: " + "; ".join(f"{t} ({why})" for t, why in sorted(r.left_out.items()))
        for r in results
        if r.left_out
    ]
    if left:
        lines += ["", "<details><summary>Tracks left out, per metric</summary>", ""]
        lines += [f"- {x}" for x in left] + ["", "</details>"]
    lines += ["", "**Health** (any of these rules a candidate out):", ""]
    lines.append(f"- `{HW_CONTEXT}` on the staging commit")
    for h in healths:
        blocking = [k for k, v in (h.get("strict") or {}).items() if v.get("mode") == "block"]
        worst = ", ".join(f"{k} {(h['strict'][k] or {}).get('worst')}" for k in blocking)
        lines.append(
            f"- run `{h.get('version') or '?'}`: rebooted {'yes' if h.get('rebooted') else 'no'}"
            + (f"; blocking strict rules (worst): {worst}" if worst else "")
        )
    lines += [
        "",
        "The owner decides. Promotion is blinky-firmware's `release_promote.py`; the box never "
        "promotes. This issue is updated in place while the comparison changes and closed when "
        "a newer candidate is proposed or this one stops qualifying.",
        "",
        'Opened by the test box (scripts/testbox/proposals.py; DEV_LOOP.md, "Stable candidates '
        'from scores").',
    ]
    return "\n".join(lines)


def _gh_input(gh_: Gh, payload: dict[str, Any], *args: str) -> Any:
    return json.loads(gh_("api", *args, "--input", "-", stdin=json.dumps(payload)) or "null")


def find_issue(gh_: Gh, title: str) -> dict[str, Any] | None:
    raw = gh_(
        "api",
        "-X",
        "GET",
        "search/issues",
        "-f",
        f'q=repo:{TOOLING_REPO} is:issue in:title "{title}"',
        "--jq",
        ".items[] | {title, number, url: .html_url, state}",
    )
    for ln in raw.splitlines():
        if ln.strip():
            it = json.loads(ln)
            if it.get("title") == title:
                return dict(it)
    return None


def upsert(gh_: Gh, st: dict[str, Any], tag: str, text: str, open_: bool) -> dict[str, Any] | None:
    """The candidate's one issue, opened, updated or closed to match. Its record, or None when
    there is none and none is wanted."""
    issues = st.setdefault("issues", {})
    rec = issues.get(tag)
    if rec is None:
        found = find_issue(gh_, TITLE.format(tag))
        if found:
            rec = {"number": found["number"], "url": found["url"], "state": found["state"]}
    digest = hashlib.sha256(text.encode()).hexdigest()[:16]
    if rec is None:
        if not open_:
            return None
        got = _gh_input(
            gh_,
            {"title": TITLE.format(tag), "body": text},
            "-X",
            "POST",
            f"repos/{TOOLING_REPO}/issues",
        )
        rec = {"number": got["number"], "url": got["html_url"], "state": "open", "digest": digest}
        log(f"proposal: opened {rec['url']}")
        issues[tag] = rec
        return dict(rec)
    patch: dict[str, Any] = {}
    if rec.get("digest") != digest:
        patch["body"] = text
    want = "open" if open_ else "closed"
    if rec.get("state") != want:
        patch["state"] = want
    if patch:
        _gh_input(gh_, patch, "-X", "PATCH", f"repos/{TOOLING_REPO}/issues/{rec['number']}")
        log(f"proposal {tag}: {', '.join(patch)} updated ({rec['url']})")
    rec.update(digest=digest, state=want)
    issues[tag] = rec
    return dict(rec)


def close(gh_: Gh, st: dict[str, Any], tag: str, why: str) -> None:
    rec = (st.get("issues") or {}).get(tag)
    if not rec or rec.get("state") == "closed":
        return
    _gh_input(
        gh_, {"body": why}, "-X", "POST", f"repos/{TOOLING_REPO}/issues/{rec['number']}/comments"
    )
    _gh_input(
        gh_, {"state": "closed"}, "-X", "PATCH", f"repos/{TOOLING_REPO}/issues/{rec['number']}"
    )
    rec["state"] = "closed"
    log(f"proposal {tag}: closed ({why})")


# -- one evaluation ----------------------------------------------------------------------------


@dataclass
class Outcome:
    plan: Plan | None
    results: list[Result]
    decision: Decision | None
    text: str | None
    message: str
    stable: str | None = None  # the current stable release, whenever it is known


def evaluate(
    gh_: Gh,
    st: dict[str, Any],
    records: list[dict[str, Any]],
    pin: str,
    blocking: set[str],
    reports: Path | None = None,
) -> Outcome:
    """Compare the newest candidate with the stable release (no writes)."""
    stable, candidate = releases(gh_)
    if stable is None or candidate is None:
        return Outcome(
            None,
            [],
            None,
            None,
            f"nothing to compare (stable {stable}, candidate {candidate})",
            stable,
        )
    cand_sha, why = staging_commit(gh_, candidate)
    if cand_sha is None:
        return Outcome(None, [], None, None, f"{candidate}: {why}", stable)
    stable_sha = commit(gh_, stable)["sha"]
    noises = noise(records)
    p = plan(records, stable, stable_sha, candidate, cand_sha, pin, noises, st.get("failed") or {})
    if not p.cand or not p.base:
        msg = p.blocked or f"waiting for {p.need['what'] if p.need else 'scores'}"
        return Outcome(p, [], None, None, f"{candidate} vs {stable}: {msg}", stable)
    results = compare(p.cand, p.base, noises)
    cand_all = [r for r in records if r.get("sha") == cand_sha and not r.get("release")]
    healths: list[dict[str, Any]] = []
    for r in cand_all:
        h = health_of(r, reports)
        if h is None:
            healths.append({"version": r.get("version"), "strict": {}, "unknown": True})
        else:
            healths.append({**h, "version": r.get("version")})
    problems = health_problems(healths, hw_state(gh_, cand_sha), blocking)
    problems += [f"run {h['version']}: health unreadable" for h in healths if h.get("unknown")]
    d = decide(results, problems)
    text = body(p, results, noises, d, healths)
    return Outcome(p, results, d, text, f"{candidate} vs {stable}: {d.why}", stable)


def apply(gh_: Gh, st: dict[str, Any], out: Outcome) -> None:
    """Open, update or close issues to match the outcome."""
    if out.stable is not None:
        # Before anything else, and with or without a candidate: right after the owner promotes
        # the proposed candidate there is no newer one, and its proposal must close anyway.
        for tag, rec in list((st.get("issues") or {}).items()):
            if number(tag) <= number(out.stable) and rec.get("state") == "open":
                why = (
                    f"{tag} is the stable release now."
                    if tag == out.stable
                    else f"{out.stable}, newer than {tag}, is the stable release now."
                )
                close(gh_, st, tag, why)
    p = out.plan
    if p is None or out.decision is None or out.text is None:
        return
    rec = upsert(gh_, st, p.candidate, out.text, out.decision.qualifies)
    if out.decision.qualifies and rec:
        for tag, old in list((st.get("issues") or {}).items()):
            if (
                tag != p.candidate
                and number(tag) < number(p.candidate)
                and old.get("state") == "open"
            ):
                close(gh_, st, tag, f"Superseded by the proposal of {p.candidate}: {rec['url']}")


def blocking_rules(server: Callable[..., Any]) -> set[str]:
    got = server("GET", "/api/test/strict-rules")
    return {k for k, v in (got.get("readiness") or {}).items() if v.get("mode") == "block"}


def main(argv: list[str] | None = None) -> int:
    """Print the comparison as the staging runner would make it now. Read-only: the runner
    (staging.py) is what opens, updates and closes issues and submits runs."""
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status", action="store_true", help="print the proposal state")
    args = ap.parse_args(argv)
    sys.path.insert(0, str(Path(__file__).parent))
    import staging  # the runner's gh, server and state

    st = staging.load_state().get("proposal") or {}
    if args.status:
        print(json.dumps(st, indent=2))
        return 0
    pin = corpus_pin(staging.LINK / "corpus")
    if pin is None:
        log("ERROR: the deployed corpus pin is unreadable")
        return 2
    out = evaluate(staging.gh, st, load_records(), pin, blocking_rules(staging.http))
    log(out.message)
    if out.plan and out.plan.need:
        log(f"next run: {out.plan.need['what']} {json.dumps(out.plan.need['body'])}")
    if out.text:
        print(out.text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
