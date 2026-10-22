#!/usr/bin/env python3
"""On-device onset accuracy from a workstation: play corpus tracks through this
machine's speakers, record a USB-connected device's onsets, and score them.

blinky-server runs the same evaluation on blinkyhost (`POST /api/test/validate`),
but its serial transport is Linux-only. This runner covers a device on a
Windows (or any) workstation's USB port. It reuses the server's code, so the
numbers are directly comparable:
  - track discovery and ground truth (`onsets_consensus` + human edits, cut
    to the segment played: `segment_ground_truth`),
  - stream parsing (`TestSession`, with firmware-clock timestamps),
  - scoring (`score_device_run`: onset F1/P/R at 50/70/100/150 ms, latency,
    PLP metrics with `--stream`).

Timing: each track segment is cut to a WAV with ffmpeg (sample-accurate) and
preceded by three sync clicks at irregular spacing after silence. The
device's onsets on the clicks give the true lag of this run (playback start +
speaker + detection delay), so audio start is known to within a few ms. Two
things this avoids, both measured on the Windows bench:
  - ffplay's startup delay varies with the seek position (165 ms at 0 s, 390 ms
    at 92 s), so "process start" is not "sound start";
  - the scorer's latency estimate matches each detection to the NEAREST
    reference onset, so it can't recover a lag over half the onset spacing
    (~140 ms on dense tracks), and a lag scan over music is ambiguous by a
    grid step on periodic drums.
The click-measured lag is used as the latency directly (scorer
`latency_ms=0` from music start): on the b216 corpus run a fine lag scan
over the music agreed with it within ~10 ms on every track with a clear peak,
while the scorer's own estimate wandered up to 89 ms and would have shifted
the detections by that much. The estimate is still printed (`est`) as a
diagnostic. A run whose clicks weren't detected is left out of the means.

The device only needs `debug transient on` to report onsets. `--stream` also
turns on the debug+fast music stream the server uses (PLP metrics), at the
cost of the loop time that stream formatting takes (#171). The default leaves
it off so the loop runs as it does in use.

Like the server, it sends `restore_runtime_settings` first (runtime tunables
only; nothing is saved, device identity is untouched). `--commands` applies
settings for a run, e.g. `--commands "set fluxthresh 0.4"`.

Usage:
    python tools/device_eval.py --port COM3 [--tracks techno-minimal-01 ...]
        [--duration-ms 35000] [--settle-ms 0] [--runs 1] [--stream]
        [--commands "set x 1" ...] [--out results.json]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path
from typing import Any

import numpy as np
import serial

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "blinky-server"))

from blinky_server.testing.scoring import format_score_summary, score_device_run  # noqa: E402
from blinky_server.testing.test_session import TestSession  # noqa: E402
from blinky_server.testing.track_discovery import (  # noqa: E402
    discover_tracks,
    load_ground_truth,
    load_track_manifest,
    segment_ground_truth,
)
from blinky_server.testing.types import TestData  # noqa: E402

CORPUS = REPO / "blinky-test-player" / "music" / "edm"
RATE = 44100
INTER_TRACK_GAP_S = 2.0
# Sync lead-in: silence, three clicks at irregular spacing (no grid to alias
# on), silence, then the music at MUSIC_AT_MS.
CLICKS_MS = (1000, 1650, 2850)
MUSIC_AT_MS = 4000
CLICK_SEARCH_MS = 300      # first device onset within this after a click is its detection
CLICK_SPREAD_MS = 30       # the clicks' lags must agree this closely


class DeviceLink:
    """One serial port: a reader thread routes stream lines into a TestSession
    (as blinky-server's Device._route_stream_line does) and collects command
    replies for `command()`."""

    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        self.ser.dtr = True
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.session: TestSession | None = None
        self._replies: list[str] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self) -> None:
        while not self._stop.is_set():
            try:
                raw = self.ser.readline()
            except serial.SerialException as e:
                print(f"ERROR: serial read failed: {e}", file=sys.stderr)
                self._stop.set()
                return
            if not raw:
                continue
            line = raw.decode(errors="replace").strip()
            if not line:
                continue
            if line.startswith("{"):
                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    data = None
                if isinstance(data, dict):
                    if "a" in data:
                        msg_type = "audio"
                    elif data.get("type") == "TRANSIENT":
                        msg_type = "transient"
                    else:
                        msg_type = None
                    if msg_type:
                        s = self.session
                        if s is not None and s.recording:
                            s.ingest(msg_type, data)
                        continue
            with self._lock:
                self._replies.append(line)

    def command(self, cmd: str, wait_s: float = 0.4) -> list[str]:
        with self._lock:
            self._replies.clear()
        self.ser.write((cmd + "\n").encode())
        time.sleep(wait_s)
        with self._lock:
            return list(self._replies)

    def sync_clock(self) -> float:
        """server_epoch_ms - firmware millis, from `json info` (as the server's
        _sync_clock). Fails loud: without it onset times can't be aligned."""
        for _ in range(3):
            with self._lock:
                self._replies.clear()
            t_send = time.time() * 1000
            self.ser.write(b"json info\n")
            deadline = time.time() + 2.0
            while time.time() < deadline:
                with self._lock:
                    found = next((r for r in self._replies if r.startswith("{") and '"millis"' in r), None)
                if found:
                    t_recv = time.time() * 1000
                    info = json.loads(found)
                    return (t_send + t_recv) / 2 - float(info["millis"])
                time.sleep(0.005)
        raise RuntimeError("clock sync failed: no `json info` reply with `millis`")

    def info(self) -> dict[str, Any]:
        for line in self.command("json info", 0.6):
            if line.startswith("{") and '"millis"' in line:
                result: dict[str, Any] = json.loads(line)
                return result
        raise RuntimeError("no `json info` reply")

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)
        self.ser.close()


