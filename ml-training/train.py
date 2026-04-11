#!/usr/bin/env python3
"""Train the onset activation CNN (PyTorch, GPU-accelerated).

Usage:
    python train.py --config configs/default.yaml
    python train.py --config configs/default.yaml --epochs 50 --batch-size 32
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import time

import sys
from functools import partial
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset, RandomSampler
from models.onset_cnn import build_onset_cnn
from scripts.audio import load_config


class MemmapBeatDataset(Dataset):
    """Dataset backed by memory-mapped .npy files for low RAM usage."""

    def __init__(self, x_path, y_path, y_teacher_path=None, max_features=None,
                 hard_binary_threshold: float = 0.0):
        self.X = np.load(x_path, mmap_mode='r')
        self.Y = np.load(y_path, mmap_mode='r')
        self.Y_teacher = np.load(y_teacher_path, mmap_mode='r') if y_teacher_path else None
        self._empty_teacher = torch.empty(0)  # Shared placeholder (avoid per-sample alloc)
        # Slice features if data has more channels than model expects
        # (e.g., data has 52=mel+delta but model wants 26=mel only)
        self._max_features = max_features
        # If > 0, binarize soft consensus targets at training time:
        # values > threshold → 1.0, else → 0.0. Published onset detectors
        # (Schlüter/Bock 2014, madmom) use hard binary targets, not soft.
        self._hard_binary_threshold = hard_binary_threshold

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        x = torch.from_numpy(self.X[idx].copy()).float()
        if self._max_features is not None and x.shape[-1] > self._max_features:
            x = x[..., :self._max_features]
        y = torch.from_numpy(self.Y[idx].copy()).float()
        if self._hard_binary_threshold > 0:
            y = (y > self._hard_binary_threshold).float()
        if y.dim() == 2:
            # Multi-channel targets (e.g., instrument model: chunk_frames × 3)
            y_out = y
        else:
            y_out = y.unsqueeze(-1)

        if self.Y_teacher is not None:
            t = torch.from_numpy(self.Y_teacher[idx].copy()).float()
            return x, y_out, t
        return x, y_out, self._empty_teacher


def spec_augment(x: torch.Tensor, num_freq_masks: int = 2,
                  max_freq_width: int = 4, num_time_masks: int = 1,
                  max_time_width: int = 8) -> torch.Tensor:
    """Apply SpecAugment (frequency + time masking) to mel spectrograms.

    Fully vectorized — builds a boolean mask on GPU, no per-sample Python loop.
    Each sample gets independently random masks.

    Args:
        x: (batch, time, n_mels) mel spectrogram tensor
        num_freq_masks: Number of frequency masks per sample
        max_freq_width: Maximum mel bands to mask per mask
        num_time_masks: Number of time masks per sample
        max_time_width: Maximum frames to mask per mask
    """
    batch_size, time_steps, n_mels = x.shape
    mask = torch.ones_like(x, dtype=torch.bool)

    # Frequency masks: zero out contiguous mel bands per sample
    freq_idx = torch.arange(n_mels, device=x.device).view(1, 1, n_mels)
    for _ in range(num_freq_masks):
        starts = torch.randint(0, n_mels, (batch_size, 1, 1), device=x.device)
        widths = torch.randint(1, max_freq_width + 1, (batch_size, 1, 1), device=x.device)
        ends = (starts + widths).clamp(max=n_mels)
        mask &= ~((freq_idx >= starts) & (freq_idx < ends))

    # Time masks: zero out contiguous frames per sample
    time_idx = torch.arange(time_steps, device=x.device).view(1, time_steps, 1)
    for _ in range(num_time_masks):
        starts = torch.randint(0, time_steps, (batch_size, 1, 1), device=x.device)
        widths = torch.randint(1, max_time_width + 1, (batch_size, 1, 1), device=x.device)
        ends = (starts + widths).clamp(max=time_steps)
        mask &= ~((time_idx >= starts) & (time_idx < ends))

    return x * mask


def specmix(x: torch.Tensor, y: torch.Tensor,
            alpha: float = 0.4) -> tuple[torch.Tensor, torch.Tensor]:
    """SpecMix: CutMix for mel spectrograms (Kim et al. 2021).

    Cuts a random rectangular region from a shuffled batch and pastes it
    into the original. For frame-level targets, labels are mixed only in
    the pasted time window (not globally) to avoid corrupting supervision
    for unmodified frames.

    Note: SpecMix is not recommended for frame-level onset detection —
    frequency-domain cuts break broadband onset semantics. Prefer standard
    mixup for onset models. See v13 post-mortem in IMPROVEMENT_PLAN.md.

    Args:
        x: (batch, time, n_mels) mel spectrogram tensor
        y: (batch, time, channels) target tensor
        alpha: Beta distribution parameter for lambda sampling
    Returns:
        (mixed_x, mixed_y)
    """
    B, T, F = x.shape
    lam = torch.distributions.Beta(alpha, alpha).sample().item()
    cut_ratio = math.sqrt(1 - lam)
    cut_t = max(1, int(T * cut_ratio))
    cut_f = max(1, int(F * cut_ratio))
    t0 = torch.randint(0, max(1, T - cut_t + 1), (1,)).item()
    f0 = torch.randint(0, max(1, F - cut_f + 1), (1,)).item()
    idx = torch.randperm(B, device=x.device)
    x_out = x.clone()
    x_out[:, t0:t0 + cut_t, f0:f0 + cut_f] = x[idx, t0:t0 + cut_t, f0:f0 + cut_f]
    # Mix labels only in the pasted time window (not globally).
    # freq_ratio captures that only part of the frequency range was replaced.
    freq_ratio = cut_f / F
    y_out = y.clone()
    y_out[:, t0:t0 + cut_t, :] = (
        (1.0 - freq_ratio) * y[:, t0:t0 + cut_t, :]
        + freq_ratio * y[idx, t0:t0 + cut_t, :]
    )
    return x_out, y_out


def _broadcast_pos_weight(pos_weight: torch.Tensor | float,
                          y: torch.Tensor) -> torch.Tensor | float:
    """Reshape pos_weight for broadcasting against y (batch, time, channels)."""
    if isinstance(pos_weight, torch.Tensor) and pos_weight.dim() == 1:
        return pos_weight.view(1, 1, -1)
    return pos_weight


def weighted_bce(y_pred: torch.Tensor, y_true: torch.Tensor,
                 pos_weight: torch.Tensor | float) -> torch.Tensor:
    """Weighted binary cross-entropy with per-channel positive class weights.

    Args:
        pos_weight: scalar (same weight for all channels) or 1D tensor with
                    one weight per output channel (e.g., per-instrument weights).
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)
    bce = nn.functional.binary_cross_entropy(y_pred, y_true, reduction="none")
    weights = y_true * pw + (1 - y_true) * 1.0
    return (bce * weights).mean()


