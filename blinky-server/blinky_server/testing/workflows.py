"""The test jobs as durable workflows (#325): validation, parameter sweep, threshold tuning and
NN capture, each surviving a restart at any moment.

Each job's units are steps (``work/durable.py`` has the rules): one validation run of one track,
one sweep run of one batch of values on one track, one threshold value on one track, one NN
capture. A restart, a deploy's included, lets the step in flight finish and the job carries on
from the next one when the server is back. Per-run setup (the audio lock, each device's runtime
defaults and test commands, streaming) is ``session``, in the workflow body, so it runs again
after a restart.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import re
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

from dbos import DBOS

from ..paths import data_dir
from ..work import durable, jobs
from .audio_lock import acquire_audio_lock, release_audio_lock
from .audio_player import kill_orphan_audio, stop_audio
from .nn_capture import capture_nn_stream
from .scoring import format_score_summary, score_device_run
from .test_runner import (
    INTER_RUN_GAP_S,
    _aggregate_sweep_results,
    _apply_settle_filter,
    _configure_device,
    _extract_metric,
    _record_and_play,
    _serialize_raw_capture,
    _start_streaming,
    _stop_streaming,
)
from .track_discovery import (
    discover_tracks,
    load_ground_truth,
    load_track_manifest,
    segment_ground_truth,
)

log = logging.getLogger(__name__)

TEST_JOBS = jobs.Family(
    prefix="test.",
    kinds=("validate", "param-sweep", "tune-threshold", "capture-nn"),
    result_steps=("validation_run", "sweep_run", "tune_run", "nn_capture"),
)

# One audio job at a time: every device hears the same room. The file lock (audio_lock.py)
# keeps other processes out; this keeps two of this server's jobs from interleaving, which a
# DBOS queue can't guarantee across a recovery. Taken in the workflow body, so a recovered job
# takes it again.
_AUDIO = asyncio.Lock()


def _fleet() -> Any:
    from ..api.deps import get_fleet

    return get_fleet()


# How long after the server starts a job waits for its devices to reconnect. Recovered jobs
# run as soon as the store launches, before discovery has connected anything; without the
# wait they would play to no device and record empty scores for good.
DEVICE_WAIT_S = 120.0


async def _connected(device_ids: list[str]) -> list[Any]:
    """The planned devices that are connected. Within DEVICE_WAIT_S of the server starting,
    waits for all of them first. With none connected after that, fails loudly; with some, goes
    on with those and logs the rest, as a run always has."""
    fleet = _fleet()
    from ..device.device import DeviceState

    while True:
        devices = [
            d
            for i in device_ids
            if (d := fleet.get_device(i)) is not None and d.state == DeviceState.CONNECTED
        ]
        if len(devices) == len(device_ids) or durable.since_launch() >= DEVICE_WAIT_S:
            break
        await asyncio.sleep(1)
    missing = sorted(set(device_ids) - {d.id for d in devices})
    if not devices:
        raise RuntimeError(f"none of the job's devices is connected: {missing}")
    if missing:
        log.warning("Devices not connected, skipped this run: %s", [m[:12] for m in missing])
    return devices


@contextlib.asynccontextmanager
async def session(
    device_ids: list[str],
    commands: list[str] | None = None,
    per_device_commands: dict[str, list[str]] | None = None,
) -> AsyncIterator[None]:
    """The audio lock, then each device at runtime defaults plus the test commands, streaming.
    Runs on every execution of the workflow, so a resumed job starts from known device state."""
    jobs.report(None, "waiting for the audio lock")
    async with _AUDIO:
        # A job that waited here for another to finish is past every boundary: if the server
        # started draining meanwhile, it stops now, before touching a device (#325 review).
        await durable.boundary()
        if not acquire_audio_lock([d[:12] for d in device_ids]):
            raise RuntimeError("Audio lock held by another process")
        # Setup and teardown are device I/O outside any step: held, so a drain that starts
        # during either waits for it to finish instead of the fleet stopping under it, which
        # would fail the job for good (#325 review).
        devices: list[Any] = []
        try:
            with durable.hold("test session setup"):
                await kill_orphan_audio()
                devices = await _connected(device_ids)
                for device in devices:
                    # One reset, then the shared commands, then the device's own: configuring
                    # twice would reset the shared ones away (#327).
                    own = (per_device_commands or {}).get(device.id) or []
                    await _configure_device(device, [*(commands or []), *own])
                await _start_streaming(devices)
            yield
        finally:
            with durable.hold("test session cleanup"):
                await _stop_streaming(devices)
                await stop_audio()
                release_audio_lock()


@durable.step
async def plan(
    device_ids: list[str],
    track_dir: str,
    track_names: list[str] | None,
    seek_sec: float | None,
) -> dict[str, Any]:
    """The devices connected now and the tracks to play, fixed for the whole job."""
    try:
        devices = [d.id for d in await _connected(device_ids)]
    except RuntimeError:
        return {"error": "No connected devices found"}
    tracks: list[dict[str, Any]] = list(discover_tracks(track_dir))
    if track_names:
        tracks = [t for t in tracks if t["name"] in track_names]
    if not tracks:
        return {"error": f"No tracks found in {track_dir}"}
    manifest = load_track_manifest(track_dir)
    for t in tracks:
        t["seek"] = (
            seek_sec if seek_sec is not None else manifest.get(t["name"], {}).get("seekOffset", 0)
        )
    return {"devices": devices, "tracks": tracks}


def _ground_truth(track: dict[str, Any], duration_ms: float) -> Any:
    return segment_ground_truth(
        load_ground_truth(
            track["ground_truth"], track.get("human_edits"), track.get("drum_labels")
        ),
        track["seek"],
        duration_ms,
    )


def _raw_path(track: str, run: int, device: str) -> Path:
    """Where a run's raw capture goes (rule 4: bulk data is a file, not a step result)."""
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{track}-{run}-{device}")
    d = data_dir() / "test-raw" / str(DBOS.workflow_id)
    d.mkdir(parents=True, exist_ok=True)
    return d / f"{safe}.json"