def find_ffmpeg(explicit: str | None) -> str:
    exe = explicit or shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg  # type: ignore[import-not-found]
    except ImportError:
        sys.exit("ERROR: ffmpeg not found (install it, pass --ffmpeg, or pip install imageio-ffmpeg)")
    return str(imageio_ffmpeg.get_ffmpeg_exe())


def click() -> np.ndarray:
    """A synthetic kick: 5 ms noise attack plus a 150->50 Hz sweep, 120 ms."""
    n = int(0.12 * RATE)
    t = np.arange(n) / RATE
    freq = 50 + 100 * np.exp(-t / 0.03)
    body = np.sin(2 * np.pi * np.cumsum(freq) / RATE) * np.exp(-t / 0.04)
    attack = np.random.default_rng(0).uniform(-1, 1, n) * np.exp(-t / 0.005)
    return 0.9 * (0.8 * body + 0.4 * attack) / 1.2


def build_wav(ffmpeg: str, src: str, seek_sec: float, duration_ms: float, dst: Path) -> None:
    """Sync clicks, then exactly [seek, seek + duration) of the track."""
    pcm = subprocess.run([ffmpeg, "-v", "error", "-ss", f"{seek_sec:.3f}", "-i", src,
                          "-t", f"{duration_ms / 1000:.3f}", "-ac", "1", "-ar", str(RATE),
                          "-f", "s16le", "-"], check=True, capture_output=True).stdout
    music = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768
    if len(music) < RATE:
        raise RuntimeError(f"ffmpeg decoded only {len(music)} samples from {src} at {seek_sec} s")
    lead = np.zeros(int(MUSIC_AT_MS * RATE / 1000), dtype=np.float32)
    k = click()
    for c in CLICKS_MS:
        i = int(c * RATE / 1000)
        lead[i:i + len(k)] += k
    audio = np.clip(np.concatenate([lead, music]), -1, 1)
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((audio * 32767).astype(np.int16).tobytes())


def play_wav(path: Path, ffplay: str | None) -> float:
    """Play a WAV, blocking. Returns the epoch ms at which playback was asked for."""
    if os.name == "nt":
        import winsound

        t = time.time() * 1000
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
        return t
    if not ffplay:
        sys.exit("ERROR: ffplay not found (install ffmpeg, or pass --ffplay)")
    t = time.time() * 1000
    subprocess.run([ffplay, "-nodisp", "-autoexit", "-loglevel", "quiet", str(path)], check=True)
    return t


