"""Audio playback in a subprocess, with the time its first sample reached the DAC.

The player (``alsa_play.py``) decodes exactly the segment asked for with ffmpeg and writes it
to ALSA, and it measures when the first sample reached the DAC from ALSA's own timestamps
(that module has the method and its sources). ``audio_start_time_ms`` stays the moment the
player was spawned; ``dac_start_ms`` is the measured start. If that player can't play (no
libasound, the device won't open), ffplay plays the file as before and the run says the start
wasn't measured (``start_measure["error"]``), never silently.

Why: the drum scorer needs the lag from the audio start to the detections. Spawning ffplay
isn't sound: on the test box the detections came 235-360 ms after the spawn, varying run to
run (blinky-corpus#10), and the scorer couldn't search the lag out of strictly periodic music.

The singleton tracks the player subprocess globally. Only one audio playback can be active at
a time. Starting a new playback kills the previous one. Server startup kills any orphan player
from prior sessions.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .alsa_play import EXIT_NOTHING_PLAYED

log = logging.getLogger(__name__)


_PLAYER = Path(__file__).with_name("alsa_play.py")
# The ALSA PCM the player writes to. plughw on the default card (the test box's ~/.asoundrc
# names its USB speaker): the kernel's own delay, which on USB audio counts the frames queued
# on the bus (dmix's delay leaves them out), and a format conversion if the card needs one.
# The audio lock keeps a second player off the device.
PCM_DEVICE = os.environ.get("BLINKY_AUDIO_PCM", "plughw")
# The per-write estimates of the first sample's DAC time agree to about 1 ms on USB audio (the
# driver dates its position with the bus frame counter); more spread than this means the
# timestamps aren't to be trusted, and the run isn't measured.
MAX_SPREAD_MS = 10.0

# The fixed part of the lag after the DAC: the speaker's own processing, the air, and the
# device's mic buffering and detection. Calibrated on the test box against the music: the
# scorer's searched lag minus the measured DAC offset, on the 14 of 28 runs it resolved
# unambiguously (2026-11-25, firmware b211-dev-c5f4d51a; techno-machine-drum x5,
# techno-minimal-01 x5, edm/ x1): 42.0-49.7 ms, median 45.2, sd 2.5. The 14 ambiguous runs
# fell in the same range (41-50). Recalibrate when the speaker, its placement or the chip's
# detection latency changes: a drift shows as ``lagCheckMs - lagMs`` in the drum scores (the
# scorer's check over the music, +-100 ms around the measured lag). None scores every run
# with the lag searched for. Override: BLINKY_AUDIO_PATH_MS.
AUDIO_PATH_MS: float | None = 45.0


def audio_path_ms() -> float | None:
    raw = os.environ.get("BLINKY_AUDIO_PATH_MS")
    return float(raw) if raw else AUDIO_PATH_MS


@dataclass
class PlaybackResult:
    success: bool
    audio_start_time_ms: float  # epoch ms when the player was spawned (sound reaches the DAC later)
    duration_ms: float  # actual playback duration
    error: str | None = None
    # Per device id: the clock syncs around the run (test_runner._record_and_play), so a raw
    # capture can put firmware times on the host clock and say how far to trust them.
    clock: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Epoch ms (host clock) at which the first sample reached the DAC, measured from ALSA's
    # timestamps. None when it wasn't measured; start_measure["error"] says why.
    dac_start_ms: float | None = None
    # How it was measured: method, device, spread_ms, n, ..., or the error.
    start_measure: dict[str, Any] = field(default_factory=dict)

    def measured_lag_ms(self) -> float | None:
        """The lag from ``audio_start_time_ms`` to the device hearing the first sample: the
        measured DAC start plus the rig's calibrated fixed path. None when either is missing."""
        path = audio_path_ms()
        if self.dac_start_ms is None or path is None:
            return None
        return self.dac_start_ms - self.audio_start_time_ms + path

    def audio_start(self) -> dict[str, Any]:
        """What a run records about its audio start (raw captures, validation results)."""
        path = audio_path_ms()
        out: dict[str, Any] = {
            "spawn_epoch_ms": self.audio_start_time_ms,
            "dac_start_epoch_ms": self.dac_start_ms,
            "dac_offset_ms": None
            if self.dac_start_ms is None
            else round(self.dac_start_ms - self.audio_start_time_ms, 2),
            "path_ms": path,
            "lag_ms": None if (lag := self.measured_lag_ms()) is None else round(lag, 2),
            **self.start_measure,
        }
        if self.dac_start_ms is not None and path is None:
            out.setdefault("note", "the rig's fixed path isn't calibrated: lag not measured")
        return out


