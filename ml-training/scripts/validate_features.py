#!/usr/bin/env python3
"""Validate that Python mel feature extraction matches firmware output.

Compares Python-computed mel bands against firmware mel bands captured
via serial streaming for the same audio clip.

Usage:
    # Generate a reference clip and its Python mel bands
    python scripts/validate_features.py --generate-reference --audio test.wav

    # Compare against firmware output (paste from serial stream)
    python scripts/validate_features.py --compare firmware_mel.csv python_mel.npy
"""

import argparse
import sys
from pathlib import Path

import librosa
import numpy as np


def generate_reference(audio_path: str, config_path: str, output_path: str):
    """Compute mel bands for an audio file using the Python pipeline."""
    from scripts.audio import load_config, firmware_mel_spectrogram_torch as firmware_mel_spectrogram
    cfg = load_config(config_path)

    audio, _ = librosa.load(audio_path, sr=cfg["audio"]["sample_rate"], mono=True)
    mel = firmware_mel_spectrogram(audio, cfg)

    np.save(output_path, mel)
    print(f"Saved {mel.shape[0]} frames x {mel.shape[1]} bands to {output_path}")
    print(f"First frame: {mel[0].round(4)}")
    print(f"Stats: min={mel.min():.4f}, max={mel.max():.4f}, mean={mel.mean():.4f}")


def compare_features(firmware_csv: str, python_npy: str):
    """Compare firmware-captured mel bands against Python-computed ones."""
    # Load firmware output (CSV: one row per frame, 26 columns)
    fw_mel = np.loadtxt(firmware_csv, delimiter=",")
    py_mel = np.load(python_npy)

    # Align lengths (firmware may have slightly different frame count)
    n = min(len(fw_mel), len(py_mel))
    fw_mel = fw_mel[:n]
    py_mel = py_mel[:n]

    # Compute error metrics
    mae = np.mean(np.abs(fw_mel - py_mel))
    rmse = np.sqrt(np.mean((fw_mel - py_mel) ** 2))
    max_err = np.max(np.abs(fw_mel - py_mel))

    # Per-band correlation
    correlations = []
    for band in range(fw_mel.shape[1]):
        if np.std(fw_mel[:, band]) > 1e-6 and np.std(py_mel[:, band]) > 1e-6:
            corr = np.corrcoef(fw_mel[:, band], py_mel[:, band])[0, 1]
        else:
            corr = float("nan")
        correlations.append(corr)

    print(f"Feature parity validation ({n} frames):")
    print(f"  MAE:  {mae:.6f}")
    print(f"  RMSE: {rmse:.6f}")
    print(f"  Max error: {max_err:.6f}")
    print(f"  Mean per-band correlation: {np.nanmean(correlations):.4f}")
    print(f"  Min per-band correlation:  {np.nanmin(correlations):.4f}")

    if mae < 0.05 and np.nanmean(correlations) > 0.95:
        print("\n  PASS: Features match firmware within tolerance")
    elif mae < 0.1:
        print("\n  WARN: Minor discrepancies — check compressor/whitening settings")
    else:
        print("\n  FAIL: Significant mismatch — investigate pipeline differences")
        # Show worst bands
        worst = np.argsort(correlations)[:5]
        for b in worst:
            print(f"    Band {b}: correlation={correlations[b]:.4f}")


def main():
    parser = argparse.ArgumentParser(description="Validate feature parity with firmware")
    parser.add_argument("--generate-reference", action="store_true")
    parser.add_argument("--audio", help="Audio file for reference generation")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--output", default="outputs/reference_mel.npy")
    parser.add_argument("--compare", nargs=2, metavar=("FIRMWARE_CSV", "PYTHON_NPY"),
                        help="Compare firmware vs Python features")
    args = parser.parse_args()

    if args.generate_reference:
        if not args.audio:
            print("Error: --audio required with --generate-reference", file=sys.stderr)
            sys.exit(1)
        generate_reference(args.audio, args.config, args.output)
    elif args.compare:
        compare_features(args.compare[0], args.compare[1])
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
