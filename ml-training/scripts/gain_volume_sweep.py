#!/usr/bin/env python3
"""
Gain x Volume sweep: measure onset discriminability across operating conditions.

Plays test tracks at different speaker volumes and hardware gain levels,
captures the raw onset detection function (ODF) from the device stream,
and computes discriminability metrics against ground truth beats.

Metrics per (gain, volume, track) combination:
  - AUC-ROC of raw ODF (threshold-free discriminability; 0.5=random, 1.0=perfect)
  - Beat SNR: mean ODF at beats / mean ODF at non-beats
  - Transient AUC: same for the ensemble transient output
  - Signal level: mean mic level (presence check)

Designed to run on blinkyhost where devices and speakers are connected.
Uses all available devices in parallel (3 gains per audio playback).

Usage:
  python3 gain_volume_sweep.py sweep --output /mnt/storage/blinky-ml-data/calibration/gain_volume_sweep.json
  python3 gain_volume_sweep.py sweep --gains 10,30,50,70 --volumes 50,100 --tracks 3
  python3 gain_volume_sweep.py analyze /path/to/gain_volume_sweep.json
"""

import argparse
import json
import os
import subprocess
import sys
import time
import threading
from pathlib import Path

import numpy as np

try:
    import serial
except ImportError:
    serial = None  # Only needed for sweep, not analyze

# ---- Defaults ----
DEFAULT_GAINS = [10, 20, 30, 40, 50, 60, 70, 80]
DEFAULT_VOLUMES = [25, 50, 75, 100]  # percent of max
DEFAULT_PORTS = ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"]
DEFAULT_AUDIO_CARD = 2
ALSA_MAX_STEPS = 35  # JBL Pebbles PCM Playback Volume range 0-35

BEAT_WINDOW_SEC = 0.100  # +/-100ms: a sample is "near a beat" if within this
SETTLE_SEC = 2.0         # wait after gain lock before playing
LEAD_IN_SEC = 1.0        # discard stream samples before audio starts
DEFAULT_DURATION = 30    # seconds of audio per test

# Representative tracks (diverse tempos/genres) - indices into sorted file list
DEFAULT_TRACK_COUNT = 4


# =============================================================================
# Serial device reader
# =============================================================================
class DeviceReader:
    """Read serial JSON stream from a blinky device in a background thread."""

    def __init__(self, port, baudrate=115200):
        if serial is None:
            raise ImportError("pyserial required for sweep mode: pip install pyserial")
        self.port = port
        self.ser = serial.Serial(port, baudrate, timeout=0.5)
        self.samples = []
        self._running = False
        self._thread = None
        # Drain any buffered data
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def send(self, cmd):
        """Send a command and drain the response."""
        self.ser.write(f"{cmd}\n".encode())
        time.sleep(0.15)
        self.ser.reset_input_buffer()

    def start_streaming(self):
        """Begin capturing stream samples in background."""
        self.samples = []
        self.send("stream on")
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def stop_streaming(self):
        """Stop background capture and turn off stream."""
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)
        self.send("stream off")
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def _read_loop(self):
        buf = b""
        while self._running:
            try:
                chunk = self.ser.read(self.ser.in_waiting or 1)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._parse_line(line)
            except serial.SerialException:
                print(f"Serial error on {self.port}, stopping reader", file=sys.stderr)
                break
            except Exception as e:
                print(f"Error in DeviceReader for {self.port}: {e}", file=sys.stderr)
                time.sleep(0.1)
                continue

    def _parse_line(self, raw):
        try:
            text = raw.decode("utf-8", errors="replace").strip()
            if not text.startswith("{"):
                return
            data = json.loads(text)
            # Stream format: {"a":{"l":..,"t":..,"pk":..,"raw":..,"h":..},"m":{"oss":..,"e":..,...}}
            # Only parse lines that have the "a" (audio) section
            a = data.get("a")
            m = data.get("m")
            if a is None or m is None:
                return  # Skip status/beat-only lines
            wall_time = time.monotonic()
            self.samples.append({
                "wall_time": wall_time,
                "oss": m.get("oss", 0.0),
                "t": a.get("t", 0.0),
                "l": a.get("l", 0.0),
                "e": m.get("e", 0.0),
                "p": m.get("p", 0.0),
                "q": m.get("q", 0),
                "raw": a.get("raw", 0.0),
                "hwgain": a.get("h", 0),
            })
        except (json.JSONDecodeError, ValueError):
            pass

    def lock_gain(self, gain):
        self.send(f"test lock hwgain {gain}")

    def unlock_gain(self):
        self.send("test unlock hwgain")

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


