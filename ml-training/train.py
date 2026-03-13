#!/usr/bin/env python3
"""Train the beat activation CNN (PyTorch, GPU-accelerated).

Usage:
    python train.py --config configs/default.yaml
    python train.py --config configs/default.yaml --epochs 50 --batch-size 32
"""

from __future__ import annotations

import argparse
import csv
import time

import sys
from functools import partial
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset
from models.beat_cnn import build_beat_cnn
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
    parser.add_argument("--loss", default="bce", choices=["bce", "focal", "shift_bce"],
                        help="Loss function: bce (default), focal, or shift_bce (shift-tolerant)")
    parser.add_argument("--focal-gamma", type=float, default=2.0,
                        help="Focal loss gamma (default: 2.0)")
    parser.add_argument("--shift-tolerance", type=int, default=3,
                        help="Shift-tolerant loss: ±N frames tolerance (default: 3 = ±48ms at 62.5 Hz)")
    parser.add_argument("--distill", default=None,
                        help="Path to teacher soft labels (Y_teacher_train.npy) for knowledge distillation")
    parser.add_argument("--distill-alpha", type=float, default=0.3,
                        help="Distillation loss weight (0-1). Total = (1-alpha)*hard + alpha*soft")
    parser.add_argument("--distill-temp", type=float, default=2.0,
                        help="Distillation temperature (default: 2.0)")
    parser.add_argument("--patience", type=int, default=None,
                        help="Early stopping patience (default: from config, or 15)")
    args = parser.parse_args()

    cfg = load_config(args.config)

    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    epochs = args.epochs or cfg["training"]["epochs"]
    batch_size = args.batch_size or cfg["training"]["batch_size"]
    lr = args.lr or cfg["training"]["learning_rate"]
    beat_pos_weight = cfg["training"]["pos_weight"]
    downbeat_pos_weight = cfg["training"].get("downbeat_pos_weight", beat_pos_weight * 4)
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
    distill_alpha = args.distill_alpha
    distill_temp = args.distill_temp
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

    # Diagnostic: measure actual positive ratio for pos_weight verification
    sample_size = min(10000, len(train_ds))
    y_sample = np.load(data_dir / "Y_train.npy", mmap_mode='r')[:sample_size]
    pos_ratio_binary = (y_sample > 0.5).mean()
    pos_ratio_mean = y_sample.mean()
    suggested_pw = (1 - pos_ratio_mean) / max(pos_ratio_mean, 1e-10)
    print(f"  Positive ratio: {pos_ratio_binary:.4f} (>0.5), mean={pos_ratio_mean:.4f}")
    print(f"  Suggested pos_weight: {suggested_pw:.1f} (configured: {beat_pos_weight})")

    train_loader = DataLoader(train_ds, batch_size=batch_size, shuffle=True,
                              num_workers=4, pin_memory=True, persistent_workers=True)
    val_loader = DataLoader(val_ds, batch_size=batch_size, shuffle=False,
                            num_workers=4, pin_memory=True, persistent_workers=True)

    # Build model
    model_type = cfg["model"].get("type", "causal_cnn")
    if model_type == "frame_fc":
        from models.beat_fc import build_beat_fc
        model = build_beat_fc(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"]["dropout"],
            downbeat=use_downbeat,
        ).to(device)
    elif model_type == "frame_conv1d":
        from models.beat_conv1d import build_beat_conv1d
        model = build_beat_conv1d(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            dropout=cfg["model"]["dropout"],
            downbeat=use_downbeat,
        ).to(device)
    else:
        model = build_beat_cnn(
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
    print(f"Model ({model_type}): {total_params} params")
    if model_type == "frame_fc":
        wf = cfg["model"]["window_frames"]
        print(f"  Window: {wf} frames ({wf / cfg['audio']['frame_rate'] * 1000:.0f} ms)")
        print(f"  Hidden: {cfg['model']['hidden_dims']}")
    elif model_type == "frame_conv1d":
        print(f"  Channels: {cfg['model']['channels']}")
        print(f"  Kernels: {cfg['model']['kernel_sizes']}")
        rf = 1
        for k in cfg["model"]["kernel_sizes"]:
            rf += k - 1
        print(f"  Receptive field: {rf} frames ({rf / cfg['audio']['frame_rate'] * 1000:.0f} ms)")

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

    # Select loss function
    if args.loss == "focal":
        loss_fn = partial(weighted_focal, pos_weight=pos_weight, gamma=args.focal_gamma)
        print(f"Loss: focal (gamma={args.focal_gamma})")
    elif args.loss == "shift_bce":
        loss_fn = partial(shift_tolerant_bce, pos_weight=pos_weight,
                          tolerance_frames=args.shift_tolerance)
        print(f"Loss: shift-tolerant BCE (±{args.shift_tolerance} frames = "
              f"±{args.shift_tolerance / cfg['audio']['frame_rate'] * 1000:.0f}ms)")
    else:
        loss_fn = partial(weighted_bce, pos_weight=pos_weight)
        print(f"Loss: weighted BCE")

    # Training loop
    print(f"\nTraining for {epochs} epochs, batch_size={batch_size}, lr={lr}")
    print(f"Output channels: {'beat + downbeat' if use_downbeat else 'beat only'}")
    if use_distill:
        print(f"Distillation: alpha={distill_alpha}, temp={distill_temp}")

    best_val_loss = float("inf")
    patience_counter = 0
    patience = args.patience or cfg["training"].get("patience", 15)
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
        epoch_start = time.time()

        for batch_idx, (X_batch, Y_batch, T_batch) in enumerate(train_loader):
            X_batch = X_batch.to(device, non_blocking=True)
            Y_batch = Y_batch.to(device, non_blocking=True)

            optimizer.zero_grad()
            Y_pred = model(X_batch)
            hard_loss = loss_fn(Y_pred, Y_batch)

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

            if (batch_idx + 1) % log_interval == 0:
                elapsed = time.time() - epoch_start
                eta = elapsed / (batch_idx + 1) * (num_train_batches - batch_idx - 1)
                running_loss = train_loss / train_total * (2 if use_downbeat else 1)
                print(f"  [{batch_idx+1}/{num_train_batches}] "
                      f"loss={running_loss:.4f} "
                      f"elapsed={elapsed:.0f}s eta={eta:.0f}s", flush=True)

        train_loss /= len(train_ds)
        train_acc = train_correct / train_total

        # --- Validate ---
        model.eval()
        val_loss = 0.0
        val_correct = 0
        val_total = 0

        with torch.no_grad():
            for X_batch, Y_batch, T_batch in val_loader:
                X_batch = X_batch.to(device, non_blocking=True)
                Y_batch = Y_batch.to(device, non_blocking=True)

                Y_pred = model(X_batch)
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

        val_loss /= len(val_ds)
        val_acc = val_correct / val_total

        epoch_elapsed = time.time() - epoch_start
        current_lr = optimizer.param_groups[0]["lr"]
        print(f"Epoch {epoch+1}/{epochs} - loss: {train_loss:.4f} - acc: {train_acc:.4f} "
              f"- val_loss: {val_loss:.4f} - val_acc: {val_acc:.4f} - lr: {current_lr:.2e} "
              f"- {epoch_elapsed:.0f}s")

        log_rows.append({
            "epoch": epoch + 1, "loss": train_loss, "binary_accuracy": train_acc,
            "val_loss": val_loss, "val_binary_accuracy": val_acc, "lr": current_lr,
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
        "loss": args.loss,
        "focal_gamma": args.focal_gamma if args.loss == "focal" else None,
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


if __name__ == "__main__":
    main()
