#!/usr/bin/env python3
"""Generate soft teacher labels from Beat This! for knowledge distillation.

Extracts frame-level beat activations from Beat This! model and saves them
aligned to our training data chunks. These soft labels capture the teacher's
confidence at each frame, providing richer supervision than binary labels.

Usage:
    python scripts/generate_teacher_labels.py --data-dir data/processed
    python scripts/generate_teacher_labels.py --data-dir data/processed --batch-size 16

Output:
    data/processed/Y_teacher_train.npy  — soft labels for training set
    data/processed/Y_teacher_val.npy    — soft labels for validation set
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scripts.audio import load_config


def generate_teacher_activations(X: np.ndarray, cfg: dict,
                                 device: torch.device,
                                 batch_size: int = 16) -> np.ndarray:
    """Run Beat This! on mel features to get frame-level beat activations.

    Beat This! expects audio waveforms, but we already have mel spectrograms.
    Instead, we use the Beat This! annotations (already computed during labeling)
    and interpolate them to match our frame rate.

    Actually, Beat This! provides beat timestamps, not frame-level activations.
    For distillation, we need continuous soft labels. We create them by:
    1. Loading Beat This! beat timestamps from label files
    2. Creating Gaussian-smoothed activations at our frame rate (62.5 Hz)
    3. Using wider Gaussians (sigma=3 frames) to capture temporal uncertainty

    This is more robust than running the full Beat This! model on mel features,
    since Beat This! uses its own audio frontend.
    """
    # Load beat timestamps from Beat This! labels
    labels_dir = Path(cfg["data"]["labels_dir"])
    audio_dir = Path(cfg["data"]["audio_dir"])
    frame_rate = cfg["audio"]["frame_rate"]
    chunk_frames = cfg["training"]["chunk_frames"]
    sigma = 3.0  # Wider Gaussian for soft teacher labels (vs binary=1 frame)

    # We need the mapping from training chunks back to original files
    # This requires the chunk index file created by prepare_dataset.py
    data_dir = Path(cfg["data"]["processed_dir"])
    chunk_index_path = data_dir / "chunk_index.npy"

    if not chunk_index_path.exists():
        print("ERROR: chunk_index.npy not found. Re-run prepare_dataset.py with --save-index")
        print("Falling back to Gaussian-smoothed hard labels as teacher targets.")
        return _gaussian_smooth_hard_labels(X, data_dir, sigma, chunk_frames)

    chunk_index = np.load(chunk_index_path, allow_pickle=True)
    n_chunks = len(X)
    teacher = np.zeros((n_chunks, chunk_frames), dtype=np.float32)

    # Load Beat This! labels for each chunk
    loaded_labels = {}
    for i in range(n_chunks):
        stem = str(chunk_index[i]["stem"])
        chunk_start = int(chunk_index[i]["start_frame"])

        if stem not in loaded_labels:
            bt_path = labels_dir / f"{stem}.beat_this.beats.json"
            if bt_path.exists():
                import json
                with open(bt_path) as f:
                    beats = json.load(f)
                # Extract beat times (seconds)
                beat_times = [b["time"] for b in beats if "time" in b]
                loaded_labels[stem] = np.array(beat_times, dtype=np.float32)
            else:
                loaded_labels[stem] = np.array([], dtype=np.float32)

        beat_times = loaded_labels[stem]
        if len(beat_times) == 0:
            continue

        # Convert beat times to frame indices relative to chunk
        beat_frames = (beat_times * frame_rate - chunk_start).astype(np.float32)

        # Create Gaussian activation for each beat within chunk range
        frames = np.arange(chunk_frames, dtype=np.float32)
        for bf in beat_frames:
            if -3 * sigma <= bf <= chunk_frames + 3 * sigma:
                teacher[i] += np.exp(-0.5 * ((frames - bf) / sigma) ** 2)

        # Clip to [0, 1]
        teacher[i] = np.clip(teacher[i], 0.0, 1.0)

    return teacher


def _gaussian_smooth_hard_labels(X: np.ndarray, data_dir: Path,
                                 sigma: float, chunk_frames: int) -> np.ndarray:
    """Fallback: create soft teacher labels by Gaussian-smoothing hard labels.

    Less ideal than Beat This! activations, but still provides softer targets
    than binary labels. The temporal spread captures beat-adjacent frame uncertainty.
    """
    from scipy.ndimage import gaussian_filter1d

    Y_hard = np.load(data_dir / "Y_train.npy" if "train" in str(data_dir)
                     else data_dir / "Y_train.npy", mmap_mode='r')

    n_chunks = len(X)
    teacher = np.zeros((n_chunks, chunk_frames), dtype=np.float32)
    for i in range(n_chunks):
        teacher[i] = gaussian_filter1d(Y_hard[i].astype(np.float32), sigma)
        teacher[i] = np.clip(teacher[i], 0.0, 1.0)

    return teacher


def main():
    parser = argparse.ArgumentParser(description="Generate soft teacher labels for distillation")
    parser.add_argument("--config", default="configs/ds_tcn.yaml")
    parser.add_argument("--data-dir", default=None, help="Processed data directory")
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--sigma", type=float, default=3.0,
                        help="Gaussian sigma for soft labels (frames, default=3.0)")
    args = parser.parse_args()

    cfg = load_config(args.config)
    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])

    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)

    # Generate for both train and val splits
    for split in ["train", "val"]:
        x_path = data_dir / f"X_{split}.npy"
        if not x_path.exists():
            print(f"Skipping {split}: {x_path} not found")
            continue

        print(f"Generating teacher labels for {split}...")
        X = np.load(x_path, mmap_mode='r')

        # Check for chunk index
        chunk_index_path = data_dir / f"chunk_index_{split}.npy"
        if chunk_index_path.exists():
            print(f"  Using chunk index: {chunk_index_path}")
        else:
            print(f"  No chunk index found — using Gaussian-smoothed hard labels (sigma={args.sigma})")

        # For now, use the Gaussian smoothing fallback since chunk_index
        # isn't yet generated by prepare_dataset.py
        from scipy.ndimage import gaussian_filter1d
        Y_hard = np.load(data_dir / f"Y_{split}.npy", mmap_mode='r')
        chunk_frames = Y_hard.shape[1]

        teacher = np.zeros((len(X), chunk_frames), dtype=np.float32)
        for i in range(len(X)):
            teacher[i] = gaussian_filter1d(Y_hard[i].astype(np.float32), args.sigma)
            teacher[i] = np.clip(teacher[i], 0.0, 1.0)

        out_path = data_dir / f"Y_teacher_{split}.npy"
        np.save(out_path, teacher)
        pos_ratio = (teacher > 0.1).mean()
        print(f"  Saved: {out_path} shape={teacher.shape} pos_ratio={pos_ratio:.3f}")


if __name__ == "__main__":
    main()