# =============================================================================
# Audio / volume control
# =============================================================================
def set_volume(card, percent):
    """Set ALSA PCM volume. percent 0-100 maps to 0-ALSA_MAX_STEPS."""
    val = max(0, min(ALSA_MAX_STEPS, int(round(percent / 100.0 * ALSA_MAX_STEPS))))
    result = subprocess.run(
        ["amixer", "-c", str(card), "set", "PCM Playback Volume", str(val)],
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"amixer failed (rc={result.returncode}): {result.stderr.decode()}")
    return val


def play_audio(audio_file, duration=None):
    """Play audio via ffplay. Returns (start_time, end_time) in monotonic clock."""
    cmd = ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]
    if duration:
        cmd += ["-t", str(duration)]
    cmd.append(str(audio_file))
    start = time.monotonic()
    result = subprocess.run(cmd, timeout=duration + 30 if duration else 300)
    if result.returncode != 0:
        raise RuntimeError(f"ffplay failed (rc={result.returncode}) for {audio_file}")
    end = time.monotonic()
    return start, end


# =============================================================================
# Ground truth
# =============================================================================
def load_ground_truth(gt_file):
    """Load beat times from .beats.json, return sorted array of seconds."""
    with open(gt_file) as f:
        data = json.load(f)
    beats = [h["time"] for h in data["hits"] if h.get("expectTrigger", True)]
    return np.array(sorted(beats))


# =============================================================================
# Analysis
# =============================================================================
def compute_auc(pos_scores, neg_scores):
    """AUC-ROC via concordant pair counting (handles ties correctly)."""
    n_pos, n_neg = len(pos_scores), len(neg_scores)
    if n_pos == 0 or n_neg == 0:
        return 0.5
    # Vectorized pairwise comparison - fine for our data sizes (<5K samples)
    if n_pos * n_neg > 5_000_000:
        # Fall back to rank-based for large arrays
        return _auc_rank_based(pos_scores, neg_scores)
    concordant = (pos_scores[:, None] > neg_scores[None, :]).sum()
    tied = (pos_scores[:, None] == neg_scores[None, :]).sum()
    return float((concordant + 0.5 * tied) / (n_pos * n_neg))


def _auc_rank_based(pos_scores, neg_scores):
    """AUC via Mann-Whitney U for large arrays."""
    n_pos, n_neg = len(pos_scores), len(neg_scores)
    combined = np.concatenate([pos_scores, neg_scores])
    labels = np.concatenate([np.ones(n_pos), np.zeros(n_neg)])
    order = np.argsort(combined, kind="mergesort")
    ranked_labels = labels[order]
    # Assign average ranks for ties
    ranks = np.empty(len(combined))
    ranks[order] = np.arange(1, len(combined) + 1, dtype=float)
    # Average ties
    sorted_vals = combined[order]
    i = 0
    while i < len(sorted_vals):
        j = i + 1
        while j < len(sorted_vals) and sorted_vals[j] == sorted_vals[i]:
            j += 1
        if j > i + 1:
            avg_rank = (ranks[order[i]] + ranks[order[j - 1]]) / 2
            for k in range(i, j):
                ranks[order[k]] = avg_rank
        i = j
    pos_rank_sum = ranks[labels == 1].sum()
    u = pos_rank_sum - n_pos * (n_pos + 1) / 2
    return float(u / (n_pos * n_neg))


