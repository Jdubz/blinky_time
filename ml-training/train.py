#!/usr/bin/env python3
"""Train the beat activation CNN.

Usage:
    python train.py --config configs/default.yaml
    python train.py --config configs/default.yaml --epochs 50 --batch-size 32
"""

import argparse
import os
import sys
from pathlib import Path

os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")

import numpy as np
import tf_keras as keras
import yaml

from models.beat_cnn import build_beat_cnn


def main():
    parser = argparse.ArgumentParser(description="Train beat activation CNN")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--data-dir", default=None, help="Override processed data dir")
    parser.add_argument("--output-dir", default="outputs", help="Output directory for checkpoints/logs")
    parser.add_argument("--epochs", type=int, default=None)
    parser.add_argument("--batch-size", type=int, default=None)
    parser.add_argument("--lr", type=float, default=None)
    parser.add_argument("--qat", action="store_true", help="Enable quantization-aware training")
    args = parser.parse_args()

    with open(args.config) as f:
        cfg = yaml.safe_load(f)

    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    epochs = args.epochs or cfg["training"]["epochs"]
    batch_size = args.batch_size or cfg["training"]["batch_size"]
    lr = args.lr or cfg["training"]["learning_rate"]
    pos_weight = cfg["training"]["pos_weight"]
    use_downbeat = cfg["model"].get("downbeat", False)

    # Load data
    print(f"Loading data from {data_dir}...")
    X_train = np.load(data_dir / "X_train.npy")
    Y_train = np.load(data_dir / "Y_train.npy")
    X_val = np.load(data_dir / "X_val.npy")
    Y_val = np.load(data_dir / "Y_val.npy")

    print(f"Train: {X_train.shape}, Val: {X_val.shape}")
    print(f"Positive ratio - Train: {Y_train.mean():.4f}, Val: {Y_val.mean():.4f}")

    # Handle multi-output (beat + downbeat)
    if use_downbeat:
        db_train_path = data_dir / "Y_db_train.npy"
        db_val_path = data_dir / "Y_db_val.npy"
        if db_train_path.exists():
            Y_db_train = np.load(db_train_path)
            Y_db_val = np.load(db_val_path)
            # Stack: (batch, time, 2) — channel 0 = beat, channel 1 = downbeat
            Y_train = np.stack([Y_train, Y_db_train], axis=-1)
            Y_val = np.stack([Y_val, Y_db_val], axis=-1)
            print(f"Downbeat ratio - Train: {Y_db_train.mean():.4f}")
        else:
            print("WARNING: downbeat=true but Y_db_train.npy not found. Training beat-only.")
            use_downbeat = False
            Y_train = Y_train[..., np.newaxis]
            Y_val = Y_val[..., np.newaxis]
    else:
        # Add channel dim to targets: (batch, time) -> (batch, time, 1)
        Y_train = Y_train[..., np.newaxis]
        Y_val = Y_val[..., np.newaxis]

    # Build model
    model = build_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=cfg["model"]["dilations"],
        dropout=cfg["model"]["dropout"],
        chunk_frames=cfg["training"]["chunk_frames"],
        downbeat=use_downbeat,
    )

    # Optional: quantization-aware training
    if args.qat or cfg["quantization"]["method"] == "qat":
        try:
            import tensorflow_model_optimization as tfmot
            model = tfmot.quantization.keras.quantize_model(model)
            print("QAT enabled")
        except Exception as e:
            print(f"Warning: QAT failed ({e}), training without QAT")

    # Weighted binary cross-entropy (per-channel)
    def weighted_bce(y_true, y_pred):
        bce = keras.backend.binary_crossentropy(y_true, y_pred)
        weights = y_true * pos_weight + (1 - y_true) * 1.0
        return keras.backend.mean(bce * weights)

    # Learning rate schedule
    if cfg["training"]["lr_schedule"] == "cosine":
        lr_schedule = keras.optimizers.schedules.CosineDecay(
            initial_learning_rate=lr,
            decay_steps=epochs * (len(X_train) // batch_size),
        )
    else:
        lr_schedule = lr

    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=lr_schedule),
        loss=weighted_bce,
        metrics=["binary_accuracy"],
    )
    model.summary()

    # Callbacks
    callbacks = [
        keras.callbacks.ModelCheckpoint(
            str(output_dir / "best_model.keras"),
            monitor="val_loss", save_best_only=True, verbose=1,
        ),
        keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=15, restore_best_weights=True, verbose=1,
        ),
        keras.callbacks.CSVLogger(str(output_dir / "training_log.csv")),
        keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", factor=0.5, patience=7, min_lr=1e-6, verbose=1,
        ),
    ]

    # Train
    print(f"\nTraining for {epochs} epochs, batch_size={batch_size}, lr={lr}")
    print(f"Output channels: {'beat + downbeat' if use_downbeat else 'beat only'}")
    history = model.fit(
        X_train, Y_train,
        validation_data=(X_val, Y_val),
        epochs=epochs,
        batch_size=batch_size,
        callbacks=callbacks,
    )

    # Save final model
    model.save(str(output_dir / "final_model.keras"))
    print(f"\nTraining complete. Models saved to {output_dir}/")

    # Print best metrics
    best_epoch = np.argmin(history.history["val_loss"])
    print(f"Best epoch: {best_epoch + 1}")
    print(f"  val_loss: {history.history['val_loss'][best_epoch]:.4f}")
    print(f"  val_binary_accuracy: {history.history['val_binary_accuracy'][best_epoch]:.4f}")


if __name__ == "__main__":
    main()
