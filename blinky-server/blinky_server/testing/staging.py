"""The staging quality suite (#349, docs/RELEASE_PROCESS.md "Staging"; docs/TEST_BOX.md): one
durable job that puts a staging build on the test chips and judges it on real hardware. Since
2026-11-25 it is non-blocking: the blocking yes/no hardware checks are
testing/hardware_checks.py, and the staging runner runs this suite only after they pass, to
score the algorithm (the report comment and the score record; it never posts a failing status).

1. Deploy: the staging build of the commit, through the fleet deploy pipeline
   (deploy_pipeline.run_deploy with ``staging_sha``: test devices only, the single flash path,
   settings restore + save, the post-deploy version assertion).
2. Health: ``stable_minutes`` of ``json info`` with no music: no reboot, and every loop window
   read for the strict rules (testing/strict_rules.py: the bench gate's fps floor, longest
   frame and overrun growth, non-blocking warnings until the owner makes them block).
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
5. The chip's detection latency against the rig's calibration (testing/detection_latency.py:
   per track the music's check minus the measured lag, ``lagCheckMs - lagMs``, and the median):
   the report's ``detection_latency``.
6. The strict rules over every loop window the run read (health's, one after each track while
   the chips stream, the last), music, beat pace and the detection latency: the report's
   ``strict`` section, and a line in the readiness record (strict_rules.record_run).

The chips' LEDs are lit from the deploy's end to the job's end, however it ends: the box's
chips idle dark (testing/leds.py). Nothing here needs the light.

The whole job holds the audio lock (workflows.reserve): no other test job plays or measures
the chips while it deploys and measures them. Fleet deploys refuse to start while it runs
(routes_firmware._deploy_active) and it refuses to start during one.

Pass = every check passes, and no strict rule in block mode fails (a warning never fails it).
The validation numbers are reported, not gated: the box's numbers
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
from . import detection_latency, strict_rules
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

# How long each measurement runs (the bench gate's, blinky-firmware's tools/release_gate.py).
# Its pass/fail limits (fps, longest frame, overrun growth, music level and onsets, beat pace)
# are the strict rules, defined once in testing/strict_rules.py.
DEFAULTS: dict[str, float] = {
    "stable_minutes": 5.0,
    "music_seconds": 15.0,
    "pace_seconds": 60.0,
    "validation_ms": 35000.0,
}
# One json info per < 5 s: every loop-metrics window is read. Health starts right after the
# deploy's settings restore + save; since blinky-firmware#49 a save doesn't stall the loop, so
# every window counts.
HEALTH_POLL_S = 4.0
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


async def _window(device_id: str, where: str) -> dict[str, Any]:
    """One json info read as a loop-metrics window, stamped with the host time of the read."""
    info = await _info(device_id)
    return strict_rules.window(info, time.time(), where)


@durable.step
async def health(device_ids: list[str], limits: dict[str, float]) -> dict[str, Any]:
    """``stable_minutes`` of json info per device, no music: no reboot, and every loop window
    (fps, longest frame, audio overruns, with the host time of each read) for the strict rules.
    Safe to repeat: it only reads."""
    windows: dict[str, list[dict[str, Any]]] = {i: [await _window(i, "health")] for i in device_ids}
    rebooted = dict.fromkeys(device_ids, False)
    end = time.monotonic() + limits["stable_minutes"] * 60
    while time.monotonic() < end:
        await asyncio.sleep(HEALTH_POLL_S)
        for i in device_ids:
            w = await _window(i, "health")
            if int(w["ms"] or 0) <= int(windows[i][-1]["ms"] or 0):
                rebooted[i] = True
            windows[i].append(w)
    out: dict[str, Any] = {}
    for i in device_ids:
        ws = windows[i]
        growth = int(ws[-1]["overruns"] or 0) - int(ws[0]["overruns"] or 0)
        out[i] = {
            "stability": {
                "pass": not rebooted[i],
                "rebooted": rebooted[i],
                "overrun_growth": growth,
            },
            # Every read, in order (small: about 75), judged by the strict rules.
            "windows": ws,
        }
    return out


@durable.step
async def loop_reads(device_ids: list[str], where: str) -> dict[str, Any]:
    """One loop window per device now (after a track: a window of streaming and music)."""
    return {i: await _window(i, where) for i in device_ids}


@durable.step
async def strict(
    device_ids: list[str],
    windows: dict[str, list[dict[str, Any]]],
    music: dict[str, Any],
    latency: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """The strict rules over the run's windows, music, beat pace and detection latency
    (strict_rules.judge)."""
    return strict_rules.judge(windows, "quality", music, latency)


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
        # Measured here, judged by the strict rules (music_level, music_onsets, beat_pace).
        if lv:
            music: dict[str, Any] = {
                "level_mean": round(sum(lv) / len(lv), 3),
                "onsets_per_s": round(onsets[d.id] / limits["music_seconds"], 2),
                "samples": len(lv),
            }
        else:
            music = {"detail": "no audio stream lines"}
        pace: dict[str, Any] | None = beat_pace(samples[d.id])
        if pace is None:
            pace = {"detail": f"no tempo tracked ({len(samples[d.id])} samples)"}
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
    """The music section when no track is long enough: nothing measured (the strict rules show
    no data), with the reason."""
    return {
        d: {
            "music": {"detail": "no track long enough for the music check"},
            "beatpace": {"detail": "no track long enough"},
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
        loop = strict_rules.window(info, time.time(), "end")
        ms = int(info.get("millis", 0))
        want = start["ms"].get(i, 0) + (time.time() - start["t"]) * 1000 - REBOOT_SLACK_MS
        out[i] = {
            "version": {"pass": info.get("version") == version, "got": info.get("version")},
            "no_reboot": {"pass": ms >= want, "uptime_s": ms // 1000},
            "alerts": {"pass": not alerts.get(i), "lines": alerts.get(i, [])[:30]},
            "timing": {k: info[k] for k in TIMING_FIELDS if k in info} or None,
            "loop": loop,
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
    "lagCheckMs",  # minus lagMs: the chip's detection latency vs the calibration
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
        # Per device: the median offset and how many tracks it counted (detection_latency).
        "detection_latency": {
            dev: {"median_ms": d.get("median_ms"), "counted": d.get("counted")}
            for dev, d in (report.get("detection_latency") or {}).items()
        },
    }


@durable.step
async def write_report(sha: str, report: dict[str, Any]) -> str:
    """The whole report as a file (bulk data is a file, not a step result), and its line in
    the score record (staging-scores.jsonl, one line per commit: a repeated step rewrites it)."""
    d = data_dir() / "staging-reports"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{sha}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    if report.get("strict") and (report.get("deploy") or {}).get("pass"):
        # A staging head's run counts for the strict rules' readiness (strict_rules.readiness).
        strict_rules.record_run(
            report["strict"],
            source="quality",
            sha=sha,
            version=report.get("version"),
            build=report.get("build"),
        )
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
    """req: sha, device_ids, track_dir, optional limits (DEFAULTS' keys), optional build_of (the
    PR head whose tree this commit has: the same firmware build, for the strict rules'
    readiness)."""
    sha, device_ids = req["sha"], list(req["device_ids"])
    limits = {**DEFAULTS, **(req.get("limits") or {})}
    report: dict[str, Any] = {
        "sha": sha,
        "build": req.get("build_of") or sha,
        "devices": device_ids,
        "limits": limits,
    }

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
    loops: dict[str, list[dict[str, Any]]] = {
        i: list(h["windows"]) for i, h in report["health"].items()
    }

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
                # The window that just closed: the chips streaming to music (blinky-firmware#23).
                for i, w in (await loop_reads(p["devices"], f"after {track['name']}")).items():
                    loops.setdefault(i, []).append(w)
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
        report["detection_latency"] = detection_latency.summarize(played, p["devices"])

    report["final"] = await final_state(device_ids, version, start)
    for i, f in report["final"].items():
        loops.setdefault(i, []).append(f["loop"])
    report["strict"] = await strict(
        device_ids, loops, report.get("music") or {}, report.get("detection_latency")
    )
    report["passed"] = _passed(
        {k: v for k, v in report.items() if k not in ("limits", "strict")}
    ) and not strict_rules.failures(report["strict"])
    await _finish(sha, report)
    jobs.report(100, "passed" if report["passed"] else "FAILED")
    return report