def estimate_latency(rel_times, oss, beat_times, duration, max_lag=1.0, step=0.01):
    """Estimate audio latency by finding the time shift that maximizes
    cross-correlation between ODF and a beat pulse train.

    Returns estimated latency in seconds (positive = ODF lags ground truth).
    """
    if len(oss) < 20 or len(beat_times) < 3:
        return 0.0

    best_corr = -1.0
    best_lag = 0.0
    lags = np.arange(0, max_lag + step, step)

    for lag in lags:
        # Shift beat times forward by lag
        is_beat = np.zeros(len(rel_times), dtype=bool)
        for bt in beat_times:
            if bt + lag > duration + 1:
                break
            is_beat |= np.abs(rel_times - (bt + lag)) <= BEAT_WINDOW_SEC
        n_beat = is_beat.sum()
        if n_beat < 3 or (~is_beat).sum() < 3:
            continue
        # Use mean difference as correlation proxy (fast)
        beat_mean = oss[is_beat].mean()
        nonbeat_mean = oss[~is_beat].mean()
        corr = beat_mean - nonbeat_mean
        if corr > best_corr:
            best_corr = corr
            best_lag = lag

    return float(best_lag)


def analyze_samples(samples, audio_start, beat_times, duration):
    """Compute discriminability metrics for captured stream samples.
    Includes automatic latency estimation via cross-correlation.
    """
    if len(samples) < 20:
        return None, []

    wall_times = np.array([s["wall_time"] for s in samples])
    oss = np.array([s["oss"] for s in samples])
    transient = np.array([s["t"] for s in samples])
    level = np.array([s["l"] for s in samples])
    energy = np.array([s["e"] for s in samples])

    # Time relative to audio start
    rel_times = wall_times - audio_start

    # Only samples during audio playback (with margin for latency)
    mask = (rel_times >= -0.5) & (rel_times <= duration + 1.5)
    rel_times = rel_times[mask]
    oss = oss[mask]
    transient = transient[mask]
    level = level[mask]
    energy = energy[mask]

    if len(oss) < 20:
        return None, []

    # Raw trace for saving (list of [relative_time, oss, transient, level])
    raw_trace = list(zip(rel_times.tolist(), oss.tolist(),
                         transient.tolist(), level.tolist()))

    # Estimate latency
    latency = estimate_latency(rel_times, oss, beat_times, duration)

    # Label with latency-corrected beat times
    shifted_beats = beat_times + latency
    is_beat = np.zeros(len(rel_times), dtype=bool)
    for bt in shifted_beats:
        if bt > duration + 1.5:
            break
        is_beat |= np.abs(rel_times - bt) <= BEAT_WINDOW_SEC

    n_beat = is_beat.sum()
    n_nonbeat = (~is_beat).sum()
    if n_beat < 3 or n_nonbeat < 3:
        return None, raw_trace

    oss_beat = oss[is_beat]
    oss_nonbeat = oss[~is_beat]
    t_beat = transient[is_beat]
    t_nonbeat = transient[~is_beat]

    oss_nb_mean = max(float(oss_nonbeat.mean()), 1e-8)
    t_nb_mean = max(float(t_nonbeat.mean()), 1e-8)

    # Peak ODF near each beat (wide window, latency-corrected)
    peak_oss_per_beat = []
    for bt in shifted_beats:
        if bt > duration + 1.5:
            break
        win = (rel_times >= bt - 0.2) & (rel_times <= bt + 0.2)
        if win.any():
            peak_oss_per_beat.append(float(oss[win].max()))
    peak_oss_per_beat = np.array(peak_oss_per_beat) if peak_oss_per_beat else np.array([0.0])

    # Non-beat baseline: median ODF at points far from any beat
    far_from_beat = np.ones(len(rel_times), dtype=bool)
    for bt in shifted_beats:
        if bt > duration + 1.5:
            break
        far_from_beat &= np.abs(rel_times - bt) > 0.2
    baseline_oss = float(np.median(oss[far_from_beat])) if far_from_beat.any() else 0.0

    return {
        "n_samples": int(len(oss)),
        "n_beat_samples": int(n_beat),
        "n_nonbeat_samples": int(n_nonbeat),
        "stream_hz": float(len(oss) / max(rel_times[-1] - rel_times[0], 0.1)),
        "estimated_latency_ms": round(latency * 1000),
        # ODF discriminability (latency-corrected)
        "oss_auc": compute_auc(oss_beat, oss_nonbeat),
        "oss_beat_mean": float(oss_beat.mean()),
        "oss_nonbeat_mean": float(oss_nonbeat.mean()),
        "oss_snr": float(oss_beat.mean() / oss_nb_mean),
        "oss_peak_at_beats_median": float(np.median(peak_oss_per_beat)),
        "oss_peak_at_beats_mean": float(np.mean(peak_oss_per_beat)),
        "oss_baseline_median": baseline_oss,
        # Transient discriminability
        "transient_auc": compute_auc(t_beat, t_nonbeat),
        "transient_beat_mean": float(t_beat.mean()),
        "transient_nonbeat_mean": float(t_nonbeat.mean()),
        "transient_snr": float(t_beat.mean() / t_nb_mean),
        # Signal presence
        "level_mean": float(level.mean()),
        "level_std": float(level.std()),
        "energy_mean": float(energy.mean()),
    }, raw_trace


