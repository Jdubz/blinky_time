#!/usr/bin/env python3
"""Sweep target_rms_db values to find the one that matches firmware mel statistics.

Firmware AGC produces mel values with mean ~0.52 (measured on device).
Training pipeline normalizes to target_rms_db=-35, giving mel mean ~0.86.
This script sweeps RMS levels to find the correct target_rms_db.

Usage:
    python tools/rms_mel_sweep.py
    python tools/rms_mel_sweep.py --target-mean 0.52 --tracks 20
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import librosa

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scripts.audio import firmware_mel_spectrogram_np


def sweep_rms_levels(audio_dir: Path, rms_values: list[float],
                     n_tracks: int = 10, sr: int = 16000) -> dict:
    """Compute mel statistics at each RMS level across multiple tracks."""
    audio_files = sorted(audio_dir.rglob("*.wav"))[:n_tracks * 10]
    audio_files += sorted(audio_dir.rglob("*.flac"))[:n_tracks * 10]
    audio_files += sorted(audio_dir.rglob("*.mp3"))[:n_tracks * 10]
    # Shuffle and take n_tracks for representative sample
    rng = np.random.RandomState(42)
    rng.shuffle(audio_files)
    audio_files = audio_files[:n_tracks]

    if not audio_files:
        print(f"No audio files found in {audio_dir}")
        sys.exit(1)

    print(f"Using {len(audio_files)} tracks from {audio_dir}")

    results = {rms_db: {"means": [], "stds": [], "medians": []} for rms_db in rms_values}

    loaded = 0
    for i, af in enumerate(audio_files):
        print(f"  [{i+1}/{len(audio_files)}] {af.name}...", end="", flush=True)
        try:
            audio, _ = librosa.load(str(af), sr=sr, mono=True)
        except Exception as e:
            print(f" SKIP ({e})")
            continue
        loaded += 1

        # Compute original RMS
        orig_rms = np.sqrt(np.mean(audio ** 2) + 1e-10)

        for rms_db in rms_values:
            target_rms = 10 ** (rms_db / 20)
            scaled = audio * (target_rms / orig_rms)
            mel = firmware_mel_spectrogram_np(scaled)  # (frames, 26)

            results[rms_db]["means"].append(mel.mean())
            results[rms_db]["stds"].append(mel.std())
            results[rms_db]["medians"].append(np.median(mel))

        print(" done")

    if loaded == 0:
        print("ERROR: No audio files could be loaded")
        sys.exit(1)

    return results


def main():
    parser = argparse.ArgumentParser(description="Sweep target_rms_db to match firmware mel levels")
    parser.add_argument("--audio-dir", default="/mnt/storage/blinky-ml-data/audio",
                        help="Directory with audio files")
    parser.add_argument("--tracks", type=int, default=10, help="Number of tracks to sample")
    parser.add_argument("--target-mean", type=float, default=0.52,
                        help="Target mel mean to match firmware (default: 0.52)")
    parser.add_argument("--rms-min", type=float, default=-75, help="Min RMS dB to sweep")
    parser.add_argument("--rms-max", type=float, default=-30, help="Max RMS dB to sweep")
    parser.add_argument("--rms-step", type=float, default=2.5, help="RMS dB step size")
    args = parser.parse_args()

    rms_values = np.arange(args.rms_min, args.rms_max + 0.1, args.rms_step).tolist()

    print(f"Sweeping {len(rms_values)} RMS levels: {args.rms_min} to {args.rms_max} dB")
    print(f"Target firmware mel mean: {args.target_mean}")
    print()

    results = sweep_rms_levels(Path(args.audio_dir), rms_values, n_tracks=args.tracks)

    # Print table
    print(f"\n{'RMS dB':>8}  {'Mel Mean':>10}  {'Mel Std':>10}  {'Mel Median':>10}  {'Δ from target':>14}")
    print("-" * 60)

    best_rms = None
    best_delta = float("inf")

    for rms_db in rms_values:
        r = results[rms_db]
        mean = np.mean(r["means"])
        std = np.mean(r["stds"])
        median = np.mean(r["medians"])
        delta = abs(mean - args.target_mean)

        if delta < best_delta:
            best_delta = delta
            best_rms = rms_db

        marker = " <-- best" if delta < best_delta + 0.001 and rms_db == best_rms else ""
        print(f"{rms_db:>8.1f}  {mean:>10.4f}  {std:>10.4f}  {median:>10.4f}  {delta:>14.4f}{marker}")

    print(f"\nBest match: target_rms_db = {best_rms:.1f} (mel mean = {np.mean(results[best_rms]['means']):.4f}, "
          f"target = {args.target_mean})")
    print(f"\nTo retrain with this value:")
    print(f"  1. Edit configs/base.yaml: target_rms_db: {best_rms:.0f}")
    print(f"  2. Reprocess: python scripts/prepare_dataset.py --config configs/frame_fc.yaml --augment")
    print(f"  3. Retrain:   python train.py --config configs/frame_fc.yaml")


if __name__ == "__main__":
    main()
