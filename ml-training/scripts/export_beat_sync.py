#!/usr/bin/env python3
"""Export trained BeatSyncClassifier to TFLite INT8 and C header for firmware.

Much simpler than export_tflite.py — FC-only model, 2 ops (FullyConnected + Logistic).
No dilated convolutions, no BatchNorm fusion, no reshape shenanigans.

Usage:
    python scripts/export_beat_sync.py --config configs/beat_sync.yaml
    python scripts/export_beat_sync.py --model /path/to/best_model.pt --config configs/beat_sync.yaml
"""

import argparse
import hashlib
import os
import struct
import sys
from datetime import datetime
from pathlib import Path

os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")
os.environ["CUDA_VISIBLE_DEVICES"] = ""  # TF export on CPU (TF GPU is broken)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import numpy as np
import tensorflow as tf
import tf_keras as keras
from tf_keras import layers
import torch

from scripts.audio import load_config


def build_tf_beat_sync(n_beats: int, features_per_beat: int,
                       hidden1: int, hidden2: int,
                       phase: str = 'A') -> keras.Model:
    """Build TF/Keras equivalent of BeatSyncClassifier.

    For TFLite export, we flatten in the model and produce a single output
    vector (all heads concatenated) for simplicity.  Firmware unpacks by index.
    """
    input_dim = n_beats * features_per_beat
    inputs = keras.Input(shape=(input_dim,), name="beat_features")

    x = layers.Dense(hidden1, activation="relu", name="fc1")(inputs)
    x = layers.Dense(hidden2, activation="relu", name="fc2")(x)

    output_list = []

    # Downbeat (always present)
    db = layers.Dense(1, activation="sigmoid", name="downbeat")(x)
    output_list.append(db)

    # Phase B: beat confidence
    if phase >= 'B':
        bc = layers.Dense(1, activation="sigmoid", name="beat_confidence")(x)
        output_list.append(bc)

    # Phase C: tempo factor (3-class softmax) + phase offset (tanh)
    if phase >= 'C':
        tf_out = layers.Dense(3, activation="softmax", name="tempo_factor")(x)
        po = layers.Dense(1, activation="tanh", name="phase_offset_raw")(x)
        # Scale tanh output by 0.5 → [-0.5, 0.5]
        po_scaled = layers.Lambda(lambda t: t * 0.5, name="phase_offset")(po)
        output_list.append(tf_out)
        output_list.append(po_scaled)

    if len(output_list) == 1:
        output = output_list[0]
    else:
        output = layers.Concatenate(name="output")(output_list)

    return keras.Model(inputs=inputs, outputs=output, name="beat_sync")


def fold_normalization_into_fc1(pt_state_dict: dict,
                                feat_mean: np.ndarray,
                                feat_std: np.ndarray,
                                n_beats: int) -> dict:
    """Fold z-score normalization into the first FC layer weights.

    Training applies z-score normalization: x_norm = (x - mean) / std
    Model computes: y = W1 @ x_norm + b1 = W1 @ ((x - μ) / σ) + b1
    Expanding: y = (W1 / σ) @ x + (b1 - W1 @ (μ / σ))

    After folding, the model accepts raw features directly (no normalization
    needed in firmware).

    Args:
        pt_state_dict: PyTorch state dict (modified in place)
        feat_mean: per-feature mean, shape (features_per_beat,)
        feat_std: per-feature std, shape (features_per_beat,)
        n_beats: number of beats (mean/std are tiled across beats)
    Returns:
        modified state dict
    """
    # Tile mean/std to match flattened input: (n_beats * features_per_beat,)
    mean_tiled = np.tile(feat_mean, n_beats)
    std_tiled = np.tile(feat_std, n_beats)

    # fc1 weight: PT shape (hidden1, input_dim)
    W1 = pt_state_dict['shared.0.weight'].numpy()  # (hidden1, input_dim)
    b1 = pt_state_dict['shared.0.bias'].numpy()     # (hidden1,)

    # Fold: W1_new[i,j] = W1[i,j] / std_tiled[j]
    W1_new = W1 / std_tiled[np.newaxis, :]  # broadcast (hidden1, input_dim) / (1, input_dim)

    # Fold: b1_new = b1 - W1_new @ mean_tiled
    # (W1_new already has 1/σ folded in, so W1_new @ μ = W1 @ (μ/σ))
    b1_new = b1 - W1_new @ mean_tiled

    pt_state_dict['shared.0.weight'] = torch.from_numpy(W1_new.astype(np.float32))
    pt_state_dict['shared.0.bias'] = torch.from_numpy(b1_new.astype(np.float32))

    return pt_state_dict


