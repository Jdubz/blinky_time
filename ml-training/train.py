#!/usr/bin/env python3
"""Train the onset activation CNN (PyTorch, GPU-accelerated).

Usage:
    python train.py --config configs/default.yaml
    python train.py --config configs/default.yaml --epochs 50 --batch-size 32
"""

from __future__ import annotations

import argparse
import csv
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

    def __init__(self, x_path, y_path, y_db_path=None, y_teacher_path=None):
        self.X = np.load(x_path, mmap_mode='r')
        self.Y = np.load(y_path, mmap_mode='r')
        self.Y_db = np.load(y_db_path, mmap_mode='r') if y_db_path else None
        self.Y_teacher = np.load(y_teacher_path, mmap_mode='r') if y_teacher_path else None
        self._empty_teacher = torch.empty(0)  # Shared placeholder (avoid per-sample alloc)

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        x = torch.from_numpy(self.X[idx].copy()).float()
        y = torch.from_numpy(self.Y[idx].copy()).float()
        if self.Y_db is not None:
            y_db = torch.from_numpy(self.Y_db[idx].copy()).float()
            y_out = torch.stack([y, y_db], dim=-1)
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
                    one weight per output channel (e.g., [10.0, 40.0] for
                    beat + downbeat).
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

    Beat This! paper reports +12.6 F1 on downbeat detection with this loss.

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

    # Use pooled predictions where target is positive (beat/downbeat frames),
    # original predictions where target is negative (non-beat frames)
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


