#!/usr/bin/env python3
"""Evaluate beat activation model offline.

Computes per-track and aggregate metrics:
  - Frame-level: precision, recall, F1 (at threshold)
  - Beat-level: precision, recall, F1 (using mir_eval with ±70ms tolerance)
  - Activation plots for visual inspection

Usage:
    # Evaluate on validation set
    python evaluate.py --config configs/default.yaml

    # Evaluate on specific tracks
    python evaluate.py --config configs/default.yaml --audio-dir ../blinky-test-player/music/edm
"""

import argparse
import json
import os
import sys
from pathlib import Path

os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import mir_eval
import numpy as np
import yaml

from scripts.prepare_dataset import firmware_mel_spectrogram, load_config


def evaluate_on_tracks(model_path: str, audio_dir: Path, cfg: dict, output_dir: Path,
                       threshold: float = 0.5):
    """Run model on full tracks and evaluate beat detection accuracy."""
    import librosa
    import tf_keras as keras

    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]

    model = keras.models.load_model(model_path, compile=False)
    chunk_frames = model.input_shape[1]

    audio_files = sorted(
        f for f in audio_dir.rglob("*")
        if f.suffix.lower() in {".mp3", ".wav", ".flac"}
    )

    all_results = []

    for audio_path in audio_files:
        label_path = audio_path.parent / f"{audio_path.stem}.beats.json"
        if not label_path.exists():
            continue

        # Load and process
        audio, _ = librosa.load(str(audio_path), sr=sr, mono=True)
        mel = firmware_mel_spectrogram(audio, cfg)

        # Run model on overlapping chunks, average predictions
        n_frames = mel.shape[0]
        activations = np.zeros(n_frames, dtype=np.float32)
        counts = np.zeros(n_frames, dtype=np.float32)

        stride = chunk_frames // 2
        for start in range(0, max(1, n_frames - chunk_frames + 1), stride):
            end = start + chunk_frames
            if end > n_frames:
                chunk = np.zeros((chunk_frames, mel.shape[1]), dtype=np.float32)
                chunk[:n_frames - start] = mel[start:n_frames]
            else:
                chunk = mel[start:end]

            pred = model.predict(chunk[np.newaxis], verbose=0)[0, :, 0]
            actual_len = min(chunk_frames, n_frames - start)
            activations[start:start + actual_len] += pred[:actual_len]
            counts[start:start + actual_len] += 1

        activations /= np.maximum(counts, 1)

        # Load ground truth beats
        with open(label_path) as f:
            labels = json.load(f)
        ref_beats = np.array([h["time"] for h in labels["hits"] if h.get("expectTrigger", True)])

        # Peak-pick activations to get estimated beat times
        est_beats = _peak_pick(activations, threshold, frame_rate)

        # Beat-level F1 using mir_eval (±70ms tolerance)
        if len(ref_beats) > 0 and len(est_beats) > 0:
            scores = mir_eval.beat.f_measure(ref_beats, est_beats, f_measure_threshold=0.07)
        else:
            scores = 0.0

        result = {
            "track": audio_path.stem,
            "ref_beats": len(ref_beats),
            "est_beats": len(est_beats),
            "f1": float(scores),
        }
        all_results.append(result)
        print(f"  {audio_path.stem}: F1={scores:.3f} (ref={len(ref_beats)}, est={len(est_beats)})")

        # Save activation plot
        _plot_activation(activations, ref_beats, est_beats, frame_rate,
                         audio_path.stem, output_dir / "plots")

    # Aggregate
    if all_results:
        f1s = [r["f1"] for r in all_results]
        print(f"\nAggregate: mean F1={np.mean(f1s):.3f}, median={np.median(f1s):.3f}, "
              f"min={np.min(f1s):.3f}, max={np.max(f1s):.3f}")

        # Save results
        with open(output_dir / "eval_results.json", "w") as f:
            json.dump(all_results, f, indent=2)


def _peak_pick(activations: np.ndarray, threshold: float,
               frame_rate: float, min_interval_s: float = 0.2) -> np.ndarray:
    """Simple peak-picking on activation signal."""
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


def _plot_activation(activations: np.ndarray, ref_beats: np.ndarray,
                     est_beats: np.ndarray, frame_rate: float,
                     title: str, plot_dir: Path):
    """Save activation plot with reference and estimated beats."""
    plot_dir.mkdir(parents=True, exist_ok=True)

    times = np.arange(len(activations)) / frame_rate
    fig, ax = plt.subplots(figsize=(14, 3))
    ax.plot(times, activations, "b-", linewidth=0.5, alpha=0.8, label="Activation")

    for bt in ref_beats:
        ax.axvline(bt, color="green", alpha=0.3, linewidth=0.5)
    for bt in est_beats:
        ax.axvline(bt, color="red", alpha=0.3, linewidth=0.5, linestyle="--")

    ax.set_xlim(0, times[-1] if len(times) > 0 else 1)
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Activation")
    ax.set_title(title)
    ax.legend(loc="upper right", fontsize=8)

    fig.tight_layout()
    fig.savefig(plot_dir / f"{title}.png", dpi=100)
    plt.close(fig)


def evaluate_validation_set(model_path: str, cfg: dict, output_dir: Path):
    """Evaluate on the processed validation set (frame-level metrics)."""
    import tf_keras as keras

    data_dir = Path(cfg["data"]["processed_dir"])
    X_val = np.load(data_dir / "X_val.npy")
    Y_val = np.load(data_dir / "Y_val.npy")

    model = keras.models.load_model(model_path, compile=False)

    # Predict
    Y_pred = model.predict(X_val, batch_size=64, verbose=1)[:, :, 0]

    # Frame-level metrics at various thresholds
    print("\nFrame-level metrics:")
    print(f"{'Threshold':>10} {'Precision':>10} {'Recall':>10} {'F1':>10}")
    for thresh in [0.3, 0.4, 0.5, 0.6, 0.7]:
        pred_binary = (Y_pred > thresh).astype(float)
        ref_binary = (Y_val > 0.5).astype(float)

        tp = np.sum(pred_binary * ref_binary)
        fp = np.sum(pred_binary * (1 - ref_binary))
        fn = np.sum((1 - pred_binary) * ref_binary)

        precision = tp / (tp + fp + 1e-10)
        recall = tp / (tp + fn + 1e-10)
        f1 = 2 * precision * recall / (precision + recall + 1e-10)
        print(f"{thresh:>10.1f} {precision:>10.3f} {recall:>10.3f} {f1:>10.3f}")


def main():
    parser = argparse.ArgumentParser(description="Evaluate beat activation model")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--model", default="outputs/best_model.keras")
    parser.add_argument("--audio-dir", default=None, help="Evaluate on full tracks")
    parser.add_argument("--output-dir", default="outputs/eval")
    parser.add_argument("--threshold", type=float, default=0.5)
    args = parser.parse_args()

    cfg = load_config(args.config)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.audio_dir:
        evaluate_on_tracks(args.model, Path(args.audio_dir), cfg, output_dir, args.threshold)
    else:
        evaluate_validation_set(args.model, cfg, output_dir)


if __name__ == "__main__":
    main()
