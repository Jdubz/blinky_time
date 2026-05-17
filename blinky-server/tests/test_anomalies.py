"""Phase-6 tests: anomaly detectors over SignalHistory.

Each detector is a pure function — these tests construct controlled
histories and assert detection vs non-detection.
"""

from __future__ import annotations

from pathlib import Path

from blinky_server.firmware.anomalies import (
    BOOTLOADER_OSCILLATING,
    CRASH_LOOP_SUSPECTED,
    NO_REBOOT_DETECTED,
    QUARANTINE_TRIGGERED,
    STALE_FIRMWARE,
    SignalHistory,
    check_all,
    detect_bootloader_oscillating,
    detect_crash_loop,
    detect_no_reboot,
    detect_quarantine,
    detect_stale_firmware,
)
from blinky_server.firmware.flash_job import FlashJob

# --- crash_loop ------------------------------------------------------------


def test_crash_loop_zero_enums_silent() -> None:
    history = SignalHistory()
    assert detect_crash_loop(history, now=1000.0) is False


def test_crash_loop_one_enum_silent() -> None:
    history = SignalHistory(re_enum_timestamps=[990.0])
    assert detect_crash_loop(history, now=1000.0) is False


def test_crash_loop_two_within_window_fires() -> None:
    history = SignalHistory(re_enum_timestamps=[950.0, 990.0])  # 40s apart
    assert detect_crash_loop(history, now=1000.0) is True


def test_crash_loop_two_outside_window_silent() -> None:
    """Two re-enums but >60s apart — that's normal reflash + boot, not a loop."""
    history = SignalHistory(re_enum_timestamps=[900.0, 990.0])  # 90s apart
    assert detect_crash_loop(history, now=1000.0, window_s=60.0) is False


def test_crash_loop_three_in_window() -> None:
    history = SignalHistory(re_enum_timestamps=[970.0, 980.0, 990.0])
    assert detect_crash_loop(history, now=1000.0) is True


# --- quarantine ------------------------------------------------------------


def test_quarantine_no_handshakes_silent() -> None:
    assert detect_quarantine(SignalHistory()) is False


def test_quarantine_status_unconfigured_fires() -> None:
    """Compact-form CLAUDE.md signature: {"status": "unconfigured"}."""
    history = SignalHistory(handshakes=[(1000.0, {"status": "unconfigured"})])
    assert detect_quarantine(history) is True


def test_quarantine_nested_safemode_fires() -> None:
    """Verbose-form signature: device.safeMode == True."""
    history = SignalHistory(handshakes=[(1000.0, {"device": {"safeMode": True}})])
    assert detect_quarantine(history) is True


def test_quarantine_normal_handshake_silent() -> None:
    history = SignalHistory(
        handshakes=[(1000.0, {"version": "b166", "device": {"safeMode": False}})]
    )
    assert detect_quarantine(history) is False


def test_quarantine_uses_most_recent_only() -> None:
    """Old quarantine + recent clean = not flagged. The current state is
    what matters; one bad sample early on shouldn't taint forever."""
    history = SignalHistory(
        handshakes=[
            (900.0, {"status": "unconfigured"}),
            (1000.0, {"version": "b166"}),
        ]
    )
    assert detect_quarantine(history) is False


# --- stale_firmware --------------------------------------------------------


def test_stale_silent_without_baseline() -> None:
    history = SignalHistory(
        handshakes=[(1000.0, {"version": "b165"})] * 10,
        previous_version=None,
        expected_version="b166",
    )
    assert detect_stale_firmware(history) is False


def test_stale_silent_when_prev_equals_expected() -> None:
    """Same-firmware reflash (b166 → b166) has no notion of "stale"."""
    history = SignalHistory(
        handshakes=[(1000.0, {"version": "b166"})] * 20,
        previous_version="b166",
        expected_version="b166",
    )
    assert detect_stale_firmware(history) is False


def test_stale_fires_after_threshold_old_version() -> None:
    history = SignalHistory(
        handshakes=[(1000.0 + i, {"version": "b165"}) for i in range(10)],
        previous_version="b165",
        expected_version="b166",
    )
    assert detect_stale_firmware(history) is True