def distillation_loss(student_pred: torch.Tensor, teacher_pred: torch.Tensor,
                      temperature: float = 2.0) -> torch.Tensor:
    """Knowledge distillation loss (KL divergence on softened predictions).

    Uses binary KL divergence since outputs are independent sigmoids, not softmax.
    Temperature softens the teacher's predictions to expose dark knowledge
    (relative confidence between beat positions).

    Args:
        student_pred: Student model output (batch, time, channels), after sigmoid
        teacher_pred: Teacher soft labels (batch, time, 1), beat activation only
        temperature: Softening temperature (higher = softer, more knowledge transfer)
    """
    # Soften predictions with temperature (apply to logits, not probabilities)
    s_logit = torch.log(student_pred[:, :, :1].clamp(1e-7, 1 - 1e-7) /
                        (1 - student_pred[:, :, :1].clamp(1e-7, 1 - 1e-7)))
    t_logit = torch.log(teacher_pred.clamp(1e-7, 1 - 1e-7) /
                        (1 - teacher_pred.clamp(1e-7, 1 - 1e-7)))

    s_soft = torch.sigmoid(s_logit / temperature)
    t_soft = torch.sigmoid(t_logit / temperature)

    # Binary KL divergence
    kl = t_soft * torch.log(t_soft / s_soft.clamp(1e-7)) + \
         (1 - t_soft) * torch.log((1 - t_soft) / (1 - s_soft).clamp(1e-7))

    return kl.mean() * (temperature ** 2)


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
                                 "asymmetric_focal"],
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
    parser.add_argument("--subsample", type=float, default=None,
                        help="Fraction of training data to sample per epoch (0-1). "
                             "Each epoch sees a different random subset. Default: from config, or 1.0")
    parser.add_argument("--num-workers", type=int, default=None,
                        help="DataLoader workers (default: from config, or 4)")
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

    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    epochs = args.epochs or cfg["training"]["epochs"]
    batch_size = args.batch_size or cfg["training"]["batch_size"]
    lr = args.lr or cfg["training"]["learning_rate"]
    beat_pos_weight = cfg["training"].get("pos_weight", 0)  # 0 = auto-calculate from data
    downbeat_pos_weight = cfg["training"].get("downbeat_pos_weight", 0)
    use_downbeat = cfg["model"].get("downbeat", False)

    if args.device is None or args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # Load data (memory-mapped for low RAM usage)
    print(f"Loading data from {data_dir}...")

    db_train_path = data_dir / "Y_db_train.npy"
    db_val_path = data_dir / "Y_db_val.npy"
    has_db = False

    if use_downbeat:
        if not db_train_path.exists() or not db_val_path.exists():
            print("WARNING: downbeat=true but Y_db_train.npy and/or Y_db_val.npy not found. "
                  "Training beat-only.")
            use_downbeat = False
        else:
            # Validate shapes match beat targets before committing to downbeat mode
            y_train_shape = np.load(data_dir / "Y_train.npy", mmap_mode='r').shape
            y_db_train_shape = np.load(db_train_path, mmap_mode='r').shape
            y_val_shape = np.load(data_dir / "Y_val.npy", mmap_mode='r').shape
            y_db_val_shape = np.load(db_val_path, mmap_mode='r').shape
            if y_db_train_shape != y_train_shape or y_db_val_shape != y_val_shape:
                print(f"WARNING: Downbeat target shapes don't match beat targets "
                      f"(Y_db_train: {y_db_train_shape} vs Y_train: {y_train_shape}, "
                      f"Y_db_val: {y_db_val_shape} vs Y_val: {y_val_shape}). "
                      f"Training beat-only.")
                use_downbeat = False
            else:
                has_db = True

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

    train_ds = MemmapBeatDataset(
        data_dir / "X_train.npy", data_dir / "Y_train.npy",
        y_db_path=db_train_path if has_db else None,
        y_teacher_path=teacher_train_path)
    val_ds = MemmapBeatDataset(
        data_dir / "X_val.npy", data_dir / "Y_val.npy",
        y_db_path=db_val_path if has_db else None,
        y_teacher_path=teacher_val_path)

    print(f"Train: {len(train_ds)} chunks, Val: {len(val_ds)} chunks")

    # Auto-calculate pos_weight from actual data positive ratio.
    # Manual overrides in config are fragile — the correct value depends entirely
    # on the label type (consensus beats vs kick-weighted onsets have very different
    # positive ratios). Auto-calculation is always correct.
    sample_size = min(10000, len(train_ds))
    y_sample = np.load(data_dir / "Y_train.npy", mmap_mode='r')[:sample_size]
    pos_ratio_binary = (y_sample > 0.5).mean()
    pos_ratio_mean = y_sample.mean()
    auto_pw = (1 - pos_ratio_mean) / max(pos_ratio_mean, 1e-10)
    if beat_pos_weight <= 0:
        beat_pos_weight = auto_pw
    if downbeat_pos_weight <= 0:
        downbeat_pos_weight = beat_pos_weight * 4
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
            downbeat=use_downbeat,
        ).to(device)
    elif model_type == "frame_fc_enhanced":
        from models.onset_fc_enhanced import build_onset_fc_enhanced
        model = build_onset_fc_enhanced(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"]["dropout"],
            downbeat=use_downbeat,
            se_ratio=cfg["model"].get("se_ratio", 0),
            conv_channels=cfg["model"].get("conv_channels", 0),
            conv_kernel=cfg["model"].get("conv_kernel", 5),
            short_window=cfg["model"].get("short_window", 0),
            short_hidden=cfg["model"].get("short_hidden", 0),
            num_tempo_bins=num_tempo_bins,
        ).to(device)
    elif model_type == "frame_conv1d":
        from models.onset_conv1d import build_onset_conv1d
        model = build_onset_conv1d(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            dropout=cfg["model"]["dropout"],
            downbeat=use_downbeat,
            sum_head=cfg["model"].get("sum_head", False),
            num_tempo_bins=num_tempo_bins,
            freq_pos_encoding=cfg["model"].get("freq_pos_encoding", False),
        ).to(device)
    elif model_type == "frame_conv1d_pool":
        from models.onset_conv1d_pool import build_onset_conv1d_pool
        model = build_onset_conv1d_pool(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            pool_sizes=cfg["model"]["pool_sizes"],
            dropout=cfg["model"]["dropout"],
            downbeat=use_downbeat,
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
            downbeat=use_downbeat,
            model_type=model_type,
            residual=cfg["model"].get("residual", False),
        ).to(device)

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

    # Optimizer + cosine LR schedule
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    total_steps = epochs * len(train_loader)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=total_steps)

    # Build per-channel pos_weight tensor
    if use_downbeat:
        pos_weight = torch.tensor([beat_pos_weight, downbeat_pos_weight], device=device)
        print(f"Pos weights: beat={beat_pos_weight}, downbeat={downbeat_pos_weight}")
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
    if loss_type == "asymmetric_focal":
        loss_cfg = cfg.get("loss", {})
        gamma_pos = loss_cfg.get("gamma_pos", 0.5)
        gamma_neg = loss_cfg.get("gamma_neg", 2.0)
        loss_fn = partial(asymmetric_focal_bce, pos_weight=pos_weight,
                          gamma_pos=gamma_pos, gamma_neg=gamma_neg)
        print(f"Loss: asymmetric focal (gamma_pos={gamma_pos}, gamma_neg={gamma_neg})")
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

    # Online mixup config (SpecMix 2021, DCASE 2024)
    use_mixup = cfg.get("training", {}).get("mixup", False)
    mixup_alpha = cfg.get("training", {}).get("mixup_alpha", 0.4)
    if use_mixup:
        print(f"Mixup: Beta({mixup_alpha}, {mixup_alpha}), p=0.5")

    # Training loop
    steps_per_epoch = len(train_loader)
    print(f"\nTraining for {epochs} epochs, batch_size={batch_size}, lr={lr}")
    print(f"  {steps_per_epoch} steps/epoch, {epochs * steps_per_epoch} total steps")
    patience = args.patience or cfg["training"].get("patience", 15)
    print(f"  Patience: {patience} epochs")
    print(f"Output channels: {'beat + downbeat' if use_downbeat else 'beat only'}")
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

    num_train_batches = len(train_loader)
    num_val_batches = len(val_loader)
    log_interval = max(1, num_train_batches // 10)  # Print ~10 times per epoch

    for epoch in range(epochs):
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

            # Online mixup augmentation (SpecMix 2021, DCASE 2024)
            if use_mixup and np.random.random() < 0.5:
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
                running_loss = train_loss / train_total * (2 if use_downbeat else 1)
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

    # Restore best weights
    model.load_state_dict(torch.load(output_dir / "best_model.pt", weights_only=True))

    # Save final model
    torch.save(model.state_dict(), output_dir / "final_model.pt")
    # Save full model info for export
    torch.save({
        "state_dict": model.state_dict(),
        "config": cfg,
        "use_downbeat": use_downbeat,
        "loss": loss_type,
        "focal_gamma": args.focal_gamma if loss_type == "focal" else None,
        "shift_tolerance": shift_tolerance if loss_type == "shift_bce" else None,
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
