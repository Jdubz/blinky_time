"""Targeted checks on the box's test chips in dev mode (docs/DEV_LOOP.md, "Dev flash and
targeted tests"): ``POST /api/dev/run``, one durable job.

The checks run in order, in one test session: the audio lock for the whole job (no other test
job plays or measures the chips meanwhile), the chips' runtime settings restored once at the
start, and the LEDs lit from the start to the end however it ends (testing/leds.py). They are
the staging suite's own functions (testing/staging.py, testing/workflows.py):

- ``info``: one ``json info`` per chip, its timers (acfUs, acfSliceUs, acfStageUs, plpUs,
  onsetUs, maxFrameMs, heapPeak, ...);
- ``health`` (``minutes``): the suite's health step, its limits;
- ``validate`` (``tracks``): one validation run per track, scored (drum metrics);
- ``pace`` (``tracks`` optional): the suite's music and beat-pace check on the first track long
  enough to hold it;
- ``commands`` (``commands``): each line sent to each chip in order, the replies reported. A
  ``set`` here holds for the checks after it (runtime settings, never saved).

Validation and pace stream from the chips while they play; health and info read ``json info``
with no stream, as the suite measures them. At the end: no reboot since the start and no
firmware ``[ASSERT]``/``[FALLBACK]`` line, the chip's detection latency over the validated tracks
(testing/detection_latency.py: ``detection_latency``), and the strict rules
(testing/strict_rules.py) over every loop window the run read (info, health, one after each
validated track), its pace and that detection latency.

Pass = every health check (no reboot) and those asserts pass, and no strict rule in block mode
fails. Strict warnings and scores (validation, music, beat pace) are reported, never gating.

Restarts: the settings restore is a step, not per-run setup, so a resumed run keeps what its
``commands`` checks set (a server restart doesn't reset the chips; a chip reset fails the run's
no-reboot assert anyway).
"""

from __future__ import annotations

import asyncio
import contextlib
import re
import time
from collections.abc import AsyncIterator
from typing import Any

from ..device.device import alerts_since
from ..work import durable, jobs
from . import detection_latency, staging, strict_rules
from .audio_player import kill_orphan_audio, stop_audio
from .test_runner import _configure_device, _start_streaming, _stop_streaming
from .workflows import _connected, lit, plan, reserve, validation_run

KINDS = ("info", "health", "validate", "pace", "commands")
# The firmware's loop metrics (maxFrameMs, every ...MaxUs) cover one 5 s window, and a dev run
# usually starts right after a deploy, whose settings restore + save stalls the loop ~100 ms
# (blinky-firmware#49). Wait out one full window plus a margin, as deploy.sh's
# LOOP_METRICS_SETTLE_S does, so the info check reports a window that began after the run did.
# A window that may still hold the save is listed as excluded by the strict rules, not hidden.
INFO_SETTLE_S = 6.0
# json info fields reported by the info check: every timer (…Us, …MaxUs, the frame times) and
# the memory and audio counters a firmware change can move.
TIMER_RE = re.compile(r"(Us|FrameMs)$")
TIMER_KEYS = (
    "fps",
    "heapPeak",
    "heapUsed",
    "heapTotal",
    "loopStackFree",
    "cmdStackFree",
    "audioOverruns",
    "audioSamplesLost",
    "mallocIsr",
    "resetReason",
)
REPLY_MAX = 2000  # characters of each command reply kept in the report
COMMAND_GAP_S = 0.1


def timers(info: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in info.items() if TIMER_RE.search(k) or k in TIMER_KEYS}


# -- steps -------------------------------------------------------------------------------------


@durable.step
async def run_start() -> float:
    """The wall time the run starts: firmware alerts from here on count."""
    return time.time()


@durable.step
async def restore_settings(device_ids: list[str]) -> None:
    """Runtime settings back to the firmware's defaults, once per run (a step: a resumed run
    keeps what its commands checks set since)."""
    for d in await _connected(device_ids):
        await _configure_device(d, None)


@durable.step
async def info(device_ids: list[str]) -> dict[str, Any]:
    await asyncio.sleep(INFO_SETTLE_S)
    out: dict[str, Any] = {}
    for i in device_ids:
        got = await staging._info(i)
        out[i] = {
            "version": got.get("version"),
            "timers": timers(got),
            "loop": strict_rules.window(got, time.time(), "info"),
        }
    return out