async def _play(device_ids: list[str], track: dict[str, Any], duration_ms: float) -> Any:
    devices = await _connected(device_ids)
    playback, recordings = await _record_and_play(
        devices, track["audio_file"], duration_ms=duration_ms, seek_sec=track["seek"]
    )
    return devices, playback, recordings


# -- validation ------------------------------------------------------------------------------


@durable.step
async def validation_run(
    device_ids: list[str],
    track: dict[str, Any],
    run: int,
    duration_ms: float,
    settle_ms: float,
    persist_raw: bool,
) -> dict[str, Any] | None:
    """One run of one track on every device. None when playback failed (the run is skipped)."""
    if run > 1:
        await asyncio.sleep(INTER_RUN_GAP_S)
    gt = _ground_truth(track, duration_ms)
    devices, playback, recordings = await _play(device_ids, track, duration_ms)
    if not playback.success:
        log.warning("Playback failed for %s: %s", track["name"], playback.error)
        return None
    scores: dict[str, Any] = {}
    raw: dict[str, str] = {}
    for device in devices:
        data = recordings.get(device.id)
        if not data:
            continue
        if settle_ms > 0:
            data = _apply_settle_filter(data, settle_ms)
        scores[device.id[:12]] = format_score_summary(
            score_device_run(data, playback.audio_start_time_ms, gt)
        )
        if persist_raw:
            path = _raw_path(track["name"], run, device.id[:12])
            path.write_text(json.dumps(_serialize_raw_capture(data, playback.audio_start_time_ms)))
            raw[device.id[:12]] = str(path)
    result: dict[str, Any] = {"track": track["name"], "run": run, "scores": scores}
    if persist_raw:
        result["raw_capture_path"] = raw
    return result


