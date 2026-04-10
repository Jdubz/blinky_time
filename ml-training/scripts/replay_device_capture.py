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
    # Prefer raw NN activation (nna) over gated pulse (onset) when available
    has_nna = "nna" in frames[0]
    if has_nna:
        device_onset = np.array([f["nna"] for f in frames], dtype=np.float32)
    else:
        device_onset = np.array([f["onset"] for f in frames], dtype=np.float32)
    timestamps = np.array([f["ts"] for f in frames], dtype=np.float64)
    nn_active = all(f.get("nn", 0) == 1 for f in frames)

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
    }


def append_features_from_cfg(mel: np.ndarray, cfg: dict) -> np.ndarray:
    """Append delta or band-flux features based on config."""
    from scripts.features import append_features
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    return append_features(mel, use_delta=use_delta, use_band_flux=use_band_flux)


def run_inference(mel_features: np.ndarray, model, cfg: dict,
                  device: torch.device) -> np.ndarray:
    """Run offline model inference on captured mel data with sliding window."""
    window_frames = cfg["model"]["window_frames"]
    n_frames = mel_features.shape[0]
    mel_tensor = torch.from_numpy(mel_features).to(device)

    # Overlapping chunk inference (same as evaluate.py)
    activations = np.zeros(n_frames, dtype=np.float32)
    counts = np.zeros(n_frames, dtype=np.float32)
    stride = window_frames // 2

    with torch.no_grad():
        for start in range(0, max(1, n_frames - window_frames + 1), stride):
            end = start + window_frames
            if end > n_frames:
                chunk = torch.zeros(window_frames, mel_features.shape[1],
                                    device=device)
                chunk[:n_frames - start] = mel_tensor[start:n_frames]
                actual_len = n_frames - start
            else:
                chunk = mel_tensor[start:end]
                actual_len = window_frames

            pred = model(chunk.unsqueeze(0))[0]  # (time, channels)
            pred_np = pred[:actual_len, 0].cpu().numpy()

            activations[start:start + actual_len] += pred_np
            counts[start:start + actual_len] += 1

    activations /= np.maximum(counts, 1)
    return activations


def peak_pick(activations: np.ndarray, threshold: float,
              frame_rate: float, min_interval_s: float = 0.05) -> np.ndarray:
    """Simple peak-picking (same as evaluate.py)."""
    min_frames = int(min_interval_s * frame_rate)
    peaks = []
    last_peak = -min_frames
    for i in range(1, len(activations) - 1):
        if (activations[i] > threshold and
                activations[i] >= activations[i - 1] and
                activations[i] >= activations[i + 1] and
                i - last_peak >= min_frames):
            peaks.append(i / frame_rate)
            last_peak = i
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
    mel_features = append_features_from_cfg(mel, cfg)
    print(f"  Features per frame: {mel_features.shape[1]} "
          f"({'mel+delta' if mel_features.shape[1] == 52 else 'mel+flux' if mel_features.shape[1] == 29 else 'mel only'})")

    # Load model
    print(f"\nLoading model: {args.model}")
    from evaluate import load_model
    model, pool_factor = load_model(args.model, cfg, device)
    if pool_factor != 1:
        print(f"  WARNING: pool_factor={pool_factor}, replay may not match firmware")

    # Run inference
    print("Running offline inference on captured mel data...")
    offline_onset = run_inference(mel_features, model, cfg, device)

    # Compare device vs offline activations
    device_onset = capture["device_onset"]
    correlation = np.corrcoef(device_onset, offline_onset)[0, 1]
    mae = np.mean(np.abs(device_onset - offline_onset))
    print(f"\n--- Parity Analysis ---")
    print(f"  Activation correlation: {correlation:.4f}")
    print(f"  Activation MAE:         {mae:.4f}")
    print(f"  Device mean activation: {device_onset.mean():.4f}")
    print(f"  Offline mean activation:{offline_onset.mean():.4f}")

    # Peak-pick both
    frame_rate = capture["actual_fps"]
    device_peaks = peak_pick(device_onset, args.threshold, frame_rate)
    offline_peaks = peak_pick(offline_onset, args.threshold, frame_rate)
    print(f"  Device onset count:     {len(device_peaks)}")
    print(f"  Offline onset count:    {len(offline_peaks)}")

    # Score against ground truth if provided
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