def test_stale_silent_below_threshold() -> None:
    history = SignalHistory(
        handshakes=[(1000.0 + i, {"version": "b165"}) for i in range(3)],
        previous_version="b165",
        expected_version="b166",
    )
    assert detect_stale_firmware(history) is False


def test_stale_silent_when_version_matches_expected() -> None:
    history = SignalHistory(
        handshakes=[(1000.0 + i, {"version": "b166"}) for i in range(10)],
        previous_version="b165",
        expected_version="b166",
    )
    assert detect_stale_firmware(history) is False


# --- no_reboot -------------------------------------------------------------


def test_no_reboot_within_threshold_silent() -> None:
    """Write completed 60s ago, threshold is 300s — don't call it yet."""
    history = SignalHistory(write_completed_at=940.0)
    assert detect_no_reboot(history, now=1000.0, threshold_s=300.0) is False


def test_no_reboot_past_threshold_no_enums_fires() -> None:
    history = SignalHistory(write_completed_at=600.0)
    assert detect_no_reboot(history, now=1000.0, threshold_s=300.0) is True


def test_no_reboot_silenced_by_post_write_enum() -> None:
    history = SignalHistory(write_completed_at=600.0, re_enum_timestamps=[700.0])
    assert detect_no_reboot(history, now=1000.0, threshold_s=300.0) is False


def test_no_reboot_ignores_pre_write_enums() -> None:
    """A re-enum that happened BEFORE the write isn't evidence of post-flash reboot."""
    history = SignalHistory(
        write_completed_at=600.0,
        re_enum_timestamps=[500.0],  # pre-write only
    )
    assert detect_no_reboot(history, now=1000.0, threshold_s=300.0) is True


# --- bootloader_oscillating ------------------------------------------------


def test_oscillating_below_threshold_silent() -> None:
    history = SignalHistory(re_enum_timestamps=[1, 2, 3])  # 3 < threshold 4
    assert detect_bootloader_oscillating(history) is False


def test_oscillating_at_threshold_fires() -> None:
    history = SignalHistory(re_enum_timestamps=[1, 2, 3, 4])
    assert detect_bootloader_oscillating(history) is True


# --- check_all integration -------------------------------------------------


def test_check_all_attaches_all_relevant_anomalies() -> None:
    """Build a history that triggers crash_loop + quarantine + oscillating
    simultaneously, confirm all three names land on the job."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    history = SignalHistory(
        re_enum_timestamps=[970.0, 980.0, 990.0, 995.0],  # 4 within 60s
        handshakes=[(1000.0, {"status": "unconfigured"})],
        write_completed_at=900.0,
    )
    check_all(job, history, now=1000.0)
    assert CRASH_LOOP_SUSPECTED in job.anomalies
    assert QUARANTINE_TRIGGERED in job.anomalies
    assert BOOTLOADER_OSCILLATING in job.anomalies
    # not these:
    assert STALE_FIRMWARE not in job.anomalies  # previous_version is None
    assert NO_REBOOT_DETECTED not in job.anomalies  # enums present after write


def test_check_all_is_idempotent() -> None:
    """Repeated calls don't duplicate anomalies — pollers can call every
    cycle without worrying about deduping."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    history = SignalHistory(
        re_enum_timestamps=[970.0, 980.0, 990.0],
        write_completed_at=900.0,
    )
    check_all(job, history, now=1000.0)
    check_all(job, history, now=1000.0)
    check_all(job, history, now=1000.0)
    assert job.anomalies.count(CRASH_LOOP_SUSPECTED) == 1


def test_check_all_clean_history_no_anomalies() -> None:
    """A normal-looking history (one re-enum, one good handshake) →
    no anomalies. This is the regression guard against false positives."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    history = SignalHistory(
        re_enum_timestamps=[950.0],
        handshakes=[(960.0, {"version": "b166"})],
        write_completed_at=940.0,
        previous_version="b165",
        expected_version="b166",
    )
    check_all(job, history, now=1000.0)
    assert job.anomalies == []
