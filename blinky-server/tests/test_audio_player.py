"""The audio player: the measured DAC start, its checks, and the explicit ffplay fallback."""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Any

import pytest

from blinky_server.testing import alsa_play, audio_player
from blinky_server.testing.audio_player import PlaybackResult


def _stub(tmp_path: Path, events: list[dict[str, Any]], code: int = 0) -> Path:
    """A stand-in for alsa_play.py: prints the events, exits with ``code``."""
    p = tmp_path / "alsa_play.py"
    lines = "".join(f"print({json.dumps(json.dumps(e))})\n" for e in events)
    p.write_text(f"import sys\n{lines}sys.exit({code})\n")
    return p


def _start(dac_offset_ms: float = 0.0, spread: float = 0.4, **kw: Any) -> dict[str, Any]:
    # The stub runs after the spawn is timed, so a DAC start "now + offset" is after it.
    return {
        "event": "start",
        "dac_start_epoch_ms": time.time() * 1000 + dac_offset_ms,
        "spread_ms": spread,
        "n": 40,
        "clock": "monotonic",
        "device": "plughw",
        **kw,
    }


@pytest.fixture
def ffplay_calls(monkeypatch: pytest.MonkeyPatch) -> list[tuple[Any, ...]]:
    calls: list[tuple[Any, ...]] = []

    async def fake_ffplay(*a: Any) -> PlaybackResult:
        calls.append(a)
        return PlaybackResult(
            success=True,
            audio_start_time_ms=1.0,
            duration_ms=1.0,
            start_measure={"method": "ffplay", "error": "not measured (ffplay)"},
        )

    monkeypatch.setattr(audio_player, "_play_ffplay", fake_ffplay)
    return calls


async def test_a_measured_start_is_kept(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, ffplay_calls: list[Any]
) -> None:
    stub = _stub(tmp_path, [_start(500.0), {"event": "end", "frames": 10, "xruns": 0}])
    monkeypatch.setattr(audio_player, "_PLAYER", stub)
    pb = await audio_player.play_audio("x.mp3", duration_ms=1000, seek_sec=3)
    assert pb.success and not ffplay_calls
    assert pb.dac_start_ms is not None
    assert 300 < pb.dac_start_ms - pb.audio_start_time_ms < 600
    assert pb.start_measure["method"] == "alsa"
    assert pb.start_measure["xruns"] == 0
    assert "error" not in pb.start_measure


@pytest.mark.parametrize(
    ("events", "why"),
    [
        ([_start(spread=25.0), {"event": "end", "frames": 1, "xruns": 0}], "spread"),
        ([_start(), {"event": "end", "frames": 1, "xruns": 2}], "underrun"),
        (
            [{"event": "start", "dac_start_epoch_ms": None, "error": "2 estimates"}],
            "2 estimates",
        ),
        ([{"event": "end", "frames": 1, "xruns": 0}], "no start"),
    ],
)
async def test_an_untrustworthy_start_is_flagged_not_used(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    ffplay_calls: list[Any],
    events: list[dict[str, Any]],
    why: str,
) -> None:
    monkeypatch.setattr(audio_player, "_PLAYER", _stub(tmp_path, events))
    pb = await audio_player.play_audio("x.mp3")
    assert pb.success and not ffplay_calls  # the audio played; only the start is untrusted
    assert pb.dac_start_ms is None
    assert why in pb.start_measure["error"]
    assert pb.measured_lag_ms() is None


async def test_nothing_played_falls_back_to_ffplay_flagged(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, ffplay_calls: list[Any]
) -> None:
    stub = _stub(tmp_path, [{"event": "error", "stage": "open", "error": "busy"}], code=2)
    monkeypatch.setattr(audio_player, "_PLAYER", stub)
    pb = await audio_player.play_audio("x.mp3", duration_ms=1000, seek_sec=3)
    assert ffplay_calls == [("x.mp3", 1000, 3)]
    assert pb.success and pb.dac_start_ms is None
    assert pb.start_measure == {"method": "ffplay", "error": "alsa player: busy"}


async def test_a_failure_mid_play_is_an_error_not_a_fallback(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, ffplay_calls: list[Any]
) -> None:
    stub = _stub(tmp_path, [_start(), {"event": "error", "stage": "play", "error": "gone"}], 3)
    monkeypatch.setattr(audio_player, "_PLAYER", stub)
    pb = await audio_player.play_audio("x.mp3")
    assert not pb.success and not ffplay_calls
    assert pb.error is not None and "gone" in pb.error
    assert pb.dac_start_ms is None and "error" in pb.start_measure


