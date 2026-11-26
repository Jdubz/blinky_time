#!/usr/bin/env python3
"""Play an audio file through ALSA and measure when its first sample reached the DAC.

Run as a subprocess by ``audio_player.play_audio`` (standalone: standard library only, so a
fault in the ctypes calls can't take the server down, and the server kills it like it killed
ffplay). ffmpeg decodes exactly ``[seek, seek + duration)`` of the file to 16-bit PCM
(an input ``-ss`` decodes and discards up to the position: sample-accurate), and this writes
it to the PCM with ``snd_pcm_writei`` through libasound, which ffplay used too.

The measurement (ALSA's timestamping, kernel ``Documentation/sound/designs/timestamping.rst``;
alsa-lib ``snd_pcm_delay``: "the overall latency from the write call to the final DAC"): each
``snd_pcm_status`` after a write gives a system timestamp T and the delay D (frames written
but not yet played). With W frames written so far, frame W is heard at T + D/rate, so frame 0
was at ``T - (W - D) / rate``. The estimates over the first seconds are summarised by their
median, and their spread is reported (on USB audio the driver's delay comes from the bus
frame counter: about 1 ms, per the kernel doc). This is PortAudio's ``outputBufferDacTime``
("the time when the first sample of the output buffer will output the DAC"), which its ALSA
host API computes from the same status.

What it can't see is downstream of the USB link: the speaker's own processing and the air.
Those are fixed for a rig; the caller adds them as a calibrated constant.

Output, JSON lines on stdout:
  {"event": "start", "dac_start_epoch_ms", "spread_ms", "n", "clock", "trigger_epoch_ms", ...}
    (dac_start_epoch_ms is None, with "error", when no estimate could be made)
  {"event": "end", "frames", "xruns"}
  {"event": "error", "stage", "error"}
Exit status: 0 played; 2 nothing was played (the caller may fall back to another player);
3 failed after audio started.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import ctypes.util
import errno
import json
import statistics
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from typing import IO, Any

RATE = 48000  # the test box speaker's only rate; plughw converts for any other device
CHANNELS = 2
BYTES_PER_FRAME = 2 * CHANNELS
BUFFER_US = 200_000  # ALSA buffer: room for this process to be late without an underrun
CHUNK_FRAMES = 2400  # 50 ms per write
MEASURE_N = 40  # estimates (one per write) summarised: the first ~2 s
MIN_ESTIMATES = 5
EXIT_NOTHING_PLAYED = 2  # the caller may play the file another way
EXIT_FAILED_PLAYING = 3

# alsa/pcm.h
SND_PCM_STREAM_PLAYBACK = 0
SND_PCM_NONBLOCK = 1
SND_PCM_FORMAT_S16_LE = 2
SND_PCM_ACCESS_RW_INTERLEAVED = 3
SND_PCM_TSTAMP_ENABLE = 1
SND_PCM_STATE_PREPARED = 2
SND_PCM_STATE_RUNNING = 3

SND_PCM_TSTAMP_TYPE_MONOTONIC = 1

# The clocks an ALSA timestamp can be on (snd_pcm_tstamp_type_t: gettimeofday, monotonic,
# monotonic_raw). configure() asks for monotonic. Only if the PCM refuses is a timestamp put on
# the clock it is nearest to now, which works for fresh timestamps: monotonic and
# monotonic_raw can be hundreds of ms apart on a long-running host, so an old one could land
# on the wrong one.
_CLOCKS = {
    "realtime": time.CLOCK_REALTIME,
    "monotonic": time.CLOCK_MONOTONIC,
    "monotonic_raw": getattr(time, "CLOCK_MONOTONIC_RAW", time.CLOCK_MONOTONIC),
}


class Timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]

    def seconds(self) -> float:
        return float(self.tv_sec) + float(self.tv_nsec) / 1e9


class Alsa:
    """The few libasound calls this needs."""

    def __init__(self) -> None:
        name = ctypes.util.find_library("asound") or "libasound.so.2"
        lib = ctypes.CDLL(name)
        p, c_int, c_long, c_ulong = ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_ulong
        sig = {
            "snd_pcm_open": (c_int, [ctypes.POINTER(p), ctypes.c_char_p, c_int, c_int]),
            "snd_pcm_nonblock": (c_int, [p, c_int]),
            "snd_pcm_set_params": (
                c_int,
                [p, c_int, c_int, ctypes.c_uint, ctypes.c_uint, c_int, ctypes.c_uint],
            ),
            "snd_pcm_get_params": (c_int, [p, ctypes.POINTER(c_ulong), ctypes.POINTER(c_ulong)]),
            "snd_pcm_writei": (c_long, [p, p, c_ulong]),
            "snd_pcm_recover": (c_int, [p, c_int, c_int]),
            "snd_pcm_start": (c_int, [p]),
            "snd_pcm_state": (c_int, [p]),
            "snd_pcm_drain": (c_int, [p]),
            "snd_pcm_close": (c_int, [p]),
            "snd_pcm_sw_params_sizeof": (ctypes.c_size_t, []),
            "snd_pcm_sw_params_current": (c_int, [p, p]),
            "snd_pcm_sw_params_set_tstamp_mode": (c_int, [p, p, c_int]),
            "snd_pcm_sw_params_set_tstamp_type": (c_int, [p, p, c_int]),
            "snd_pcm_sw_params": (c_int, [p, p]),
            "snd_pcm_status_sizeof": (ctypes.c_size_t, []),
            "snd_pcm_status": (c_int, [p, p]),
            "snd_pcm_status_get_state": (c_int, [p]),
            "snd_pcm_status_get_delay": (c_long, [p]),
            "snd_pcm_status_get_htstamp": (None, [p, ctypes.POINTER(Timespec)]),
            "snd_pcm_status_get_trigger_htstamp": (None, [p, ctypes.POINTER(Timespec)]),
            "snd_strerror": (ctypes.c_char_p, [c_int]),
        }
        for fn, (res, args) in sig.items():
            f = getattr(lib, fn)
            f.restype, f.argtypes = res, args
        self.lib = lib

    def check(self, rc: int, what: str) -> int:
        if rc < 0:
            raise OSError(-rc, f"{what}: {self.lib.snd_strerror(rc).decode()}")
        return rc


def to_epoch_ms(ts: float, clock: str | None = None) -> tuple[float, str] | None:
    """An ALSA timestamp on the host wall clock (epoch ms), and the clock it was on: ``clock``
    when it is known (None if the timestamp is more than a minute from it now: a zero or
    garbage), else the nearest one now (None if it is more than 1 s from all of them)."""
    if clock is not None:
        clk = _CLOCKS[clock]
        if abs(time.clock_gettime(clk) - ts) > 60.0:
            return None
        return (ts + time.time() - time.clock_gettime(clk)) * 1000.0, clock
    best = None
    for name, clk in _CLOCKS.items():
        now = time.clock_gettime(clk)
        if abs(now - ts) < 1.0 and (best is None or abs(now - ts) < best[0]):
            best = (abs(now - ts), name, clk)
    if best is None:
        return None
    _, name, clk = best
    offset = time.time() - time.clock_gettime(clk)
    return (ts + offset) * 1000.0, name


def emit(out: IO[str], event: str, **kw: Any) -> None:
    out.write(json.dumps({"event": event, **kw}) + "\n")
    out.flush()


def read_up_to(f: IO[bytes], n: int) -> bytes:
    """Up to n bytes, fewer only at EOF."""
    buf = b""
    while len(buf) < n:
        b = f.read(n - len(buf))
        if not b:
            break
        buf += b
    return buf


class Meter:
    """Frame 0's time at the DAC, from status snapshots taken after writes."""

    def __init__(self, alsa: Alsa, pcm: ctypes.c_void_p, clock: str | None = None) -> None:
        self.alsa, self.pcm = alsa, pcm
        self.status = ctypes.create_string_buffer(alsa.lib.snd_pcm_status_sizeof())
        self.estimates: list[float] = []
        self.known_clock = clock  # the timestamps' clock, when configure() could set it
        self.clock: str | None = clock
        self.trigger_ms: float | None = None
        self.rejected = 0
        self.done = False

    def sample(self, written: int) -> None:
        if self.done:
            return
        lib = self.alsa.lib
        if lib.snd_pcm_status(self.pcm, self.status) < 0:
            self.rejected += 1
            return
        if lib.snd_pcm_status_get_state(self.status) != SND_PCM_STATE_RUNNING:
            return
        ts = Timespec()
        lib.snd_pcm_status_get_htstamp(self.status, ctypes.byref(ts))
        delay = lib.snd_pcm_status_get_delay(self.status)
        at = to_epoch_ms(ts.seconds(), self.known_clock)
        if at is None or delay < 0 or delay > written:
            self.rejected += 1
            return
        t_ms, self.clock = at
        self.estimates.append(t_ms - (written - delay) * 1000.0 / RATE)
        if self.trigger_ms is None:
            trig = Timespec()
            lib.snd_pcm_status_get_trigger_htstamp(self.status, ctypes.byref(trig))
            t = to_epoch_ms(trig.seconds(), self.known_clock)
            self.trigger_ms = t[0] if t else None

    def report(self, out: IO[str], extra: dict[str, Any]) -> None:
        self.done = True
        fields: dict[str, Any] = {
            "n": len(self.estimates),
            "rejected": self.rejected,
            "clock": self.clock,
            "trigger_epoch_ms": self.trigger_ms,
            **extra,
        }
        if len(self.estimates) < MIN_ESTIMATES:
            emit(
                out,
                "start",
                dac_start_epoch_ms=None,
                error=f"{len(self.estimates)} timestamp estimates (< {MIN_ESTIMATES})",
                **fields,
            )
            return
        est = self.estimates
        emit(
            out,
            "start",
            dac_start_epoch_ms=statistics.median(est),
            spread_ms=max(est) - min(est),
            **fields,
        )