def shift_tolerant_bce(y_pred: torch.Tensor, y_true: torch.Tensor,
                       pos_weight: torch.Tensor | float,
                       tolerance_frames: int = 3) -> torch.Tensor:
    """Shift-tolerant weighted BCE (from Beat This!, Schreiber et al.).

    Max-pools predictions over ±tolerance_frames before computing loss
    for positive targets. This allows the model to fire slightly before
    or after the exact annotation without penalty.

    At 62.5 Hz, tolerance_frames=3 means ±48ms tolerance, matching
    typical annotation jitter in consensus labels.

    Beat This! paper reports significant F1 improvement with this loss.

    Args:
        tolerance_frames: Half-window for max pooling (default 3 = ±48ms at 62.5 Hz)
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)

    pool_size = 2 * tolerance_frames + 1
    # Max-pool predictions in time dimension: (batch, time, channels) → pool over time
    y_pooled = F.max_pool1d(
        y_pred.permute(0, 2, 1),  # (batch, channels, time)
        kernel_size=pool_size, stride=1, padding=tolerance_frames
    ).permute(0, 2, 1)  # back to (batch, time, channels)

    # Use pooled predictions where target is positive (onset frames),
    # original predictions where target is negative (non-onset frames)
    y_effective = torch.where(y_true > 0.5, y_pooled, y_pred)

    bce = nn.functional.binary_cross_entropy(y_effective, y_true, reduction="none")
    weights = y_true * pw + (1 - y_true) * 1.0
    return (bce * weights).mean()


def confidence_shift_bce(y_pred: torch.Tensor, y_true: torch.Tensor,
                         pos_weight: torch.Tensor | float,
                         tolerance_frames: int = 3) -> torch.Tensor:
    """Shift-tolerant BCE with confidence weighting from soft targets.

    Consensus labels encode per-beat agreement as soft target values
    (e.g., 2/7 systems → 0.2857, 7/7 → 1.0). The standard shift_tolerant_bce
    uses y_true > 0.5 to gate shift tolerance, so beats with < 4/7 agreement
    get no tolerance and push the model toward soft outputs.

    This loss reinterprets soft targets as confidence weights on hard (0/1)
    targets:
      - ALL positive frames (y_true > 0) get shift tolerance
      - Model learns to output sharp 1.0 peaks at all beats
      - Low-confidence beats reduce gradient magnitude, not target value

    This produces sharper ODF peaks for CBSS beat tracking.
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)

    # Split soft targets into binary + confidence
    is_positive = y_true > 0
    confidence = torch.where(is_positive, y_true, torch.ones_like(y_true))
    y_binary = is_positive.float()

    pool_size = 2 * tolerance_frames + 1
    y_pooled = F.max_pool1d(
        y_pred.permute(0, 2, 1),
        kernel_size=pool_size, stride=1, padding=tolerance_frames
    ).permute(0, 2, 1)

    # Use pooled predictions for ALL positive frames (not just > 0.5)
    y_effective = torch.where(is_positive, y_pooled, y_pred)

    bce = nn.functional.binary_cross_entropy(y_effective, y_binary, reduction="none")
    class_weight = y_binary * pw + (1 - y_binary) * 1.0
    return (bce * class_weight * confidence).mean()


def asymmetric_focal_bce(y_pred: torch.Tensor, y_true: torch.Tensor,
                         pos_weight: torch.Tensor | float,
                         gamma_pos: float = 0.5,
                         gamma_neg: float = 2.0) -> torch.Tensor:
    """Asymmetric Focal Loss for onset detection (Imoto & Mishima 2022).

    Low gamma_pos preserves gradient for ALL onset frames.
    High gamma_neg aggressively suppresses easy negative (silence) frames.
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)
    bce = F.binary_cross_entropy(y_pred, y_true, reduction='none')
    is_positive = y_true > 0.5

    # Asymmetric focusing: different gamma for positive vs negative
    pt = torch.where(is_positive, y_pred, 1 - y_pred)
    gamma = torch.where(is_positive,
                        torch.tensor(gamma_pos, device=y_pred.device),
                        torch.tensor(gamma_neg, device=y_pred.device))
    focal_weight = (1 - pt) ** gamma

    # Class weight for positive examples
    class_weight = torch.where(is_positive, pw, 1.0)

    return (bce * focal_weight * class_weight).mean()


def shift_tolerant_focal(y_pred: torch.Tensor, y_true: torch.Tensor,
                         pos_weight: torch.Tensor | float,
                         gamma_pos: float = 0.5,
                         gamma_neg: float = 2.0,
                         tolerance_frames: int = 3) -> torch.Tensor:
    """Shift-tolerant asymmetric focal loss — combines the best of both.

    Shift tolerance (Beat This!): max-pools predictions at positive targets
    so the model isn't penalized for firing ±tolerance_frames from annotation.
    Handles annotation jitter in consensus labels.

    Asymmetric focal (Imoto & Mishima 2022): low gamma_pos preserves gradient
    for all onset frames, high gamma_neg suppresses easy negatives.
    Handles the 97% class imbalance.

    These address orthogonal problems and were never tested together before.
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)
    is_positive = y_true > 0.5

    # Shift tolerance: max-pool predictions at positive target positions
    pool_size = 2 * tolerance_frames + 1
    y_pooled = F.max_pool1d(
        y_pred.permute(0, 2, 1),
        kernel_size=pool_size, stride=1, padding=tolerance_frames
    ).permute(0, 2, 1)
    y_effective = torch.where(is_positive, y_pooled, y_pred)

    # BCE on the shift-tolerant predictions
    bce = F.binary_cross_entropy(y_effective, y_true, reduction='none')

    # Asymmetric focal modulation
    pt = torch.where(is_positive, y_effective, 1 - y_effective)
    gamma = torch.where(is_positive,
                        torch.tensor(gamma_pos, device=y_pred.device),
                        torch.tensor(gamma_neg, device=y_pred.device))
    focal_weight = (1 - pt) ** gamma

    class_weight = torch.where(is_positive, pw, 1.0)
    return (bce * focal_weight * class_weight).mean()