def test_the_measured_lag_needs_the_calibrated_path(monkeypatch: pytest.MonkeyPatch) -> None:
    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.dac_start_ms = 1200.0
    monkeypatch.setattr(audio_player, "AUDIO_PATH_MS", None)
    monkeypatch.delenv("BLINKY_AUDIO_PATH_MS", raising=False)
    assert pb.measured_lag_ms() is None
    rec = pb.audio_start()
    assert rec["dac_offset_ms"] == 200.0 and rec["lag_ms"] is None and "calibrated" in rec["note"]
    monkeypatch.setattr(audio_player, "AUDIO_PATH_MS", 60.0)
    assert pb.measured_lag_ms() == 260.0
    assert pb.audio_start()["lag_ms"] == 260.0
    monkeypatch.setenv("BLINKY_AUDIO_PATH_MS", "40")
    assert pb.measured_lag_ms() == 240.0
    monkeypatch.setenv("BLINKY_AUDIO_PATH_MS", "none")  # off: the lag is searched (recalibration)
    assert pb.measured_lag_ms() is None
    monkeypatch.setenv("BLINKY_AUDIO_PATH_MS", "")  # empty: the constant
    assert pb.measured_lag_ms() == 260.0
    pb.dac_start_ms = None
    assert pb.measured_lag_ms() is None


# -- alsa_play.py ------------------------------------------------------------------------------


def test_timestamps_go_onto_the_wall_clock() -> None:
    mono = time.clock_gettime(time.CLOCK_MONOTONIC)
    got = alsa_play.to_epoch_ms(mono - 0.25, "monotonic")
    assert got is not None and got[1] == "monotonic"
    assert got[0] == pytest.approx(time.time() * 1000 - 250, abs=20)
    # A trigger seconds old is still dated on a known clock; a zero isn't.
    got = alsa_play.to_epoch_ms(mono - 5.0, "monotonic")
    assert got is not None and got[0] == pytest.approx(time.time() * 1000 - 5000, abs=20)
    assert alsa_play.to_epoch_ms(0.0, "monotonic") is None
    # Unknown clock: a fresh timestamp goes on the nearest one.
    got = alsa_play.to_epoch_ms(time.clock_gettime(time.CLOCK_MONOTONIC))
    assert got is not None and got[0] == pytest.approx(time.time() * 1000, abs=20)
    got = alsa_play.to_epoch_ms(time.time() - 0.001)
    assert got is not None and got[1] == "realtime"
    assert alsa_play.to_epoch_ms(12.0) is None  # on no clock


def test_the_decoder_cuts_the_exact_segment() -> None:
    cmd = alsa_play.ffmpeg_cmd("a.mp3", 17.0, 35.0)
    # Input seek (decoded and discarded up to it), output duration, the speaker's format.
    assert cmd[cmd.index("-ss") + 1] == "17.000" and cmd.index("-ss") < cmd.index("-i")
    assert cmd[cmd.index("-t") + 1] == "35.000" and cmd.index("-t") > cmd.index("-i")
    assert cmd[-7:] == ["-f", "s16le", "-ac", "2", "-ar", "48000", "-"]
    assert "-ss" not in alsa_play.ffmpeg_cmd("a.mp3", 0, None)


class _FakeLib:
    """snd_pcm_status and its getters for one snapshot: timestamp, delay, state."""

    def __init__(self) -> None:
        self.ts = 0.0
        self.delay = 0
        self.state = alsa_play.SND_PCM_STATE_RUNNING

    def snd_pcm_status_sizeof(self) -> int:
        return 64

    def snd_pcm_status(self, pcm: Any, st: Any) -> int:
        return 0

    def snd_pcm_status_get_state(self, st: Any) -> int:
        return self.state

    def snd_pcm_status_get_delay(self, st: Any) -> int:
        return self.delay

    def _put(self, ref: Any, t: float) -> None:
        obj = ref._obj
        obj.tv_sec, obj.tv_nsec = int(t), int((t - int(t)) * 1e9)

    def snd_pcm_status_get_htstamp(self, st: Any, ref: Any) -> None:
        self._put(ref, self.ts)

    def snd_pcm_status_get_trigger_htstamp(self, st: Any, ref: Any) -> None:
        self._put(ref, self.ts - 0.2)