def open_pcm(alsa: Alsa, name: str) -> ctypes.c_void_p:
    """Open the PCM without waiting: a blocking open of a device another process holds waits
    for it to be freed, and nothing above would time it out. Non-blocking, a busy device is
    EBUSY at once (nothing played: the caller falls back). Writes block again after."""
    pcm = ctypes.c_void_p()
    alsa.check(
        alsa.lib.snd_pcm_open(
            ctypes.byref(pcm), name.encode(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK
        ),
        f"open {name}",
    )
    rc = alsa.lib.snd_pcm_nonblock(pcm, 0)
    if rc < 0:
        alsa.lib.snd_pcm_close(pcm)
        alsa.check(rc, f"blocking mode on {name}")
    return pcm


def configure(alsa: Alsa, pcm: ctypes.c_void_p) -> tuple[int, int, str | None]:
    """The stream's format and buffer, and timestamps on: with the software params' default
    timestamp mode (none), the kernel leaves a running stream's status timestamp at zero
    (``snd_pcm_status64``: the tstamp is filled only for SND_PCM_TSTAMP_ENABLE), so there would
    be nothing to date the DAC by. dmix turns it on for its own slave the same way
    (alsa-lib ``pcm_direct.c``). Returns the buffer and period sizes (frames) and the
    timestamps' clock (None if the PCM wouldn't take monotonic)."""
    lib = alsa.lib
    alsa.check(
        lib.snd_pcm_set_params(
            pcm,
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            CHANNELS,
            RATE,
            1,
            BUFFER_US,
        ),
        "set_params",
    )
    sw = ctypes.create_string_buffer(lib.snd_pcm_sw_params_sizeof())
    alsa.check(lib.snd_pcm_sw_params_current(pcm, sw), "sw_params_current")
    alsa.check(
        lib.snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE),
        "sw_params_set_tstamp_mode",
    )
    # The clock, too, so a timestamp's clock is known rather than guessed (alsa-lib >= 1.0.29).
    clock = (
        "monotonic"
        if lib.snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC) >= 0
        else None
    )
    alsa.check(lib.snd_pcm_sw_params(pcm, sw), "sw_params")
    buf_frames, period_frames = ctypes.c_ulong(), ctypes.c_ulong()
    alsa.check(
        lib.snd_pcm_get_params(pcm, ctypes.byref(buf_frames), ctypes.byref(period_frames)),
        "get_params",
    )
    return buf_frames.value, period_frames.value, clock