def audio_seconds(audio_file: str) -> float:
    """The audio file's length in seconds, from ffprobe (it ships with the ffmpeg that plays
    the file). Fails loudly: a track whose length can't be read is an error, never 0."""
    cmd = ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0"]
    try:
        out = subprocess.run(
            [*cmd, audio_file], capture_output=True, text=True, timeout=30, check=False
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RuntimeError(f"ffprobe {audio_file}: {e}") from e
    if out.returncode != 0:
        raise RuntimeError(f"ffprobe {audio_file} exited {out.returncode}: {out.stderr.strip()}")
    try:
        seconds = float(out.stdout.strip())
    except ValueError as e:
        raise RuntimeError(f"ffprobe {audio_file}: no duration in {out.stdout!r}") from e
    if not seconds > 0:
        raise RuntimeError(f"ffprobe {audio_file}: duration {seconds}")
    return seconds


# ---------------------------------------------------------------------------
# Singleton audio player — guarantees at most one ffplay at a time
# ---------------------------------------------------------------------------

_active_proc: asyncio.subprocess.Process | None = None
# Cached on first call, never refreshed. If the USB speaker is hot-plugged
# after server start, restart the server to re-detect. This is intentional —
# audio device changes mid-test would be more disruptive than stale cache.
_cached_audio_env: dict[str, str] | None = None


async def _get_audio_env() -> dict[str, str]:
    """Get environment for audio playback.

    Does NOT set AUDIODEV — ffplay on Raspberry Pi works best with
    the default ALSA routing. Setting AUDIODEV=hw:N,0 bypasses dmix
    and fails with 'audio open failed' on most configurations.
    """
    global _cached_audio_env
    if _cached_audio_env is not None:
        return _cached_audio_env

    _cached_audio_env = dict(os.environ)
    # Remove any inherited AUDIODEV to ensure ffplay uses system default
    _cached_audio_env.pop("AUDIODEV", None)
    return _cached_audio_env


async def stop_audio() -> None:
    """Kill the currently playing audio, if any. Idempotent."""
    global _active_proc
    proc = _active_proc
    _active_proc = None
    if proc is not None and proc.returncode is None:
        log.info("Stopping active audio playback (pid %s)", proc.pid)
        try:
            proc.kill()
            await proc.wait()
        except ProcessLookupError:
            pass


async def play_audio(
    audio_file: str,
    duration_ms: float | None = None,
    seek_sec: float | None = None,
) -> PlaybackResult:
    """Play an audio file, blocking until complete, and measure when it started at the DAC.

    Kills any previously playing audio first. The subprocess is tracked
    globally so it can be killed on cancellation, server shutdown, or
    when a new test starts. The ALSA player plays it; if that one can't (nothing was played),
    ffplay does, unmeasured and flagged (``start_measure["error"]``).
    """
    # Kill any previous playback — only one audio at a time
    await stop_audio()

    cmd = [sys.executable, str(_PLAYER), "--device", PCM_DEVICE]
    if seek_sec is not None:
        cmd.extend(["--seek", f"{seek_sec:.3f}"])
    if duration_ms is not None:
        cmd.extend(["--duration", f"{duration_ms / 1000:.3f}"])
    cmd.append(audio_file)
    result = await _run_player(cmd, capture=True)
    if result is None:  # no Python to run it with: can't happen, but never silently
        fb = await _play_ffplay(audio_file, duration_ms, seek_sec)
        fb.start_measure["error"] = f"alsa player: {cmd[0]} not found"
        return fb
    pb, out, code = result
    events = _events(out)
    start = events.get("start") or {}
    end = events.get("end")
    err = events.get("error")
    if code == EXIT_NOTHING_PLAYED:
        reason = (err or {}).get("error") or "nothing played"
        log.warning("[FALLBACK] audio player: %s; playing with ffplay, start not measured", reason)
        fb = await _play_ffplay(audio_file, duration_ms, seek_sec)
        fb.start_measure["error"] = f"alsa player: {reason}"
        return fb
    measure: dict[str, Any] = {"method": "alsa"}
    measure.update({k: start[k] for k in ("device", "spread_ms", "n", "clock") if k in start})
    if start.get("trigger_epoch_ms") is not None:
        measure["trigger_offset_ms"] = round(start["trigger_epoch_ms"] - pb.audio_start_time_ms, 2)
    if err is not None:
        pb.error = f"{pb.error or 'audio player'}: {err.get('error')}"
    dac = start.get("dac_start_epoch_ms")
    problem = None
    if not start:
        problem = "the player reported no start"
    elif dac is None:
        problem = start.get("error") or "no DAC start"
    elif (start.get("spread_ms") or 0.0) > MAX_SPREAD_MS:
        problem = f"timestamp spread {start['spread_ms']:.1f} ms > {MAX_SPREAD_MS:g} ms"
    elif end is not None and end.get("xruns"):
        problem = f"{end['xruns']} underrun(s): the audio after one is late by its gap"
    elif end is None:
        problem = "the player reported no end" if pb.success else "the playback didn't finish"
    if end is not None:
        measure["xruns"] = end.get("xruns", 0)
    if problem is None and dac is not None:
        pb.dac_start_ms = float(dac)
    else:
        measure["error"] = problem
        log.warning("[FALLBACK] audio start not measured: %s", problem)
    pb.start_measure = measure
    return pb


def _events(out: str) -> dict[str, dict[str, Any]]:
    """The player's JSON lines by event (the last of each)."""
    events: dict[str, dict[str, Any]] = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        if isinstance(ev, dict) and isinstance(ev.get("event"), str):
            events[ev["event"]] = ev
    return events


async def _run_player(
    cmd: list[str], *, capture: bool
) -> tuple[PlaybackResult, str, int | None] | None:
    """Run a player subprocess to its end, tracked for stop_audio: the result, its stdout and
    its exit status (None when it failed to run). None when the program isn't there."""
    global _active_proc
    env = await _get_audio_env()
    log.info("Playing: %s", " ".join(cmd))
    t0 = time.time() * 1000
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE if capture else asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
            env=env,
        )
        _active_proc = proc
        audio_start_time_ms = time.time() * 1000
        out, _ = await proc.communicate()

        # Clear tracking (process finished normally)
        if _active_proc is proc:
            _active_proc = None

        elapsed = time.time() * 1000 - t0
        text = (out or b"").decode(errors="replace")
        if proc.returncode == 0:
            return (
                PlaybackResult(
                    success=True, audio_start_time_ms=audio_start_time_ms, duration_ms=elapsed
                ),
                text,
                0,
            )
        return (
            PlaybackResult(
                success=False,
                audio_start_time_ms=audio_start_time_ms,
                duration_ms=elapsed,
                error=f"{Path(cmd[1] if cmd[0] == sys.executable else cmd[0]).name} exited "
                f"with code {proc.returncode}",
            ),
            text,
            proc.returncode,
        )
    except FileNotFoundError:
        _active_proc = None
        return None
    except asyncio.CancelledError:
        # Kill the player when the task is cancelled (test failure, server shutdown)
        await stop_audio()
        raise
    except Exception as e:
        await stop_audio()
        return (
            PlaybackResult(
                success=False,
                audio_start_time_ms=t0,
                duration_ms=time.time() * 1000 - t0,
                error=str(e),
            ),
            "",
            None,
        )


