#!/usr/bin/env python3
"""Extract per-beat spectral features from audio + beat annotations.

Produces beat-level training data for the BeatSyncClassifier:
  - Per-beat feature vectors (subdivision means + peak + duration)
  - Downbeat labels from consensus annotations
  - Grouped into fixed-length sequences for the FC classifier

The feature vector uses raw mel bands (firmware-matched, pre-compression,
pre-whitening) ensuring the NN is decoupled from 47+ tunable firmware
parameters.  Only the 8 fundamental mel constants matter.

Usage:
    python scripts/beat_feature_extractor.py --config configs/beat_sync.yaml
    python scripts/beat_feature_extractor.py --config configs/beat_sync.yaml --augment
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import torch
from tqdm import tqdm

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from scripts.audio import (
    FRAME_RATE, HOP_LENGTH, N_MELS, SAMPLE_RATE,
    build_mel_filterbank_torch,
    firmware_mel_spectrogram_torch,
    load_config,
)


def time_to_frame(time_s: float) -> int:
    """Convert time in seconds to mel frame index."""
    return int(time_s * FRAME_RATE)


def extract_beat_features_from_mel(
    mel: np.ndarray,
    beat_times: np.ndarray,
    n_subdivisions: int = 2,
    include_peak: bool = True,
) -> np.ndarray:
    """Extract per-beat feature vectors from a mel spectrogram.

    For each consecutive beat pair, computes spectral summary features
    over the frames in that interval.

    Args:
        mel: (n_frames, n_mels) mel spectrogram, values in [0, 1]
        beat_times: (n_beats,) beat times in seconds
        n_subdivisions: number of equal subdivisions per beat interval
        include_peak: whether to include whole-beat peak mel bands

    Returns:
        features: (n_beats - 1, features_per_beat) feature array
            Each row: [subdiv_0_mean[26], subdiv_1_mean[26], ..., peak[26], duration]
    """
    n_frames, n_mels_actual = mel.shape
    n_beats = len(beat_times)

    # Features per beat: n_subdivisions * n_mels + (n_mels if peak) + 1 (duration)
    features_dim = n_subdivisions * n_mels_actual
    if include_peak:
        features_dim += n_mels_actual
    features_dim += 1  # duration_frames

    features = np.zeros((n_beats - 1, features_dim), dtype=np.float32)

    for i in range(n_beats - 1):
        f_start = time_to_frame(beat_times[i])
        f_end = time_to_frame(beat_times[i + 1])

        # Clamp to valid range
        f_start = max(0, min(f_start, n_frames - 1))
        f_end = max(f_start + 1, min(f_end, n_frames))

        interval_frames = mel[f_start:f_end]
        n_interval = len(interval_frames)

        if n_interval == 0:
            continue

        offset = 0

        # Subdivision means
        for sub in range(n_subdivisions):
            sub_start = (n_interval * sub) // n_subdivisions
            sub_end = (n_interval * (sub + 1)) // n_subdivisions
            sub_end = max(sub_end, sub_start + 1)  # at least 1 frame
            sub_frames = interval_frames[sub_start:sub_end]
            features[i, offset:offset + n_mels_actual] = sub_frames.mean(axis=0)
            offset += n_mels_actual

        # Whole-beat peak
        if include_peak:
            features[i, offset:offset + n_mels_actual] = interval_frames.max(axis=0)
            offset += n_mels_actual

        # Duration in frames (normalized: divide by typical beat length at 120 BPM)
        # 120 BPM = 0.5s per beat = 31.25 frames at 62.5 Hz
        typical_frames = FRAME_RATE * 0.5  # 31.25
        features[i, offset] = n_interval / typical_frames

    return features


def load_beat_annotations(label_path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Load beat times and downbeat flags from consensus_v2 JSON.

    Returns:
        beat_times: (n_beats,) sorted beat times in seconds
        is_downbeat: (n_beats,) boolean array
    """
    with open(label_path) as f:
        data = json.load(f)

    hits = data.get("hits", [])
    if not hits:
        return np.array([]), np.array([], dtype=bool)

    # Filter to beats with expectTrigger=true (high-confidence only)
    beats = [h for h in hits if h.get("expectTrigger", True)]
    if not beats:
        return np.array([]), np.array([], dtype=bool)

    # Sort by time
    beats.sort(key=lambda h: h["time"])

    times = np.array([h["time"] for h in beats], dtype=np.float64)
    downbeats = np.array([h.get("isDownbeat", False) for h in beats], dtype=bool)

    return times, downbeats