@durable.step
async def strict(
    device_ids: list[str],
    windows: dict[str, list[dict[str, Any]]],
    music: dict[str, Any],
    latency: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """The strict rules over the run's loop windows, its pace check and detection latency, if
    any."""
    return strict_rules.judge(device_ids, windows, "dev", music, latency)


def validated(checks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Every played validation result of the run's validate checks, in order."""
    return [
        r
        for c in checks
        if c["kind"] == "validate"
        for r in c.get("results") or []
        if not r.get("failed")
    ]


def loop_windows(checks: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    """Every loop window the run's checks read, per device, in order."""
    out: dict[str, list[dict[str, Any]]] = {}
    for c in checks:
        if c["kind"] == "info":
            for i, d in (c.get("devices") or {}).items():
                out.setdefault(i, []).append(d["loop"])
        elif c["kind"] == "health":
            for i, d in (c.get("devices") or {}).items():
                out.setdefault(i, []).extend(d["windows"])
        for i, ws in (c.get("loop") or {}).items():
            out.setdefault(i, []).extend(ws)
    return out


@durable.step
async def commands(device_ids: list[str], lines: list[str]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for d in await _connected(device_ids):
        replies = []
        for line in lines:
            reply = await d.protocol.send_command(line)
            replies.append({"command": line, "reply": str(reply)[:REPLY_MAX]})
            await asyncio.sleep(COMMAND_GAP_S)
        out[d.id] = replies
    return out


@durable.step
async def asserts(device_ids: list[str], start: dict[str, Any]) -> dict[str, Any]:
    """No reboot since ``start`` (staging.uptimes) and no firmware alert since the run began;
    the version each chip reports."""
    out: dict[str, Any] = {}
    alerts = alerts_since(device_ids, start["alert_t"])
    for i in device_ids:
        got = await staging._info(i)
        ms = int(got.get("millis", 0))
        want = start["ms"].get(i, 0) + (time.time() - start["t"]) * 1000 - staging.REBOOT_SLACK_MS
        out[i] = {
            "version": got.get("version"),
            "no_reboot": {"pass": ms >= want, "uptime_s": ms // 1000},
            "alerts": {"pass": not alerts.get(i), "lines": alerts.get(i, [])[:30]},
        }
    return out


# -- the job -----------------------------------------------------------------------------------


@contextlib.asynccontextmanager
async def _streaming(device_ids: list[str]) -> AsyncIterator[None]:
    """The chips streaming while a check plays a track (validation's recordings, the music
    check's stream). Device I/O outside a step, so held (a drain waits for it)."""
    devices: list[Any] = []
    try:
        with durable.hold("dev run streaming on"):
            await kill_orphan_audio()
            devices = await _connected(device_ids)
            await _start_streaming(devices)
        yield
    finally:
        with durable.hold("dev run streaming off"):
            await _stop_streaming(devices)
            await stop_audio()


async def _check(
    device_ids: list[str], check: dict[str, Any], track_dir: str, limits: dict[str, float]
) -> dict[str, Any]:
    kind = check["kind"]
    if kind == "info":
        return {"kind": kind, "devices": await info(device_ids)}
    if kind == "health":
        minutes = float(check.get("minutes") or limits["stable_minutes"])
        return {
            "kind": kind,
            "minutes": minutes,
            "devices": await staging.health(device_ids, {**limits, "stable_minutes": minutes}),
        }
    if kind == "commands":
        return {"kind": kind, "devices": await commands(device_ids, list(check["commands"]))}
    p = await plan(device_ids, track_dir, check.get("tracks") or None, None)
    if "error" in p:
        return {"kind": kind, "detail": p["error"]}
    if kind == "validate":
        results = []
        loop: dict[str, list[dict[str, Any]]] = {}
        async with _streaming(p["devices"]):
            for track in p["tracks"]:
                jobs.report(None, f"validate: {track['name']}")
                r = await validation_run(
                    p["devices"], track, 1, limits["validation_ms"], 0.0, False
                )
                results.append(r if r is not None else {"track": track["name"], "failed": True})
                after = await staging.loop_reads(p["devices"], f"after {track['name']}")
                for dev, w in after.items():
                    loop.setdefault(dev, []).append(w)
        played = sum(1 for r in results if not r.get("failed"))
        return {
            "kind": kind,
            "tracks": len(p["tracks"]),
            "played": played,
            "results": results,
            "loop": loop,
        }
    # pace: from the track's start (seek), not after a validation window.
    i = await staging.pace_track(p["tracks"], {**limits, "validation_ms": 0.0})
    if i is None:
        return {"kind": kind, "detail": "no track long enough for the music and pace checks"}
    track = p["tracks"][i]
    jobs.report(None, f"music and beat pace: {track['name']}")
    async with _streaming(p["devices"]):
        res = await staging.music_and_pace(p["devices"], track, limits)
    return {"kind": kind, "track": track["name"], "devices": res}


def verdict(report: dict[str, Any]) -> bool:
    """Every health check and the final asserts pass, and no block-mode strict rule fails;
    strict warnings and scores never gate."""
    health = [c for c in report["checks"] if c["kind"] == "health"]
    return (
        all(staging._passed(c) for c in health)
        and staging._passed(report.get("final"))
        and not strict_rules.failures(report.get("strict"))
    )


@durable.workflow("test.dev-run")
async def dev_run(req: dict[str, Any]) -> dict[str, Any]:
    """req: device_ids, checks (each {"kind", ...}), track_dir, optional limits."""
    device_ids, checks = list(req["device_ids"]), list(req["checks"])
    limits = {**staging.DEFAULTS, **(req.get("limits") or {})}
    report: dict[str, Any] = {"devices": device_ids, "checks": [], "gates": ["health", "final"]}
    async with reserve(device_ids):
        start = await staging.uptimes(device_ids, await run_start())
        async with lit(device_ids, needs_light=False, purpose="a dev run"):
            await restore_settings(device_ids)
            for n, check in enumerate(checks, 1):
                jobs.report(100 * (n - 1) // len(checks), f"{check['kind']} ({n}/{len(checks)})")
                report["checks"].append(await _check(device_ids, check, req["track_dir"], limits))
            report["final"] = await asserts(device_ids, start)
            pace = next(
                (c for c in report["checks"] if c["kind"] == "pace" and "devices" in c), None
            )
            played = validated(report["checks"])
            if played:
                report["detection_latency"] = detection_latency.summarize(played, device_ids)
            report["strict"] = await strict(
                device_ids,
                loop_windows(report["checks"]),
                (pace or {}).get("devices") or {},
                report.get("detection_latency"),
            )
    report["passed"] = verdict(report)
    jobs.report(100, "passed" if report["passed"] else "FAILED")
    return report