@durable.workflow("test.validate")
async def validate(req: dict[str, Any]) -> dict[str, Any]:
    p = await plan(req["device_ids"], req["track_dir"], req.get("track_names"), req.get("seek_sec"))
    if "error" in p:
        return {"status": "error", "message": p["error"]}
    tracks, num_runs = p["tracks"], req["num_runs"]
    results = []
    async with session(p["devices"], req.get("commands"), req.get("per_device_commands")):
        total, n = len(tracks) * num_runs, 0
        for track in tracks:
            for run in range(1, num_runs + 1):
                n += 1
                jobs.report(100 * n // total, f"{track['name']} run {run}/{num_runs}")
                r = await validation_run(
                    p["devices"],
                    track,
                    run,
                    req["duration_ms"],
                    req["settle_ms"],
                    req["persist_raw"],
                )
                if r is not None:
                    results.append(r)
    return {"status": "ok", "tracks": len(tracks), "runs": num_runs, "results": results}


# -- parameter sweep -------------------------------------------------------------------------


@durable.step
async def sweep_run(
    assignments: list[tuple[str, float]],
    param_name: str,
    track: dict[str, Any],
    run: int,
    duration_ms: float,
    settle_ms: float,
) -> list[dict[str, Any]]:
    """One run of one track with each device set to its value of this batch."""
    if run > 1:
        await asyncio.sleep(INTER_RUN_GAP_S)
    connected = {d.id: d for d in await _connected([d for d, _ in assignments])}
    for device_id, value in assignments:
        if device_id in connected:
            await connected[device_id].protocol.send_command(f"set {param_name} {value}")
            await asyncio.sleep(0.1)
    gt = _ground_truth(track, duration_ms)
    devices, playback, recordings = await _play([d for d, _ in assignments], track, duration_ms)
    if not playback.success:
        return []
    by_id = {d.id: d for d in devices}
    entries = []
    for device_id, value in assignments:
        data = recordings.get(device_id) if device_id in by_id else None
        if not data:
            continue
        data = _apply_settle_filter(data, settle_ms)
        entries.append(
            {
                "track": track["name"],
                "run": run,
                "device": device_id[:12],
                "value": value,
                "score": format_score_summary(
                    score_device_run(data, playback.audio_start_time_ms, gt)
                ),
            }
        )
    return entries


@durable.workflow("test.param-sweep")
async def param_sweep(req: dict[str, Any]) -> dict[str, Any]:
    p = await plan(req["device_ids"], req["track_dir"], req.get("track_names"), None)
    if "error" in p:
        return {"status": "error", "message": p["error"]}
    devices, tracks, values = p["devices"], p["tracks"], req["values"]
    num_runs, name = req["num_runs"], req["param_name"]
    # One value per device per pass: 3 devices and 9 values is 3 audio passes.
    batches = [values[i : i + len(devices)] for i in range(0, len(values), len(devices))]
    per_value: dict[float, list[dict[str, Any]]] = {v: [] for v in values}
    async with session(devices, req.get("commands")):
        total, n = len(batches) * len(tracks) * num_runs, 0
        for batch in batches:
            assignments = list(zip(devices, batch, strict=False))
            shown = "/".join(str(v) for v in batch)
            for track in tracks:
                for run in range(1, num_runs + 1):
                    n += 1
                    jobs.report(
                        100 * n // total, f"{name}={shown}, {track['name']} run {run}/{num_runs}"
                    )
                    for e in await sweep_run(
                        assignments, name, track, run, req["duration_ms"], req["settle_ms"]
                    ):
                        per_value[e["value"]].append(e)
    return {
        "status": "ok",
        "param": name,
        "values": values,
        "tracks": len(tracks),
        "aggregated": _aggregate_sweep_results(per_value),
        "raw": per_value,
    }


# -- threshold tuning ------------------------------------------------------------------------


@durable.step
async def tune_run(
    device_id: str,
    param_name: str,
    value: float,
    track: dict[str, Any],
    duration_ms: float,
    settle_ms: float,
    target_metric: str,
    gap: bool,
) -> float | None:
    """One value on one track: the target metric, or None when there is none to average."""
    if gap:
        await asyncio.sleep(INTER_RUN_GAP_S)
    device = (await _connected([device_id]))[0]
    await device.protocol.send_command(f"set {param_name} {value}")
    await asyncio.sleep(0.2)
    gt = _ground_truth(track, duration_ms)
    _, playback, recordings = await _play([device_id], track, duration_ms)
    data = recordings.get(device_id) if playback.success else None
    if not data:
        return None
    data = _apply_settle_filter(data, settle_ms)
    summary = format_score_summary(score_device_run(data, playback.audio_start_time_ms, gt))
    val = _extract_metric(summary, target_metric)
    if val is None:
        # A drum metric on a track with no drum labels, or a run with no lag estimate: leave
        # the track out rather than average it in as 0.
        log.warning(
            "%s: no %s for %s=%s, track left out", track["name"], target_metric, param_name, value
        )
    return val


@durable.workflow("test.tune-threshold")
async def tune_threshold(req: dict[str, Any]) -> dict[str, Any]:
    """Coarse sweep, then refine around the best coarse value."""
    _extract_metric({}, req["target_metric"])  # an unknown metric fails now, not after a sweep
    p = await plan([req["device_id"]], req["track_dir"], req.get("track_names"), None)
    if "error" in p:
        return {
            "status": "error",
            "message": "Device not connected"
            if p["error"].startswith("No connected")
            else p["error"],
        }
    device, tracks, name = p["devices"][0], p["tracks"], req["param_name"]
    low, high, max_steps, metric = req["low"], req["high"], req["max_steps"], req["target_metric"]
    coarse_n = max(min(6, max_steps - 2), 3)
    coarse = [round(low + i * (high - low) / (coarse_n - 1), 4) for i in range(coarse_n)]
    refine_n = max(max_steps - coarse_n, 3)
    total = coarse_n + refine_n
    history: list[dict[str, Any]] = []
    best_value, best_score = coarse[0], -1.0

    async def evaluate(value: float, phase: str) -> float:
        jobs.report(
            100 * (len(history) + 1) // total, f"{phase} {len(history) + 1}/{total}: {name}={value}"
        )
        scores = []
        for i, track in enumerate(tracks):
            s = await tune_run(
                device,
                name,
                value,
                track,
                req["duration_ms"],
                req["settle_ms"],
                metric,
                gap=bool(history) and i == 0,
            )
            if s is not None:
                scores.append(s)
        if not scores:
            log.warning("All tracks failed for %s=%s — returning NaN score", name, value)
        avg = sum(scores) / len(scores) if scores else float("nan")
        history.append(
            {
                "step": len(history) + 1,
                "phase": phase,
                "value": value,
                "metric": metric,
                "score": round(avg, 4),
            }
        )
        return avg

    async with session([device], req.get("commands")):
        coarse_scores = []
        for value in coarse:
            avg = await evaluate(value, "coarse")
            coarse_scores.append(avg)
            if avg > best_score:
                best_value, best_score = value, avg
        best_i = max(range(len(coarse)), key=lambda i: coarse_scores[i])
        lo, hi = coarse[max(best_i - 1, 0)], coarse[min(best_i + 1, len(coarse) - 1)]
        refine = [round(lo + i * (hi - lo) / (refine_n + 1), 4) for i in range(1, refine_n + 1)]
        for value in [v for v in refine if v not in set(coarse)][:refine_n]:
            avg = await evaluate(value, "refine")
            if avg > best_score:
                best_value, best_score = value, avg
    return {
        "status": "ok",
        "param": name,
        "optimal_value": best_value,
        "optimal_score": round(best_score, 4),
        "metric": metric,
        "steps": len(history),
        "history": history,
    }


# -- NN capture ------------------------------------------------------------------------------


@durable.step
async def nn_capture(device_id: str, duration_ms: float, output: str) -> dict[str, Any]:
    device = (await _connected([device_id]))[0]
    r = await capture_nn_stream(device, duration_ms, output)
    return {
        "status": "ok",
        "frames": r.frames,
        "duration_sec": r.duration_sec,
        "frame_rate": r.frame_rate,
        "output_path": r.output_path,
        "nn_active": r.nn_active,
        "onset_stats": r.onset_stats,
        "level_stats": r.level_stats,
    }


@durable.workflow("test.capture-nn")
async def capture_nn(device_id: str, duration_ms: float, output: str) -> dict[str, Any]:
    jobs.report(None, f"capturing {duration_ms / 1000:.0f} s")
    return await nn_capture(device_id, duration_ms, output)


def inline_raw(job: dict[str, Any]) -> dict[str, Any]:
    """A validation job with its runs' raw captures read back from their files, as the API
    returned them before they moved out of the step results (gate-(b) tooling reads them)."""
    res = job.get("result")
    if not isinstance(res, dict):
        return job
    for entry in res.get("results", []) + res.get("partial_results", []):
        paths = entry.pop("raw_capture_path", None) if isinstance(entry, dict) else None
        if paths:
            entry["raw_capture"] = {d: json.loads(Path(p).read_text()) for d, p in paths.items()}
    return job
