#!/usr/bin/env python3
"""Replay captured device mel data through offline model for parity analysis.

Loads JSONL from POST /api/test/capture-nn/{device_id} (blinky-server),
extracts the 26-band mel arrays the device NN actually saw, runs the same
PyTorch model offline, and compares onset predictions.

This isolates the sim-to-real gap:
  - If offline model on device mel also scores ~0.47 → gap is mel domain shift
  - If offline model on device mel scores ~0.75 → gap is quantization/inference

Usage:
    python scripts/replay_device_capture.py \\
        --capture capture.jsonl \\
        --model outputs/v16-no-delta/best_model.pt \\
        --config configs/conv1d_w16_onset_v16.yaml

    # With ground truth onset labels for F1 scoring:
    python scripts/replay_device_capture.py \\
        --capture capture.jsonl \\
        --model outputs/v16-no-delta/best_model.pt \\
        --config configs/conv1d_w16_onset_v16.yaml \\
        --labels ../blinky-test-player/music/edm/techno-minimal-01.beats.json
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from scripts.audio import load_config
from evaluate import load_model


def load_capture(jsonl_path: str) -> dict:
    """Parse JSONL capture file from blinky-server capture-nn endpoint."""
    frames = []
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            frame = json.loads(line)
            if frame.get("type") == "NN":
                frames.append(frame)

    if not frames:
        print("ERROR: No NN frames found in capture file", file=sys.stderr)
        sys.exit(1)

    mel = np.array([f["mel"] for f in frames], dtype=np.float32)
    # Prefer raw NN activation (nna) over gated pulse (onset) when available.
    # Post-b107 firmware: onset=gated pulse (mostly zero), nna=raw continuous activation.
    # Pre-b107 captures: onset=raw continuous activation (old meaning), nna absent.
    has_nna = "nna" in frames[0]
    if not has_nna:
        print("  WARNING: No 'nna' field — using 'onset'.", file=sys.stderr)
        print("  In post-b107 captures 'onset' may be gated; in pre-b107 captures it may be raw.",
              file=sys.stderr)
        print("  Post-b107 captures should have 'nna', and parity metrics may be misleading when",
              file=sys.stderr)
        print("  falling back to gated 'onset'.", file=sys.stderr)
    if has_nna:
        device_onset = np.array([f["nna"] for f in frames], dtype=np.float32)
    else:
        device_onset = np.array([f["onset"] for f in frames], dtype=np.float32)
    timestamps = np.array([f["ts"] for f in frames], dtype=np.float64)
    nn_active = all(f.get("nn", 0) == 1 for f in frames)

    # Frame counter (b2xx+, #170): the device emits one NN line per loop update,
    # so frames processed in a catch-up update are missing. "fc" exposes them.
    has_fc = any("fc" in f for f in frames)
    if has_fc:
        if not all("fc" in f for f in frames):
            print("ERROR: some NN lines lack 'fc' (mixed firmware in one capture?)", file=sys.stderr)
            sys.exit(1)
        fc = np.array([f["fc"] for f in frames], dtype=np.int64)
        steps = np.diff(fc)
        if np.any(steps <= 0):
            print(f"ERROR: frame counter not increasing at {int(np.sum(steps <= 0))} line(s) "
                  "(device reboot or merged captures?)", file=sys.stderr)
            sys.exit(1)
        missing = int(np.sum(steps - 1))
        n_missing = missing
        total = int(fc[-1] - fc[0] + 1)
        if missing:
            print(f"  WARNING: {missing}/{total} frames ({100.0 * missing / total:.1f}%) missing "
                  f"across {int(np.sum(steps > 1))} gap(s); parity uses gap-free windows only.",
                  file=sys.stderr)
    else:
        fc = None
        n_missing = None
        print("  WARNING: no 'fc' field (pre-#170 capture): dropped frames cannot be detected; "
              "windows spanning a gap will corrupt parity silently.", file=sys.stderr)

    # Compute actual frame rate from timestamps
    if len(timestamps) > 1:
        dt_ms = np.median(np.diff(timestamps))
        actual_fps = 1000.0 / dt_ms
    else:
        actual_fps = 62.5

    return {
        "mel": mel,
        "device_onset": device_onset,
        "timestamps": timestamps,
        "nn_active": nn_active,
        "actual_fps": actual_fps,
        "n_frames": len(frames),
        "onset_field": "nna" if has_nna else "onset",
        "fc": fc,
        "n_missing": n_missing,  # None = unknown (pre-#170 capture)
    }


def gap_free_mask(fc: np.ndarray | None, window: int) -> np.ndarray | None:
    """True where the frame and the window-1 frames before it are consecutive."""
    if fc is None:
        return None
    gap_before = np.concatenate([[False], np.diff(fc) != 1])  # line i follows a gap
    # Lines since the most recent gap (inclusive of the current line).
    last_gap = np.maximum.accumulate(np.where(gap_before, np.arange(len(fc)), 0))
    since = np.arange(len(fc)) - last_gap + 1
    return since >= window


def append_features_from_cfg(mel: np.ndarray, cfg: dict) -> np.ndarray:
    """Append delta or band-flux features based on config."""
    from scripts.features import append_features
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    return append_features(mel, use_delta=use_delta, use_band_flux=use_band_flux)


def run_inference(mel_features: np.ndarray, model, cfg: dict,
                  device: torch.device) -> np.ndarray:
    """Run offline model inference on captured mel data with sliding window."""
    from scripts.features import sliding_window_inference
    window_frames = cfg["model"]["window_frames"]
    return sliding_window_inference(mel_features, model, window_frames, device)


def peak_pick(activations: np.ndarray, threshold: float,
              frame_rate: float, min_interval_s: float = 0.05,
              times: np.ndarray | None = None) -> np.ndarray:
    """Simple peak-picking (same as evaluate.py).

    times: per-line time in seconds (from the frame counter) when the capture
    has gaps; otherwise line index / frame_rate.
    """
    if times is None:
        times = np.arange(len(activations)) / frame_rate
    # Same quantization as evaluate.py's frame-count check (int(0.05*62.5) = 3
    # frames = 48 ms), expressed in seconds.
    min_gap_s = int(min_interval_s * frame_rate) / frame_rate
    peaks = []
    last_t = -np.inf
    for i in range(1, len(activations) - 1):
        # Spacing is measured in time, so a dropped frame between two peaks
        # can't make them look closer than they are.
        if (activations[i] > threshold and
                activations[i] >= activations[i - 1] and
                activations[i] >= activations[i + 1] and
                times[i] - last_t >= min_gap_s - 1e-9):
            peaks.append(times[i])
            last_t = times[i]
    return np.array(peaks)


def main():
    parser = argparse.ArgumentParser(description="Replay device mel capture through offline model")
    parser.add_argument("--capture", required=True, help="JSONL capture file from capture-nn endpoint")
    parser.add_argument("--model", required=True, help="Trained model checkpoint (.pt)")
    parser.add_argument("--config", required=True, help="Model config YAML")
    parser.add_argument("--labels", default=None, help="Ground truth onset labels (.beats.json)")
    parser.add_argument("--threshold", type=float, default=0.5, help="Onset detection threshold")
    parser.add_argument("--device", default="cpu", help="Torch device (cpu/cuda)")
    args = parser.parse_args()

    cfg = load_config(args.config)
    device = torch.device(args.device)

    # Load capture
    print(f"Loading capture: {args.capture}")
    capture = load_capture(args.capture)
    print(f"  Frames: {capture['n_frames']}, FPS: {capture['actual_fps']:.1f}, "
          f"Duration: {capture['n_frames'] / capture['actual_fps']:.1f}s, "
          f"NN active: {capture['nn_active']}, "
          f"onset field: {capture['onset_field']}")

    # Mel distribution summary
    mel = capture["mel"]
    print(f"\n--- Device Mel Distribution ---")
    print(f"  Mean: {mel.mean():.4f}  Std: {mel.std():.4f}")
    print(f"  Min:  {mel.min():.4f}  Max: {mel.max():.4f}")
    print(f"  Per-band mean range: [{mel.mean(axis=0).min():.4f}, {mel.mean(axis=0).max():.4f}]")

    # Append features (delta / band-flux)
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    mel_features = append_features_from_cfg(mel, cfg)
    print(f"  Features per frame: {mel_features.shape[1]} "
          f"({'mel+delta' if use_delta else 'mel+flux' if use_band_flux else 'mel only'})")

    # Load model
    print(f"\nLoading model: {args.model}")
    model, pool_factor = load_model(args.model, cfg, device)
    if pool_factor != 1:
        print(f"  WARNING: pool_factor={pool_factor}, replay may not match firmware")

    # Run inference
    print("Running offline inference on captured mel data...")
    offline_onset = run_inference(mel_features, model, cfg, device)

    # Compare device vs offline activations, only where the model window is
    # gap-free (offline windows spanning a dropped frame see different input).
    device_onset = capture["device_onset"]
    mask = gap_free_mask(capture["fc"], cfg["model"]["window_frames"])
    if mask is not None:
        if not mask.any():
            print("ERROR: no gap-free model windows in this capture", file=sys.stderr)
            sys.exit(1)
        print(f"  Parity frames (gap-free windows): {int(mask.sum())}/{len(mask)}")
        dev_cmp, off_cmp = device_onset[mask], offline_onset[mask]
    else:
        dev_cmp, off_cmp = device_onset, offline_onset
    correlation = np.corrcoef(dev_cmp, off_cmp)[0, 1]
    mae = np.mean(np.abs(dev_cmp - off_cmp))
    print(f"\n--- Parity Analysis ---")
    print(f"  Activation correlation: {correlation:.4f}")
    print(f"  Activation MAE:         {mae:.4f}")
    print(f"  Device mean activation: {device_onset.mean():.4f}")
    print(f"  Offline mean activation:{offline_onset.mean():.4f}")

    # Peak-pick both
    frame_rate = capture["actual_fps"]
    # With a frame counter, time each line by its frame index at the nominal
    # 62.5 Hz frame rate, so dropped frames don't shift later peaks.
    times = (capture["fc"] - capture["fc"][0]) / 62.5 if capture["fc"] is not None else None
    device_peaks = peak_pick(device_onset, args.threshold, frame_rate, times=times)
    offline_peaks = peak_pick(offline_onset, args.threshold, frame_rate, times=times)
    print(f"  Device onset count:     {len(device_peaks)}")
    print(f"  Offline onset count:    {len(offline_peaks)}")

    # Score against ground truth if provided. Not with dropped frames: offline
    # predictions near a gap saw different input than the device did, so F1
    # differences would mix capture loss with model differences.
    if args.labels and capture["n_missing"]:
        print(f"ERROR: --labels scoring refused: capture is missing {capture['n_missing']} "
              "frame(s). Re-capture without catch-up frames (or score a gap-free span).",
              file=sys.stderr)
        sys.exit(1)
    if args.labels:
        print(f"\n--- Ground Truth Scoring ---")
        with open(args.labels) as f:
            label_data = json.load(f)
        ref_times = np.array(label_data.get("beats", label_data.get("onsets", [])))
        if len(ref_times) == 0:
            print("  No reference onsets found in labels file")
        else:
            import mir_eval
            # Score device detections
            if len(device_peaks) > 0:
                d_f1, d_p, d_r = mir_eval.onset.f_measure(ref_times, device_peaks, window=0.05)
            else:
                d_f1, d_p, d_r = 0.0, 0.0, 0.0
            # Score offline detections
            if len(offline_peaks) > 0:
                o_f1, o_p, o_r = mir_eval.onset.f_measure(ref_times, offline_peaks, window=0.05)
            else:
                o_f1, o_p, o_r = 0.0, 0.0, 0.0
            print(f"  {'':20s} {'F1':>8s} {'Prec':>8s} {'Recall':>8s}")
            print(f"  {'Device (firmware)':20s} {d_f1:>8.3f} {d_p:>8.3f} {d_r:>8.3f}")
            print(f"  {'Offline (PyTorch)':20s} {o_f1:>8.3f} {o_p:>8.3f} {o_r:>8.3f}")
            gap = o_f1 - d_f1
            print(f"  Gap (offline - device): {gap:+.3f}")
            if abs(gap) < 0.03:
                print("  → Gap is small — mel domain shift is NOT the primary cause")
                print("    Investigate: quantization, threshold, cooldown differences")
            elif gap > 0.1:
                print("  → Large gap — mel domain shift IS the primary cause")
                print("    PCEN normalization should help")

    print("\nDone.")


if __name__ == "__main__":
    main()