def transfer_weights(pt_state_dict: dict, tf_model: keras.Model,
                     phase: str = 'A') -> float:
    """Transfer PyTorch weights to TF/Keras model.

    Returns max absolute difference for verification.
    """
    max_diff = 0.0
    weight_map = {
        # shared.0 = Linear(input_dim, hidden1)
        'shared.0.weight': ('fc1', 'kernel'),  # PT (out, in) → TF (in, out)
        'shared.0.bias': ('fc1', 'bias'),
        # shared.3 = Linear(hidden1, hidden2)
        'shared.3.weight': ('fc2', 'kernel'),
        'shared.3.bias': ('fc2', 'bias'),
        # downbeat head
        'downbeat_head.weight': ('downbeat', 'kernel'),
        'downbeat_head.bias': ('downbeat', 'bias'),
    }

    if phase >= 'B':
        weight_map.update({
            'beat_conf_head.weight': ('beat_confidence', 'kernel'),
            'beat_conf_head.bias': ('beat_confidence', 'bias'),
        })

    if phase >= 'C':
        weight_map.update({
            'tempo_head.weight': ('tempo_factor', 'kernel'),
            'tempo_head.bias': ('tempo_factor', 'bias'),
            'phase_head.weight': ('phase_offset_raw', 'kernel'),
            'phase_head.bias': ('phase_offset_raw', 'bias'),
        })

    for pt_name, (tf_layer_name, tf_weight_type) in weight_map.items():
        if pt_name not in pt_state_dict:
            continue

        pt_w = pt_state_dict[pt_name].numpy()
        tf_layer = tf_model.get_layer(tf_layer_name)

        if tf_weight_type == 'kernel':
            # PyTorch Linear: (out_features, in_features)
            # TF Dense: (in_features, out_features)
            tf_w = pt_w.T
            tf_layer.kernel.assign(tf_w)
            # Verify
            diff = np.abs(tf_layer.kernel.numpy() - tf_w).max()
        else:
            tf_layer.bias.assign(pt_w)
            diff = np.abs(tf_layer.bias.numpy() - pt_w).max()

        max_diff = max(max_diff, diff)

    return max_diff