def create_beat_sequences(
    features: np.ndarray,
    is_downbeat: np.ndarray,
    n_beats: int = 4,
    stride: int = 1,
) -> tuple[np.ndarray, np.ndarray]:
    """Group per-beat features into overlapping sequences.

    The classifier predicts the label of the LAST beat in each sequence
    (it needs N-1 beats of context to classify the Nth).

    Args:
        features: (n_intervals, features_per_beat) — from extract_beat_features
        is_downbeat: (n_intervals,) — downbeat labels (aligned: label[i] is
            for the interval between beat[i] and beat[i+1], classified by
            whether beat[i+1] is a downbeat)
        n_beats: sequence length
        stride: step between sequences

    Returns:
        X: (n_sequences, n_beats, features_per_beat)
        Y: (n_sequences,) downbeat label for last beat in each sequence
    """
    n_intervals = len(features)
    if n_intervals < n_beats:
        return np.empty((0, n_beats, features.shape[1])), np.empty(0)

    sequences = []
    labels = []

    for start in range(0, n_intervals - n_beats + 1, stride):
        seq = features[start:start + n_beats]
        # Label is whether the LAST beat boundary is a downbeat
        label = float(is_downbeat[start + n_beats - 1])
        sequences.append(seq)
        labels.append(label)

    return np.array(sequences, dtype=np.float32), np.array(labels, dtype=np.float32)