def ffmpeg_cmd(path: str, seek: float | None, duration: float | None) -> list[str]:
    cmd = ["ffmpeg", "-nostdin", "-v", "error"]
    if seek:
        cmd += ["-ss", f"{seek:.3f}"]
    cmd += ["-i", path]
    if duration is not None:
        cmd += ["-t", f"{duration:.3f}"]
    return [*cmd, "-f", "s16le", "-ac", str(CHANNELS), "-ar", str(RATE), "-"]


def play(args: argparse.Namespace, out: IO[str]) -> int:
    # ffmpeg's messages go to a file, not a pipe: a pipe nobody reads until the end would fill
    # on a run of decode warnings and stall the decoder mid-play (an underrun).
    with tempfile.TemporaryFile() as log:
        try:
            dec = subprocess.Popen(
                ffmpeg_cmd(args.file, args.seek, args.duration),
                stdout=subprocess.PIPE,
                stderr=log,
            )
        except OSError as e:
            emit(out, "error", stage="decode", error=f"ffmpeg: {e}")
            return EXIT_NOTHING_PLAYED

        def errors() -> str:
            log.seek(0)
            return log.read().decode(errors="replace").strip()[-300:]

        try:
            return _play(args, out, dec, errors)
        finally:
            with contextlib.suppress(OSError):
                dec.kill()
            dec.wait()


