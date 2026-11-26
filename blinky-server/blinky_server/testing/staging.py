"""The staging quality suite (#349, docs/RELEASE_PROCESS.md "Staging"; docs/TEST_BOX.md): one
durable job that puts a staging build on the test chips and judges it on real hardware. Since
2026-11-25 it is non-blocking: the blocking yes/no hardware checks are
testing/hardware_checks.py, and the staging runner runs this suite only after they pass, to
score the algorithm (the report comment and the score record; it never posts a failing status).

1. Deploy: the staging build of the commit, through the fleet deploy pipeline
   (deploy_pipeline.run_deploy with ``staging_sha``: test devices only, the single flash path,
   settings restore + save, the post-deploy version assertion).
2. Health: ``stable_minutes`` of ``json info`` with no music, the bench gate's checks
   (blinky-firmware's tools/release_gate.py): fps floor, longest frame, no reboot, overrun growth.
3. In a test session (runtime defaults, streaming), with the box speaker, every track is
   played once:
   - validation: every edm/ track, scored like /api/test/validate (drum metrics);
   - music and beat pace: the first track long enough to hold them goes on playing after its
     validation window: ``music_seconds`` of mean level and pulse-onset rate, then
     ``pace_seconds`` of beats counted against the tempo integrated over wall-clock time
     (release_gate.beat_pace).
4. After: version unchanged, no reboot across the whole job (uptime against wall time since
   the deploy), the onset-path timing from ``json info`` when the firmware reports it, and no
   firmware [ASSERT] / [FALLBACK] line from the flash on (the server's alert record,
   device.alerts_since, which survives a restart).

The chips' LEDs are lit from the deploy's end to the job's end, however it ends: the box's
chips idle dark (testing/leds.py). Nothing here needs the light.

The whole job holds the audio lock (workflows.reserve): no other test job plays or measures
the chips while it deploys and measures them. Fleet deploys refuse to start while it runs
(routes_firmware._deploy_active) and it refuses to start during one.

Pass = every check passes. The validation numbers are reported, not gated: the box's numbers
aren't trusted as a floor yet (docs/TEST_BOX.md, "Trusting the box's numbers").
The operator's visual check is not here: it happens at promotion to stable.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import subprocess
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .. import telemetry
from ..device.device import alerts_since
from ..firmware import deploy_pipeline
from ..paths import data_dir
from ..work import durable, jobs
from .audio_player import play_audio, stop_audio
from .workflows import (
    _connected,
    _fleet,
    lit,
    plan,
    reserve,
    session,
    track_seconds,
    validation_run,
)

log = logging.getLogger(__name__)

# The bench gate's thresholds (blinky-firmware's tools/release_gate.py defaults and
# docs/RELEASE_PROCESS.md).
DEFAULTS: dict[str, float] = {
    "stable_minutes": 5.0,
    "min_fps": 60.0,
    "max_frame_ms": 100.0,
    "max_overrun_growth": 5.0,
    "music_seconds": 15.0,
    "min_level": 0.05,
    "min_onsets_per_s": 1.0,
    "pace_seconds": 60.0,
    "max_pace_error": 0.03,
    "validation_ms": 35000.0,
}
HEALTH_POLL_S = 4.0  # one json info per < 5 s: every loop-metrics window is read
# The firmware publishes the last *closed* 5 s loop-metrics window. The deploy step just
# before health ends with restore_runtime_settings + save, and a flash page erase stalls the
# loop ~85 ms; that can sit in the window open when health starts (the first run on the box's
# Sense Plus: 113 ms in the first window, 30-35 ms in the other 72). Health waits out two
# windows before its baseline, so it measures the steady state it is meant to.
HEALTH_SETTLE_S = 2 * 5.0
# json info fields with onset-path timing, when the firmware reports them (#178).
TIMING_FIELDS = ("onsetUs", "onsetMaxUs")


# Uptime may trail wall time by this much without counting as a reboot (clock rates, polling).
REBOOT_SLACK_MS = 15_000


async def _info(device_id: str) -> dict[str, Any]:
    # A step resumed after a restart can run before discovery reconnects the device: wait.
    (d,) = await _connected([device_id])
    return await deploy_pipeline._json_info(d)


def beat_pace(samples: list[tuple[float, float, int]]) -> dict[str, Any] | None:
    """tools/release_gate.beat_pace, the same arithmetic: beats counted (uint16, wraps) vs the
    tempo integrated over wall-clock time, over [(wall_s, bpm, beatCount)]."""
    if len(samples) < 2:
        return None
    counted, expected = 0, 0.0
    for (t0, b0, c0), (t1, b1, c1) in itertools.pairwise(samples):
        counted += (c1 - c0) % 65536
        expected += (b0 + b1) / 2 / 60 * (t1 - t0)
    if expected <= 0:
        return None
    return {
        "counted": counted,
        "expected": round(expected, 1),
        "error": round(abs(counted - expected) / expected, 3),
        "seconds": round(samples[-1][0] - samples[0][0], 1),
    }


# -- steps -------------------------------------------------------------------------------------


@durable.step
async def health(device_ids: list[str], limits: dict[str, float]) -> dict[str, Any]:
    """``stable_minutes`` of json info per device, no music: fps, longest frame, reboots,
    overrun growth. Safe to repeat: it only reads."""
    await asyncio.sleep(HEALTH_SETTLE_S)
    first = {i: await _info(i) for i in device_ids}
    seen: dict[str, dict[str, Any]] = {
        i: {
            "fps": [],
            "frame_ms": [],
            "over": [],
            "rebooted": False,
            "last_ms": int(first[i].get("millis", 0)),
        }
        for i in device_ids
    }
    last = dict(first)
    end = time.monotonic() + limits["stable_minutes"] * 60
    while time.monotonic() < end:
        await asyncio.sleep(HEALTH_POLL_S)
        for i in device_ids:
            cur = await _info(i)
            s = seen[i]
            ms = int(cur.get("millis", 0))
            if ms <= s["last_ms"]:
                s["rebooted"] = True
            s["last_ms"] = ms
            s["fps"].append(float(cur.get("fps") or 0.0))
            if cur.get("maxFrameMs") is not None:
                frame = int(cur["maxFrameMs"])
                s["frame_ms"].append(frame)
                if frame > limits["max_frame_ms"]:
                    # When: the device's uptime and wall time, so a long frame can be lined
                    # up with what the server and box were doing then (the first full run
                    # had one 1118 ms window out of 73, and only its size was kept).
                    s["over"].append(
                        {"uptime_s": ms // 1000, "at": round(time.time(), 1), "max_ms": frame}
                    )
            last[i] = cur
    out: dict[str, Any] = {}
    for i in device_ids:
        s = seen[i]
        growth = int(last[i].get("audioOverruns") or 0) - int(first[i].get("audioOverruns") or 0)
        fps_min = min(s["fps"]) if s["fps"] else 0.0
        # Missing maxFrameMs is an instrumentation regression, not a pass (release_gate).
        frame_max = max(s["frame_ms"]) if s["frame_ms"] else None
        out[i] = {
            "fps": {"pass": fps_min >= limits["min_fps"], "min": fps_min},
            "frametime": {
                "pass": frame_max is not None and frame_max <= limits["max_frame_ms"],
                "max_ms": frame_max,
                "windows": len(s["frame_ms"]),
                "over_limit": s["over"],
                # Every 5 s window's longest frame, in order (small: about 75 numbers).
                "series_ms": s["frame_ms"],
            },
            "stability": {
                "pass": not s["rebooted"] and growth <= limits["max_overrun_growth"],
                "rebooted": s["rebooted"],
                "overrun_growth": growth,
            },
        }
    return out


@durable.step
async def music_and_pace(
    device_ids: list[str], track: dict[str, Any], limits: dict[str, float]
) -> dict[str, Any]:
    """One track on the box speaker. First ``music_seconds`` of the stream: level and pulse
    onsets (release_gate.music_check); then ``pace_seconds`` of json rhythm (pace_check)."""
    devices = await _connected(device_ids)
    total_s = limits["music_seconds"] + limits["pace_seconds"] + 5
    player = asyncio.create_task(
        play_audio(track["audio_file"], duration_ms=total_s * 1000, seek_sec=track["seek"])
    )
    try:
        await asyncio.sleep(2.0)  # the player's start-up
        queues = {d.id: d.subscribe_stream() for d in devices}
        levels: dict[str, list[float]] = {d.id: [] for d in devices}
        onsets: dict[str, int] = dict.fromkeys(queues, 0)
        prev_p: dict[str, float] = dict.fromkeys(queues, 0.0)
        end = time.monotonic() + limits["music_seconds"]
        try:
            while time.monotonic() < end:
                for i, q in queues.items():
                    while not q.empty():
                        msg = q.get_nowait()
                        if not msg or msg.get("type") != "audio":
                            continue
                        data = msg["data"]
                        levels[i].append(float(data["a"]["l"]))
                        p = float((data.get("m") or {}).get("p", 0.0))
                        if p > 0.4 and prev_p[i] <= 0.4:  # rising edge of the pulse envelope
                            onsets[i] += 1
                        prev_p[i] = p
                await asyncio.sleep(0.05)
        finally:
            for d in devices:
                d.unsubscribe_stream(queues[d.id])
        samples: dict[str, list[tuple[float, float, int]]] = {d.id: [] for d in devices}
        end = time.monotonic() + limits["pace_seconds"]
        while time.monotonic() < end:
            for d in devices:
                raw = await d.protocol.send_command("json rhythm")
                for line in raw.splitlines():
                    line = line.strip()
                    if line.startswith("{") and '"beatCount"' in line:
                        r = json.loads(line)
                        samples[d.id].append(
                            (time.monotonic(), float(r["bpm"]), int(r["beatCount"]))
                        )
                        break
            await asyncio.sleep(1.0)
    finally:
        await stop_audio()
        player.cancel()
    out: dict[str, Any] = {}
    for d in devices:
        lv = levels[d.id]
        if lv:
            mean = sum(lv) / len(lv)
            rate = onsets[d.id] / limits["music_seconds"]
            music: dict[str, Any] = {
                "pass": mean >= limits["min_level"] and rate >= limits["min_onsets_per_s"],
                "level_mean": round(mean, 3),
                "onsets_per_s": round(rate, 2),
                "samples": len(lv),
            }
        else:
            music = {"pass": False, "detail": "no audio stream lines"}
        pace: dict[str, Any] | None = beat_pace(samples[d.id])
        if pace is None:
            pace = {"pass": False, "detail": f"no tempo tracked ({len(samples[d.id])} samples)"}
        else:
            # A beat either way is sampling, not drift: +-1 at each end of the window.
            pace["pass"] = abs(pace["counted"] - pace["expected"]) <= max(
                2.0, limits["max_pace_error"] * pace["expected"]
            )
        out[d.id] = {"music": music, "beatpace": pace}
    return out


def choose_pace_track(
    tracks: list[dict[str, Any]],
    limits: dict[str, float],
    seconds: Callable[[dict[str, Any]], float],
) -> int | None:
    """The first track with room, after its validation window, for the music and beat-pace
    checks (plus ffplay's start-up), so they continue that track instead of replaying one.
    None when no track is long enough."""
    need = limits["validation_ms"] / 1000 + limits["music_seconds"] + limits["pace_seconds"] + 5
    for i, t in enumerate(tracks):
        if seconds(t) - float(t["seek"]) >= need:
            return i
    return None


def no_pace_track(device_ids: list[str]) -> dict[str, Any]:
    """The music section when no track is long enough: both checks fail, with the reason."""
    return {
        d: {
            "music": {"pass": False, "detail": "no track long enough for the music check"},
            "beatpace": {"pass": False, "detail": "no track long enough"},
        }
        for d in device_ids
    }


@durable.step
async def pace_track(tracks: list[dict[str, Any]], limits: dict[str, float]) -> int | None:
    """choose_pace_track over the tracks' audio lengths. A step: it probes the audio files,
    in a worker thread so ffprobe never blocks the event loop the devices share."""
    return await asyncio.to_thread(choose_pace_track, tracks, limits, track_seconds)


@durable.step
async def final_state(device_ids: list[str], version: str, start: dict[str, Any]) -> Any:
    """Version unchanged; no reboot since ``start`` (uptimes()): a chip that kept running has
    gained the wall time that passed, a reset one restarted from zero; no firmware alert
    recorded since the flash; the onset-path timing if reported."""
    out: dict[str, Any] = {}
    alerts = alerts_since(device_ids, start["alert_t"])
    for i in device_ids:
        info = await _info(i)
        ms = int(info.get("millis", 0))
        want = start["ms"].get(i, 0) + (time.time() - start["t"]) * 1000 - REBOOT_SLACK_MS
        out[i] = {
            "version": {"pass": info.get("version") == version, "got": info.get("version")},
            "no_reboot": {"pass": ms >= want, "uptime_s": ms // 1000},
            "alerts": {"pass": not alerts.get(i), "lines": alerts.get(i, [])[:30]},
            "timing": {k: info[k] for k in TIMING_FIELDS if k in info} or None,
        }
    return out


@durable.step
async def uptimes(device_ids: list[str], alert_t: float) -> dict[str, Any]:
    """Each chip's uptime and the wall time it was read: final_state's reboot baseline. With
    ``alert_t``, the wall time just before the flash (deploy_pipeline._flash_time): alerts
    from then on count, the old firmware's never."""
    ms = {i: int((await _info(i)).get("millis", 0)) for i in device_ids}
    return {"t": time.time(), "ms": ms, "alert_t": alert_t}


def _git_head(path: str) -> str | None:
    try:
        r = subprocess.run(
            ["git", "-C", path, "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    out = r.stdout.strip()
    return out if r.returncode == 0 and len(out) == 40 else None


@durable.step
async def corpus_pin(track_dir: str) -> str | None:
    """The corpus commit the tracks come from (the blinky-corpus submodule), or None: scores
    compare only within one pin (docs/DEV_LOOP.md, "Stable candidates from scores")."""
    return await asyncio.to_thread(_git_head, track_dir)


# The drum score fields the score record keeps per track and device, with the scorer's lag
# evidence (blinky-corpus blinky_scoring/lag.py): a fallback or ambiguous lag is flagged.
SCORE_FIELDS = (
    "drumF",
    "kickR50",
    "snareR50",
    "onDrum",
    "loneHat",
    "lagMs",
    "lagSource",
    "lagAmbiguous",
    "lagNote",
)


def score_record(report: dict[str, Any]) -> dict[str, Any]:
    """One line of the score record: the commit, version, corpus pin and setup, and each
    track's drum scores per device with where its lag came from."""
    rows = []
    for r in (report.get("validation") or {}).get("results") or []:
        for dev, s in (r.get("scores") or {}).items():
            drums = s.get("drums") or {}
            rows.append(
                {"track": r.get("track"), "device": dev, **{k: drums.get(k) for k in SCORE_FIELDS}}
            )
    return {
        "sha": report.get("sha"),
        "version": report.get("version"),
        "corpus": report.get("corpus"),
        "devices": report.get("devices"),
        "limits": report.get("limits"),
        "at": round(time.time(), 1),
        "tracks": rows,
    }


@durable.step
async def write_report(sha: str, report: dict[str, Any]) -> str:
    """The whole report as a file (bulk data is a file, not a step result), and its line in
    the score record (staging-scores.jsonl, one line per commit: a repeated step rewrites it)."""
    d = data_dir() / "staging-reports"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{sha}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    if (report.get("validation") or {}).get("results"):
        rec = data_dir() / "staging-scores.jsonl"
        keep = [
            ln
            for ln in (rec.read_text().splitlines() if rec.exists() else [])
            if ln.strip() and json.loads(ln).get("sha") != sha
        ]
        tmp = rec.with_suffix(".tmp")
        tmp.write_text("".join(f"{ln}\n" for ln in [*keep, json.dumps(score_record(report))]))
        tmp.replace(rec)
    telemetry.record_staging(report)  # gauges, so a repeated step just sets them again
    return str(path)


@durable.step
async def store_report(path: str) -> str | None:
    """The report in the Spark artifact store (under inputs/, never reaped), so it outlives the
    box's disk and can be linked from anywhere: the key, or None when the Spark client isn't
    configured or the upload fails. Logged either way; archiving never fails a report."""
    from ..spark import service as spark_service
    from ..spark.client import SparkError

    svc = spark_service.get_service()
    if svc is None:
        log.warning("staging report not archived: %s", spark_service.disabled_reason())
        return None
    try:
        key = await asyncio.to_thread(svc.client.upload, Path(path))
    except (SparkError, OSError, ValueError) as e:  # ValueError: a reply that isn't JSON
        log.warning("staging report not archived: upload of %s failed: %s", path, e)
        return None
    _record_archived(key)
    log.info("staging report archived as %s", key)
    return key


def _archived_index() -> Path:
    return data_dir() / "staging-reports" / "archived-keys.txt"


def _record_archived(key: str) -> None:
    """Remember a key this box archived: the artifact link serves only these (routes_spark)."""
    if is_archived(key):
        return
    path = _archived_index()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as f:
        f.write(key + "\n")


def is_archived(key: str) -> bool:
    """True if the staging suite archived ``key`` (so a report links to it)."""
    try:
        return key in _archived_index().read_text().splitlines()
    except FileNotFoundError:
        return False


async def _finish(sha: str, report: dict[str, Any]) -> dict[str, Any]:
    """Write the report, then archive it (the archive key goes in the returned report)."""
    report["path"] = await write_report(sha, report)
    key = await store_report(report["path"])
    if key:
        report["artifact"] = key
    return report


# -- the job -----------------------------------------------------------------------------------


def _passed(node: Any) -> bool:
    """Every dict carrying "pass" is True."""
    if isinstance(node, dict):
        if node.get("pass") is False:
            return False
        return all(_passed(v) for v in node.values())
    if isinstance(node, list):
        return all(_passed(v) for v in node)
    return True


@durable.workflow("test.staging")
async def staging_suite(req: dict[str, Any]) -> dict[str, Any]:
    """req: sha, device_ids, track_dir, optional limits (DEFAULTS' keys)."""
    sha, device_ids = req["sha"], list(req["device_ids"])
    limits = {**DEFAULTS, **(req.get("limits") or {})}
    report: dict[str, Any] = {"sha": sha, "devices": device_ids, "limits": limits}

    async with reserve(device_ids):
        report["corpus"] = await corpus_pin(req["track_dir"])
        return await _suite(sha, device_ids, req, limits, report)


async def _suite(
    sha: str,
    device_ids: list[str],
    req: dict[str, Any],
    limits: dict[str, float],
    report: dict[str, Any],
) -> dict[str, Any]:
    from ..api.routes_firmware import _store_staging_hex
    from ..firmware.fleet_workflows import flash_devices

    jobs.report(0, "deploying the staging build")
    deploy = await deploy_pipeline.run_deploy(
        _fleet(),
        jobs.Progress(),
        release_tag=None,
        device_ids=device_ids,
        download_dir=data_dir() / "staging-download",
        store_firmware=_store_staging_hex,
        flash=flash_devices,
        staging_sha=sha,
    )
    report["deploy"] = {"pass": deploy.get("status") == "ok", **deploy}
    if deploy.get("status") != "ok":
        report["passed"] = False
        return await _finish(sha, report)
    version = deploy["version"]
    report["version"] = version
    start = await uptimes(device_ids, float(deploy["steps"]["store"]["flash_t"]))
    # The deploy rebooted the chips (dark: the box's config brightness is 0). Lit from here to
    # the end, failure or cancel included (testing/leds.py); nothing here needs the light.
    async with lit(device_ids, needs_light=False, purpose="the staging suite"):
        return await _measure(sha, device_ids, req, limits, report, version, start)


async def _measure(
    sha: str,
    device_ids: list[str],
    req: dict[str, Any],
    limits: dict[str, float],
    report: dict[str, Any],
    version: str,
    start: dict[str, Any],
) -> dict[str, Any]:
    """Health, validation with music and beat pace, and the final state, on the deployed build."""
    jobs.report(10, f"health: {limits['stable_minutes']:.0f} min of json info")
    report["health"] = await health(device_ids, limits)

    p = await plan(device_ids, req["track_dir"], req.get("track_names"), None)
    if "error" in p:
        report["validation"] = {"pass": False, "detail": p["error"]}
    else:
        pace_i = await pace_track(p["tracks"], limits)
        if pace_i is None:
            report["music"] = no_pace_track(p["devices"])
        async with session(p["devices"], reserved=True):
            results = []
            for n, track in enumerate(p["tracks"], 1):
                jobs.report(30 + 65 * n // len(p["tracks"]), f"validation: {track['name']}")
                r = await validation_run(
                    p["devices"], track, 1, limits["validation_ms"], 0.0, False
                )
                results.append(r if r is not None else {"track": track["name"], "failed": True})
                if n - 1 == pace_i:
                    # The same track goes on from where its validation window ended.
                    jobs.report(
                        30 + 65 * n // len(p["tracks"]), f"music and beat pace: {track['name']}"
                    )
                    rest = {**track, "seek": float(track["seek"]) + limits["validation_ms"] / 1000}
                    report["music"] = await music_and_pace(p["devices"], rest, limits)
        played = [r for r in results if not r.get("failed")]
        report["validation"] = {
            "pass": len(played) == len(p["tracks"]),
            "tracks": len(p["tracks"]),
            "played": len(played),
            "results": results,
        }

    report["final"] = await final_state(device_ids, version, start)
    report["passed"] = _passed({k: v for k, v in report.items() if k != "limits"})
    await _finish(sha, report)
    jobs.report(100, "passed" if report["passed"] else "FAILED")
    return report
