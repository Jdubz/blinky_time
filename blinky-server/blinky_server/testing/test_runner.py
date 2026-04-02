"""Test orchestration — play audio, record from devices, score results.

Two test types:
1. Validation: play tracks, score onset F1 + PLP metrics against ground truth
2. Parameter sweep: test multiple parameter values across devices in parallel
"""

from __future__ import annotations

import asyncio
import logging
from typing import TYPE_CHECKING, Any

from .audio_lock import acquire_audio_lock, release_audio_lock
from .audio_player import PlaybackResult, kill_orphan_audio, play_audio
from .job_manager import Job
from .scoring import format_score_summary, score_device_run
from .test_session import TestSession
from .track_discovery import discover_tracks, load_ground_truth, load_track_manifest
from .types import TestData

if TYPE_CHECKING:
    from ..device.device import Device
    from ..device.manager import FleetManager

log = logging.getLogger(__name__)

INTER_RUN_GAP_S = 5
SETTLE_MS = 12000  # ACF convergence time — skip early readings


async def _configure_device(device: Device, commands: list[str] | None = None) -> None:
    """Send setup commands to a device before testing."""
    if commands:
        for cmd in commands:
            await device.protocol.send_command(cmd)
            await asyncio.sleep(0.1)


async def _start_streaming(devices: list[Device]) -> None:
    """Start fast streaming on all devices."""
    for device in devices:
        await device.protocol.start_stream("fast")
        await asyncio.sleep(0.1)


async def _stop_streaming(devices: list[Device]) -> None:
    """Stop streaming on all devices. Best-effort, never raises."""
    for device in devices:
        try:  # noqa: SIM105
            await device.protocol.stop_stream()
        except Exception:
            pass


async def _record_and_play(
    devices: list[Device],
    audio_file: str,
    duration_ms: float | None = None,
    seek_sec: float | None = None,
) -> tuple[PlaybackResult, dict[str, TestData]]:
    """Start recording on all devices, play audio, stop recording.

    Returns (playback_result, {device_id: TestData}).
    """
    sessions: dict[str, TestSession] = {}
    for device in devices:
        session = device.start_test_session()
        session.start_recording()
        sessions[device.id] = session

    try:
        playback = await play_audio(audio_file, duration_ms=duration_ms, seek_sec=seek_sec)
    finally:
        # Always stop recording, even if playback fails
        results: dict[str, TestData] = {}
        for device in devices:
            if device.id in sessions:
                results[device.id] = sessions[device.id].stop_recording()
                device.stop_test_session()

    return playback, results


def _apply_settle_filter(test_data: TestData, settle_ms: float) -> TestData:
    """Filter out data from the settle period (ACF convergence).

    Removes transients and music states from the first settle_ms of recording.
    The CJS param_sweep_multidev.cjs does this by filtering readings where
    time < startTime + settleMs.
    """
    if settle_ms <= 0:
        return test_data
    cutoff = test_data.start_time + settle_ms
    return TestData(
        duration=test_data.duration,
        start_time=test_data.start_time,
        transients=[t for t in test_data.transients if t.timestamp_ms >= cutoff],
        music_states=[s for s in test_data.music_states if s.timestamp_ms >= cutoff],
    )


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


