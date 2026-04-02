"""Audio playback via ffplay subprocess — explicit process tracking.

The AudioPlayer singleton tracks the ffplay subprocess globally. Only one
audio playback can be active at a time. Starting a new playback kills the
previous one. Server startup kills any orphan ffplay from prior sessions.
"""

from __future__ import annotations

import asyncio
import logging
import os
import signal
import time
from dataclasses import dataclass

log = logging.getLogger(__name__)


@dataclass
class PlaybackResult:
    success: bool
    audio_start_time_ms: float  # epoch ms when playback started
    duration_ms: float  # actual playback duration
    error: str | None = None


# ---------------------------------------------------------------------------
# Singleton audio player — guarantees at most one ffplay at a time
# ---------------------------------------------------------------------------

_active_proc: asyncio.subprocess.Process | None = None
# Cached on first call, never refreshed. If the USB speaker is hot-plugged
# after server start, restart the server to re-detect. This is intentional —
# audio device changes mid-test would be more disruptive than stale cache.
_cached_audio_env: dict[str, str] | None = None


async def _get_audio_env() -> dict[str, str]:
    """Get environment with AUDIODEV set to USB speakers if available."""
    global _cached_audio_env
    if _cached_audio_env is not None:
        return _cached_audio_env

    env = dict(os.environ)
    if "AUDIODEV" not in env:
        try:
            result = await asyncio.create_subprocess_exec(
                "aplay",
                "-l",
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
            stdout, _ = await result.communicate()
            for line in stdout.decode().split("\n"):
                if "USB Audio" in line or "Pebbles" in line:
                    card = line.split("card ")[1].split(":")[0]
                    env["AUDIODEV"] = f"hw:{card},0"
                    log.info("Auto-detected audio device: %s", env["AUDIODEV"])
                    break
        except Exception:
            pass
    _cached_audio_env = env
    return env


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
    """Play an audio file via ffplay, blocking until complete.

    Kills any previously playing audio first. The subprocess is tracked
    globally so it can be killed on cancellation, server shutdown, or
    when a new test starts.
    """
    global _active_proc

    # Kill any previous playback — only one audio at a time
    await stop_audio()

    cmd = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]
    if seek_sec is not None:
        cmd.extend(["-ss", f"{seek_sec:.1f}"])
    if duration_ms is not None:
        cmd.extend(["-t", f"{duration_ms / 1000:.1f}"])
    cmd.append(audio_file)

    env = await _get_audio_env()

    log.info("Playing: %s", " ".join(cmd))
    t0 = time.time() * 1000

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
            env=env,
        )
        _active_proc = proc
        audio_start_time_ms = time.time() * 1000

        await proc.wait()

        # Clear tracking (process finished normally)
        if _active_proc is proc:
            _active_proc = None

        elapsed = time.time() * 1000 - t0

        if proc.returncode == 0:
            return PlaybackResult(
                success=True,
                audio_start_time_ms=audio_start_time_ms,
                duration_ms=elapsed,
            )
        return PlaybackResult(
            success=False,
            audio_start_time_ms=audio_start_time_ms,
            duration_ms=elapsed,
            error=f"ffplay exited with code {proc.returncode}",
        )
    except FileNotFoundError:
        _active_proc = None
        return PlaybackResult(
            success=False,
            audio_start_time_ms=t0,
            duration_ms=0,
            error="ffplay not found — install ffmpeg",
        )
    except asyncio.CancelledError:
        # Kill ffplay when the task is cancelled (test failure, server shutdown)
        await stop_audio()
        raise
    except Exception as e:
        await stop_audio()
        return PlaybackResult(
            success=False,
            audio_start_time_ms=t0,
            duration_ms=time.time() * 1000 - t0,
            error=str(e),
        )


async def kill_orphan_audio() -> None:
    """Kill any orphan ffplay processes from previous server sessions.

    Called at server startup. Uses SIGTERM then SIGKILL.
    Also clears the active process tracker.
    """
    global _active_proc
    _active_proc = None

    try:
        # Only kill ffplay owned by current user (avoid killing other users' audio)
        proc = await asyncio.create_subprocess_exec(
            "pgrep",
            "-u",
            str(os.getuid()),
            "-f",
            "ffplay",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        stdout, _ = await proc.communicate()
        pids = [int(p) for p in stdout.decode().strip().split() if p.isdigit()]
        if not pids:
            return

        log.warning("Killing %d orphan ffplay process(es): %s", len(pids), pids)
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