def generate_c_header(tflite_bytes: bytes, output_path: Path,
                      c_array_name: str = "beat_sync_model_data",
                      model_info: dict | None = None) -> None:
    """Generate C header file with model data."""
    sha256 = hashlib.sha256(tflite_bytes).hexdigest()[:16]
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    lines = [
        "#pragma once",
        "",
        "// Beat-synchronous classifier model (auto-generated)",
        f"// Exported: {timestamp}",
        f"// SHA256:   {sha256}...",
        f"// Size:     {len(tflite_bytes)} bytes ({len(tflite_bytes)/1024:.1f} KB)",
    ]
    if model_info:
        lines.append(f"// Phase:    {model_info.get('phase', '?')}")
        lines.append(f"// Beats:    {model_info.get('n_beats', '?')}")
        lines.append(f"// Features: {model_info.get('features_per_beat', '?')}")
        lines.append(f"// Params:   {model_info.get('total_params', '?')}")
    lines.extend([
        "",
        f"alignas(16) const unsigned char {c_array_name}[] = {{",
    ])

    # Hex data (12 bytes per line)
    for i in range(0, len(tflite_bytes), 12):
        chunk = tflite_bytes[i:i+12]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_str},")

    lines.extend([
        "};",
        f"const unsigned int {c_array_name}_len = {len(tflite_bytes)};",
        "",
    ])

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(
        description="Export BeatSyncClassifier to TFLite INT8")
    parser.add_argument("--config", default="configs/beat_sync.yaml")
    parser.add_argument("--model", default=None,
                        help="Path to best_model.pt (default: auto from output-dir)")
    parser.add_argument("--output-dir", default=None,
                        help="Directory for TFLite output")
    parser.add_argument("--data-dir", default="data/beat_sync",
                        help="Beat feature data dir (for calibration)")
    parser.add_argument("--phase", default=None)
    args = parser.parse_args()

    cfg = load_config(args.config)
    model_cfg = cfg["model"]
    phase = args.phase or model_cfg.get("phase", "A")

    n_beats = model_cfg.get("n_beats", 4)
    features_per_beat = model_cfg.get("features_per_beat", 79)
    hidden1 = model_cfg.get("hidden1", 32)
    hidden2 = model_cfg.get("hidden2", 16)
    input_dim = n_beats * features_per_beat

    output_dir = Path(args.output_dir or
                      f"/mnt/storage/blinky-ml-data/outputs/beat-sync-phase{phase}")
    model_path = Path(args.model or (output_dir / "best_model.pt"))
    data_dir = Path(args.data_dir)

    # Load PyTorch model
    print(f"Loading PyTorch model from {model_path}...")
    from models.beat_sync import build_beat_sync
    pt_state = torch.load(model_path, weights_only=True, map_location='cpu')

    total_params = sum(v.numel() for v in pt_state.values())
    print(f"  Phase={phase}, {total_params} params, input_dim={input_dim}")

    # Load feature normalization stats and fold into fc1 weights.
    # Training applies z-score normalization: x_norm = (x - mean) / std.
    # By absorbing this into the first layer weights, the model accepts
    # raw features directly — no normalization needed in firmware.
    feat_mean_path = output_dir / "feature_mean.npy"
    feat_std_path = output_dir / "feature_std.npy"
    if feat_mean_path.exists() and feat_std_path.exists():
        feat_mean = np.load(feat_mean_path)
        feat_std = np.load(feat_std_path)
        print(f"Folding z-score normalization into fc1 weights...")
        print(f"  mean range: [{feat_mean.min():.4f}, {feat_mean.max():.4f}]")
        print(f"  std range:  [{feat_std.min():.4f}, {feat_std.max():.4f}]")
        fold_normalization_into_fc1(pt_state, feat_mean, feat_std, n_beats)
    else:
        print("WARNING: feature_mean.npy / feature_std.npy not found!")
        print("  Model will expect z-score normalized input (firmware sends raw).")
        print(f"  Looked in: {output_dir}")

    # Build PT model with folded weights (for verification)
    pt_model = build_beat_sync(
        n_beats=n_beats, features_per_beat=features_per_beat,
        hidden1=hidden1, hidden2=hidden2, phase=phase, dropout=0.0)
    pt_model.load_state_dict(pt_state, strict=False)
    pt_model.eval()

    # Build TF model
    print(f"Building TF model...")
    tf_model = build_tf_beat_sync(n_beats, features_per_beat,
                                   hidden1, hidden2, phase=phase)
    tf_model.summary()

    # Transfer weights (already has normalization folded in)
    print("Transferring weights...")
    max_diff = transfer_weights(pt_state, tf_model, phase=phase)
    print(f"  PT vs TF max weight diff: {max_diff:.6f} {'OK' if max_diff < 0.001 else 'WARNING'}")

    # Verify forward pass with raw features (both models should accept raw input now)
    print("Verifying forward pass (raw features, normalization folded in)...")
    test_input = np.random.rand(1, input_dim).astype(np.float32)  # raw-scale [0, 1]
    pt_input = torch.from_numpy(test_input.reshape(1, n_beats, features_per_beat))
    with torch.no_grad():
        pt_out = pt_model(pt_input)
    tf_out = tf_model.predict(test_input, verbose=0)

    # Compare downbeat output (first element)
    pt_db = pt_out['downbeat'].numpy().item()
    tf_db = tf_out[0, 0]
    fwd_diff = abs(pt_db - tf_db)
    print(f"  Forward pass diff (downbeat): {fwd_diff:.6f} "
          f"{'OK' if fwd_diff < 0.01 else 'WARNING'}")

    # Prepare representative dataset for quantization calibration
    print("Preparing calibration data...")
    x_train_path = data_dir / "X_train.npy"
    if x_train_path.exists():
        x_cal = np.load(x_train_path, mmap_mode='r')
        n_cal = min(500, len(x_cal))
        cal_data = x_cal[:n_cal].reshape(n_cal, input_dim).astype(np.float32)
    else:
        print("  WARNING: No training data for calibration, using random data")
        cal_data = np.random.randn(200, input_dim).astype(np.float32) * 0.5 + 0.5

    def representative_dataset():
        for i in range(len(cal_data)):
            yield [cal_data[i:i+1]]

    # Export to TFLite INT8
    print("Exporting INT8 TFLite model...")
    converter = tf.lite.TFLiteConverter.from_keras_model(tf_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()

    # Save TFLite binary
    tflite_path = output_dir / "beat_sync_int8.tflite"
    tflite_path.parent.mkdir(parents=True, exist_ok=True)
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)
    print(f"TFLite model: {len(tflite_model)/1024:.1f} KB ({tflite_path})")

    # Validate TFLite model
    print("\nModel validation:")
    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    print(f"  Input:  {inp['shape']} dtype={inp['dtype']}")
    print(f"  Output: {out['shape']} dtype={out['dtype']}")

    # Estimate tensor arena
    # For FC-only models, arena ≈ max(input_size, hidden1, hidden2, output_size) × 2
    # Plus quantization buffers
    max_layer_size = max(input_dim, hidden1, hidden2, out['shape'][-1])
    est_arena = max_layer_size * 4 * 2  # float32 × 2 buffers
    est_arena = max(est_arena, 4096)  # minimum 4 KB
    # Round up to next KB
    est_arena_kb = (est_arena + 1023) // 1024
    print(f"  Estimated tensor arena: ~{est_arena_kb} KB")
    print(f"  Recommended TENSOR_ARENA_SIZE: {est_arena_kb * 1024:,}")

    # Generate C header
    export_cfg = cfg.get("export", {})
    header_path = Path(export_cfg.get("output_header",
                                       "../blinky-things/audio/beat_sync_model_data.h"))
    c_array_name = export_cfg.get("c_array_name", "beat_sync_model_data")

    model_info = {
        "phase": phase,
        "n_beats": n_beats,
        "features_per_beat": features_per_beat,
        "total_params": total_params,
    }
    generate_c_header(tflite_model, header_path, c_array_name, model_info)
    print(f"C header: {header_path}")

    # Also copy TFLite to firmware dir for reference
    fw_tflite = Path("../blinky-things/audio/beat_sync_model.tflite")
    fw_tflite.parent.mkdir(parents=True, exist_ok=True)
    with open(fw_tflite, "wb") as f:
        f.write(tflite_model)
    print(f"TFLite copy: {fw_tflite}")

    print("\nDone!")


if __name__ == "__main__":
    main()