def click_lag(det_ms: list[float]) -> tuple[float | None, str]:
    """Lag of this run from the sync clicks, or (None, reason)."""
    lags = []
    for c in CLICKS_MS:
        after = [d - c for d in det_ms if c <= d < c + CLICK_SEARCH_MS]
        if after:
            lags.append(min(after))
    if len(lags) < 2:
        return None, f"device fired on {len(lags)} of {len(CLICKS_MS)} sync clicks (turn the volume up?)"
    if max(lags) - min(lags) > CLICK_SPREAD_MS:
        return None, f"sync click lags disagree: {[round(x) for x in lags]} ms"
    return float(statistics.median(lags)), ""


def settle(data: TestData, settle_ms: float, music_start_ms: float) -> TestData:
    if settle_ms <= 0:
        return data
    cutoff = music_start_ms + settle_ms
    return TestData(
        duration=data.duration,
        start_time=data.start_time,
        transients=[t for t in data.transients if t.timestamp_ms >= cutoff],
        music_states=[s for s in data.music_states if s.timestamp_ms >= cutoff],
        signal_frames=[f for f in data.signal_frames if f.timestamp_ms >= cutoff],
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--track-dir", default=str(CORPUS))
    ap.add_argument("--tracks", nargs="*", help="track stems (default: whole corpus)")
    ap.add_argument("--duration-ms", type=float, default=35000, help="music per track (server default)")
    ap.add_argument("--settle-ms", type=float, default=0, help="ignore the first N ms of each track's music")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--stream", action="store_true",
                    help="also stream debug+fast music state (PLP metrics; costs loop time, #171)")
    ap.add_argument("--commands", nargs="*", default=[], help="settings applied after the reset")
    ap.add_argument("--no-reset", action="store_true", help="skip restore_runtime_settings")
    ap.add_argument("--ffmpeg")
    ap.add_argument("--ffplay", default=shutil.which("ffplay"), help="playback off Windows")
    ap.add_argument("--out", help="write the full per-track results as JSON")
    args = ap.parse_args()

    ffmpeg = find_ffmpeg(args.ffmpeg)
    tracks = discover_tracks(args.track_dir)
    if args.tracks:
        missing = set(args.tracks) - {t["name"] for t in tracks}
        if missing:
            sys.exit(f"ERROR: no audio + onsets_consensus for: {', '.join(sorted(missing))}")
        tracks = [t for t in tracks if t["name"] in args.tracks]
    if not tracks:
        sys.exit(f"ERROR: no tracks found in {args.track_dir}")
    manifest = load_track_manifest(args.track_dir)

    link = DeviceLink(args.port)
    results: list[dict[str, Any]] = []
    tmpdir = Path(tempfile.mkdtemp(prefix="device_eval_"))
    info_end: dict[str, Any] = {}
    try:
        info = link.info()
        version = info.get("version")
        device_id = (info.get("device") or {}).get("id")
        overruns0 = info.get("audioOverruns")
        print(f"device {device_id} {version} on {args.port}; {len(tracks)} track(s) x {args.runs} run(s), "
              f"{args.duration_ms / 1000:.0f} s each, stream {'debug+fast' if args.stream else 'off'}")
        if not args.no_reset:
            link.command("restore_runtime_settings")
        for c in args.commands:
            reply = link.command(c)
            if any("unknown" in r.lower() or "error" in r.lower() for r in reply):
                sys.exit(f"ERROR: setup command {c!r} failed: {reply}")
        offset = link.sync_clock()
        if args.stream:
            link.command("stream debug")
            link.command("stream fast")
        link.command("debug transient on")

        print(f"\n{'track':34s} {'F1':>6s} {'P':>6s} {'R':>6s} {'F1@50':>6s} {'det':>5s} {'ref':>5s} "
              f"{'sync':>5s} {'est':>5s} {'rate/s':>7s}")
        for track in tracks:
            seek = float(manifest.get(track["name"], {}).get("seekOffset", 0))
            gt = segment_ground_truth(load_ground_truth(track["ground_truth"], track.get("human_edits")),
                                      seek, args.duration_ms)
            wav = tmpdir / f"{track['name']}.wav"
            build_wav(ffmpeg, track["audio_file"], seek, args.duration_ms, wav)
            for run in range(args.runs):
                session = TestSession()
                session.set_clock_offset(offset)
                link.session = session
                session.start_recording()
                try:
                    play_start = play_wav(wav, args.ffplay)
                finally:
                    data = session.stop_recording()
                    link.session = None
                lag, why = click_lag([t.timestamp_ms - play_start for t in data.transients])
                entry: dict[str, Any] = {"track": track["name"], "run": run + 1, "seek": seek}
                if lag is None:
                    print(f"{track['name']:34s} [SYNC FAILED] {why}: run left out", flush=True)
                    entry["error"] = why
                    results.append(entry)
                else:
                    music_start = play_start + MUSIC_AT_MS + lag
                    data = settle(data, args.settle_ms, music_start)
                    summary = format_score_summary(score_device_run(data, music_start, gt, latency_ms=0.0))
                    ot = summary["onsetTracking"]
                    est = summary["timing"]["latencyMs"]
                    print(f"{track['name']:34s} {ot['f1']:6.3f} {ot['precision']:6.3f} {ot['recall']:6.3f} "
                          f"{ot['f1_50ms']:6.3f} {ot['count']:5d} {ot['refOnsets']:5d} "
                          f"{lag:5.0f} {est if est is not None else '-':>5} "
                          f"{summary['diagnostics']['onsetRate']:7.1f}", flush=True)
                    entry.update({
                        "score": summary, "sync_lag_ms": lag, "ok": True,
                        # Raw onsets in ms from music start (click-synced), for offline analysis.
                        "onsets_ms": [round(t.timestamp_ms - music_start, 1) for t in data.transients
                                      if t.timestamp_ms >= music_start],
                        "strengths": [t.strength for t in data.transients if t.timestamp_ms >= music_start],
                    })
                    results.append(entry)
                time.sleep(INTER_TRACK_GAP_S)
                # Re-sync between tracks: the two clocks drift a few ms per minute.
                link.command("debug transient off")
                offset = link.sync_clock()
                link.command("debug transient on")
    finally:
        try:
            link.command("debug transient off")
            if args.stream:
                link.command("stream off")
            info_end = link.info()
        except (serial.SerialException, RuntimeError, OSError) as e:  # report, don't mask the original error
            print(f"WARN: cleanup failed: {e}", file=sys.stderr)
        link.close()
        shutil.rmtree(tmpdir, ignore_errors=True)

    good = [r for r in results if r.get("ok")]
    bad = [r["track"] for r in results if not r.get("ok")]
    if bad:
        print(f"\nERROR: {len(bad)} run(s) unreliable and left out of the means: {', '.join(bad)}")
    if good:
        def mean(k: str) -> float:
            return statistics.mean(r["score"]["onsetTracking"][k] for r in good)

        f1s = [r["score"]["onsetTracking"]["f1"] for r in good]
        print(f"\nmean F1 {mean('f1'):.3f}  P {mean('precision'):.3f}  R {mean('recall'):.3f}  "
              f"F1@50 {mean('f1_50ms'):.3f}  F1@70 {mean('f1_70ms'):.3f}  "
              f"(median F1 {statistics.median(f1s):.3f}, {len(good)} run(s))")
    overruns1 = info_end.get("audioOverruns")
    if overruns0 is not None and overruns1 is not None and overruns1 != overruns0:
        print(f"WARN: audio overruns grew {overruns0} -> {overruns1} during the eval")
    if info_end and info_end.get("version") != version:
        print(f"WARN: device version changed during the eval: {version} -> {info_end.get('version')}")
    if args.out:
        Path(args.out).write_text(json.dumps({
            "device": device_id, "version": version, "port": args.port, "stream": args.stream,
            "commands": args.commands, "duration_ms": args.duration_ms, "settle_ms": args.settle_ms,
            "results": results,
        }, indent=1))
        print(f"results: {args.out}")
    return 0 if good and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