def test_frame_zero_is_dated_from_each_snapshot() -> None:
    lib = _FakeLib()
    alsa = type("A", (), {"lib": lib})()
    meter = alsa_play.Meter(alsa, ctypes.c_void_p(), "monotonic")
    now = time.clock_gettime(time.CLOCK_MONOTONIC)
    start = now - 0.5  # frame 0 left the DAC half a second ago
    for written in (24000, 26400, 28800, 31200, 33600):
        # 9600 frames queued: frame (written - 9600) is at the DAC at time ts.
        lib.delay = 9600
        lib.ts = start + (written - 9600) / alsa_play.RATE
        meter.sample(written)
    assert len(meter.estimates) == 5
    epoch_start = (start + (time.time() - time.clock_gettime(time.CLOCK_MONOTONIC))) * 1000
    assert all(e == pytest.approx(epoch_start, abs=2) for e in meter.estimates)
    lib.state = alsa_play.SND_PCM_STATE_PREPARED  # not started: no estimate
    meter.sample(40000)
    lib.state, lib.delay = alsa_play.SND_PCM_STATE_RUNNING, 50000  # more queued than written
    meter.sample(40000)
    assert len(meter.estimates) == 5 and meter.rejected == 1


def test_the_device_is_opened_without_waiting_for_it() -> None:
    """A blocking open of a busy device waits for it; non-blocking, it's EBUSY at once (and the
    caller falls back to ffplay). Writes go back to blocking."""
    calls: list[tuple[Any, ...]] = []

    class Lib:
        def __init__(self, open_rc: int) -> None:
            self.open_rc = open_rc

        def snd_pcm_open(self, ref: Any, name: bytes, stream: int, mode: int) -> int:
            calls.append(("open", name, stream, mode))
            return self.open_rc

        def snd_pcm_nonblock(self, pcm: Any, on: int) -> int:
            calls.append(("nonblock", on))
            return 0

        def snd_strerror(self, rc: int) -> bytes:
            return b"Device or resource busy"

    alsa = alsa_play.Alsa.__new__(alsa_play.Alsa)
    alsa.lib = Lib(0)  # type: ignore[assignment]
    alsa_play.open_pcm(alsa, "plughw")
    assert calls == [
        ("open", b"plughw", alsa_play.SND_PCM_STREAM_PLAYBACK, alsa_play.SND_PCM_NONBLOCK),
        ("nonblock", 0),
    ]
    alsa.lib = Lib(-16)  # type: ignore[assignment]
    with pytest.raises(OSError, match="busy"):
        alsa_play.open_pcm(alsa, "plughw")


def test_the_stream_is_configured_with_timestamps_on() -> None:
    """Without SND_PCM_TSTAMP_ENABLE the kernel leaves a running stream's status timestamp at
    zero on real hardware (the null PCM fills it in anyway, so the end-to-end test can't
    see this): every estimate would be rejected."""
    calls: list[tuple[Any, ...]] = []

    class Lib:
        def __getattr__(self, name: str) -> Any:
            def f(*a: Any) -> int:
                calls.append((name, *a))
                if name == "snd_pcm_get_params":
                    a[1]._obj.value, a[2]._obj.value = 9600, 2400
                return 64 if name.endswith("_sizeof") else 0

            return f

    alsa = alsa_play.Alsa.__new__(alsa_play.Alsa)
    alsa.lib = Lib()  # type: ignore[assignment]
    assert alsa_play.configure(alsa, ctypes.c_void_p()) == (9600, 2400, "monotonic")
    names = [c[0] for c in calls]
    tstamp = names.index("snd_pcm_sw_params_set_tstamp_mode")
    assert calls[tstamp][3] == alsa_play.SND_PCM_TSTAMP_ENABLE
    assert names.index("snd_pcm_set_params") < tstamp < names.index("snd_pcm_sw_params")
    ttype = names.index("snd_pcm_sw_params_set_tstamp_type")
    assert calls[ttype][3] == alsa_play.SND_PCM_TSTAMP_TYPE_MONOTONIC
    assert ttype < names.index("snd_pcm_sw_params")
    # A zero timestamp (what the mode left off gives) dates nothing.
    assert alsa_play.to_epoch_ms(0.0) is None