def augment_beat_features(
    X: np.ndarray,
    Y: np.ndarray,
    jitter_ms: float = 0.0,
    feature_noise_std: float = 0.0,
    beat_drop_prob: float = 0.0,
    beat_insert_prob: float = 0.0,
    rng: np.random.Generator | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Apply augmentations to beat-level feature sequences.

    Args:
        X: (n_seq, n_beats, features_per_beat)
        Y: (n_seq,) labels
        jitter_ms: max beat time perturbation (affects duration feature)
        feature_noise_std: Gaussian noise std on mel features
        beat_drop_prob: probability of zeroing out a beat in a sequence
        beat_insert_prob: not implemented yet (placeholder)
        rng: numpy random generator

    Returns:
        X_aug, Y_aug: augmented copies (original data is not modified)
    """
    if rng is None:
        rng = np.random.default_rng()

    X_aug = X.copy()
    Y_aug = Y.copy()
    n_seq, n_beats, feat_dim = X_aug.shape

    # Feature noise
    if feature_noise_std > 0:
        noise = rng.normal(0, feature_noise_std, X_aug.shape).astype(np.float32)
        # Don't add noise to the duration feature (last element)
        noise[:, :, -1] = 0
        X_aug += noise
        X_aug = np.clip(X_aug, 0, 2.0)  # mel values are [0,1], allow slight overflow

    # Beat time jitter: perturb the duration feature
    if jitter_ms > 0:
        jitter_frames = (jitter_ms / 1000.0) * FRAME_RATE
        typical_frames = FRAME_RATE * 0.5  # normalization constant
        duration_jitter = rng.uniform(-jitter_frames, jitter_frames,
                                       (n_seq, n_beats)).astype(np.float32)
        duration_jitter /= typical_frames  # same normalization as duration feature
        X_aug[:, :, -1] += duration_jitter
        X_aug[:, :, -1] = np.maximum(X_aug[:, :, -1], 0.1)  # min duration

    # Beat drop: zero out random beats (simulates CBSS missing a beat)
    if beat_drop_prob > 0:
        drop_mask = rng.random((n_seq, n_beats)) < beat_drop_prob
        # Never drop the last beat (that's the classification target)
        drop_mask[:, -1] = False
        X_aug[drop_mask] = 0.0

    return X_aug, Y_aug


def process_track(
    audio_path: Path,
    label_path: Path,
    cfg: dict,
    mel_fb: torch.Tensor,
    window: torch.Tensor,
    device: torch.device,
) -> tuple[np.ndarray, np.ndarray] | None:
    """Process one track: load audio, compute mel, extract beat features.

    Returns:
        (features, is_downbeat) or None if track has insufficient beats.
    """
    beat_cfg = cfg.get("beat_features", {})
    n_subdivisions = beat_cfg.get("subdivisions", 2)
    include_peak = beat_cfg.get("include_peak", True)

    # Load beat annotations
    beat_times, is_downbeat = load_beat_annotations(label_path)
    if len(beat_times) < 5:  # Need at least a few beats
        return None

    # Load audio
    try:
        audio, sr = librosa.load(str(audio_path), sr=SAMPLE_RATE, mono=True)
    except Exception as e:
        print(f"  WARNING: Could not load {audio_path}: {e}")
        return None

    if len(audio) < SAMPLE_RATE * 5:  # Skip very short tracks
        return None

    # Normalize RMS to -35 dB (simulate firmware AGC)
    target_rms_db = cfg["audio"].get("target_rms_db", -35)
    target_rms = 10 ** (target_rms_db / 20.0)
    current_rms = np.sqrt(np.mean(audio ** 2))
    if current_rms > 1e-10:
        audio = audio * (target_rms / current_rms)

    # Compute mel spectrogram (GPU)
    audio_tensor = torch.from_numpy(audio.astype(np.float32)).to(device)
    mel = firmware_mel_spectrogram_torch(audio_tensor, cfg, mel_fb, window)
    # mel: (n_frames, n_mels), values in [0, 1]

    # Filter beats to valid time range
    max_time = len(audio) / SAMPLE_RATE
    valid = beat_times < max_time
    beat_times = beat_times[valid]
    is_downbeat = is_downbeat[valid]

    if len(beat_times) < 5:
        return None

    # Extract per-beat features
    features = extract_beat_features_from_mel(
        mel, beat_times,
        n_subdivisions=n_subdivisions,
        include_peak=include_peak,
    )

    # Downbeat labels: is_downbeat[i+1] labels the interval (beat[i], beat[i+1])
    # features has n_beats-1 rows (one per interval)
    # Align: the feature for interval [beat_i, beat_{i+1}) gets the downbeat
    # label of beat_{i+1} (the beat that ends the interval)
    db_labels = is_downbeat[1:len(beat_times)]  # skip first beat, align with intervals
    if len(db_labels) != len(features):
        db_labels = db_labels[:len(features)]

    return features, db_labels.astype(np.float32)


def main():
    parser = argparse.ArgumentParser(
        description="Extract beat-level features for BeatSyncClassifier")
    parser.add_argument("--config", default="configs/beat_sync.yaml")
    parser.add_argument("--output-dir", default=None,
                        help="Output directory (default: data/beat_sync)")
    parser.add_argument("--augment", action="store_true",
                        help="Apply beat-level augmentations")
    parser.add_argument("--device", default="auto")
    parser.add_argument("--max-tracks", type=int, default=None,
                        help="Limit number of tracks (for debugging)")
    args = parser.parse_args()

    cfg = load_config(args.config)
    beat_cfg = cfg.get("beat_features", {})
    model_cfg = cfg["model"]

    n_beats = model_cfg.get("n_beats", 4)
    audio_dir = Path(cfg["data"]["audio_dir"])
    labels_dir = Path(cfg["data"]["labels_dir"])
    output_dir = Path(args.output_dir or "data/beat_sync")
    output_dir.mkdir(parents=True, exist_ok=True)

    # Device setup
    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # Build mel filterbank (GPU)
    mel_fb = build_mel_filterbank_torch(cfg, device)
    from scipy.signal.windows import hamming
    window = torch.from_numpy(
        hamming(cfg["audio"]["n_fft"], sym=True).astype(np.float32)
    ).to(device)

    # Discover tracks: find label files and match to audio
    label_files = sorted(labels_dir.glob("*.beats.json"))
    print(f"Found {len(label_files)} label files in {labels_dir}")

    if args.max_tracks:
        label_files = label_files[:args.max_tracks]

    # Pre-index audio files for O(1) lookup (avoids repeated rglob per track)
    print(f"Indexing audio files in {audio_dir}...")
    audio_index: dict[str, Path] = {}
    for ext in ["*.mp3", "*.wav", "*.flac", "*.ogg"]:
        for p in audio_dir.rglob(ext):
            audio_index[p.stem] = p
    print(f"  Indexed {len(audio_index)} audio files")

    # Collect all features across tracks
    all_features = []
    all_labels = []
    track_ids = []
    skipped = 0

    for label_path in tqdm(label_files, desc="Extracting beat features"):
        # Find matching audio file
        track_id = label_path.stem.replace(".beats", "")
        audio_path = audio_index.get(track_id)

        if audio_path is None:
            skipped += 1
            continue

        result = process_track(audio_path, label_path, cfg, mel_fb, window, device)
        if result is None:
            skipped += 1
            continue

        features, db_labels = result
        all_features.append(features)
        all_labels.append(db_labels)
        track_ids.append(track_id)

    print(f"\nProcessed {len(track_ids)} tracks, skipped {skipped}")

    if not all_features:
        print("ERROR: No features extracted!")
        return

    # Compute feature statistics
    all_feat_concat = np.concatenate(all_features)
    all_lab_concat = np.concatenate(all_labels)
    print(f"Total beat intervals: {len(all_feat_concat)}")
    print(f"Feature shape per beat: {all_feat_concat.shape[1]}")
    print(f"Downbeat ratio: {all_lab_concat.mean():.4f} "
          f"({all_lab_concat.sum():.0f}/{len(all_lab_concat)})")

    # Train/val split (file-level to prevent leakage)
    val_split = cfg["training"].get("val_split", 0.15)
    rng = np.random.default_rng(cfg["training"].get("seed", 42))
    n_tracks = len(track_ids)
    n_val = max(1, int(n_tracks * val_split))
    perm = rng.permutation(n_tracks)
    val_indices = set(perm[:n_val])

    train_feats, train_labels = [], []
    val_feats, val_labels = [], []

    for i, (feat, lab) in enumerate(zip(all_features, all_labels)):
        if i in val_indices:
            val_feats.append(feat)
            val_labels.append(lab)
        else:
            train_feats.append(feat)
            train_labels.append(lab)

    # Create sequences from each track independently (no cross-track sequences)
    def make_sequences(feat_list, label_list):
        all_X, all_Y = [], []
        for feat, lab in zip(feat_list, label_list):
            X, Y = create_beat_sequences(feat, lab, n_beats=n_beats, stride=1)
            if len(X) > 0:
                all_X.append(X)
                all_Y.append(Y)
        if all_X:
            return np.concatenate(all_X), np.concatenate(all_Y)
        return np.empty((0, n_beats, all_feat_concat.shape[1])), np.empty(0)

    X_train, Y_train = make_sequences(train_feats, train_labels)
    X_val, Y_val = make_sequences(val_feats, val_labels)

    print(f"\nTrain: {len(X_train)} sequences ({n_tracks - n_val} tracks)")
    print(f"Val:   {len(X_val)} sequences ({n_val} tracks)")
    print(f"Train downbeat ratio: {Y_train.mean():.4f}" if len(Y_train) > 0 else "")
    print(f"Val downbeat ratio:   {Y_val.mean():.4f}" if len(Y_val) > 0 else "")

    # Apply augmentation to training set
    if args.augment and len(X_train) > 0:
        jitter_ms = beat_cfg.get("jitter_ms", 15)
        noise_std = beat_cfg.get("feature_noise_std", 0.03)
        drop_prob = beat_cfg.get("beat_drop_prob", 0.05)

        X_aug, Y_aug = augment_beat_features(
            X_train, Y_train,
            jitter_ms=jitter_ms,
            feature_noise_std=noise_std,
            beat_drop_prob=drop_prob,
            rng=rng,
        )
        # Combine original + augmented
        X_train = np.concatenate([X_train, X_aug])
        Y_train = np.concatenate([Y_train, Y_aug])
        print(f"After augmentation: {len(X_train)} train sequences")

    # Save
    print(f"\nSaving to {output_dir}/...")
    np.save(output_dir / "X_train.npy", X_train)
    np.save(output_dir / "Y_train.npy", Y_train)
    np.save(output_dir / "X_val.npy", X_val)
    np.save(output_dir / "Y_val.npy", Y_val)

    # Save metadata
    metadata = {
        "n_beats": n_beats,
        "features_per_beat": int(X_train.shape[2]) if len(X_train) > 0 else 0,
        "n_subdivisions": beat_cfg.get("subdivisions", 2),
        "include_peak": beat_cfg.get("include_peak", True),
        "n_train_sequences": int(len(X_train)),
        "n_val_sequences": int(len(X_val)),
        "n_train_tracks": n_tracks - n_val,
        "n_val_tracks": n_val,
        "train_downbeat_ratio": float(Y_train.mean()) if len(Y_train) > 0 else 0,
        "val_downbeat_ratio": float(Y_val.mean()) if len(Y_val) > 0 else 0,
        "augmented": args.augment,
        "feature_names": _feature_names(beat_cfg),
    }
    with open(output_dir / "metadata.json", "w") as f:
        json.dump(metadata, f, indent=2)

    print("Done!")
    print(f"  X_train: {X_train.shape} ({X_train.nbytes / 1024 / 1024:.1f} MB)")
    print(f"  Y_train: {Y_train.shape}")
    print(f"  X_val:   {X_val.shape}")
    print(f"  Y_val:   {Y_val.shape}")


def _feature_names(beat_cfg: dict) -> list[str]:
    """Generate human-readable feature names for documentation."""
    n_sub = beat_cfg.get("subdivisions", 2)
    names = []
    for s in range(n_sub):
        for b in range(N_MELS):
            names.append(f"sub{s}_mel{b}")
    if beat_cfg.get("include_peak", True):
        for b in range(N_MELS):
            names.append(f"peak_mel{b}")
    names.append("duration_frames")
    return names


if __name__ == "__main__":
    main()