async def run_validation(
    fleet: FleetManager,
    device_ids: list[str],
    *,
    track_dir: str,
    track_names: list[str] | None = None,
    duration_ms: float = 35000,
    seek_sec: float | None = None,
    num_runs: int = 1,
    settle_ms: float = 0,
    commands: list[str] | None = None,
    per_device_commands: dict[str, list[str]] | None = None,
    job: Job | None = None,
) -> dict[str, Any]:
    """Run validation suite: play tracks, score each device.

    Args:
        fleet: FleetManager instance
        device_ids: Device IDs to test
        track_dir: Directory containing audio files + .beats.json
        track_names: Specific tracks to test (None = all discovered)
        duration_ms: Playback duration per track
        seek_sec: Seek position (None = use track manifest if available)
        num_runs: Number of runs per track
        settle_ms: Skip data from first N ms of recording (ACF convergence)
        commands: Setup commands sent to all devices
        per_device_commands: Per-device setup commands
        job: Job for progress updates (optional)

    Returns:
        Results dict with per-track, per-device, per-run scores.
    """
    devices = _resolve_devices(fleet, device_ids)
    if not devices:
        return {"status": "error", "message": "No connected devices found"}

    tracks = discover_tracks(track_dir)
    if track_names:
        tracks = [t for t in tracks if t["name"] in track_names]
    if not tracks:
        return {"status": "error", "message": f"No tracks found in {track_dir}"}

    manifest = load_track_manifest(track_dir)

    if not acquire_audio_lock([d.id[:12] for d in devices]):
        return {"status": "error", "message": "Audio lock held by another process"}

    try:
        await kill_orphan_audio()

        for device in devices:
            await _configure_device(device, commands)
            device_cmds = (per_device_commands or {}).get(device.id)
            if device_cmds:
                await _configure_device(device, device_cmds)

        await _start_streaming(devices)

        try:
            all_results: list[dict[str, Any]] = []
            total_steps = len(tracks) * num_runs
            step = 0

            for track in tracks:
                track_name = track["name"]
                gt = load_ground_truth(
                    track["ground_truth"],
                    track.get("onset_ground_truth"),
                )
                track_seek = seek_sec
                if track_seek is None:
                    track_manifest = manifest.get(track_name, {})
                    track_seek = track_manifest.get("seekOffset", 0)

                for run_idx in range(num_runs):
                    step += 1
                    if job:
                        job.progress = int(100 * step / total_steps)
                        job.progress_message = f"{track_name} run {run_idx + 1}/{num_runs}"

                    playback, recordings = await _record_and_play(
                        devices,
                        track["audio_file"],
                        duration_ms=duration_ms,
                        seek_sec=track_seek,
                    )

                    if not playback.success:
                        log.warning("Playback failed for %s: %s", track_name, playback.error)
                        continue

                    run_scores: dict[str, Any] = {}
                    for device in devices:
                        test_data = recordings.get(device.id)
                        if not test_data:
                            continue
                        if settle_ms > 0:
                            test_data = _apply_settle_filter(test_data, settle_ms)
                        score = score_device_run(test_data, playback.audio_start_time_ms, gt)
                        run_scores[device.id[:12]] = format_score_summary(score)

                    all_results.append(
                        {
                            "track": track_name,
                            "run": run_idx + 1,
                            "scores": run_scores,
                        }
                    )

                    if run_idx < num_runs - 1:
                        await asyncio.sleep(INTER_RUN_GAP_S)

            return {
                "status": "ok",
                "tracks": len(tracks),
                "runs": num_runs,
                "results": all_results,
            }
        finally:
            await _stop_streaming(devices)

    finally:
        release_audio_lock()


# ---------------------------------------------------------------------------
# Parameter sweep
# ---------------------------------------------------------------------------


