"""Test orchestration — play audio, record from devices, score results.

Two test types:
1. Validation: play tracks, score onset F1 + PLP metrics against ground truth
2. Parameter sweep: test multiple parameter values across devices in parallel
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import time
from typing import TYPE_CHECKING, Any

from .audio_player import PlaybackResult, play_audio
from .test_session import TestSession
from .types import TestData

if TYPE_CHECKING:
    from ..device.device import Device
    from ..device.manager import FleetManager

log = logging.getLogger(__name__)

INTER_RUN_GAP_S = 5
SETTLE_MS = 12000  # ACF convergence time — skip early readings


async def _sync_clock(device: Device) -> float | None:
    """Measure clock offset between server and firmware.

    Sends `json info` (which includes `millis` field) and measures
    round-trip time. Returns offset_ms = server_epoch_ms - firmware_millis,
    or None if sync fails.

    A None return means the test session will record without a clock
    offset, leaving firmware-timestamped events uncorrelated with server
    audio playback timing. Each failure path now logs at WARN with
    [FALLBACK] so the degradation is visible in normal validation logs;
    previous DEBUG-level logging hid systemic issues (e.g. firmware build
    that doesn't include the `millis` field at all) until per-track
    timing analysis surfaced the misalignment downstream.

    Caller stays responsible for deciding policy — typically one device
    failing clock-sync is tolerable, but all devices failing means the
    entire run's timing is unreliable and the caller should halt.
    """
    t_send = time.time() * 1000
    try:
        resp = await device.protocol.send_command("json info")
    # `TimeoutError` here covers `asyncio.TimeoutError` too: pyproject.toml
    # requires Python >= 3.11, where asyncio.TimeoutError is an alias for
    # builtins.TimeoutError. ruff UP041 enforces this modern idiom — listing
    # both would be redundant. If pyproject.toml ever drops to 3.10, this
    # catch becomes incorrect and asyncio.TimeoutError would slip through
    # to the `except Exception` arm below (still handled, but at ERROR
    # level instead of WARN).
    except (TimeoutError, ConnectionError) as e:
        log.warning("[FALLBACK] clock sync %s: send_command failed: %s", device.id[:12], e)
        return None
    except Exception as e:
        # Unexpected exception type from send_command (protocol-level
        # RuntimeError, AttributeError on a partially-init device, etc).
        # The narrow catch above intentionally only covers the two known-
        # recoverable failure modes; catching everything would conflate
        # "lost connection" (per-device, recoverable) with "server bug"
        # (systemic, should bubble). Log at ERROR with the type so
        # operators investigate, then return None so the caller's
        # all-devices-failed-aggregator can still escalate cleanly. If
        # every device hits this we'll see one ERROR per device + one
        # RuntimeError from _record_and_play — the right blast radius.
        log.error(
            "[FALLBACK] clock sync %s: UNEXPECTED %s from send_command: %s",
            device.id[:12],
            type(e).__name__,
            e,
        )
        return None
    t_recv = time.time() * 1000

    try:
        info = json.loads(resp)
    except json.JSONDecodeError as e:
        log.warning(
            "[FALLBACK] clock sync %s: malformed json info response (%d chars): %s",
            device.id[:12],
            len(resp),
            e,
        )
        return None

    fw_millis_raw = info.get("millis")
    if fw_millis_raw is None:
        log.warning(
            "[FALLBACK] clock sync %s: firmware response has no `millis` field "
            "(keys=%s) — firmware build may pre-date b117",
            device.id[:12],
            sorted(info.keys()),
        )
        return None
    try:
        fw_millis = float(fw_millis_raw)
    except (TypeError, ValueError) as e:
        # `millis` is supposed to be a number per the b117+ contract, but
        # if the firmware ever ships a string / nested dict / null-shaped
        # value here we'd crash uncaught past both prior try blocks. The
        # split-exception design of this function deserves a third guard.
        log.warning(
            "[FALLBACK] clock sync %s: `millis` field not numeric (raw=%r): %s",
            device.id[:12],
            fw_millis_raw,
            e,
        )
        return None

    # Estimate firmware time at midpoint of round-trip
    rtt = t_recv - t_send
    server_midpoint = t_send + rtt / 2
    offset = server_midpoint - fw_millis
    log.debug("Clock sync %s: offset=%.0fms rtt=%.0fms", device.id[:12], offset, rtt)
    return offset


async def _configure_device(device: Device, commands: list[str] | None = None) -> None:
    """Reset runtime settings, then apply any test-specific commands.

    Always resets runtime tunables first — stale settings from prior
    experiments persist in flash and silently corrupt test results.
    Note: this does NOT wipe device identity (matrix size, deviceId).
    For full reprovisioning, use deploy.sh --reprovision (sends
    `wipe_device_identity`).
    """
    # Send the new self-documenting name; firmware accepts the old 'defaults'
    # alias too with a deprecation warning. See #141.
    await device.protocol.send_command("restore_runtime_settings")
    await asyncio.sleep(0.1)
    if commands:
        for cmd in commands:
            await device.protocol.send_command(cmd)
            await asyncio.sleep(0.1)


async def _start_streaming(devices: list[Device]) -> None:
    """Start debug+fast streaming and enable transient channel.

    Each send_command pauses streaming, sends the command, waits for
    acknowledgment, then resumes — no arbitrary sleeps needed.

    Order: debug first (sets streamDebug_), then fast (sets streamFast_
    without clearing streamDebug_), then enable TRANSIENT events.
    """
    for device in devices:
        await device.protocol.start_stream("debug")
        await device.protocol.start_stream("fast")
        await device.protocol.send_command("debug transient on")


async def _stop_streaming(devices: list[Device]) -> None:
    """Stop streaming and disable transient debug channel. Best-effort."""
    for device in devices:
        try:
            await device.protocol.send_command("debug transient off")
            await device.protocol.stop_stream()
        except Exception as e:
            log.debug("stream-stop for %s failed (non-fatal): %s", device.id, e)


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
    sync_failures = 0
    # Sync all clocks BEFORE starting any recording. The previous order
    # ("start_recording in the same loop as sync") meant a final raise
    # in the all-failed guard left every session in the recording state
    # — buffers allocated, OS resources held, no path to clean shutdown.
    # Two-pass design: pass 1 checks the all-failed predicate cleanly;
    # pass 2 actually opens recordings only after we know the run is
    # viable.
    for device in devices:
        session = device.start_test_session()
        offset = await _sync_clock(device)
        if offset is not None:
            session.set_clock_offset(offset)
        else:
            sync_failures += 1
        sessions[device.id] = session

    # If every device failed clock sync, downstream timing is unreliable for
    # the entire test (firmware events can't be aligned with audio playback).
    # Halt loudly rather than recording a run that looks superficially normal
    # but produces wrong F1 numbers. Clean up the test sessions we just
    # opened so we don't leak per-device state across the raise.
    if devices and sync_failures == len(devices):
        for device in devices:
            if device.id in sessions:
                with contextlib.suppress(Exception):
                    device.stop_test_session()
        raise RuntimeError(
            f"Clock sync failed on ALL {len(devices)} devices — every "
            f"firmware-timestamped event will be uncorrelated with audio "
            f"playback timing. See [FALLBACK] log lines above for the per-"
            f"device failure mode (commonly: firmware build missing the "
            f"`millis` field, or all devices lost connection)."
        )

    # Run is viable — start recording on every session.
    for device in devices:
        if device.id in sessions:
            sessions[device.id].start_recording()

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
        signal_frames=[f for f in test_data.signal_frames if f.timestamp_ms >= cutoff],
    )


def _serialize_raw_capture(test_data: TestData, audio_start_time_ms: float) -> dict[str, Any]:
    """Serialize the raw signal_frames + transients for gate-(b) analysis.

    Timestamps are normalized to seconds from audio start so downstream
    tooling doesn't have to re-apply the audio_epoch_ms subtraction.
    `values` is a flat dict of signal_name → float for each frame.

    Only called when the caller opted into `persist_raw` — this can be
    ~0.5-2 MB per device per 35 s clip, so we don't serialize it by default.
    """
    frames_out = []
    for f in test_data.signal_frames:
        t_sec = (f.timestamp_ms - audio_start_time_ms) / 1000.0
        if t_sec < 0:
            continue
        frames_out.append(
            {
                "t": round(t_sec, 4),
                "nn": round(f.activation, 4),
                "v": {k: round(v, 4) for k, v in f.values.items()},
            }
        )
    transients_out: list[dict[str, Any]] = []
    for t in test_data.transients:
        t_sec = (t.timestamp_ms - audio_start_time_ms) / 1000.0
        if t_sec < 0:
            continue
        entry: dict[str, Any] = {"t": round(t_sec, 4), "strength": round(t.strength, 3)}
        # T1.4/T1.5 — only emit when firmware provided them (b145+).
        if t.gate_mask is not None:
            entry["gateMask"] = t.gate_mask
        if t.features:
            entry["features"] = {k: round(v, 4) for k, v in t.features.items()}
        transients_out.append(entry)
    return {
        "signal_frames": frames_out,
        "transients": transients_out,
    }


def _aggregate_sweep_results(
    per_value: dict[float, list[dict[str, Any]]],
) -> list[dict[str, Any]]:
    """Compute mean scores per parameter value."""

    def _drum(drums: list[dict[str, Any]], key: str) -> float | None:
        # None values (no lag estimate for that run) are skipped, not averaged in as 0.
        xs = [d[key] for d in drums if d.get(key) is not None]
        return round(sum(xs) / len(xs), 3) if xs else None

    results: list[dict[str, Any]] = []
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
        # Drum metrics, the primary score (#294).
        drums = [r["score"]["drums"] for r in runs if r.get("score", {}).get("drums")]

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
                "kickR50": _drum(drums, "kickR50"),
                "onDrum": _drum(drums, "onDrum"),
                "drumF": _drum(drums, "drumF"),
                "snareR50": _drum(drums, "snareR50"),
            }
        )
    return results


_METRIC_PATHS: dict[str, tuple[str, str]] = {
    "onsetF1": ("onsetTracking", "f1"),
    "onsetPrecision": ("onsetTracking", "precision"),
    "onsetRecall": ("onsetTracking", "recall"),
    "plpAtTransient": ("plp", "atTransient"),
    "plpAtTransientNorm": ("plp", "atTransientNorm"),
    "plpAutoCorr": ("plp", "autoCorr"),
    "plpPeakiness": ("plp", "peakiness"),
    # Drum metrics, the primary score (#294). snareR50 is provisional until #284.
    "kickR50": ("drums", "kickR50"),
    "kickR30": ("drums", "kickR30"),
    "onDrum": ("drums", "onDrum"),
    "drumF": ("drums", "drumF"),
    "snareR50": ("drums", "snareR50"),
}


def _extract_metric(summary: dict[str, Any], metric: str) -> float | None:
    """Extract a named metric from a score summary dict.

    Raises ValueError if the metric name is unknown. Returns None when the run has no value
    for it (a track without drum labels has `drums: null`; a run without a lag estimate has
    null drum metrics), so the caller leaves it out instead of scoring it as 0.
    """
    if metric not in _METRIC_PATHS:
        msg = f"Unknown metric '{metric}'. Valid metrics: {', '.join(sorted(_METRIC_PATHS))}"
        raise ValueError(msg)

    section, key = _METRIC_PATHS[metric]
    val = (summary.get(section) or {}).get(key)
    return float(val) if isinstance(val, (int, float)) and not isinstance(val, bool) else None


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