# =============================================================================
# Sweep runner
# =============================================================================
def select_tracks(audio_dir, gt_dir, count):
    """Select a diverse subset of tracks that have ground truth."""
    audio_dir = Path(audio_dir)
    gt_dir = Path(gt_dir) if gt_dir != audio_dir else audio_dir

    pairs = []
    for gt in sorted(gt_dir.glob("*.beats.json")):
        stem = gt.name.replace(".beats.json", "")
        for ext in (".mp3", ".wav", ".flac"):
            audio = audio_dir / f"{stem}{ext}"
            if audio.exists():
                pairs.append((audio, gt))
                break

    if not pairs:
        print(f"ERROR: No audio+ground_truth pairs found in {audio_dir}", file=sys.stderr)
        sys.exit(1)

    # Deterministic diverse selection: evenly spaced from sorted list
    if len(pairs) <= count:
        return pairs
    indices = np.linspace(0, len(pairs) - 1, count, dtype=int)
    return [pairs[i] for i in indices]


def run_sweep(args):
    """Main sweep loop."""
    gains = [int(g) for g in args.gains.split(",")]
    volumes = [int(v) for v in args.volumes.split(",")]
    ports = [p.strip() for p in args.ports.split(",")]
    ports = [p for p in ports if Path(p).exists()]

    if not ports:
        print("ERROR: No serial ports found", file=sys.stderr)
        sys.exit(1)

    audio_dir = Path(args.audio_dir)
    gt_dir = Path(args.gt_dir) if args.gt_dir else audio_dir
    tracks = select_tracks(audio_dir, gt_dir, args.tracks)

    print(f"Sweep configuration:")
    print(f"  Gains:   {gains}")
    print(f"  Volumes: {volumes}%")
    print(f"  Ports:   {ports} ({len(ports)} devices)")
    print(f"  Tracks:  {len(tracks)}")
    print(f"  Duration: {args.duration}s per test")

    n_batches_per_vol = -(-len(gains) // len(ports))  # ceil division
    n_plays = len(volumes) * n_batches_per_vol * len(tracks)
    est_time = n_plays * (args.duration + SETTLE_SEC + 3)
    print(f"  Estimated time: {est_time / 60:.0f} minutes ({n_plays} audio plays)")
    print()

    results = []
    all_traces = []

    for vol in volumes:
        alsa_val = set_volume(args.audio_card, vol)
        print(f"=== Volume {vol}% (ALSA {alsa_val}/{ALSA_MAX_STEPS}) ===")

        # Batch gains across available devices
        gain_batches = []
        for i in range(0, len(gains), len(ports)):
            gain_batches.append(gains[i : i + len(ports)])

        for track_audio, track_gt in tracks:
            beat_times = load_ground_truth(track_gt)
            track_name = track_audio.stem
            print(f"\n  Track: {track_name} ({len(beat_times)} beats)")

            for batch in gain_batches:
                # Open devices for this batch
                devices = []
                for i, gain in enumerate(batch):
                    port = ports[i % len(ports)]
                    try:
                        dev = DeviceReader(port)
                        devices.append((dev, gain, port))
                    except Exception as e:
                        print(f"    WARN: Cannot open {port}: {e}")

                if not devices:
                    print("    ERROR: No devices available, skipping batch")
                    continue

                gain_str = ",".join(str(g) for _, g, _ in devices)
                print(f"    Gains [{gain_str}] ... ", end="", flush=True)

                try:
                    # Lock gains
                    for dev, gain, _ in devices:
                        dev.lock_gain(gain)

                    # Let AGC settle
                    time.sleep(SETTLE_SEC)

                    # Start streaming on all devices
                    for dev, _, _ in devices:
                        dev.start_streaming()

                    # Small lead-in for stream to stabilize
                    time.sleep(LEAD_IN_SEC)

                    # Play audio (all devices hear the same audio)
                    audio_start, audio_end = play_audio(
                        track_audio, duration=args.duration
                    )
                    actual_duration = audio_end - audio_start

                    # Stop streaming
                    for dev, _, _ in devices:
                        dev.stop_streaming()

                    # Analyze each device's capture
                    for dev, gain, port in devices:
                        metrics, raw_trace = analyze_samples(
                            dev.samples, audio_start, beat_times, actual_duration
                        )
                        if metrics:
                            entry = {
                                "gain": gain,
                                "volume_pct": vol,
                                "track": track_name,
                                "port": port,
                                "duration": round(actual_duration, 1),
                                **metrics,
                            }
                            # Save raw trace for re-analysis
                            if raw_trace:
                                entry["_trace_len"] = len(raw_trace)
                                all_traces.append({
                                    "gain": gain, "volume_pct": vol,
                                    "track": track_name, "port": port,
                                    "trace": raw_trace,
                                    "beat_times": beat_times.tolist(),
                                })
                            results.append(entry)
                            lat = metrics.get("estimated_latency_ms", 0)
                            print(
                                f"g{gain}:AUC={metrics['oss_auc']:.3f}(lat={lat}ms) ",
                                end="",
                                flush=True,
                            )
                        else:
                            print(f"g{gain}:FAIL ", end="", flush=True)

                    print()  # newline after batch

                finally:
                    # Always unlock and close
                    for dev, _, _ in devices:
                        try:
                            dev.unlock_gain()
                        except Exception:
                            pass
                        dev.close()

    # Restore volume to 100%
    set_volume(args.audio_card, 100)

    # Save results
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump({"sweep_results": results, "timestamp": time.time()}, f, indent=2)
    print(f"\nResults saved to {output_path}")
    print(f"Total measurements: {len(results)}")

    # Save raw traces for re-analysis (separate file, can be large)
    if all_traces:
        trace_path = output_path.with_suffix(".traces.json")
        with open(trace_path, "w") as f:
            json.dump(all_traces, f)
        print(f"Raw traces saved to {trace_path} ({len(all_traces)} traces)")

    # Print summary
    print_summary(results)

    return results


# =============================================================================
# Summary / analysis
# =============================================================================
def print_summary(results):
    """Print a summary table of results."""
    if not results:
        print("No results to summarize.")
        return

    # Aggregate by (gain, volume) averaging across tracks and devices
    from collections import defaultdict

    agg = defaultdict(list)
    for r in results:
        key = (r["gain"], r["volume_pct"])
        agg[key].append(r)

    gains = sorted(set(r["gain"] for r in results))
    volumes = sorted(set(r["volume_pct"] for r in results))

    # Show estimated latency
    latencies = [r.get("estimated_latency_ms", 0) for r in results if r.get("estimated_latency_ms", 0) > 0]
    if latencies:
        print(f"\nEstimated audio latency: median={int(np.median(latencies))}ms, "
              f"range={min(latencies)}-{max(latencies)}ms")

    print("\n" + "=" * 70)
    print("ODF AUC-ROC (higher = better discriminability, 0.5 = random)")
    print("  (latency-corrected per measurement)")
    print("=" * 70)
    header = f"{'Gain':>6}" + "".join(f"  Vol {v}%" for v in volumes)
    print(header)
    print("-" * len(header))
    for g in gains:
        row = f"{g:>6}"
        for v in volumes:
            entries = agg.get((g, v), [])
            if entries:
                mean_auc = np.mean([e["oss_auc"] for e in entries])
                row += f"  {mean_auc:6.3f}"
            else:
                row += f"  {'---':>6}"
        print(row)

    print(f"\n{'':>6}" + "".join(f"  Vol {v}%" for v in volumes))
    print("ODF Beat SNR (mean ODF at beats / mean ODF at non-beats)")
    print("-" * len(header))
    for g in gains:
        row = f"{g:>6}"
        for v in volumes:
            entries = agg.get((g, v), [])
            if entries:
                mean_snr = np.mean([e["oss_snr"] for e in entries])
                row += f"  {mean_snr:6.2f}"
            else:
                row += f"  {'---':>6}"
        print(row)

    print(f"\n{'':>6}" + "".join(f"  Vol {v}%" for v in volumes))
    print("Transient AUC-ROC")
    print("-" * len(header))
    for g in gains:
        row = f"{g:>6}"
        for v in volumes:
            entries = agg.get((g, v), [])
            if entries:
                mean_auc = np.mean([e["transient_auc"] for e in entries])
                row += f"  {mean_auc:6.3f}"
            else:
                row += f"  {'---':>6}"
        print(row)

    print(f"\n{'':>6}" + "".join(f"  Vol {v}%" for v in volumes))
    print("Mean Signal Level")
    print("-" * len(header))
    for g in gains:
        row = f"{g:>6}"
        for v in volumes:
            entries = agg.get((g, v), [])
            if entries:
                mean_lvl = np.mean([e["level_mean"] for e in entries])
                row += f"  {mean_lvl:6.3f}"
            else:
                row += f"  {'---':>6}"
        print(row)

    # Find the "cliff" — where does AUC drop below 0.7?
    print("\n" + "=" * 70)
    print("Usability assessment (AUC > 0.7 = usable, > 0.8 = good, > 0.9 = excellent)")
    print("=" * 70)
    for v in volumes:
        usable_gains = []
        for g in gains:
            entries = agg.get((g, v), [])
            if entries:
                mean_auc = np.mean([e["oss_auc"] for e in entries])
                if mean_auc >= 0.7:
                    usable_gains.append(g)
        if usable_gains:
            print(f"  Volume {v:3d}%: usable gains {min(usable_gains)}-{max(usable_gains)}")
        else:
            print(f"  Volume {v:3d}%: no usable gains found")


def analyze_file(path):
    """Load and re-analyze a saved sweep results file."""
    with open(path) as f:
        data = json.load(f)
    results = data["sweep_results"]
    print(f"Loaded {len(results)} measurements from {path}")
    print_summary(results)


# =============================================================================
# Entry point
# =============================================================================
def main():
    parser = argparse.ArgumentParser(
        description="Gain x Volume sweep for onset discriminability"
    )
    sub = parser.add_subparsers(dest="command")

    # Sweep subcommand
    sweep = sub.add_parser("sweep", help="Run the gain/volume sweep")
    sweep.add_argument(
        "--gains",
        default=",".join(str(g) for g in DEFAULT_GAINS),
        help="Comma-separated gain levels to test (default: %(default)s)",
    )
    sweep.add_argument(
        "--volumes",
        default=",".join(str(v) for v in DEFAULT_VOLUMES),
        help="Comma-separated volume percentages (default: %(default)s)",
    )
    sweep.add_argument(
        "--ports",
        default=",".join(DEFAULT_PORTS),
        help="Comma-separated serial ports (default: %(default)s)",
    )
    sweep.add_argument(
        "--audio-dir",
        default=str(Path.home() / "blinky_time/blinky-test-player/music/edm"),
        help="Directory with test audio files",
    )
    sweep.add_argument(
        "--gt-dir",
        default=None,
        help="Directory with .beats.json ground truth (default: same as audio-dir)",
    )
    sweep.add_argument(
        "--tracks",
        type=int,
        default=DEFAULT_TRACK_COUNT,
        help="Number of test tracks to use (default: %(default)s)",
    )
    sweep.add_argument(
        "--duration",
        type=int,
        default=DEFAULT_DURATION,
        help="Seconds of audio per test (default: %(default)s)",
    )
    sweep.add_argument(
        "--audio-card",
        type=int,
        default=DEFAULT_AUDIO_CARD,
        help="ALSA card number for volume control (default: %(default)s)",
    )
    sweep.add_argument(
        "--output",
        default="gain_volume_sweep.json",
        help="Output JSON file (default: %(default)s)",
    )

    # Analyze subcommand
    analyze = sub.add_parser("analyze", help="Re-analyze saved sweep results")
    analyze.add_argument("input", help="Path to saved sweep JSON file")

    args = parser.parse_args()

    if args.command == "sweep":
        run_sweep(args)
    elif args.command == "analyze":
        analyze_file(args.input)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