def weighted_focal(y_pred: torch.Tensor, y_true: torch.Tensor,
                   pos_weight: torch.Tensor | float,
                   gamma: float = 2.0) -> torch.Tensor:
    """Focal loss (Lin et al. 2017) with per-channel positive class weights.

    Down-weights well-classified (easy) examples with high p_t by a factor of
    (1 - p_t)^gamma, and up-weights hard examples with low p_t, including
    confidently wrong predictions.

    Note: pos_weight and focal modulation multiply together, so easy positives
    are suppressed from both directions. This makes pos_weight feel weaker in
    focal mode than in plain BCE — may need higher pos_weight to compensate.
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)
    bce = nn.functional.binary_cross_entropy(y_pred, y_true, reduction="none")
    p_t = y_true * y_pred + (1 - y_true) * (1 - y_pred)
    focal_weight = (1 - p_t) ** gamma
    class_weight = y_true * pw + (1 - y_true) * 1.0
    return (bce * focal_weight * class_weight).mean()


_owbce_kernel_cache: dict[tuple, torch.Tensor] = {}


def asymmetric_focal_owbce(y_pred: torch.Tensor, y_true: torch.Tensor,
                           pos_weight: torch.Tensor | float,
                           gamma_pos: float = 0.5,
                           gamma_neg: float = 2.0,
                           proximity_window: int = 5,
                           proximity_boost: float = 0.5) -> torch.Tensor:
    """Asymmetric focal loss + onset-proximity weighting (OWBCE).

    Combines asymmetric focal (Imoto & Mishima 2022) with onset-proximity
    boost (Song et al. 2024). A sinusoidal kernel convolved over the onset
    map produces a smooth proximity signal that boosts the positive class
    weight near onsets, training the model to produce sharper activation peaks.

    The proximity boost is applied ONLY to the positive class weight (not the
    total loss). This avoids penalizing the model for correctly predicting low
    values near onset boundaries.
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    pw = _broadcast_pos_weight(pos_weight, y_true)
    bce = F.binary_cross_entropy(y_pred, y_true, reduction='none')
    is_positive = y_true > 0.5

    # Asymmetric focal modulation (no per-batch tensor allocation)
    focal_weight = torch.where(is_positive,
                               (1 - y_pred) ** gamma_pos,
                               y_pred ** gamma_neg)

    # Cached proximity kernel (constant for a given window/device)
    hw = proximity_window
    ks = 2 * hw + 1
    cache_key = (ks, y_pred.device)
    if cache_key not in _owbce_kernel_cache:
        t = torch.linspace(-1, 1, ks, device=y_pred.device)
        _owbce_kernel_cache[cache_key] = (0.5 + 0.5 * torch.cos(math.pi * t)).view(1, 1, ks)
    kernel = _owbce_kernel_cache[cache_key]

    # Use low threshold to include neighbor frames (y=0.25) in onset map
    C = y_true.shape[2]
    onset_map = (y_true > 0.1).float().permute(0, 2, 1)  # (B,C,T)
    proximity = F.conv1d(onset_map, kernel.expand(C, -1, -1),
                         padding=hw, groups=C)
    proximity = proximity.permute(0, 2, 1).clamp(0, 1)  # (B,T,C)

    # Apply proximity boost to positive class weight ONLY (not total loss)
    onset_weight = 1.0 + proximity_boost * proximity
    class_weight = torch.where(is_positive, pw * onset_weight, 1.0)

    return (bce * focal_weight * class_weight).mean()


def distillation_loss(student_pred: torch.Tensor, teacher_pred: torch.Tensor,
                      temperature: float = 2.0) -> torch.Tensor:
    """Knowledge distillation loss — MSE between student and teacher activations.

    MSE is the standard loss for sigmoid KD in sound event detection (DCASE
    2018-2024 baselines). Temperature scaling is not applicable here because
    we only have the teacher's post-sigmoid probabilities, not raw logits.
    The temperature parameter is accepted but ignored for backward compat.

    Args:
        student_pred: Student model output (batch, time, channels), after sigmoid
        teacher_pred: Teacher soft labels (batch, time, 1), continuous [0,1]
        temperature: Ignored (kept for API compatibility)
    """
    return torch.nn.functional.mse_loss(student_pred[:, :, :1], teacher_pred)


