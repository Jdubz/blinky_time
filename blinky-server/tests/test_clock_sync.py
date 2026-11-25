"""Clock sync for validation runs: best-of-N, drift after the run, timing in the raw capture
(so the box camera can put detections on its own clock)."""

from __future__ import annotations

import asyncio
import json
import time
from typing import Any

import pytest
from blinky_scoring.types import TestData, TransientEvent

from blinky_server.testing import test_runner
from blinky_server.testing.audio_player import PlaybackResult
from blinky_server.testing.test_session import TestSession


class _Protocol:
    """``json info`` with a firmware clock ``fw_offset_ms`` behind the host's, answering after
    the next delay in ``delays`` (seconds): the reply's midpoint is where millis is read."""

    def __init__(self, delays: list[float], fw_offset_ms: float = 1_000_000.0) -> None:
        self.delays = list(delays)
        self.fw_offset_ms = fw_offset_ms
        self.calls = 0
        self.fps = 130.0
        self.fps_seq: list[float] = []  # per-call fps, consumed first (then self.fps)

    async def send_command_timed(self, cmd: str) -> tuple[str, float, list[float]]:
        assert cmd == "json info"
        self.calls += 1
        d = self.delays.pop(0) if self.delays else 0.002
        sent = time.time()
        await asyncio.sleep(d / 2)
        millis = time.time() * 1000 - self.fw_offset_ms
        await asyncio.sleep(d / 2)
        arrived = time.time()
        await asyncio.sleep(0.05)  # the silence gap before the reply is finalized: not counted
        fps = self.fps_seq.pop(0) if self.fps_seq else self.fps
        info = {"version": "b0", "millis": millis, "fps": fps, "maxFrameMs": 31, "heapUsed": 1}
        return json.dumps(info), sent, [arrived]


class _Device:
    def __init__(self, protocol: _Protocol, dev_id: str = "AAAABBBBCCCCDDDD") -> None:
        self.id = dev_id
        self.protocol = protocol
        self.session: TestSession | None = None

    def start_test_session(self) -> TestSession:
        self.session = TestSession()
        return self.session

    def stop_test_session(self) -> None:
        self.session = None


async def test_the_sync_keeps_the_shortest_round_trip() -> None:
    proto = _Protocol([0.060, 0.004, 0.040, 0.030, 0.050])
    sync = await test_runner._sync_clock(_Device(proto))  # type: ignore[arg-type]
    assert sync is not None
    assert proto.calls == test_runner.CLOCK_SYNC_SAMPLES
    assert sync["samples"] == 5
    assert sync["rtt_ms"] < 20  # the 4 ms one, not the 60 ms one, and not the 50 ms gap
    assert sync["offset_ms"] == pytest.approx(1_000_000.0, abs=sync["rtt_ms"] / 2 + 2)


async def test_a_sync_with_no_reply_is_none() -> None:
    class _Dead:
        async def send_command_timed(self, cmd: str) -> tuple[str, float, list[float]]:
            raise TimeoutError

    assert await test_runner._sync_clock(_Device(_Dead())) is None  # type: ignore[arg-type]


async def test_a_run_records_both_syncs_and_the_drift(monkeypatch: pytest.MonkeyPatch) -> None:
    proto = _Protocol([])
    dev = _Device(proto)

    async def fake_play(*a: Any, **k: Any) -> PlaybackResult:
        proto.fw_offset_ms += 3.0  # the firmware clock loses 3 ms during the music
        return PlaybackResult(success=True, audio_start_time_ms=123.0, duration_ms=1.0)

    async def fake_play_streaming(*a: Any, **k: Any) -> PlaybackResult:
        proto.fps = 60.0  # streaming during the run halves the loop rate
        # A window closes partway through the post-run sync and holds its pauses: only the
        # first sample's window is the run's.
        proto.fps_seq = [60.0, 90.0, 90.0, 90.0, 90.0]
        return await fake_play(*a, **k)

    monkeypatch.setattr(test_runner, "play_audio", fake_play_streaming)
    playback, _ = await test_runner._record_and_play([dev], "x.mp3")  # type: ignore[list-item]
    c = playback.clock[dev.id]
    assert c["loop"]["fps"] == 130.0  # before the run
    assert c["after_loop"] == {"fps": 60.0, "maxFrameMs": 31}  # the first sample's; no other keys
    assert {"offset_ms", "rtt_ms", "samples", "after_offset_ms", "after_rtt_ms", "drift_ms"} <= set(
        c
    )
    assert c["drift_ms"] == pytest.approx(3.0, abs=2.0)
    assert proto.calls == 2 * test_runner.CLOCK_SYNC_SAMPLES


def test_the_raw_capture_carries_the_audio_start_and_the_clock() -> None:
    data = TestData(
        duration=1000.0,
        start_time=0.0,
        transients=[TransientEvent(timestamp_ms=1500.0, type="transient", strength=0.5)],
    )
    clock = {"offset_ms": 1.0, "rtt_ms": 2.0, "samples": 5}
    out = test_runner._serialize_raw_capture(data, 1000.0, clock)
    assert out["audio_start_epoch_ms"] == 1000.0
    assert out["clock"] == clock
    assert out["transients"][0]["t"] == 0.5
