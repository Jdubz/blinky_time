"""Phase-6: anomaly detectors for in-flight flash jobs.

Detectors observe accumulated verify-signal history and surface
operator-actionable warnings via ``job.add_anomaly``. They NEVER
transition the job state on their own — failing the job is always the
orchestrator's call (and per design, ``run_verify`` never auto-fails;
operators decide when to abort).

The anomaly names are stable strings (constants below) because they
appear in the job snapshot at ``GET /api/flash-jobs/{id}`` and any UI
or log filter would key off them.

Each detector is a pure function over ``SignalHistory``. ``check_all``
runs every detector and is the single call site Phase 7 wires into the
verify polling loop.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Any

from .flash_job import FlashJob
from .utils import extract_version

log = logging.getLogger(__name__)

# Stable anomaly names — used in job.anomalies, log lines, and UI.
CRASH_LOOP_SUSPECTED = "crash_loop_suspected"
QUARANTINE_TRIGGERED = "quarantine_triggered"
STALE_FIRMWARE = "stale_firmware"
NO_REBOOT_DETECTED = "no_reboot_detected"
BOOTLOADER_OSCILLATING = "bootloader_oscillating"


# Default thresholds — overridable via ``check_all`` args. Conservative
# values: the goal is to surface real problems, not to fire on normal
# boot-time transients.
CRASH_LOOP_WINDOW_S = 60.0
CRASH_LOOP_THRESHOLD = 2

STALE_FIRMWARE_THRESHOLD = 10

NO_REBOOT_THRESHOLD_S = 300.0  # 5 min

BOOTLOADER_OSCILLATING_THRESHOLD = 4


@dataclass
class SignalHistory:
    """Time-stamped history of signals observed during one verify run.

    The orchestrator (Phase 7) maintains a per-job instance and appends
    to it on every poll cycle. Detectors only look at recent entries,
    so this can grow without bound across a long-running verify; future
    optimisation could prune old entries past the longest window we
    care about.
    """

    # ``time.time()`` at each USB re-enumeration we observed for this device.
    re_enum_timestamps: list[float] = field(default_factory=list)

    # ``(time.time(), info_dict)`` for each successful handshake response.
    handshakes: list[tuple[float, dict[str, Any]]] = field(default_factory=list)

    # Anchors the "since" point for ``no_reboot_detected``. Set by the
    # orchestrator from ``job.write_completed_at``.
    write_completed_at: float = 0.0

    # The version this device reported BEFORE the flash attempt — used
    # by ``stale_firmware`` to distinguish "still booting the old image"
    # from "reporting some unexpected version." If ``None``, the stale
    # detector is silent (we can't tell).
    previous_version: str | None = None

    # The version this flash is meant to land. ``None`` means "any
    # responsive handshake counts as success" (stale detector silent).
    expected_version: str | None = None


def detect_crash_loop(
    history: SignalHistory,
    *,
    window_s: float = CRASH_LOOP_WINDOW_S,
    threshold: int = CRASH_LOOP_THRESHOLD,
    now: float | None = None,
) -> bool:
    """``threshold`` or more USB re-enumerations within ``window_s``.

    A healthy boot produces exactly one re-enumeration. Two within
    60 s means the device is rebooting unexpectedly — most commonly
    the firmware is crashing on boot and the bootloader is re-entering
    DFU (the ``DEFAULT_TO_OTA_DFU`` path from CLAUDE.md).
    """
    cutoff = (now if now is not None else time.time()) - window_s
    recent = [t for t in history.re_enum_timestamps if t >= cutoff]
    return len(recent) >= threshold


def detect_quarantine(history: SignalHistory) -> bool:
    """Most-recent handshake reports the SafeBoot quarantine signature.

    Per CLAUDE.md "60-second rule": if too many resets accumulate, the
    firmware's RebootFrequencyCounter trips and ``configStorage.
    quarantineDeviceConfig()`` flips ``data_.device.isValid = false`` —
    device boots into safeMode with ``{"status":"unconfigured"}``.

    Without this detector, an operator sees a "successful" flash that
    actually landed but the device is in safe mode and won't render
    LEDs — and the fix is a separate ``device upload`` step they need
    to know to take.
    """
    if not history.handshakes:
        return False
    _, last_info = history.handshakes[-1]
    # Top-level "status" field (compact form).
    if last_info.get("status") == "unconfigured":
        return True
    # Nested ``device.safeMode`` (legacy / verbose form).
    device_info = last_info.get("device")
    return isinstance(device_info, dict) and device_info.get("safeMode") is True


def detect_stale_firmware(
    history: SignalHistory, *, threshold: int = STALE_FIRMWARE_THRESHOLD
) -> bool:
    """``threshold`` or more handshakes reporting the pre-flash version.

    Distinct from "wrong version" generally (handled by the verify
    sub-state machine staying in AWAITING_VERSION_MATCH): stale
    specifically means *the device is still running the OLD image*.
    That points at "the flash never took" rather than "the flash took
    but reports a different version than we expected."

    Silent when either previous_version or expected_version is None —
    we don't have the data to detect "stuck on the old image."
    """
    if history.previous_version is None or history.expected_version is None:
        return False
    if history.previous_version == history.expected_version:
        return False  # no diff to detect
    stale_count = 0
    for _, info in history.handshakes:
        v = _extract_version(info)
        if v == history.previous_version:
            stale_count += 1
    return stale_count >= threshold


def detect_no_reboot(
    history: SignalHistory,
    *,
    threshold_s: float = NO_REBOOT_THRESHOLD_S,
    now: float | None = None,
) -> bool:
    """No USB re-enum since ``write_completed_at`` for ``threshold_s``.

    Specifically the 0.8.0-4 bootloader's "stuck after MSC write"
    failure mode we hit on 2026-05-17 — the bootloader has received
    all UF2 blocks but doesn't reboot. The operator's likely next move
    is to send ``bootloader`` again or do an SWD recovery; this anomaly
    surfaces the situation before they're left wondering.
    """
    current = now if now is not None else time.time()
    if (current - history.write_completed_at) < threshold_s:
        return False  # not enough time elapsed to call it
    if not history.re_enum_timestamps:
        return True  # zero re-enums since write
    last_enum = max(history.re_enum_timestamps)
    return last_enum < history.write_completed_at


def detect_bootloader_oscillating(
    history: SignalHistory, *, threshold: int = BOOTLOADER_OSCILLATING_THRESHOLD
) -> bool:
    """``threshold`` or more re-enumerations over the verify lifetime.

    Distinguished from crash_loop_suspected by timescale: crash loops
    are rapid-fire (within 60 s); oscillation is the slower bootloader
    ↔ app ping-pong that some quarantine scenarios produce. Once both
    detectors are firing, operator has a strong "the app is genuinely
    broken" signal.
    """
    return len(history.re_enum_timestamps) >= threshold


def check_all(
    job: FlashJob,
    history: SignalHistory,
    *,
    now: float | None = None,
) -> None:
    """Run every detector against ``history`` and attach any new
    anomalies to ``job`` via ``add_anomaly`` (idempotent).

    Phase 7 calls this at the end of each ``run_verify`` poll cycle.
    ``add_anomaly`` is itself idempotent, so calling ``check_all`` on
    every cycle is fine and gives the most up-to-date snapshot.
    """
    if detect_crash_loop(history, now=now):
        job.add_anomaly(CRASH_LOOP_SUSPECTED)
    if detect_quarantine(history):
        job.add_anomaly(QUARANTINE_TRIGGERED)
    if detect_stale_firmware(history):
        job.add_anomaly(STALE_FIRMWARE)
    if detect_no_reboot(history, now=now):
        job.add_anomaly(NO_REBOOT_DETECTED)
    if detect_bootloader_oscillating(history):
        job.add_anomaly(BOOTLOADER_OSCILLATING)


def _extract_version(info: dict[str, Any]) -> str | None:
    """Pull the firmware version string out of a ``json info`` response.

    Delegates to ``firmware.utils.extract_version`` — the canonical
    version-key list lives there and is shared with ``verify.py`` to
    avoid the sync hazard of duplicated key tables.
    """
    return extract_version(info)