async def _play_ffplay(
    audio_file: str, duration_ms: float | None, seek_sec: float | None
) -> PlaybackResult:
    """The fallback player: ffplay, as before. Its start isn't measured."""
    cmd = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]
    if seek_sec is not None:
        cmd.extend(["-ss", f"{seek_sec:.3f}"])
    if duration_ms is not None:
        cmd.extend(["-t", f"{duration_ms / 1000:.3f}"])
    cmd.append(audio_file)
    result = await _run_player(cmd, capture=False)
    if result is None:
        t0 = time.time() * 1000
        return PlaybackResult(
            success=False,
            audio_start_time_ms=t0,
            duration_ms=0,
            error="ffplay not found — install ffmpeg",
            start_measure={"method": "ffplay", "error": "no player"},
        )
    pb, _, _ = result
    pb.start_measure = {"method": "ffplay", "error": "not measured (ffplay)"}
    return pb


async def kill_orphan_audio() -> None:
    """Kill any orphan players (alsa_play.py, ffplay) from previous server sessions.

    Called at server startup. Uses SIGTERM then SIGKILL.
    Also clears the active process tracker.
    """
    global _active_proc
    _active_proc = None

    try:
        # Only kill players owned by current user (avoid killing other users' audio). The
        # ALSA player's ffmpeg decoder ends with it (a write to the closed pipe).
        proc = await asyncio.create_subprocess_exec(
            "pgrep",
            "-u",
            str(os.getuid()),
            "-f",
            r"ffplay|alsa_play\.py",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        stdout, _ = await proc.communicate()
        pids = [int(p) for p in stdout.decode().strip().split() if p.isdigit()]
        if not pids:
            return

        log.warning("Killing %d orphan player process(es): %s", len(pids), pids)
        import contextlib

        for pid in pids:
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGTERM)

        await asyncio.sleep(2)
        for pid in pids:
            with contextlib.suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)
    except Exception:
        pass