async def run_param_sweep(
    fleet: FleetManager,
    device_ids: list[str],
    *,
    param_name: str,
    values: list[float],
    track_dir: str,
    track_names: list[str] | None = None,
    duration_ms: float = 35000,
    settle_ms: float = SETTLE_MS,
    num_runs: int = 1,
    commands: list[str] | None = None,
    job: Job | None = None,
) -> dict[str, Any]:
    """Multi-device parameter sweep.

    Batches parameter values across devices to minimize audio passes.
    With 3 devices and 9 values, only 3 audio passes are needed.
    Devices in the last batch may be fewer than total if values don't
    divide evenly (extras reuse their previous param value — harmless).

    Args:
        fleet: FleetManager instance
        device_ids: Device IDs to test (each gets different param values)
        param_name: Setting name to sweep
        values: Parameter values to test
        track_dir: Directory containing audio files
        track_names: Specific tracks (None = all)
        duration_ms: Playback duration per track
        settle_ms: Skip data from first N ms of recording (ACF convergence)
        num_runs: Runs per value per track
        commands: Setup commands for all devices
        job: Job for progress updates

    Returns:
        Results dict with per-value aggregate scores.
    """
    devices = _resolve_devices(fleet, device_ids)
    if not devices:
        return {"status": "error", "message": "No connected devices found"}

    tracks = discover_tracks(track_dir)
    if track_names:
        tracks = [t for t in tracks if t["name"] in track_names]
    if not tracks:
        return {"status": "error", "message": f"No tracks found in {track_dir}"}

    manifest = load_track_manifest(track_dir)

    if not acquire_audio_lock([d.id[:12] for d in devices]):
        return {"status": "error", "message": "Audio lock held by another process"}

    try:
        await kill_orphan_audio()

        for device in devices:
            await _configure_device(device, commands)

        await _start_streaming(devices)

        try:
            n_devices = len(devices)
            batches: list[list[float]] = []
            for i in range(0, len(values), n_devices):
                batches.append(values[i : i + n_devices])

            total_steps = len(batches) * len(tracks) * num_runs
            step = 0
            per_value_results: dict[float, list[dict[str, Any]]] = {v: [] for v in values}

            for batch in batches:
                # Assign one value per device; last batch may have fewer values
                assignments: list[tuple[Device, float]] = list(zip(devices, batch, strict=False))

                for device, value in assignments:
                    await device.protocol.send_command(f"set {param_name} {value}")
                    await asyncio.sleep(0.1)

                for track in tracks:
                    track_name = track["name"]
                    gt = load_ground_truth(
                        track["ground_truth"],
                        track.get("onset_ground_truth"),
                    )
                    track_manifest = manifest.get(track_name, {})
                    track_seek = track_manifest.get("seekOffset", 0)

                    for run_idx in range(num_runs):
                        step += 1
                        if job:
                            job.progress = int(100 * step / total_steps)
                            values_str = "/".join(str(v) for _, v in assignments)
                            job.progress_message = f"{param_name}={values_str}, {track_name} run {run_idx + 1}/{num_runs}"

                        playback, recordings = await _record_and_play(
                            [d for d, _ in assignments],
                            track["audio_file"],
                            duration_ms=duration_ms,
                            seek_sec=track_seek,
                        )

                        if not playback.success:
                            continue

                        for device, value in assignments:
                            test_data = recordings.get(device.id)
                            if not test_data:
                                continue
                            test_data = _apply_settle_filter(test_data, settle_ms)
                            score = score_device_run(test_data, playback.audio_start_time_ms, gt)
                            per_value_results[value].append(
                                {
                                    "track": track_name,
                                    "run": run_idx + 1,
                                    "device": device.id[:12],
                                    "score": format_score_summary(score),
                                }
                            )

                        if run_idx < num_runs - 1:
                            await asyncio.sleep(INTER_RUN_GAP_S)

            aggregated = _aggregate_sweep_results(per_value_results)
            return {
                "status": "ok",
                "param": param_name,
                "values": values,
                "tracks": len(tracks),
                "aggregated": aggregated,
                "raw": per_value_results,
            }
        finally:
            await _stop_streaming(devices)

    finally:
        release_audio_lock()


def _aggregate_sweep_results(
    per_value: dict[float, list[dict[str, Any]]],
) -> list[dict[str, Any]]:
    """Compute mean scores per parameter value."""
    results = []
    for value, runs in per_value.items():
        if not runs:
            results.append({"value": value, "n": 0})
            continue

        plp_at = [r["score"]["plp"]["atTransient"] for r in runs if "plp" in r.get("score", {})]
        plp_ac = [r["score"]["plp"]["autoCorr"] for r in runs if "plp" in r.get("score", {})]
        plp_pk = [r["score"]["plp"]["peakiness"] for r in runs if "plp" in r.get("score", {})]
        onset_f1 = [
            r["score"]["onsetTracking"]["f1"] for r in runs if "onsetTracking" in r.get("score", {})
        ]

        def _mean(xs: list[float]) -> float:
            return sum(xs) / len(xs) if xs else 0.0

        results.append(
            {
                "value": value,
                "n": len(runs),
                "plpAtTransient": round(_mean(plp_at), 3),
                "plpAutoCorr": round(_mean(plp_ac), 3),
                "plpPeakiness": round(_mean(plp_pk), 2),
                "onsetF1": round(_mean(onset_f1), 3),
            }
        )
    return results


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _resolve_devices(fleet: FleetManager, device_ids: list[str]) -> list[Device]:
    """Look up devices by ID, returning only connected ones."""
    from ..device.device import DeviceState

    devices = []
    for did in device_ids:
        device = fleet.get_device(did)
        if device and device.state == DeviceState.CONNECTED:
            devices.append(device)
        else:
            log.warning("Device %s not found or not connected, skipping", did[:12])
    return devices