@pytest.mark.skipif(
    shutil.which("ffmpeg") is None or ctypes.util.find_library("asound") is None,
    reason="needs ffmpeg and libasound",
)
def test_the_player_runs_end_to_end_on_the_null_device(tmp_path: Path) -> None:
    """Mechanics only (the null PCM doesn't play in real time, so its timestamps don't date
    anything): decode, open, write, report, exit 0."""
    import subprocess

    wav = tmp_path / "tone.wav"
    subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", str(wav)],
        check=True,
    )
    out = subprocess.run(
        [sys.executable, alsa_play.__file__, "--device", "null", "--duration", "0.5", str(wav)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    events = [json.loads(line) for line in out.stdout.splitlines()]
    assert out.returncode == 0, out
    assert [e["event"] for e in events] == ["start", "end"]
    assert events[1]["frames"] == alsa_play.RATE // 2 and events[1]["xruns"] == 0
    bad = subprocess.run(
        [sys.executable, alsa_play.__file__, "--device", "no-such-pcm", str(wav)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert bad.returncode == alsa_play.EXIT_NOTHING_PLAYED
    assert json.loads(bad.stdout.splitlines()[-1])["stage"] == "open"


def test_a_run_is_scored_at_the_measured_lag_or_says_why_not(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from blinky_server.testing import workflows

    seen: list[Any] = []

    def fake_score(
        data: Any, start: float, gt: Any, latency_ms: float | None = None, *, lag_search: Any
    ) -> Any:
        seen.append((start, latency_ms, lag_search.prior_ms))
        return {}

    monkeypatch.setattr(workflows, "score_device_run", fake_score)
    monkeypatch.setattr(workflows, "format_score_summary", lambda s: {"drums": {}})
    monkeypatch.setattr(audio_player, "AUDIO_PATH_MS", 70.0)
    monkeypatch.delenv("BLINKY_AUDIO_PATH_MS", raising=False)
    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.dac_start_ms, pb.start_measure = 1180.0, {"method": "alsa", "spread_ms": 0.5}
    out = workflows._score("data", pb, None)
    assert seen[-1] == (1000.0, 250.0, 220.0)
    assert out["audioStart"] == {
        "player": "alsa",
        "measured": True,
        "lagMs": 250.0,
        "dacOffsetMs": 180.0,
        "pathMs": 70.0,
        "error": None,
    }
    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.start_measure = {"method": "ffplay", "error": "alsa player: busy"}
    out = workflows._score("data", pb, None)
    # The scorer searches, its tie-break prior ffplay's typical lag: ffplay played.
    assert seen[-1] == (1000.0, None, 270.0)
    assert out["audioStart"]["measured"] is False and out["audioStart"]["player"] == "ffplay"
    assert out["audioStart"]["error"] == "alsa player: busy"
    # The ALSA player played but its timestamps weren't trusted: its own prior, not ffplay's.
    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.start_measure = {"method": "alsa", "spread_ms": 14.2, "error": "timestamp spread"}
    out = workflows._score("data", pb, None)
    assert seen[-1] == (1000.0, None, 220.0)
    assert out["audioStart"]["measured"] is False and out["audioStart"]["player"] == "alsa"
    # A playback that doesn't say which player ran can't pick a prior: it raises, never guesses.
    with pytest.raises(ValueError, match="unknown audio player"):
        workflows._score("data", PlaybackResult(True, 1000.0, 1.0), None)


def test_the_raw_capture_carries_the_measured_start() -> None:
    from blinky_scoring.types import TestData

    from blinky_server.testing import test_runner

    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.dac_start_ms = 1150.0
    cap = test_runner._serialize_raw_capture(
        TestData(duration=1.0, start_time=0.0), 1000.0, None, pb.audio_start()
    )
    assert cap["audio_start_epoch_ms"] == 1000.0  # kept: every t is from it
    assert cap["audio_dac_start_epoch_ms"] == 1150.0
    assert cap["audio_start"]["dac_offset_ms"] == 150.0


def test_the_box_is_calibrated_so_runs_are_scored_at_the_measured_lag(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The test box's fixed path is calibrated (docs/TEST_BOX.md, "Audio start"): a measured
    start gives a measured lag without any configuration."""
    monkeypatch.delenv("BLINKY_AUDIO_PATH_MS", raising=False)
    assert audio_player.AUDIO_PATH_MS is not None and 0 < audio_player.AUDIO_PATH_MS < 150
    pb = PlaybackResult(success=True, audio_start_time_ms=1000.0, duration_ms=1.0)
    pb.dac_start_ms = 1170.0
    assert pb.measured_lag_ms() == 170.0 + audio_player.AUDIO_PATH_MS
    assert "note" not in pb.audio_start()