def _play(
    args: argparse.Namespace,
    out: IO[str],
    dec: subprocess.Popen[bytes],
    errors: Callable[[], str],
) -> int:
    assert dec.stdout is not None
    # Decode the first buffer's worth before opening the device: the stream starts full.
    pending = read_up_to(dec.stdout, RATE * BUFFER_US // 1_000_000 * BYTES_PER_FRAME)
    if len(pending) < BYTES_PER_FRAME:
        dec.wait()
        err = errors()
        emit(out, "error", stage="decode", error=f"ffmpeg gave no audio: {err}")
        return EXIT_NOTHING_PLAYED
    try:
        alsa = Alsa()
        pcm = open_pcm(alsa, args.device)
    except OSError as e:
        emit(out, "error", stage="open", error=str(e))
        return EXIT_NOTHING_PLAYED
    started = False
    try:
        try:
            buf_frames, period_frames, clock = configure(alsa, pcm)
        except OSError as e:
            emit(out, "error", stage="open", error=str(e))
            return EXIT_NOTHING_PLAYED
        info = {
            "device": args.device,
            "rate": RATE,
            "buffer_frames": buf_frames,
            "period_frames": period_frames,
        }
        meter = Meter(alsa, pcm, clock)
        written = xruns = 0
        eof = False
        while True:
            if len(pending) < CHUNK_FRAMES * BYTES_PER_FRAME and not eof:
                more = read_up_to(dec.stdout, CHUNK_FRAMES * BYTES_PER_FRAME)
                eof = not more
                pending += more
            frames = len(pending) // BYTES_PER_FRAME
            if frames == 0:
                break
            n = min(frames, CHUNK_FRAMES)
            r = alsa.lib.snd_pcm_writei(pcm, pending, n)
            if r < 0:
                if r == -errno.EPIPE:
                    # An underrun: the audio after it is late by the gap, so the start no longer
                    # dates the rest. Reported; the caller doesn't trust the measurement.
                    xruns += 1
                alsa.check(alsa.lib.snd_pcm_recover(pcm, r, 1), "write")
                continue
            started = True
            pending = pending[r * BYTES_PER_FRAME :]
            written += r
            meter.sample(written)
            if not meter.done and len(meter.estimates) >= MEASURE_N:
                meter.report(out, info)
        # A file shorter than the buffer never filled it, so the stream hasn't started.
        if alsa.lib.snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED:
            alsa.check(alsa.lib.snd_pcm_start(pcm), "start")
        deadline = time.monotonic() + 1.0
        while not meter.done and len(meter.estimates) < MEASURE_N and time.monotonic() < deadline:
            meter.sample(written)
            time.sleep(0.005)
        if not meter.done:
            meter.report(out, info)
        alsa.lib.snd_pcm_drain(pcm)
        rc = dec.wait()
        if rc != 0:
            emit(out, "error", stage="decode", error=f"ffmpeg exited {rc}: {errors()}")
            return EXIT_FAILED_PLAYING
        emit(out, "end", frames=written, xruns=xruns)
        return 0
    except OSError as e:
        emit(out, "error", stage="play" if started else "open", error=str(e))
        return EXIT_FAILED_PLAYING if started else EXIT_NOTHING_PLAYED
    finally:
        alsa.lib.snd_pcm_close(pcm)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--device", default="default", help="ALSA PCM name")
    ap.add_argument("--seek", type=float, default=None, help="start this far in (s)")
    ap.add_argument("--duration", type=float, default=None, help="play this long (s)")
    return play(ap.parse_args(argv), sys.stdout)


if __name__ == "__main__":
    sys.exit(main())
