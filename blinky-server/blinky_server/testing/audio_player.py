"""Audio playback via ffplay subprocess.

Provides async audio playback with seek and duration control, used by
the test runner to play music through the room speakers.
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass

log = logging.getLogger(__name__)


@dataclass
class PlaybackResult:
    success: bool
    audio_start_time_ms: float  # epoch ms when playback started
    duration_ms: float  # actual playback duration
    error: str | None = None


async def play_audio(
    audio_file: str,
    duration_ms: float | None = None,
    seek_sec: float | None = None,
) -> PlaybackResult:
    """Play an audio file via ffplay, blocking until complete or duration elapsed.

    Args:
        audio_file: Path to audio file (.mp3, .wav, .flac)
        duration_ms: Stop after this many ms (None = play entire file)
        seek_sec: Seek to this position before playing

    Returns:
        PlaybackResult with timing info for scoring alignment.
    """
    cmd = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]
    if seek_sec is not None:
        cmd.extend(["-ss", f"{seek_sec:.1f}"])
    if duration_ms is not None:
        cmd.extend(["-t", f"{duration_ms / 1000:.1f}"])
    cmd.append(audio_file)

    # Set audio output device. AUDIODEV selects ALSA device for ffplay.
    # Default: auto-detect USB speakers, fall back to system default.
    env = dict(__import__("os").environ)
    if "AUDIODEV" not in env:
        # Auto-detect USB audio (JBL Pebbles or similar)
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
            pass  # Fall back to system default

    log.info("Playing: %s", " ".join(cmd))
    t0 = time.time() * 1000

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
            env=env,
        )
        audio_start_time_ms = time.time() * 1000
        await proc.wait()
        elapsed = time.time() * 1000 - t0

        if proc.returncode == 0:
            return PlaybackResult(
                success=True,
                audio_start_time_ms=audio_start_time_ms,
                duration_ms=elapsed,
            )
        else:
            return PlaybackResult(
                success=False,
                audio_start_time_ms=audio_start_time_ms,
                duration_ms=elapsed,
                error=f"ffplay exited with code {proc.returncode}",
            )
    except FileNotFoundError:
        return PlaybackResult(
            success=False,
            audio_start_time_ms=t0,
            duration_ms=0,
            error="ffplay not found — install ffmpeg",
        )
    except Exception as e:
        return PlaybackResult(
            success=False,
            audio_start_time_ms=t0,
            duration_ms=time.time() * 1000 - t0,
            error=str(e),
        )


async def kill_orphan_audio() -> None:
    """Kill any orphan ffplay processes from previous test runs."""
    try:
        proc = await asyncio.create_subprocess_exec(
            "pkill",
            "-f",
            "ffplay",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
        await proc.wait()
    except Exception:
        pass
