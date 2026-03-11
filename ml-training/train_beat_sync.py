#!/usr/bin/env python3
"""Train the beat-synchronous classifier (PyTorch, GPU-accelerated).

Trains BeatSyncClassifier on beat-level features extracted by
beat_feature_extractor.py.  Much faster than frame-level training
(~80K examples at 316 floats each vs 1.2M chunks at 128×26).

Usage:
    python train_beat_sync.py --config configs/beat_sync.yaml
    python train_beat_sync.py --config configs/beat_sync.yaml --data-dir data/beat_sync
    python train_beat_sync.py --config configs/beat_sync.yaml --phase B
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset

from models.beat_sync import build_beat_sync
from scripts.audio import load_config


class BeatSyncDataset(Dataset):
    """Dataset of beat-level feature sequences for BeatSyncClassifier."""

    def __init__(self, x_path: str | Path, y_path: str | Path,
                 mean: np.ndarray | None = None, std: np.ndarray | None = None):
        self.X = np.load(x_path, mmap_mode='r')
        self.Y = np.load(y_path, mmap_mode='r')
        self.mean = mean  # (features_per_beat,) or None
        self.std = std    # (features_per_beat,) or None
        assert len(self.X) == len(self.Y), \
            f"X/Y length mismatch: {len(self.X)} vs {len(self.Y)}"

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        x = torch.from_numpy(self.X[idx].copy()).float()
        # Z-score normalize per feature (broadcast across beats)
        if self.mean is not None and self.std is not None:
            x = (x - self.mean) / self.std
        y = torch.tensor(self.Y[idx], dtype=torch.float32)
        return x, y


def compute_feature_stats(x_path: str | Path,
                          max_samples: int = 100000) -> tuple[np.ndarray, np.ndarray]:
    """Compute per-feature mean and std from training data.

    Returns (mean, std) as numpy arrays of shape (features_per_beat,).
    Stats are computed across all beats in all sequences.
    """
    X = np.load(x_path, mmap_mode='r')
    n = min(max_samples, len(X))
    # Reshape to (n * n_beats, features_per_beat)
    sample = X[:n].reshape(-1, X.shape[-1])
    mean = sample.mean(axis=0).astype(np.float32)
    std = sample.std(axis=0).astype(np.float32)
    # Prevent division by zero
    std = np.maximum(std, 1e-6)
    return mean, std


def focal_loss(y_pred: torch.Tensor, y_true: torch.Tensor,
               alpha: float = 0.75, gamma: float = 2.0) -> torch.Tensor:
    """Binary focal loss for class-imbalanced downbeat detection.

    Args:
        y_pred: (batch,) predicted probabilities after sigmoid
        y_true: (batch,) binary labels (0 or 1)
        alpha: weight for positive class (downbeats)
        gamma: focusing parameter (higher = more focus on hard examples)
    """
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    bce = F.binary_cross_entropy(y_pred, y_true, reduction="none")
    p_t = y_true * y_pred + (1 - y_true) * (1 - y_pred)
    focal_weight = (1 - p_t) ** gamma
    alpha_weight = y_true * alpha + (1 - y_true) * (1 - alpha)
    return (bce * focal_weight * alpha_weight).mean()


def weighted_bce(y_pred: torch.Tensor, y_true: torch.Tensor,
                 pos_weight: float = 3.0) -> torch.Tensor:
    """Weighted binary cross-entropy."""
    y_pred = y_pred.clamp(1e-7, 1.0 - 1e-7)
    bce = F.binary_cross_entropy(y_pred, y_true, reduction="none")
    weights = y_true * pos_weight + (1 - y_true) * 1.0
    return (bce * weights).mean()


def compute_metrics(y_pred: np.ndarray, y_true: np.ndarray,
                    threshold: float = 0.5) -> dict:
    """Compute classification metrics."""
    pred_binary = (y_pred >= threshold).astype(float)
    tp = ((pred_binary == 1) & (y_true == 1)).sum()
    fp = ((pred_binary == 1) & (y_true == 0)).sum()
    fn = ((pred_binary == 0) & (y_true == 1)).sum()

    precision = tp / max(tp + fp, 1)
    recall = tp / max(tp + fn, 1)
    f1 = 2 * precision * recall / max(precision + recall, 1e-10)
    accuracy = (pred_binary == y_true).mean()

    return {
        "accuracy": float(accuracy),
        "precision": float(precision),
        "recall": float(recall),
        "f1": float(f1),
        "tp": int(tp), "fp": int(fp), "fn": int(fn),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Train beat-synchronous classifier")
    parser.add_argument("--config", default="configs/beat_sync.yaml")
    parser.add_argument("--data-dir", default=None,
                        help="Override beat feature data directory")
    parser.add_argument("--output-dir", default=None,
                        help="Output directory for checkpoints/logs")
    parser.add_argument("--epochs", type=int, default=None)
    parser.add_argument("--batch-size", type=int, default=None)
    parser.add_argument("--lr", type=float, default=None)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--phase", default=None, choices=["A", "B", "C"],
                        help="Override model phase")
    parser.add_argument("--loss", default=None, choices=["bce", "focal"],
                        help="Override loss function")
    parser.add_argument("--pos-weight", type=float, default=None,
                        help="Override positive class weight")
    args = parser.parse_args()

    cfg = load_config(args.config)
    model_cfg = cfg["model"]
    train_cfg = cfg["training"]

    # Resolve paths
    data_dir = Path(args.data_dir or "data/beat_sync")
    output_dir = Path(args.output_dir or
                      f"/mnt/storage/blinky-ml-data/outputs/beat-sync-phase{args.phase or model_cfg.get('phase', 'A')}")
    output_dir.mkdir(parents=True, exist_ok=True)

    # Training params
    epochs = args.epochs or train_cfg.get("epochs", 200)
    batch_size = args.batch_size or train_cfg.get("batch_size", 256)
    lr = args.lr or train_cfg.get("learning_rate", 0.001)
    patience = train_cfg.get("patience", 25)
    phase = args.phase or model_cfg.get("phase", "A")
    pos_weight = args.pos_weight or train_cfg.get("pos_weight", 2.3)
    loss_type = args.loss or train_cfg.get("loss", "bce")
    use_focal = loss_type == "focal"
    focal_gamma = train_cfg.get("focal_gamma", 2.0)

    # Device
    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    # Load data
    print(f"Loading data from {data_dir}...")

    # Load metadata for feature info
    metadata_path = data_dir / "metadata.json"
    if metadata_path.exists():
        with open(metadata_path) as f:
            metadata = json.load(f)
        features_per_beat = metadata.get("features_per_beat",
                                          model_cfg.get("features_per_beat", 79))
        n_beats = metadata.get("n_beats", model_cfg.get("n_beats", 4))
    else:
        features_per_beat = model_cfg.get("features_per_beat", 79)
        n_beats = model_cfg.get("n_beats", 4)

    # Diagnostic: check counts and positive ratio
    x_mmap = np.load(data_dir / "X_train.npy", mmap_mode='r')
    x_val_mmap = np.load(data_dir / "X_val.npy", mmap_mode='r')
    y_sample = np.load(data_dir / "Y_train.npy", mmap_mode='r')
    print(f"Train: {len(x_mmap)} sequences, Val: {len(x_val_mmap)} sequences")
    print(f"Shape: {n_beats} beats × {features_per_beat} features")
    db_ratio = y_sample.mean()
    print(f"Downbeat ratio: {db_ratio:.4f} ({y_sample.sum():.0f}/{len(y_sample)})")
    del x_mmap, x_val_mmap

    # Compute feature normalization stats from training data
    print("Computing feature normalization stats...")
    feat_mean, feat_std = compute_feature_stats(data_dir / "X_train.npy")
    feat_mean_t = torch.from_numpy(feat_mean)
    feat_std_t = torch.from_numpy(feat_std)
    print(f"  Feature mean range: [{feat_mean.min():.4f}, {feat_mean.max():.4f}]")
    print(f"  Feature std range:  [{feat_std.min():.4f}, {feat_std.max():.4f}]")

    # Save stats for export (firmware needs these for inference)
    np.save(output_dir / "feature_mean.npy", feat_mean)
    np.save(output_dir / "feature_std.npy", feat_std)

    train_ds = BeatSyncDataset(data_dir / "X_train.npy", data_dir / "Y_train.npy",
                                mean=feat_mean_t, std=feat_std_t)
    val_ds = BeatSyncDataset(data_dir / "X_val.npy", data_dir / "Y_val.npy",
                              mean=feat_mean_t, std=feat_std_t)

    train_loader = DataLoader(train_ds, batch_size=batch_size, shuffle=True,
                              num_workers=2, pin_memory=True)
    val_loader = DataLoader(val_ds, batch_size=batch_size, shuffle=False,
                            num_workers=2, pin_memory=True)

    # Build model
    model = build_beat_sync(
        n_beats=n_beats,
        features_per_beat=features_per_beat,
        hidden1=model_cfg.get("hidden1", 32),
        hidden2=model_cfg.get("hidden2", 16),
        dropout=model_cfg.get("dropout", 0.15),
        phase=phase,
    ).to(device)

    total_params = sum(p.numel() for p in model.parameters())
    print(f"Model: BeatSyncClassifier (phase={phase}), {total_params} params")
    print(f"  INT8 estimate: {total_params / 1024:.1f} KB")
    print(f"  Outputs: {model.output_names}")

    # Optimizer + scheduler
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    total_steps = epochs * len(train_loader)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=total_steps)

    # Loss function
    if use_focal:
        print(f"Loss: focal (gamma={focal_gamma}, alpha=0.75)")
    else:
        print(f"Loss: weighted BCE (pos_weight={pos_weight})")

    # Training loop
    print(f"\nTraining for {epochs} epochs, batch={batch_size}, lr={lr}")
    best_val_loss = float("inf")
    patience_counter = 0
    log_rows = []

    for epoch in range(epochs):
        # --- Train ---
        model.train()
        train_loss = 0.0
        train_preds = []
        train_trues = []

        for X_batch, Y_batch in train_loader:
            X_batch = X_batch.to(device, non_blocking=True)
            Y_batch = Y_batch.to(device, non_blocking=True)

            optimizer.zero_grad()
            outputs = model(X_batch)
            db_pred = outputs['downbeat']

            if use_focal:
                loss = focal_loss(db_pred, Y_batch, gamma=focal_gamma)
            else:
                loss = weighted_bce(db_pred, Y_batch, pos_weight=pos_weight)

            loss.backward()
            optimizer.step()
            scheduler.step()

            train_loss += loss.item() * X_batch.size(0)
            train_preds.append(db_pred.detach().cpu().numpy())
            train_trues.append(Y_batch.cpu().numpy())

        train_loss /= len(train_ds)
        train_metrics = compute_metrics(
            np.concatenate(train_preds), np.concatenate(train_trues))

        # --- Validate ---
        model.eval()
        val_loss = 0.0
        val_preds = []
        val_trues = []

        with torch.no_grad():
            for X_batch, Y_batch in val_loader:
                X_batch = X_batch.to(device, non_blocking=True)
                Y_batch = Y_batch.to(device, non_blocking=True)

                outputs = model(X_batch)
                db_pred = outputs['downbeat']

                if use_focal:
                    loss = focal_loss(db_pred, Y_batch, gamma=focal_gamma)
                else:
                    loss = weighted_bce(db_pred, Y_batch, pos_weight=pos_weight)

                val_loss += loss.item() * X_batch.size(0)
                val_preds.append(db_pred.cpu().numpy())
                val_trues.append(Y_batch.cpu().numpy())

        val_loss /= len(val_ds)
        val_metrics = compute_metrics(
            np.concatenate(val_preds), np.concatenate(val_trues))

        current_lr = optimizer.param_groups[0]["lr"]
        print(f"Epoch {epoch+1}/{epochs} - "
              f"loss: {train_loss:.4f} F1: {train_metrics['f1']:.3f} - "
              f"val_loss: {val_loss:.4f} val_F1: {val_metrics['f1']:.3f} - "
              f"lr: {current_lr:.2e}")

        log_rows.append({
            "epoch": epoch + 1,
            "loss": train_loss,
            "f1": train_metrics["f1"],
            "precision": train_metrics["precision"],
            "recall": train_metrics["recall"],
            "accuracy": train_metrics["accuracy"],
            "val_loss": val_loss,
            "val_f1": val_metrics["f1"],
            "val_precision": val_metrics["precision"],
            "val_recall": val_metrics["recall"],
            "val_accuracy": val_metrics["accuracy"],
            "lr": current_lr,
        })

        # Checkpointing
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            patience_counter = 0
            torch.save(model.state_dict(), output_dir / "best_model.pt")
            print(f"  Saved best model (val_loss={val_loss:.4f}, "
                  f"val_F1={val_metrics['f1']:.3f})")
        else:
            patience_counter += 1
            if patience_counter >= patience:
                print(f"  Early stopping at epoch {epoch+1} "
                      f"(no improvement for {patience} epochs)")
                break

    # Restore best weights
    model.load_state_dict(
        torch.load(output_dir / "best_model.pt", weights_only=True))

    # Final evaluation at multiple thresholds
    print("\n--- Threshold sweep on validation set ---")
    model.eval()
    all_val_preds = []
    all_val_trues = []
    with torch.no_grad():
        for X_batch, Y_batch in val_loader:
            X_batch = X_batch.to(device, non_blocking=True)
            outputs = model(X_batch)
            all_val_preds.append(outputs['downbeat'].cpu().numpy())
            all_val_trues.append(Y_batch.numpy())

    val_preds_all = np.concatenate(all_val_preds)
    val_trues_all = np.concatenate(all_val_trues)

    best_f1 = 0
    best_thresh = 0.5
    print(f"  {'Thresh':>8s}  {'F1':>6s}  {'Prec':>6s}  {'Recall':>6s}  {'Acc':>6s}")
    for thresh in [0.2, 0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.7]:
        m = compute_metrics(val_preds_all, val_trues_all, threshold=thresh)
        print(f"  {thresh:>8.2f}  {m['f1']:>6.3f}  {m['precision']:>6.3f}  "
              f"{m['recall']:>6.3f}  {m['accuracy']:>6.3f}")
        if m['f1'] > best_f1:
            best_f1 = m['f1']
            best_thresh = thresh

    print(f"\nBest threshold: {best_thresh} (F1={best_f1:.3f})")

    # Save final model + metadata
    torch.save(model.state_dict(), output_dir / "final_model.pt")
    torch.save({
        "state_dict": model.state_dict(),
        "config": cfg,
        "phase": phase,
        "n_beats": n_beats,
        "features_per_beat": features_per_beat,
        "best_threshold": best_thresh,
        "best_val_f1": best_f1,
    }, output_dir / "model_checkpoint.pt")

    # Save training log
    if log_rows:
        with open(output_dir / "training_log.csv", "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=log_rows[0].keys())
            writer.writeheader()
            writer.writerows(log_rows)

    # Save evaluation results
    best_metrics = compute_metrics(val_preds_all, val_trues_all,
                                    threshold=best_thresh)
    eval_results = {
        "phase": phase,
        "n_beats": n_beats,
        "features_per_beat": features_per_beat,
        "total_params": total_params,
        "int8_size_kb": total_params / 1024,
        "best_threshold": best_thresh,
        "best_val_f1": best_f1,
        "metrics_at_best_threshold": best_metrics,
        "train_sequences": len(train_ds),
        "val_sequences": len(val_ds),
    }
    with open(output_dir / "eval_results.json", "w") as f:
        json.dump(eval_results, f, indent=2)

    print(f"\nTraining complete. Results in {output_dir}/")
    best_epoch = min(log_rows, key=lambda r: r["val_loss"])
    print(f"Best epoch: {best_epoch['epoch']}")
    print(f"  val_loss: {best_epoch['val_loss']:.4f}")
    print(f"  val_F1:   {best_epoch['val_f1']:.3f}")


if __name__ == "__main__":
    main()
