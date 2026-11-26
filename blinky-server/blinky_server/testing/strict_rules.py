"""The strict rules (owner, 2026-11-25; docs/DEV_LOOP.md, "Strict rules"): the bench release
gate's limits (blinky-firmware tools/release_gate.py) that don't block on the box yet.

> We want to get the firmware and tooling in a place where it elegantly passes the strictest
> rules. Until then leave them as non-blocking warnings. As soon as they pass we can enable the
> strict rules.

Each rule is one row of RULES: a name, a threshold, the blinky-firmware issues it waits on and a
mode. The lifecycle:

1. **warn** (every rule today): measured and reported in every run (the hardware checks, the
   staging quality suite, dev runs), exported as metrics, and never a failure: a warning never
   changes a commit status.
2. **ready**: it has passed on the last READY_AFTER distinct firmware builds with real runs
   (staging heads and PR heads that passed the hardware checks), with no warning in between.
   The staging report says so, and the staging runner opens one blinky_time issue per rule.
3. **block**: the owner switches its row's mode to BLOCK (one line here). A failing block rule
   is then a failing check in the hardware checks (a red ``Hardware test`` status) and fails a
   dev run. Never automatic.

**No exclusions.** Settings saves used to stall the loop ~85-115 ms erasing a flash page, and
the windows that might hold one of the server's saves were left out of the frame-time rule.
Since blinky-firmware#49 (the settings journal) a save only writes flash, so every window
counts, and a slow frame is a slow frame.

The window a ``json info`` read reports: the firmware publishes its last *closed* 5 s window
(blinky-firmware audio/LoopMetrics.h). Firmware since blinky-firmware#80 also says when it
closed and how long it ran (``frameWindowEndMs``, ``frameWindowMs``, uptime ms); the reply's
own ``millis`` maps that into host time (``w_start``, ``w_end`` in the run record).
"""

from __future__ import annotations

import itertools
import json
import logging
import os
import time
from dataclasses import asdict, dataclass
from typing import Any

from ..paths import data_dir

log = logging.getLogger(__name__)

WARN, BLOCK = "warn", "block"
# A rule's result in one run.
PASS, WARNED, FAILED, NO_DATA = "pass", "warn", "fail", "no data"


@dataclass(frozen=True)
class Rule:
    name: str
    what: str
    threshold: float
    unit: str
    # "max": the run's largest value must be <= threshold; "min": its smallest >=; "abs": its
    # value must be within +-threshold (a signed offset from a calibration).
    worst: str
    issues: tuple[int, ...]  # the blinky-firmware issues it waits on
    mode: str = WARN


# The strict rules, defined once. Switching one to blocking is its mode, WARN -> BLOCK: the
# owner's decision, once the readiness tracker shows it passing (docs/DEV_LOOP.md).
RULES: tuple[Rule, ...] = (
    Rule(
        "frame_time",
        "longest frame in every 5 s loop window (maxFrameMs)",
        100.0,
        "ms",
        "max",
        (),
        WARN,
    ),
    Rule("fps", "loop fps in every 5 s window, sustained", 60.0, "fps", "min", (), WARN),
    Rule("overrun_growth", "audioOverruns growth across the run", 0.0, "", "max", (), WARN),
    # Stream lines the firmware skipped whole because the host wasn't draining its serial
    # buffer (blinky-firmware#85: ``streamDropped`` in ``json info``). Any skip means the
    # server missed analysis data the run judged on.
    Rule(
        "stream_dropped",
        "stream lines skipped by the firmware across the run (streamDropped growth)",
        0.0,
        "",
        "max",
        (),
        WARN,
    ),
    Rule("beat_pace", "beats counted vs the tempo over 60 s of music", 0.03, "", "max", (), WARN),
    Rule("music_level", "mean mic level with music playing", 0.05, "", "min", (), WARN),
    Rule("music_onsets", "pulse onsets with music playing", 1.0, "/s", "min", (), WARN),
    # The chip's detection latency within +-10 ms of the rig's calibration (testing/
    # detection_latency.py): per track the music's check minus the measured lag, the median over
    # the run's tracks that caught kickR50 >= 0.5. Calibration spread: sd 2.9 ms per track, so
    # even a one-track run sits 3.4 sd inside; 10 ms is a third of the tight +-30 ms window
    # (kickR30), so a drift warns well before it costs score at that tolerance.
    Rule(
        "detection_latency",
        "detection latency vs the 45 ms calibration: median lagCheckMs - lagMs over tracks "
        "with kickR50 >= 0.5",
        10.0,
        "ms",
        "abs",
        (),
        WARN,
    ),
)
BY_NAME = {r.name: r for r in RULES}