def main():
    parser = argparse.ArgumentParser(description="Train beat activation CNN")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--data-dir", default=None, help="Override processed data dir")
    parser.add_argument("--output-dir", default="outputs", help="Output directory for checkpoints/logs")
    parser.add_argument("--epochs", type=int, default=None)
    parser.add_argument("--batch-size", type=int, default=None)
    parser.add_argument("--lr", type=float, default=None)
    parser.add_argument("--device", default=None, help="Device: cuda, cpu, or auto")
    parser.add_argument("--loss", default=None,
                        choices=["bce", "focal", "shift_bce", "confidence_shift_bce",
                                 "asymmetric_focal", "asymmetric_focal_owbce",
                                 "shift_focal", "weighted_bce"],
                        help="Loss function (default: from config, or shift_bce)")
    parser.add_argument("--focal-gamma", type=float, default=2.0,
                        help="Focal loss gamma (default: 2.0)")
    parser.add_argument("--shift-tolerance", type=int, default=None,
                        help="Shift-tolerant loss: ±N frames tolerance (default: from config, or 3)")
    parser.add_argument("--no-spec-augment", action="store_true",
                        help="Disable online SpecAugment during training")
    parser.add_argument("--distill", default=None,
                        help="Path to teacher soft labels (Y_teacher_train.npy) for knowledge distillation")
    parser.add_argument("--distill-alpha", type=float, default=None,
                        help="Distillation loss weight (0-1). Total = (1-alpha)*hard + alpha*soft. "
                             "Default: from config, or 0.3")
    parser.add_argument("--distill-temp", type=float, default=None,
                        help="Distillation temperature. Default: from config, or 2.0")
    parser.add_argument("--patience", type=int, default=None,
                        help="Early stopping patience (default: from config, or 15)")
    parser.add_argument("--swa", action="store_true",
                        help="Enable Stochastic Weight Averaging over final epochs")
    parser.add_argument("--subsample", type=float, default=None,
                        help="Fraction of training data to sample per epoch (0-1). "
                             "Each epoch sees a different random subset. Default: from config, or 1.0")
    parser.add_argument("--num-workers", type=int, default=None,
                        help="DataLoader workers (default: from config, or 4)")
    parser.add_argument("--finetune", default=None,
                        help="Path to pretrained model weights. Loads weights and trains with 10x lower LR. "
                             "For quick experiments: test new labels/loss without full training from scratch.")
    parser.add_argument("--allow-foreground", action="store_true",
                        help="Allow running outside tmux/screen (not recommended)")
    args = parser.parse_args()

    # Guard: training takes hours — must run in tmux/screen to survive session end.
    # Claude Code background tasks die when the session closes, killing training mid-run.
    if not args.allow_foreground:
        in_tmux = os.environ.get("TMUX")
        in_screen = os.environ.get("STY")
        if not (in_tmux or in_screen):
            print("ERROR: Training must run inside tmux or screen to survive session disconnects.")
            print("  tmux new-session -d -s training 'source venv/bin/activate && python train.py ...'")
            print("  Or pass --allow-foreground to override (not recommended).")
            sys.exit(1)

    cfg = load_config(args.config)

    # Validate labels_type vs num_output_channels consistency.
    # instrument labels produce 3-channel targets; the model must match.
    labels_type = cfg.get("labels", {}).get("labels_type", "consensus")
    num_output_channels = cfg["model"].get("num_output_channels", 0)
    if labels_type == "instrument" and num_output_channels != 3:
        print(f"ERROR: labels_type='instrument' produces 3-channel targets but "
              f"num_output_channels={num_output_channels}. Set num_output_channels: 3 in config.",
              file=sys.stderr)
        sys.exit(1)
    if num_output_channels == 3 and labels_type != "instrument":
        print(f"ERROR: num_output_channels=3 but labels_type='{labels_type}'. "
              f"Set labels_type: 'instrument' to generate 3-channel targets.",
              file=sys.stderr)
        sys.exit(1)

    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    epochs = args.epochs or cfg["training"]["epochs"]
    batch_size = args.batch_size or cfg["training"]["batch_size"]
    lr = args.lr or cfg["training"]["learning_rate"]
    beat_pos_weight = cfg["training"].get("pos_weight", 0)  # 0 = auto-calculate from data

    if args.device is None or args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # Load data (memory-mapped for low RAM usage)
    print(f"Loading data from {data_dir}...")

    # Resolve teacher label paths for knowledge distillation
    teacher_train_path = None
    teacher_val_path = None
    use_distill = False
    distill_cfg = cfg.get("distillation", {})
    distill_alpha = args.distill_alpha if args.distill_alpha is not None else distill_cfg.get("alpha", 0.3)
    distill_temp = args.distill_temp if args.distill_temp is not None else distill_cfg.get("temperature", 2.0)
    if args.distill:
        teacher_path = Path(args.distill)
        if teacher_path.exists():
            teacher_train_path = teacher_path
            teacher_val_path = teacher_path.parent / teacher_path.name.replace("train", "val")
            if not teacher_val_path.exists():
                teacher_val_path = None
            use_distill = True
            print(f"Knowledge distillation: alpha={distill_alpha}, temp={distill_temp}")
            print(f"  Teacher train: {teacher_train_path}")
            print(f"  Teacher val: {teacher_val_path}")
        else:
            print(f"WARNING: Teacher labels not found at {teacher_path}, training without distillation")

    # Determine expected feature count from config (for slicing data with extra channels)
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    if use_delta:
        expected_features = cfg["audio"]["n_mels"] * 2
    elif use_band_flux:
        expected_features = cfg["audio"]["n_mels"] + 3
    else:
        expected_features = cfg["audio"]["n_mels"]

    hard_binary_threshold = cfg.get("labels", {}).get("hard_binary_threshold", 0.0)
    if hard_binary_threshold > 0:
        print(f"  Hard binary targets: y > {hard_binary_threshold} → 1.0")
    train_ds = MemmapBeatDataset(
        data_dir / "X_train.npy", data_dir / "Y_train.npy",
        y_teacher_path=teacher_train_path, max_features=expected_features,
        hard_binary_threshold=hard_binary_threshold)
    val_ds = MemmapBeatDataset(
        data_dir / "X_val.npy", data_dir / "Y_val.npy",
        y_teacher_path=teacher_val_path, max_features=expected_features,
        hard_binary_threshold=hard_binary_threshold)

    print(f"Train: {len(train_ds)} chunks, Val: {len(val_ds)} chunks")

    # Auto-calculate pos_weight from actual data positive ratio.
    # Manual overrides in config are fragile — the correct value depends entirely
    # on the label type (consensus beats vs kick-weighted onsets have very different
    # positive ratios). Auto-calculation is always correct.
    sample_size = min(10000, len(train_ds))
    y_all = np.load(data_dir / "Y_train.npy", mmap_mode='r')
    rng = np.random.default_rng(cfg["training"].get("seed", 42))
    sample_idx = rng.choice(len(y_all), size=sample_size, replace=False)
    y_sample = y_all[sample_idx]
    if hard_binary_threshold > 0:
        y_sample = (y_sample > hard_binary_threshold).astype(np.float32)

    # Detect multi-channel targets (e.g., instrument model: N × T × 3)
    num_output_channels = cfg["model"].get("num_output_channels", 0)
    multichannel_targets = y_sample.ndim == 3 and y_sample.shape[2] > 1

    if multichannel_targets:
        # Per-channel auto pos_weight for instrument/multi-output models.
        n_ch = y_sample.shape[2]
        channel_names = cfg["model"].get("output_channel_names", [f"ch{i}" for i in range(n_ch)])
        auto_pws = []
        for ch in range(n_ch):
            ch_mean = y_sample[:, :, ch].mean()
            ch_pw = (1 - ch_mean) / max(ch_mean, 1e-10)
            auto_pws.append(ch_pw)
            print(f"  {channel_names[ch] if ch < len(channel_names) else f'ch{ch}'}: "
                  f"positive ratio={ch_mean:.4f}, auto pos_weight={ch_pw:.1f}")
        # Config can override with a list, e.g., pos_weight: [10.0, 8.0, 5.0]
        cfg_pw = cfg["training"].get("pos_weight", 0)
        if isinstance(cfg_pw, list) and len(cfg_pw) == n_ch:
            per_channel_pw = cfg_pw
            print(f"  pos_weight: {per_channel_pw} (from config)")
        else:
            per_channel_pw = auto_pws
            print(f"  pos_weight: auto {[f'{pw:.1f}' for pw in auto_pws]}")
    else:
        pos_ratio_binary = (y_sample > 0.5).mean()
        pos_ratio_mean = y_sample.mean()
        auto_pw = (1 - pos_ratio_mean) / max(pos_ratio_mean, 1e-10)
        if beat_pos_weight <= 0:
            beat_pos_weight = auto_pw
        print(f"  Positive ratio: {pos_ratio_binary:.4f} (>0.5), mean={pos_ratio_mean:.4f}")
        print(f"  pos_weight: {beat_pos_weight:.1f} (auto={auto_pw:.1f})")

    num_workers = args.num_workers if args.num_workers is not None else cfg["training"].get("num_workers", 4)
    subsample = args.subsample if args.subsample is not None else cfg["training"].get("subsample", 1.0)

    if subsample < 1.0:
        subset_size = max(1, int(len(train_ds) * subsample))
        train_sampler = RandomSampler(train_ds, num_samples=subset_size, replacement=False)
        train_loader = DataLoader(train_ds, batch_size=batch_size, sampler=train_sampler,
                                  num_workers=num_workers, pin_memory=True, persistent_workers=True)
        print(f"Subsampling: {subset_size:,} of {len(train_ds):,} per epoch ({subsample:.0%})")
    else:
        train_loader = DataLoader(train_ds, batch_size=batch_size, shuffle=True,
                                  num_workers=num_workers, pin_memory=True, persistent_workers=True)
    val_loader = DataLoader(val_ds, batch_size=batch_size, shuffle=False,
                            num_workers=num_workers, pin_memory=True, persistent_workers=True)

    # Build model
    model_type = cfg["model"].get("type", "causal_cnn")
    num_tempo_bins = cfg["model"].get("num_tempo_bins", 0)
    if model_type == "frame_fc":
        from models.onset_fc import build_onset_fc
        model = build_onset_fc(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"]["dropout"],
        ).to(device)
    elif model_type == "frame_fc_enhanced":
        from models.onset_fc_enhanced import build_onset_fc_enhanced
        model = build_onset_fc_enhanced(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"]["dropout"],
            se_ratio=cfg["model"].get("se_ratio", 0),
            conv_channels=cfg["model"].get("conv_channels", 0),
            conv_kernel=cfg["model"].get("conv_kernel", 5),
            short_window=cfg["model"].get("short_window", 0),
            short_hidden=cfg["model"].get("short_hidden", 0),
            num_tempo_bins=num_tempo_bins,
        ).to(device)
    elif model_type == "frame_conv1d":
        from models.onset_conv1d import build_onset_conv1d
        # With delta features, input has 2× n_mels channels (mel + delta_mel)
        use_delta = cfg.get("features", {}).get("use_delta", False)
        use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
        if use_delta:
            input_features = cfg["audio"]["n_mels"] * 2
        elif use_band_flux:
            input_features = cfg["audio"]["n_mels"] + 3
        else:
            input_features = cfg["audio"]["n_mels"]
        model = build_onset_conv1d(
            n_mels=input_features,
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            dropout=cfg["model"]["dropout"],
            num_tempo_bins=num_tempo_bins,
            freq_pos_encoding=cfg["model"].get("freq_pos_encoding", False),
            num_output_channels=cfg["model"].get("num_output_channels", 0),
        ).to(device)
    elif model_type == "frame_conv1d_pool":
        from models.onset_conv1d_pool import build_onset_conv1d_pool
        model = build_onset_conv1d_pool(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            pool_sizes=cfg["model"]["pool_sizes"],
            dropout=cfg["model"]["dropout"],
            use_stride=cfg["model"].get("use_stride", False),
        ).to(device)
    else:
        model = build_onset_cnn(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_size=cfg["model"]["kernel_size"],
            dilations=cfg["model"]["dilations"],
            dropout=cfg["model"]["dropout"],
            chunk_frames=cfg["training"]["chunk_frames"],
            model_type=model_type,
            residual=cfg["model"].get("residual", False),
        ).to(device)

    # Load pretrained weights for fine-tuning
    if args.finetune:
        ft_path = Path(args.finetune)
        if not ft_path.exists():
            print(f"ERROR: Finetune model not found: {ft_path}", file=sys.stderr)
            sys.exit(1)
        ft_state = torch.load(ft_path, map_location=device, weights_only=True)
        if isinstance(ft_state, dict) and "state_dict" in ft_state:
            ft_state = ft_state["state_dict"]
        # strict=False allows fine-tuning across output head changes (e.g., 1→3 channels).
        # Optimizer state is discarded — fresh Adam moments for the new task.
        load_result = model.load_state_dict(ft_state, strict=False)
        if load_result.missing_keys or load_result.unexpected_keys:
            print(f"  Fine-tune partial load:")
            if load_result.missing_keys:
                print(f"    Missing (randomly initialized): {load_result.missing_keys}")
            if load_result.unexpected_keys:
                print(f"    Unexpected (ignored): {load_result.unexpected_keys}")
        # Reduce LR 10x for fine-tuning (pretrained features, just adjusting to new labels)
        lr = lr / 10
        print(f"Fine-tuning from {ft_path} (LR reduced to {lr:.1e})")

    total_params = sum(p.numel() for p in model.parameters())
    print(f"Model ({model_type}): {total_params:,} params")
    if model_type in ("frame_fc", "frame_fc_enhanced"):
        wf = cfg["model"]["window_frames"]
        print(f"  Window: {wf} frames ({wf / cfg['audio']['frame_rate'] * 1000:.0f} ms)")
        print(f"  Hidden: {cfg['model']['hidden_dims']}")
        if model_type == "frame_fc_enhanced":
            features = []
            if cfg["model"].get("se_ratio", 0) > 0:
                features.append(f"SE(ratio={cfg['model']['se_ratio']})")
            if cfg["model"].get("conv_channels", 0) > 0:
                features.append(f"Conv1D({cfg['model']['conv_channels']}ch)")
            if cfg["model"].get("short_window", 0) > 0:
                features.append(f"MultiWindow(short={cfg['model']['short_window']})")
            if num_tempo_bins > 0:
                features.append(f"Tempo({num_tempo_bins} bins)")
            print(f"  Enhancements: {', '.join(features)}")
    elif model_type in ("frame_conv1d", "frame_conv1d_pool"):
        print(f"  Channels: {cfg['model']['channels']}")
        print(f"  Kernels: {cfg['model']['kernel_sizes']}")
        rf = 1
        for k in cfg["model"]["kernel_sizes"]:
            rf += k - 1
        print(f"  Receptive field: {rf} frames ({rf / cfg['audio']['frame_rate'] * 1000:.0f} ms)")
        if num_tempo_bins > 0:
            _tc = cfg["model"].get("tempo", {})
            print(f"  Tempo auxiliary head: {num_tempo_bins} bins "
                  f"({_tc.get('min_bpm', 60)}-{_tc.get('max_bpm', 200)} BPM, "
                  f"weight={_tc.get('loss_weight', 0.1)})")
        if model_type == "frame_conv1d_pool":
            pool_sizes = cfg["model"]["pool_sizes"]
            pool_factor = 1
            for p in pool_sizes:
                pool_factor *= p
            print(f"  Pool sizes: {pool_sizes} (total {pool_factor}x temporal compression)")
            wf = cfg["model"]["window_frames"]
            print(f"  Window: {wf} frames -> {wf // pool_factor} output frames")

    # Optimizer + cosine LR schedule with warmup
    # 3 epochs of linear warmup stabilizes early training with large batches
    # (Goyal et al. 2017). Prevents overshooting when batch_size/dataset is large.
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    total_steps = epochs * len(train_loader)
    warmup_steps = min(3 * len(train_loader), total_steps // 2)  # 3 epochs, clamped to half total
    warmup_scheduler = torch.optim.lr_scheduler.LinearLR(
        optimizer, start_factor=0.1, end_factor=1.0, total_iters=max(warmup_steps, 1))
    cosine_scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=max(total_steps - warmup_steps, 1))
    scheduler = torch.optim.lr_scheduler.SequentialLR(
        optimizer, schedulers=[warmup_scheduler, cosine_scheduler],
        milestones=[warmup_steps])

    # Build per-channel pos_weight tensor
    if multichannel_targets:
        pos_weight = torch.tensor(per_channel_pw, dtype=torch.float32, device=device)
        print(f"Pos weights (per-channel): {per_channel_pw}")
    else:
        pos_weight = beat_pos_weight

    # Select loss function (CLI overrides config, config overrides default)
    loss_type = args.loss or cfg.get("loss", {}).get("type", "shift_bce")
    shift_tolerance = (args.shift_tolerance if args.shift_tolerance is not None
                       else cfg.get("loss", {}).get("shift_tolerance", 3))
    # Scale shift tolerance for pooled models: each output frame already spans
    # pool_factor input frames, so ±3 at 62.5 Hz (±48ms) becomes ±0 at 3.9 Hz
    pf = getattr(model, 'pool_factor', 1)
    if pf > 1:
        shift_tolerance = max(0, shift_tolerance // pf)
    if loss_type == "shift_focal":
        loss_cfg = cfg.get("loss", {})
        gamma_pos = loss_cfg.get("gamma_pos", 0.5)
        gamma_neg = loss_cfg.get("gamma_neg", 2.0)
        loss_fn = partial(shift_tolerant_focal, pos_weight=pos_weight,
                          gamma_pos=gamma_pos, gamma_neg=gamma_neg,
                          tolerance_frames=shift_tolerance)
        print(f"Loss: shift-tolerant focal (gamma_pos={gamma_pos}, gamma_neg={gamma_neg}, "
              f"tolerance={shift_tolerance} frames)")
    elif loss_type == "asymmetric_focal":
        loss_cfg = cfg.get("loss", {})
        gamma_pos = loss_cfg.get("gamma_pos", 0.5)
        gamma_neg = loss_cfg.get("gamma_neg", 2.0)
        loss_fn = partial(asymmetric_focal_bce, pos_weight=pos_weight,
                          gamma_pos=gamma_pos, gamma_neg=gamma_neg)
        print(f"Loss: asymmetric focal (gamma_pos={gamma_pos}, gamma_neg={gamma_neg})")
    elif loss_type == "asymmetric_focal_owbce":
        loss_cfg = cfg.get("loss", {})
        gamma_pos = loss_cfg.get("gamma_pos", 0.5)
        gamma_neg = loss_cfg.get("gamma_neg", 2.0)
        prox_window = loss_cfg.get("proximity_window", 5)
        prox_boost = loss_cfg.get("proximity_boost", 0.5)
        loss_fn = partial(asymmetric_focal_owbce, pos_weight=pos_weight,
                          gamma_pos=gamma_pos, gamma_neg=gamma_neg,
                          proximity_window=prox_window, proximity_boost=prox_boost)
        print(f"Loss: asymmetric focal + OWBCE (gamma_pos={gamma_pos}, gamma_neg={gamma_neg}, "
              f"proximity_window=±{prox_window} frames, boost={prox_boost})")
    elif loss_type == "focal":
        loss_fn = partial(weighted_focal, pos_weight=pos_weight, gamma=args.focal_gamma)
        print(f"Loss: focal (gamma={args.focal_gamma})")
    elif loss_type == "confidence_shift_bce":
        loss_fn = partial(confidence_shift_bce, pos_weight=pos_weight,
                          tolerance_frames=shift_tolerance)
        effective_rate = cfg['audio']['frame_rate'] / pf
        print(f"Loss: confidence-weighted shift-tolerant BCE (±{shift_tolerance} frames = "
              f"±{shift_tolerance / effective_rate * 1000:.0f}ms at {effective_rate:.1f} Hz)")
    elif loss_type == "shift_bce":
        loss_fn = partial(shift_tolerant_bce, pos_weight=pos_weight,
                          tolerance_frames=shift_tolerance)
        effective_rate = cfg['audio']['frame_rate'] / pf
        print(f"Loss: shift-tolerant BCE (±{shift_tolerance} frames = "
              f"±{shift_tolerance / effective_rate * 1000:.0f}ms at {effective_rate:.1f} Hz)")
    else:
        loss_fn = partial(weighted_bce, pos_weight=pos_weight)
        print(f"Loss: weighted BCE")

    # SpecAugment config (online augmentation during training)
    sa_cfg = cfg.get("augmentation", {}).get("spec_augment", {})
    use_spec_augment = sa_cfg.get("enabled", False) and not args.no_spec_augment
    sa_num_freq = sa_cfg.get("num_freq_masks", 2)
    sa_max_freq = sa_cfg.get("max_freq_width", 4)
    sa_num_time = sa_cfg.get("num_time_masks", 1)
    sa_max_time = sa_cfg.get("max_time_width", 8)
    if use_spec_augment:
        print(f"SpecAugment: {sa_num_freq} freq masks (max {sa_max_freq} bands), "
              f"{sa_num_time} time masks (max {sa_max_time} frames)")

    # Online mixup / SpecMix config
    use_specmix = cfg.get("training", {}).get("specmix", False)
    specmix_alpha = cfg.get("training", {}).get("specmix_alpha", 0.4)
    use_mixup = cfg.get("training", {}).get("mixup", False) and not use_specmix
    mixup_alpha = cfg.get("training", {}).get("mixup_alpha", 0.4)
    if use_specmix:
        print(f"SpecMix (CutMix): Beta({specmix_alpha}, {specmix_alpha}), p=0.5")
    elif use_mixup:
        print(f"Mixup: Beta({mixup_alpha}, {mixup_alpha}), p=0.5")

    # Training loop
    steps_per_epoch = len(train_loader)
    print(f"\nTraining for {epochs} epochs, batch_size={batch_size}, lr={lr}")
    print(f"  {steps_per_epoch} steps/epoch, {epochs * steps_per_epoch} total steps")
    patience = args.patience or cfg["training"].get("patience", 15)
    print(f"  Patience: {patience} epochs")
    print(f"Output channels: {model.out_channels}")
    if use_distill:
        print(f"Distillation: alpha={distill_alpha}, temp={distill_temp}")

    # Pre-compute tempo config (used in inner loop when tempo aux is enabled)
    tempo_cfg = cfg["model"].get("tempo", {})
    tempo_min_bpm = tempo_cfg.get("min_bpm", 60)
    tempo_max_bpm = tempo_cfg.get("max_bpm", 200)
    tempo_loss_weight = tempo_cfg.get("loss_weight", 0.1)

    best_val_loss = float("inf")
    patience_counter = 0
    log_rows = []
    start_epoch = 0

    # Resume from checkpoint if available
    ckpt_path = output_dir / "training_checkpoint.pt"
    if ckpt_path.exists():
        print(f"Resuming from checkpoint: {ckpt_path}")
        ckpt = torch.load(ckpt_path, weights_only=False)
        model.load_state_dict(ckpt["model_state"])
        optimizer.load_state_dict(ckpt["optimizer_state"])
        scheduler.load_state_dict(ckpt["scheduler_state"])
        start_epoch = ckpt["epoch"] + 1
        best_val_loss = ckpt["best_val_loss"]
        patience_counter = ckpt["patience_counter"]
        log_rows = ckpt.get("log_rows", [])
        print(f"  Resuming at epoch {start_epoch + 1}/{epochs}, best_val_loss={best_val_loss:.4f}")

    num_train_batches = len(train_loader)
    num_val_batches = len(val_loader)
    log_interval = max(1, num_train_batches // 10)  # Print ~10 times per epoch

    for epoch in range(start_epoch, epochs):
        # --- Train ---
        model.train()
        train_loss = 0.0
        train_correct = 0
        train_total = 0
        train_samples = 0
        epoch_start = time.time()

        for batch_idx, (X_batch, Y_batch, T_batch) in enumerate(train_loader):
            X_batch = X_batch.to(device, non_blocking=True)
            Y_batch = Y_batch.to(device, non_blocking=True)

            # Online SpecAugment: random freq/time masking (different each batch)
            if use_spec_augment:
                X_batch = spec_augment(X_batch, sa_num_freq, sa_max_freq,
                                       sa_num_time, sa_max_time)

            # Online augmentation: SpecMix (CutMix) or standard mixup
            if use_specmix and np.random.random() < 0.5:
                X_batch, Y_batch = specmix(X_batch, Y_batch, specmix_alpha)
            elif use_mixup and np.random.random() < 0.5:
                lam = np.random.beta(mixup_alpha, mixup_alpha)
                idx = torch.randperm(X_batch.size(0), device=X_batch.device)
                X_batch = lam * X_batch + (1 - lam) * X_batch[idx]
                Y_batch = lam * Y_batch + (1 - lam) * Y_batch[idx]

            optimizer.zero_grad()
            model_out = model(X_batch)

            # Handle tempo auxiliary head (enhanced model returns tuple)
            if isinstance(model_out, tuple):
                Y_pred, tempo_logits = model_out
            else:
                Y_pred = model_out
                tempo_logits = None

            # Downsample labels for pooling models (output time < input time)
            # Max-pool over each pool window so any beat within the window is preserved.
            # Point-sampling (stride) loses ~95% of binary beat labels.
            if hasattr(model, 'pool_factor') and model.pool_factor > 1:
                pf = model.pool_factor
                T = Y_batch.shape[1]
                T_trunc = (T // pf) * pf
                # (batch, T_trunc, channels) -> (batch, T_trunc//pf, pf, channels) -> max over pf
                Y_batch = Y_batch[:, :T_trunc].reshape(
                    Y_batch.shape[0], -1, pf, Y_batch.shape[2]
                ).max(dim=2).values
                Y_batch = Y_batch[:, :Y_pred.shape[1]]

            hard_loss = loss_fn(Y_pred, Y_batch)

            # Tempo auxiliary loss (Bock et al. 2019: +5 F1 from tempo regularization)
            if tempo_logits is not None and num_tempo_bins > 0:
                # Compute tempo from beat targets: median IBI → BPM → bin
                with torch.no_grad():
                    beat_mask = Y_batch[:, :, 0] > 0.5  # (batch, time)
                    tempo_targets = torch.zeros(Y_batch.shape[0], dtype=torch.long, device=device)
                    frame_rate = cfg["audio"]["frame_rate"]
                    for bi in range(Y_batch.shape[0]):
                        beat_frames = beat_mask[bi].nonzero(as_tuple=True)[0]
                        if len(beat_frames) >= 2:
                            ibis = beat_frames[1:] - beat_frames[:-1]
                            median_ibi = ibis.float().median()
                            bpm = 60.0 * frame_rate / max(median_ibi.item(), 1)
                            bpm = max(tempo_min_bpm, min(tempo_max_bpm, bpm))
                            bin_idx = int((bpm - tempo_min_bpm) / (tempo_max_bpm - tempo_min_bpm) * num_tempo_bins)
                            tempo_targets[bi] = min(bin_idx, num_tempo_bins - 1)
                tempo_loss = F.cross_entropy(tempo_logits, tempo_targets)
                hard_loss = hard_loss + tempo_loss_weight * tempo_loss

            # Knowledge distillation: blend hard and soft losses
            if use_distill and T_batch.numel() > 0:
                T_batch = T_batch.to(device, non_blocking=True)
                if T_batch.dim() == 2:
                    T_batch = T_batch.unsqueeze(-1)  # (batch, time) → (batch, time, 1)
                soft_loss = distillation_loss(Y_pred, T_batch, temperature=distill_temp)
                loss = (1 - distill_alpha) * hard_loss + distill_alpha * soft_loss
            else:
                loss = hard_loss

            loss.backward()
            optimizer.step()
            scheduler.step()

            train_loss += loss.item() * X_batch.size(0)
            train_correct += ((Y_pred > 0.5) == (Y_batch > 0.5)).float().sum().item()
            train_total += Y_batch.numel()
            train_samples += X_batch.size(0)

            if (batch_idx + 1) % log_interval == 0:
                elapsed = time.time() - epoch_start
                eta = elapsed / (batch_idx + 1) * (num_train_batches - batch_idx - 1)
                running_loss = train_loss / max(train_samples, 1)
                print(f"  [{batch_idx+1}/{num_train_batches}] "
                      f"loss={running_loss:.4f} "
                      f"elapsed={elapsed:.0f}s eta={eta:.0f}s", flush=True)

        train_loss /= max(train_samples, 1)
        train_acc = train_correct / train_total

        # --- Validate ---
        model.eval()
        val_loss = 0.0
        val_correct = 0
        val_total = 0
        # Precision/recall accumulators (interpretable across label types)
        val_tp = 0
        val_fp = 0
        val_fn = 0

        with torch.no_grad():
            for X_batch, Y_batch, T_batch in val_loader:
                X_batch = X_batch.to(device, non_blocking=True)
                Y_batch = Y_batch.to(device, non_blocking=True)

                model_out = model(X_batch)
                Y_pred = model_out[0] if isinstance(model_out, tuple) else model_out
                # Downsample labels for pooling models (max-pool to preserve beats)
                if hasattr(model, 'pool_factor') and model.pool_factor > 1:
                    pf = model.pool_factor
                    T = Y_batch.shape[1]
                    T_trunc = (T // pf) * pf
                    Y_batch = Y_batch[:, :T_trunc].reshape(
                        Y_batch.shape[0], -1, pf, Y_batch.shape[2]
                    ).max(dim=2).values
                    Y_batch = Y_batch[:, :Y_pred.shape[1]]
                hard_loss = loss_fn(Y_pred, Y_batch)

                if use_distill and T_batch.numel() > 0:
                    T_batch = T_batch.to(device, non_blocking=True)
                    if T_batch.dim() == 2:
                        T_batch = T_batch.unsqueeze(-1)
                    soft_loss = distillation_loss(Y_pred, T_batch, temperature=distill_temp)
                    loss = (1 - distill_alpha) * hard_loss + distill_alpha * soft_loss
                else:
                    loss = hard_loss

                val_loss += loss.item() * X_batch.size(0)
                val_correct += ((Y_pred > 0.5) == (Y_batch > 0.5)).float().sum().item()
                val_total += Y_batch.numel()

                # Precision/recall on first channel (onset/beat)
                pred_pos = Y_pred[:, :, 0] > 0.5
                ref_pos = Y_batch[:, :, 0] > 0.5
                val_tp += (pred_pos & ref_pos).sum().item()
                val_fp += (pred_pos & ~ref_pos).sum().item()
                val_fn += (~pred_pos & ref_pos).sum().item()

        val_loss /= len(val_ds)
        val_acc = val_correct / val_total
        val_precision = val_tp / max(val_tp + val_fp, 1)
        val_recall = val_tp / max(val_tp + val_fn, 1)
        val_f1 = 2 * val_precision * val_recall / max(val_precision + val_recall, 1e-10)

        epoch_elapsed = time.time() - epoch_start
        current_lr = optimizer.param_groups[0]["lr"]
        print(f"Epoch {epoch+1}/{epochs} - loss: {train_loss:.4f} - acc: {train_acc:.4f} "
              f"- val_loss: {val_loss:.4f} - val_acc: {val_acc:.4f} "
              f"- P: {val_precision:.3f} R: {val_recall:.3f} F1: {val_f1:.3f} "
              f"- lr: {current_lr:.2e} - {epoch_elapsed:.0f}s")

        log_rows.append({
            "epoch": epoch + 1, "loss": train_loss, "binary_accuracy": train_acc,
            "val_loss": val_loss, "val_binary_accuracy": val_acc,
            "val_precision": val_precision, "val_recall": val_recall, "val_f1": val_f1,
            "lr": current_lr,
        })

        # Checkpointing
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            patience_counter = 0
            torch.save(model.state_dict(), output_dir / "best_model.pt")
            print(f"  Saved best model (val_loss={val_loss:.4f})")
        else:
            patience_counter += 1
            if patience_counter >= patience:
                print(f"  Early stopping at epoch {epoch+1} (no improvement for {patience} epochs)")
                break

        # Save resumable checkpoint every epoch
        torch.save({
            "model_state": model.state_dict(),
            "optimizer_state": optimizer.state_dict(),
            "scheduler_state": scheduler.state_dict(),
            "epoch": epoch,
            "best_val_loss": best_val_loss,
            "patience_counter": patience_counter,
            "log_rows": log_rows,
        }, output_dir / "training_checkpoint.pt")

    # Restore best weights
    model.load_state_dict(torch.load(output_dir / "best_model.pt", weights_only=True))

    # Stochastic Weight Averaging: retrain from best weights with constant LR,
    # average weights across epochs. Finds flatter optima that generalize better.
    # (Izmailov et al. 2018). Zero cost at inference.
    if args.swa:
        from torch.optim.swa_utils import AveragedModel, SWALR
        swa_epochs = 10
        swa_lr = 1e-4
        print(f"\nSWA: {swa_epochs} epochs at lr={swa_lr}")
        swa_model = AveragedModel(model)
        swa_optimizer = torch.optim.Adam(model.parameters(), lr=swa_lr)

        for swa_ep in range(swa_epochs):
            model.train()
            for batch_idx, (X_batch, Y_batch, T_batch) in enumerate(train_loader):
                X_batch = X_batch.to(device)
                Y_batch = Y_batch.to(device)
                swa_optimizer.zero_grad()
                model_out = model(X_batch)
                Y_pred = model_out[0] if isinstance(model_out, tuple) else model_out
                loss = loss_fn(Y_pred, Y_batch)
                loss.backward()
                swa_optimizer.step()
            swa_model.update_parameters(model)
            print(f"  SWA epoch {swa_ep+1}/{swa_epochs} loss={loss.item():.4f}")

        # Replace model weights with SWA average
        torch.optim.swa_utils.update_bn(train_loader, swa_model, device=device)
        model.load_state_dict(swa_model.module.state_dict())
        print("  SWA weights applied")

    # Save final model
    torch.save(model.state_dict(), output_dir / "final_model.pt")
    # Save full model info for export
    torch.save({
        "state_dict": model.state_dict(),
        "config": cfg,
        "loss": loss_type,
        "focal_gamma": args.focal_gamma if loss_type == "focal" else None,
        "shift_tolerance": shift_tolerance if loss_type in ("shift_bce", "shift_focal") else None,
        "spec_augment": use_spec_augment,
        "distillation": {"alpha": distill_alpha, "temp": distill_temp} if use_distill else None,
    }, output_dir / "model_checkpoint.pt")

    # Save training log
    with open(output_dir / "training_log.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=log_rows[0].keys())
        writer.writeheader()
        writer.writerows(log_rows)

    print(f"\nTraining complete. Models saved to {output_dir}/")
    best_epoch = min(log_rows, key=lambda r: r["val_loss"])
    print(f"Best epoch: {best_epoch['epoch']}")
    print(f"  val_loss: {best_epoch['val_loss']:.4f}")
    print(f"  val_binary_accuracy: {best_epoch['val_binary_accuracy']:.4f}")
    print(f"  val_precision: {best_epoch['val_precision']:.4f}")
    print(f"  val_recall: {best_epoch['val_recall']:.4f}")
    print(f"  val_f1: {best_epoch['val_f1']:.4f}")


if __name__ == "__main__":
    main()
