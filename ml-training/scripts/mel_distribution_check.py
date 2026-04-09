#!/usr/bin/env python3
"""Compare mel band distributions between training data and device captures.

Diagnoses sim-to-real domain shift by measuring per-band statistical
divergence between what the model trained on and what the device actually sees.

Usage:
    # Compare training data vs one device capture
    python scripts/mel_distribution_check.py \\
        --training /mnt/storage/blinky-ml-data/processed_v18/X_train.npy \\
        --capture capture.jsonl

    # Compare against multiple captures
    python scripts/mel_distribution_check.py \\
        --training /mnt/storage/blinky-ml-data/processed_v18/X_train.npy \\
        --capture capture1.jsonl capture2.jsonl capture3.jsonl
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np


def load_training_mels(npy_path: str, max_samples: int = 100000) -> np.ndarray:
    """Load training mel data from processed .npy file.

    X_train.npy has shape (n_chunks, chunk_frames, n_features).
    We flatten to (n_frames, n_mels) taking only the first 26 columns
    (mel bands, excluding delta/band-flux features).
    """
    X = np.load(npy_path, mmap_mode='r')
    n_mels = min(26, X.shape[2])  # first 26 cols are mel bands
    # Sample randomly to avoid loading entire dataset
    total_frames = X.shape[0] * X.shape[1]
    if total_frames > max_samples:
        # Sample random chunks
        rng = np.random.default_rng(42)
        n_chunks = min(max_samples // X.shape[1], X.shape[0])
        idx = rng.choice(X.shape[0], size=n_chunks, replace=False)
        mels = X[idx, :, :n_mels].reshape(-1, n_mels)
    else:
        mels = X[:, :, :n_mels].reshape(-1, n_mels)
    return mels.astype(np.float32)


def load_capture_mels(jsonl_paths: list[str]) -> np.ndarray:
    """Load mel data from one or more JSONL capture files."""
    all_mels = []
    for path in jsonl_paths:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                frame = json.loads(line)
                if frame.get("type") == "NN" and "mel" in frame:
                    all_mels.append(frame["mel"])
    return np.array(all_mels, dtype=np.float32)


def compute_stats(mels: np.ndarray) -> dict:
    """Compute per-band statistics."""
    return {
        "mean": mels.mean(axis=0),
        "std": mels.std(axis=0),
        "p5": np.percentile(mels, 5, axis=0),
        "p25": np.percentile(mels, 25, axis=0),
        "p50": np.percentile(mels, 50, axis=0),
        "p75": np.percentile(mels, 75, axis=0),
        "p95": np.percentile(mels, 95, axis=0),
        "min": mels.min(axis=0),
        "max": mels.max(axis=0),
    }


def wasserstein_1d(a: np.ndarray, b: np.ndarray) -> float:
    """1D Wasserstein distance (Earth Mover's Distance) between two samples."""
    a_sorted = np.sort(a)
    b_sorted = np.sort(b)
    # Interpolate to common length for comparison
    n = min(len(a_sorted), len(b_sorted), 10000)
    a_interp = np.interp(np.linspace(0, 1, n), np.linspace(0, 1, len(a_sorted)), a_sorted)
    b_interp = np.interp(np.linspace(0, 1, n), np.linspace(0, 1, len(b_sorted)), b_sorted)
    return np.mean(np.abs(a_interp - b_interp))


def main():
    parser = argparse.ArgumentParser(description="Compare training vs device mel distributions")
    parser.add_argument("--training", required=True, help="Training data .npy (X_train.npy)")
    parser.add_argument("--capture", required=True, nargs="+", help="Device capture JSONL file(s)")
    parser.add_argument("--max-samples", type=int, default=100000, help="Max training frames to sample")
    args = parser.parse_args()

    print("Loading training mel data...")
    train_mels = load_training_mels(args.training, args.max_samples)
    print(f"  Training: {train_mels.shape[0]} frames, {train_mels.shape[1]} bands")

    print("Loading device capture mel data...")
    device_mels = load_capture_mels(args.capture)
    print(f"  Device:   {device_mels.shape[0]} frames, {device_mels.shape[1]} bands")

    if train_mels.shape[1] != device_mels.shape[1]:
        print(f"ERROR: Band count mismatch: training={train_mels.shape[1]}, device={device_mels.shape[1]}")
        sys.exit(1)

    n_bands = train_mels.shape[1]
    train_stats = compute_stats(train_mels)
    device_stats = compute_stats(device_mels)

    # Per-band comparison
    print(f"\n{'Band':>4s} {'Train μ':>8s} {'Dev μ':>8s} {'Δμ':>8s} "
          f"{'Train σ':>8s} {'Dev σ':>8s} {'W-dist':>8s}")
    print("-" * 60)

    w_distances = []
    for i in range(n_bands):
        t_mean = train_stats["mean"][i]
        d_mean = device_stats["mean"][i]
        delta = d_mean - t_mean
        t_std = train_stats["std"][i]
        d_std = device_stats["std"][i]
        w_dist = wasserstein_1d(train_mels[:, i], device_mels[:, i])
        w_distances.append(w_dist)
        print(f"{i:>4d} {t_mean:>8.4f} {d_mean:>8.4f} {delta:>+8.4f} "
              f"{t_std:>8.4f} {d_std:>8.4f} {w_dist:>8.4f}")

    # Summary
    w_mean = np.mean(w_distances)
    w_max = np.max(w_distances)
    mean_shift = np.mean(device_stats["mean"] - train_stats["mean"])

    print(f"\n--- Summary ---")
    print(f"  Overall mean shift:      {mean_shift:+.4f}")
    print(f"  Mean Wasserstein dist:   {w_mean:.4f}")
    print(f"  Max Wasserstein dist:    {w_max:.4f} (band {np.argmax(w_distances)})")
    print(f"  Training range usage:    [{train_mels.min():.3f}, {train_mels.max():.3f}]")
    print(f"  Device range usage:      [{device_mels.min():.3f}, {device_mels.max():.3f}]")

    if w_mean > 0.10:
        print(f"\n  *** LARGE distribution shift detected (W={w_mean:.3f} > 0.10) ***")
        print(f"  The model is seeing significantly different mel distributions on-device.")
        print(f"  PCEN normalization is likely to help.")
    elif w_mean > 0.05:
        print(f"\n  Moderate distribution shift (W={w_mean:.3f}). PCEN may help.")
    else:
        print(f"\n  Small distribution shift (W={w_mean:.3f}). Gap may be from quantization/inference.")


if __name__ == "__main__":
    main()
