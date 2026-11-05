"""The staging hardware suite (#349, docs/RELEASE_PROCESS.md "Staging"): one durable job that
puts a staging build on the test chips and tests it on real hardware.

1. Deploy: the staging build of the commit, through the fleet deploy pipeline
   (deploy_pipeline.run_deploy with ``staging_sha``: test devices only, the single flash path,
   settings restore + save, the post-deploy version assertion).
2. Health: ``stable_minutes`` of ``json info`` with no music, the bench gate's checks
   (tools/release_gate.py): fps floor, longest frame, no reboot, overrun growth.
3. In a test session (runtime defaults, streaming), with the box speaker:
   - music: the first track for ``music_seconds``: mean level and pulse-onset rate;
   - beat pace: the same track for ``pace_seconds`` more: beats counted against the tempo
     integrated over wall-clock time (release_gate.beat_pace);
   - validation: every edm/ track once, scored like /api/test/validate (drum metrics).
4. After: version unchanged, no reboot across the whole job (uptime against wall time since
   the deploy), the onset-path timing from ``json info`` when the firmware reports it, and no
   firmware [ASSERT] / [FALLBACK] line from the flash on (the server's alert record,
   device.alerts_since, which survives a restart).

The whole job holds the audio lock (workflows.reserve): no other test job plays or measures
the chips while it deploys and measures them. Fleet deploys refuse to start while it runs
(routes_firmware._deploy_active) and it refuses to start during one.

Pass = every check passes. The validation numbers are reported, not gated: the box hasn't
passed its acceptance gates (TEST_BOX_BUILD_PLAN.md), so its F1 isn't trusted as a floor yet.
The operator's visual check is not here: it happens at promotion to stable.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import time
from typing import Any

from ..device.device import alerts_since
from ..firmware import deploy_pipeline
from ..paths import data_dir
from ..work import durable, jobs
from .audio_player import play_audio, stop_audio
from .workflows import _connected, _fleet, plan, reserve, session, validation_run

log = logging.getLogger(__name__)

# The bench gate's thresholds (tools/release_gate.py defaults, docs/RELEASE_PROCESS.md).
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
    first = {i: await _info(i) for i in device_ids}
    seen: dict[str, dict[str, Any]] = {
        i: {"fps": [], "frame_ms": [], "rebooted": False, "last_ms": int(first[i].get("millis", 0))}
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
                s["frame_ms"].append(int(cur["maxFrameMs"]))
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
        await asyncio.sleep(2.0)  # ffplay start-up
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


@durable.step
async def write_report(sha: str, report: dict[str, Any]) -> str:
    """The whole report as a file (bulk data is a file, not a step result)."""
    d = data_dir() / "staging-reports"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{sha}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    return str(path)


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
        report["path"] = await write_report(sha, report)
        return report
    version = deploy["version"]
    report["version"] = version
    start = await uptimes(device_ids, float(deploy["steps"]["store"]["flash_t"]))

    jobs.report(10, f"health: {limits['stable_minutes']:.0f} min of json info")
    report["health"] = await health(device_ids, limits)

    p = await plan(device_ids, req["track_dir"], req.get("track_names"), None)
    if "error" in p:
        report["validation"] = {"pass": False, "detail": p["error"]}
    else:
        async with session(p["devices"], reserved=True):
            jobs.report(30, "music and beat pace")
            report["music"] = await music_and_pace(p["devices"], p["tracks"][0], limits)
            results = []
            for n, track in enumerate(p["tracks"], 1):
                jobs.report(30 + 65 * n // len(p["tracks"]), f"validation: {track['name']}")
                r = await validation_run(
                    p["devices"], track, 1, limits["validation_ms"], 0.0, False
                )
                results.append(r if r is not None else {"track": track["name"], "failed": True})
        played = [r for r in results if not r.get("failed")]
        report["validation"] = {
            "pass": len(played) == len(p["tracks"]),
            "tracks": len(p["tracks"]),
            "played": len(played),
            "results": results,
        }

    report["final"] = await final_state(device_ids, version, start)
    report["passed"] = _passed({k: v for k, v in report.items() if k != "limits"})
    report["path"] = await write_report(sha, report)
    jobs.report(100, "passed" if report["passed"] else "FAILED")
    return report