# Readiness: passed on this many distinct firmware builds in a row (docs/DEV_LOOP.md).
READY_AFTER = int(os.environ.get("BLINKY_STRICT_READY_AFTER") or 5)
WINDOW_S = 5.0  # blinky-firmware LoopMetrics::WINDOW_MS
PACE_SLACK_BEATS = 2.0  # a beat either way at each end of the window is sampling, not drift
RECORD_KEEP = 500  # lines of the run record kept

ORDER = {FAILED: 3, WARNED: 2, PASS: 1, NO_DATA: 0}


def threshold(name: str) -> float:
    return BY_NAME[name].threshold


def table() -> list[dict[str, Any]]:
    return [asdict(r) for r in RULES]


def clock(t: float) -> str:
    """hh:mm:ss.mmm, the box's local time."""
    ms = round(t * 1000)  # rounded once, so .9996 s carries into the next second
    return time.strftime("%H:%M:%S", time.localtime(ms // 1000)) + f".{ms % 1000:03d}"


# -- measuring ---------------------------------------------------------------------------------


def window(info: dict[str, Any], t: float, where: str) -> dict[str, Any]:
    """One ``json info`` read's loop metrics, with the host time it was read and where. With
    firmware that reports its window's close time, also the window's host-time span
    (``w_start``, ``w_end``)."""
    out = {
        "t": round(t, 3),
        "ms": info.get("millis"),
        "fps": info.get("fps"),
        "max_frame_ms": info.get("maxFrameMs"),
        "overruns": info.get("audioOverruns"),
        "stream_dropped": info.get("streamDropped"),  # firmware before blinky-firmware#85: None
        "where": where,
    }
    span = window_span(info, t)
    if span is not None:
        out["w_start"], out["w_end"] = round(span[0], 3), round(span[1], 3)
    return out


def window_span(info: dict[str, Any], t: float) -> tuple[float, float] | None:
    """The host-time span of the window a read at ``t`` reports, or None if the firmware doesn't
    say when it closed (before blinky-firmware#80) or no window has closed yet."""
    end_ms, len_ms, now_ms = (
        info.get("frameWindowEndMs"),
        info.get("frameWindowMs"),
        info.get("millis"),
    )
    if not (
        isinstance(end_ms, int | float)
        and isinstance(len_ms, int | float)
        and isinstance(now_ms, int | float)
    ):
        return None
    if end_ms <= 0 or len_ms <= 0 or end_ms > now_ms:
        return None  # nothing closed yet, or a reading that can't be placed
    w_end = t - (now_ms - end_ms) / 1000.0  # the read's own host-minus-millis offset
    return w_end - len_ms / 1000.0, w_end


def _row(rule: Rule, value: float | None, ok: bool | None, **extra: Any) -> dict[str, Any]:
    status = NO_DATA if value is None or ok is None else PASS if ok else WARNED
    if status == WARNED and rule.mode == BLOCK:
        status = FAILED
    return {
        "rule": rule.name,
        "mode": rule.mode,
        "threshold": rule.threshold,
        "unit": rule.unit,
        "worst_is": rule.worst,
        "issues": list(rule.issues),
        "status": status,
        "worst": value,
        **extra,
    }


def _within(rule: Rule, value: float) -> bool:
    if rule.worst == "abs":
        return abs(value) <= rule.threshold
    return value <= rule.threshold if rule.worst == "max" else value >= rule.threshold


def _badness(rule: Rule, value: float | None) -> float:
    """How far toward failing a value is, larger worse (the worst device first)."""
    v = float(value or 0)
    return abs(v) if rule.worst == "abs" else v if rule.worst == "max" else -v


def _frame_time(windows: list[dict[str, Any]]) -> dict[str, Any]:
    rule = BY_NAME["frame_time"]
    got = [w for w in windows if w.get("max_frame_ms") is not None]
    if not got:
        return _row(rule, None, None, samples=0)
    w = max(got, key=lambda x: float(x["max_frame_ms"]))
    v = float(w["max_frame_ms"])
    return _row(rule, v, _within(rule, v), at=clock(w["t"]), where=w.get("where"), samples=len(got))


def _fps(windows: list[dict[str, Any]]) -> dict[str, Any]:
    rule = BY_NAME["fps"]
    got = [w for w in windows if w.get("fps") is not None]
    if not got:
        return _row(rule, None, None, samples=0)
    w = min(got, key=lambda x: float(x["fps"]))
    v = float(w["fps"])
    return _row(rule, v, _within(rule, v), at=clock(w["t"]), where=w.get("where"), samples=len(got))


def _growth(rule: Rule, key: str, windows: list[dict[str, Any]]) -> dict[str, Any]:
    """A counter's growth summed over consecutive reads, skipping a pair across a reboot (the
    counter restarts; the reboot itself is the blocking checks' to judge)."""
    got = sorted(
        (w for w in windows if w.get(key) is not None and w.get("ms") is not None),
        key=lambda x: float(x["t"]),
    )
    if len(got) < 2:
        return _row(rule, None, None, samples=len(got))
    growth = 0
    for a, b in itertools.pairwise(got):
        if int(b["ms"]) >= int(a["ms"]):
            growth += max(0, int(b[key]) - int(a[key]))
    return _row(
        rule,
        float(growth),
        _within(rule, growth),
        at=f"{clock(got[0]['t'])}-{clock(got[-1]['t'])}",
        where=f"{got[0].get('where')} to {got[-1].get('where')}",
        samples=len(got),
    )


def _overruns(windows: list[dict[str, Any]]) -> dict[str, Any]:
    return _growth(BY_NAME["overrun_growth"], "overruns", windows)


def _stream_dropped(windows: list[dict[str, Any]]) -> dict[str, Any]:
    return _growth(BY_NAME["stream_dropped"], "stream_dropped", windows)


def _music(music: dict[str, Any] | None, pace: dict[str, Any] | None) -> list[dict[str, Any]]:
    out = []
    level = (music or {}).get("level_mean")
    onsets = (music or {}).get("onsets_per_s")
    r = BY_NAME["music_level"]
    out.append(_row(r, level, None if level is None else _within(r, float(level))))
    r = BY_NAME["music_onsets"]
    out.append(_row(r, onsets, None if onsets is None else _within(r, float(onsets))))
    r = BY_NAME["beat_pace"]
    p = pace or {}
    if p.get("error") is None:
        out.append(_row(r, None, None))
    else:
        err = float(p["error"])
        slack = abs(float(p["counted"]) - float(p["expected"])) <= PACE_SLACK_BEATS
        out.append(
            _row(
                r,
                err,
                _within(r, err) or slack,
                where=f"{p.get('counted')} counted, {p.get('expected')} expected",
            )
        )
    return out


def _detection(latency: dict[str, Any] | None) -> dict[str, Any]:
    """detection_latency over one device's runs (detection_latency.summarize's entry): the
    median offset of the runs that count; the ones left out are named with the reason."""
    rule = BY_NAME["detection_latency"]
    if not latency:
        return _row(rule, None, None)
    runs = latency.get("runs") or []
    left_out = [
        {"track": x.get("track"), "offset_ms": x.get("offset_ms"), "why": x.get("why")}
        for x in runs
        if not x.get("counted")
    ]
    n = int(latency.get("counted") or 0)
    median = latency.get("median_ms")
    where = f"median of {n} of {len(runs)} tracks" if n else f"none of {len(runs)} tracks counted"
    if median is None:
        return _row(rule, None, None, where=where, samples=0, left_out=left_out)
    v = float(median)
    return _row(rule, v, _within(rule, v), where=where, samples=n, left_out=left_out)


def evaluate(
    windows: list[dict[str, Any]],
    music: dict[str, Any] | None = None,
    pace: dict[str, Any] | None = None,
    latency: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    """One device's run: every rule's row (status, worst value, where, exclusions)."""
    rows = [
        _frame_time(windows),
        _fps(windows),
        _overruns(windows),
        _stream_dropped(windows),
        *_music(music, pace),
        _detection(latency),
    ]
    by = {r["rule"]: r for r in rows}
    return [by[r.name] for r in RULES]


def combine(per_device: dict[str, list[dict[str, Any]]]) -> list[dict[str, Any]]:
    """Per rule, the worst device's row (its status first, then its value), named."""
    out = []
    for rule in RULES:
        rows = [
            {**r, "device": dev}
            for dev, rs in per_device.items()
            for r in rs
            if r["rule"] == rule.name
        ]
        if not rows:
            continue
        rows.sort(key=lambda r: (ORDER[r["status"]], _badness(rule, r["worst"])), reverse=True)
        out.append(rows[0])
    return out


def report(
    windows: dict[str, list[dict[str, Any]]],
    music: dict[str, dict[str, Any]] | None = None,
    latency: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """The run's strict section: {"rules": the summary rows, "devices": each device's rows}.
    ``music``: per device, the quality suite's {"music", "beatpace"} (None: not measured).
    ``latency``: per device, detection_latency.summarize over the run's scored tracks (None:
    the run scored none)."""
    devices = sorted(set(windows) | set(music or {}) | set(latency or {}))
    per = {
        d: evaluate(
            windows.get(d) or [],
            ((music or {}).get(d) or {}).get("music"),
            ((music or {}).get(d) or {}).get("beatpace"),
            (latency or {}).get(d),
        )
        for d in devices
    }
    return {"rules": combine(per), "devices": per}


def failures(strict: dict[str, Any] | None) -> list[dict[str, Any]]:
    """The block-mode rules that failed: these fail the run."""
    return [r for r in (strict or {}).get("rules") or [] if r["status"] == FAILED]


def warnings(strict: dict[str, Any] | None) -> list[dict[str, Any]]:
    return [r for r in (strict or {}).get("rules") or [] if r["status"] == WARNED]


def judge(
    windows: dict[str, list[dict[str, Any]]],
    source: str,
    music: dict[str, dict[str, Any]] | None = None,
    latency: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """The run's strict section, and the metrics set."""
    from .. import telemetry

    out = report(windows, music, latency)
    telemetry.record_strict(source, out)
    if latency is not None:
        telemetry.record_detection_latency(source, latency)
    return out


# -- readiness: the record of runs, and each rule's streak --------------------------------------


def record_path() -> Any:
    """One line per firmware run that counts for readiness: JSON {at, source, sha, build,
    version, pr, rules: {name: {status, worst}}}."""
    return data_dir() / "strict-record.jsonl"


def records() -> list[dict[str, Any]]:
    p = record_path()
    if not p.exists():
        return []
    out = []
    for raw in p.read_text().splitlines():
        try:
            out.append(json.loads(raw))
        except json.JSONDecodeError:
            log.warning("strict record: an unreadable line skipped: %r", raw[:80])
    return out


def record_run(
    strict: dict[str, Any],
    *,
    source: str,
    sha: str,
    version: str | None,
    build: str | None = None,
    pr: int | None = None,
    at: float | None = None,
) -> dict[str, Any]:
    """Add a run (a staging quality suite, or hardware checks that passed on a PR or staging
    head) to the record. ``build``: what identifies the firmware build (default the sha; a
    staging commit with a PR head's tree is that head's build). A repeat of the same source and
    sha replaces its line (a retest, a repeated step)."""
    line = {
        "at": round(time.time() if at is None else at, 1),
        "source": source,
        "sha": sha,
        "build": build or sha,
        "version": version,
        "pr": pr,
        "rules": {
            r["rule"]: {"status": r["status"], "worst": r.get("worst")}
            for r in strict.get("rules") or []
        },
    }
    keep = [r for r in records() if (r.get("source"), r.get("sha")) != (source, sha)]
    rows = [*keep, line][-RECORD_KEEP:]
    p = record_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text("".join(json.dumps(r) + "\n" for r in rows))
    tmp.replace(p)
    from .. import telemetry

    telemetry.record_streaks(readiness(rows))
    return line


def readiness(rows: list[dict[str, Any]] | None = None, n: int | None = None) -> dict[str, Any]:
    """Per rule: its streak (distinct builds in a row, newest first, whose every run with data
    passed), ready (streak >= n), and the evidence (those builds, and the warning that ended
    the streak, if any). A run without data for a rule doesn't count either way."""
    rows = records() if rows is None else rows
    n = READY_AFTER if n is None else n
    out: dict[str, Any] = {}
    for rule in RULES:
        builds: dict[str, dict[str, Any]] = {}
        for r in sorted(rows, key=lambda x: float(x.get("at") or 0)):
            res = (r.get("rules") or {}).get(rule.name) or {}
            if res.get("status") not in (PASS, WARNED, FAILED):
                continue
            b = builds.setdefault(r["build"], {"build": r["build"], "runs": []})
            b["last"] = float(r.get("at") or 0)
            b["runs"].append(
                {
                    "source": r.get("source"),
                    "sha": r.get("sha"),
                    "version": r.get("version"),
                    "pr": r.get("pr"),
                    "status": res["status"],
                    "worst": res.get("worst"),
                    "at": r.get("at"),
                }
            )
        streak: list[dict[str, Any]] = []
        broke: dict[str, Any] | None = None
        for b in sorted(builds.values(), key=lambda x: x["last"], reverse=True):
            bad = next((x for x in b["runs"] if x["status"] != PASS), None)
            if bad is not None:
                broke = bad
                break
            streak.append(b)
        out[rule.name] = {
            "mode": rule.mode,
            "streak": len(streak),
            "ready": len(streak) >= n,
            "needed": n,
            "builds": streak[:n],
            "broken_by": broke,
        }
    return out
