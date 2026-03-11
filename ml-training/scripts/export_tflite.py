#!/usr/bin/env python3
"""Export trained PyTorch model to TFLite INT8 and C header for firmware.

Loads PyTorch weights, rebuilds equivalent TF/Keras model, and converts
to TFLite INT8. TF is used only for export (CPU-only is fine).

Usage:
    python scripts/export_tflite.py --config configs/default.yaml
    python scripts/export_tflite.py --config configs/default.yaml --model outputs/best_model.pt
    python scripts/export_tflite.py --inference-frames 32  # Smaller context = less device RAM
"""

import argparse
import os
import sys
from pathlib import Path

os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")
os.environ["CUDA_VISIBLE_DEVICES"] = ""  # TF export runs on CPU (TF GPU is broken)

# Ensure ml-training root is on path (for "from models.beat_cnn import ...")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import numpy as np
import tensorflow as tf
import tf_keras as keras
from tf_keras import layers
import torch

from scripts.audio import load_config


def build_tf_beat_cnn(n_mels: int, channels: int, kernel_size: int,
                      dilations: list[int], chunk_frames: int,
                      downbeat: bool, fuse_bn: bool = False,
                      model_type: str = "causal_cnn",
                      residual: bool = False) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch BeatCNN or DSTCNBeatCNN.

    If fuse_bn=True, BatchNorm is omitted (weights will be fused into Conv
    by transfer_pytorch_weights). This eliminates expensive unfused ADD/MUL
    ops in TFLite for dilated convolutions.
    """
    if model_type == "ds_tcn":
        return _build_tf_ds_tcn(n_mels, channels, kernel_size, dilations,
                                chunk_frames, downbeat, fuse_bn, residual)

    out_channels = 2 if downbeat else 1
    inputs = keras.Input(shape=(chunk_frames, n_mels), name="mel_input")
    x = inputs

    for i, dilation in enumerate(dilations):
        pad_size = (kernel_size - 1) * dilation
        x = layers.ZeroPadding1D(padding=(pad_size, 0))(x)
        if fuse_bn:
            # BN is fused into conv weights; ReLU as conv activation
            x = layers.Conv1D(channels, kernel_size, dilation_rate=dilation,
                              padding="valid", use_bias=True, activation="relu",
                              name=f"conv{i+1}_d{dilation}")(x)
        else:
            x = layers.Conv1D(channels, kernel_size, dilation_rate=dilation,
                              padding="valid", use_bias=True,
                              name=f"conv{i+1}_d{dilation}")(x)
            x = layers.BatchNormalization(name=f"bn{i+1}")(x)
            x = layers.ReLU(name=f"relu{i+1}")(x)

    x = layers.Conv1D(out_channels, 1, padding="valid", activation="sigmoid",
                      name="output_conv")(x)

    return keras.Model(inputs=inputs, outputs=x, name="beat_cnn")


def _build_tf_ds_tcn(n_mels: int, channels: int, kernel_size: int,
                     dilations: list[int], chunk_frames: int,
                     downbeat: bool, fuse_bn: bool, residual: bool) -> keras.Model:
    """Build TF/Keras equivalent of DSTCNBeatCNN.

    Depthwise separable blocks: ZeroPad → DepthwiseConv1D → [BN] → ReLU →
                                Conv1D(1×1) → [BN] → ReLU → [+ residual]
    """
    out_channels = 2 if downbeat else 1
    inputs = keras.Input(shape=(chunk_frames, n_mels), name="mel_input")

    # Input projection (standard conv, n_mels → channels)
    pad_size = (kernel_size - 1) * dilations[0]
    x = layers.ZeroPadding1D(padding=(pad_size, 0))(inputs)
    if fuse_bn:
        x = layers.Conv1D(channels, kernel_size, dilation_rate=dilations[0],
                          padding="valid", use_bias=True, activation="relu",
                          name="input_conv")(x)
    else:
        x = layers.Conv1D(channels, kernel_size, dilation_rate=dilations[0],
                          padding="valid", use_bias=True,
                          name="input_conv")(x)
        x = layers.BatchNormalization(name="input_bn")(x)
        x = layers.ReLU(name="input_relu")(x)

    # DS-TCN blocks for remaining dilations
    for i, dilation in enumerate(dilations[1:]):
        block_idx = i + 1  # 1-indexed (input conv is block 0)
        skip = x  # Save for residual

        pad_size = (kernel_size - 1) * dilation
        x = layers.ZeroPadding1D(padding=(pad_size, 0))(x)

        # Depthwise conv (each channel independently)
        # When BN is fused, depthwise needs use_bias=True (synthetic bias from BN params)
        if fuse_bn:
            x = layers.DepthwiseConv1D(kernel_size, dilation_rate=dilation,
                                       padding="valid", use_bias=True, activation="relu",
                                       name=f"dw_conv{block_idx}_d{dilation}")(x)
        else:
            x = layers.DepthwiseConv1D(kernel_size, dilation_rate=dilation,
                                       padding="valid", use_bias=False,
                                       name=f"dw_conv{block_idx}_d{dilation}")(x)
            x = layers.BatchNormalization(name=f"dw_bn{block_idx}")(x)
            x = layers.ReLU(name=f"dw_relu{block_idx}")(x)

        # Pointwise conv (1×1, mixes channels)
        if fuse_bn:
            x = layers.Conv1D(channels, 1, padding="valid", use_bias=True,
                              activation="relu",
                              name=f"pw_conv{block_idx}")(x)
        else:
            x = layers.Conv1D(channels, 1, padding="valid", use_bias=True,
                              name=f"pw_conv{block_idx}")(x)
            x = layers.BatchNormalization(name=f"pw_bn{block_idx}")(x)
            x = layers.ReLU(name=f"pw_relu{block_idx}")(x)

        # Residual connection
        if residual:
            x = layers.Add(name=f"residual{block_idx}")([x, skip])

    x = layers.Conv1D(out_channels, 1, padding="valid", activation="sigmoid",
                      name="output_conv")(x)

    return keras.Model(inputs=inputs, outputs=x, name="ds_tcn")


def transfer_pytorch_weights(tf_model: keras.Model, pt_state_dict: dict,
                             dilations: list[int], dropout: float = 0.1,
                             fuse_bn: bool = False,
                             model_type: str = "causal_cnn"):
    """Copy weights from PyTorch state_dict to equivalent TF/Keras model.

    Handles the Conv1D weight transposition (PyTorch: out,in,k → TF: k,in,out)
    and BatchNorm parameter mapping.

    If fuse_bn=True, folds BatchNorm parameters into Conv weights:
        fused_w = w * gamma / sqrt(var + eps)
        fused_b = gamma * (b - mean) / sqrt(var + eps) + beta
    This eliminates unfused ADD/MUL ops in TFLite, saving ~67% inference time
    on Cortex-M4 where element-wise ops lack CMSIS-NN SIMD optimization.
    """
    if model_type == "ds_tcn":
        _transfer_ds_tcn_weights(tf_model, pt_state_dict, dilations, fuse_bn)
        return

    # PyTorch backbone block: Pad, Conv1d, BatchNorm1d, ReLU[, Dropout]
    # Stride depends on whether Dropout is present
    stride = 5 if dropout > 0 else 4
    bn_eps = 1e-5  # PyTorch default

    for i, dilation in enumerate(dilations):
        # Conv weights: PyTorch (out_ch, in_ch, kernel) → TF (kernel, in_ch, out_ch)
        conv_name = f"conv{i+1}_d{dilation}"
        conv_key = f"backbone.{i*stride+1}.weight"
        assert conv_key in pt_state_dict, \
            f"Expected Conv1d at {conv_key}; check dropout={dropout} matches trained model"
        pt_w = pt_state_dict[conv_key].numpy()  # (out, in, k)
        pt_b = pt_state_dict[f"backbone.{i*stride+1}.bias"].numpy()

        if fuse_bn:
            # Fuse BatchNorm into Conv weights
            gamma = pt_state_dict[f"backbone.{i*stride+2}.weight"].numpy()
            beta = pt_state_dict[f"backbone.{i*stride+2}.bias"].numpy()
            mean = pt_state_dict[f"backbone.{i*stride+2}.running_mean"].numpy()
            var = pt_state_dict[f"backbone.{i*stride+2}.running_var"].numpy()

            std = np.sqrt(var + bn_eps)
            scale = gamma / std  # per-channel scale factor

            # Fuse: w_fused[out, in, k] = w[out, in, k] * scale[out]
            pt_w = pt_w * scale[:, np.newaxis, np.newaxis]
            # Fuse: b_fused[out] = scale[out] * (b[out] - mean[out]) + beta[out]
            pt_b = scale * (pt_b - mean) + beta
        else:
            # Transfer BatchNorm weights separately
            bn_name = f"bn{i+1}"
            gamma = pt_state_dict[f"backbone.{i*stride+2}.weight"].numpy()
            beta = pt_state_dict[f"backbone.{i*stride+2}.bias"].numpy()
            mean = pt_state_dict[f"backbone.{i*stride+2}.running_mean"].numpy()
            var = pt_state_dict[f"backbone.{i*stride+2}.running_var"].numpy()
            tf_model.get_layer(bn_name).set_weights([gamma, beta, mean, var])

        tf_w = np.transpose(pt_w, (2, 1, 0))  # (k, in, out)
        tf_model.get_layer(conv_name).set_weights([tf_w, pt_b])

    # Output conv
    pt_w = pt_state_dict["output_conv.weight"].numpy()
    pt_b = pt_state_dict["output_conv.bias"].numpy()
    tf_w = np.transpose(pt_w, (2, 1, 0))
    tf_model.get_layer("output_conv").set_weights([tf_w, pt_b])


def _fuse_bn_into_conv(pt_w, pt_b, gamma, beta, mean, var, bn_eps=1e-5):
    """Fuse BatchNorm parameters into Conv weights."""
    std = np.sqrt(var + bn_eps)
    scale = gamma / std
    fused_w = pt_w * scale[:, np.newaxis, np.newaxis]
    fused_b = scale * (pt_b - mean) + beta
    return fused_w, fused_b


def _fuse_bn_into_depthwise(pt_w, gamma, beta, mean, var, bn_eps=1e-5):
    """Fuse BatchNorm parameters into depthwise conv weights (no bias).

    Depthwise conv has no bias in our model, so we create one from BN params.
    Weight shape: (ch, 1, k) for PyTorch depthwise.
    """
    std = np.sqrt(var + bn_eps)
    scale = gamma / std
    # Depthwise weight: (ch, 1, k) — scale per output channel
    fused_w = pt_w * scale[:, np.newaxis, np.newaxis]
    # Bias from BN: -mean * scale + beta
    fused_b = -mean * scale + beta
    return fused_w, fused_b


def _transfer_ds_tcn_weights(tf_model: keras.Model, pt_state_dict: dict,
                              dilations: list[int], fuse_bn: bool):
    """Transfer DSTCNBeatCNN weights to TF/Keras equivalent.

    DSTCNBeatCNN PyTorch state_dict key layout:
      input_conv.weight, input_conv.bias
      input_bn.weight, input_bn.bias, input_bn.running_mean, input_bn.running_var
      blocks.0.dw_conv.weight          (no bias)
      blocks.0.dw_bn.weight/bias/running_mean/running_var
      blocks.0.pw_conv.weight, blocks.0.pw_conv.bias
      blocks.0.pw_bn.weight/bias/running_mean/running_var
      ...
      output_conv.weight, output_conv.bias
    """
    bn_eps = 1e-5

    # --- Input conv ---
    pt_w = pt_state_dict["input_conv.weight"].numpy()  # (out, in, k)
    pt_b = pt_state_dict["input_conv.bias"].numpy()

    if fuse_bn:
        gamma = pt_state_dict["input_bn.weight"].numpy()
        beta = pt_state_dict["input_bn.bias"].numpy()
        mean = pt_state_dict["input_bn.running_mean"].numpy()
        var = pt_state_dict["input_bn.running_var"].numpy()
        pt_w, pt_b = _fuse_bn_into_conv(pt_w, pt_b, gamma, beta, mean, var, bn_eps)
    else:
        gamma = pt_state_dict["input_bn.weight"].numpy()
        beta = pt_state_dict["input_bn.bias"].numpy()
        mean = pt_state_dict["input_bn.running_mean"].numpy()
        var = pt_state_dict["input_bn.running_var"].numpy()
        tf_model.get_layer("input_bn").set_weights([gamma, beta, mean, var])

    tf_w = np.transpose(pt_w, (2, 1, 0))  # (k, in, out)
    tf_model.get_layer("input_conv").set_weights([tf_w, pt_b])

    # --- DS-TCN blocks ---
    for i, dilation in enumerate(dilations[1:]):
        block_idx = i + 1

        # Depthwise conv: PyTorch shape (ch, 1, k) → TF shape (k, 1, ch)
        dw_w = pt_state_dict[f"blocks.{i}.dw_conv.weight"].numpy()  # (ch, 1, k)

        if fuse_bn:
            gamma = pt_state_dict[f"blocks.{i}.dw_bn.weight"].numpy()
            beta = pt_state_dict[f"blocks.{i}.dw_bn.bias"].numpy()
            mean = pt_state_dict[f"blocks.{i}.dw_bn.running_mean"].numpy()
            var = pt_state_dict[f"blocks.{i}.dw_bn.running_var"].numpy()
            dw_w, dw_b = _fuse_bn_into_depthwise(dw_w, gamma, beta, mean, var, bn_eps)
            # TF DepthwiseConv1D weight: (k, ch, depth_mul=1)
            # PyTorch depthwise: (ch, 1, k) → permute(2, 0, 1) → (k, ch, 1)
            tf_dw_w = np.transpose(dw_w, (2, 0, 1))  # (k, ch, 1)
            tf_model.get_layer(f"dw_conv{block_idx}_d{dilation}").set_weights([tf_dw_w, dw_b])
        else:
            gamma = pt_state_dict[f"blocks.{i}.dw_bn.weight"].numpy()
            beta = pt_state_dict[f"blocks.{i}.dw_bn.bias"].numpy()
            mean = pt_state_dict[f"blocks.{i}.dw_bn.running_mean"].numpy()
            var = pt_state_dict[f"blocks.{i}.dw_bn.running_var"].numpy()
            tf_model.get_layer(f"dw_bn{block_idx}").set_weights([gamma, beta, mean, var])
            # (ch, 1, k) → (k, ch, 1)
            tf_dw_w = np.transpose(dw_w, (2, 0, 1))
            tf_model.get_layer(f"dw_conv{block_idx}_d{dilation}").set_weights([tf_dw_w])

        # Pointwise conv: PyTorch (out, in, 1) → TF (1, in, out)
        pw_w = pt_state_dict[f"blocks.{i}.pw_conv.weight"].numpy()
        pw_b = pt_state_dict[f"blocks.{i}.pw_conv.bias"].numpy()

        if fuse_bn:
            gamma = pt_state_dict[f"blocks.{i}.pw_bn.weight"].numpy()
            beta = pt_state_dict[f"blocks.{i}.pw_bn.bias"].numpy()
            mean = pt_state_dict[f"blocks.{i}.pw_bn.running_mean"].numpy()
            var = pt_state_dict[f"blocks.{i}.pw_bn.running_var"].numpy()
            pw_w, pw_b = _fuse_bn_into_conv(pw_w, pw_b, gamma, beta, mean, var, bn_eps)
        else:
            gamma = pt_state_dict[f"blocks.{i}.pw_bn.weight"].numpy()
            beta = pt_state_dict[f"blocks.{i}.pw_bn.bias"].numpy()
            mean = pt_state_dict[f"blocks.{i}.pw_bn.running_mean"].numpy()
            var = pt_state_dict[f"blocks.{i}.pw_bn.running_var"].numpy()
            tf_model.get_layer(f"pw_bn{block_idx}").set_weights([gamma, beta, mean, var])

        tf_pw_w = np.transpose(pw_w, (2, 1, 0))  # (1, in, out)
        tf_model.get_layer(f"pw_conv{block_idx}").set_weights([tf_pw_w, pw_b])

    # --- Output conv ---
    pt_w = pt_state_dict["output_conv.weight"].numpy()
    pt_b = pt_state_dict["output_conv.bias"].numpy()
    tf_w = np.transpose(pt_w, (2, 1, 0))
    tf_model.get_layer("output_conv").set_weights([tf_w, pt_b])


def representative_dataset_gen(data_path: Path, inference_frames: int = None,
                                n_samples: int = 200):
    """Generator for calibration data (required for full INT8 quantization)."""
    X = np.load(data_path / "X_train.npy", mmap_mode='r')
    indices = np.random.choice(len(X), size=min(n_samples, len(X)), replace=False)
    for i in indices:
        sample = X[i:i+1].astype(np.float32)
        if inference_frames is not None and sample.shape[1] != inference_frames:
            sample = sample[:, :inference_frames, :]
        yield [sample]


def export_tflite(model: keras.Model, data_path: Path, output_path: str,
                  quantize_int8: bool = True, inference_frames: int = None) -> bytes:
    """Convert Keras model to TFLite, optionally with INT8 quantization."""
    converter = tf.lite.TFLiteConverter.from_keras_model(model)

    if quantize_int8:
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.representative_dataset = lambda: representative_dataset_gen(
            data_path, inference_frames=inference_frames)
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        converter.inference_input_type = tf.int8
        converter.inference_output_type = tf.int8

    tflite_model = converter.convert()

    with open(output_path, "wb") as f:
        f.write(tflite_model)

    return tflite_model


def tflite_to_c_header(tflite_bytes: bytes, c_array_name: str, output_path: str):
    """Convert TFLite model bytes to a C header file with integrity metadata."""
    import hashlib
    from datetime import datetime, timezone

    model_hash = hashlib.sha256(tflite_bytes).hexdigest()[:16]
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    hex_lines = []
    for i in range(0, len(tflite_bytes), 12):
        chunk = tflite_bytes[i:i+12]
        hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
        hex_lines.append(f"  {hex_str},")

    header = f"""// Auto-generated by export_tflite.py — do not edit
// Model size: {len(tflite_bytes)} bytes ({len(tflite_bytes)/1024:.1f} KB)
// SHA256 prefix: {model_hash}
// Exported: {timestamp}

#ifndef BEAT_MODEL_DATA_H
#define BEAT_MODEL_DATA_H

#define BEAT_MODEL_HASH "{model_hash}"
#define BEAT_MODEL_SIZE {len(tflite_bytes)}

alignas(8) static const unsigned char {c_array_name}[] = {{
{chr(10).join(hex_lines)}
}};

static const unsigned int {c_array_name}_len = {len(tflite_bytes)};

#endif // BEAT_MODEL_DATA_H
"""
    with open(output_path, "w") as f:
        f.write(header)


def estimate_tensor_arena(tflite_path: str) -> tuple[int, int]:
    """Estimate TFLite Micro tensor arena size from a TFLite model file.

    Parses the TFLite flatbuffer to identify mutable tensors (activations that
    need arena RAM, as opposed to constant weights stored in flash), then
    simulates TFLite Micro's GreedyMemoryPlanner to compute peak memory usage.

    The arena must also hold per-tensor metadata, per-op node structs, and
    scratch buffers. These are estimated via a calibration factor derived from
    the known-good v6 model (5L ch32, 128-frame: 16 KB arena for 8 KB planner).

    Returns (estimated_bytes, recommended_bytes) where recommended includes
    a 10% safety margin rounded up to 1 KB.
    """
    from tensorflow.lite.python import schema_py_generated as schema_fb

    with open(tflite_path, "rb") as f:
        buf = bytearray(f.read())

    model = schema_fb.Model.GetRootAs(buf, 0)
    sg = model.Subgraphs(0)

    ALIGNMENT = 16  # TFLite Micro tensor buffer alignment

    # --- Identify mutable tensors ---
    # Constant tensors (weights, biases) have non-empty buffer data in the
    # flatbuffer and are read directly from flash. Mutable tensors (activations,
    # inputs, outputs) have empty buffers and must be allocated in the arena.
    # TensorType enum → element size in bytes (from TFLite schema)
    # 0=FLOAT32, 1=FLOAT16, 2=INT32, 3=UINT8, 4=INT64, 6=BOOL,
    # 7=INT16, 8=COMPLEX64, 9=INT8, 12=FLOAT64, 15=UINT32
    dtype_sizes = {0: 4, 1: 2, 2: 4, 3: 1, 4: 8, 6: 1, 7: 2, 8: 8, 9: 1, 12: 8, 15: 4}

    mutable = []
    for i in range(sg.TensorsLength()):
        t = sg.Tensors(i)
        buf_idx = t.Buffer()
        b = model.Buffers(buf_idx)
        if buf_idx != 0 and b.DataLength() > 0:
            continue  # Constant tensor — stored in flash, not arena

        shape = [t.Shape(j) for j in range(t.ShapeLength())]
        elem_size = dtype_sizes.get(t.Type(), 1)
        size_bytes = int(np.prod(shape)) * elem_size if shape else 0
        mutable.append({"index": i, "size": size_bytes})

    # --- Compute tensor lifetimes from operator execution order ---
    n_tensors = sg.TensorsLength()
    first_use = [float("inf")] * n_tensors
    last_use = [-1] * n_tensors

    for j in range(sg.InputsLength()):
        first_use[sg.Inputs(j)] = -1
    for j in range(sg.OutputsLength()):
        last_use[sg.Outputs(j)] = sg.OperatorsLength()

    for op_i in range(sg.OperatorsLength()):
        op = sg.Operators(op_i)
        for j in range(op.InputsLength()):
            t_idx = op.Inputs(j)
            if t_idx >= 0:
                first_use[t_idx] = min(first_use[t_idx], op_i)
                last_use[t_idx] = max(last_use[t_idx], op_i)
        for j in range(op.OutputsLength()):
            t_idx = op.Outputs(j)
            if t_idx >= 0:
                first_use[t_idx] = min(first_use[t_idx], op_i)
                last_use[t_idx] = max(last_use[t_idx], op_i)

    # --- Simulate GreedyMemoryPlanner (first-fit-decreasing with alignment) ---
    allocations = []
    for m in mutable:
        idx = m["index"]
        if first_use[idx] == float("inf"):
            continue  # Unused tensor
        aligned_size = ((m["size"] + ALIGNMENT - 1) // ALIGNMENT) * ALIGNMENT
        allocations.append({
            "size": aligned_size, "alloc": first_use[idx],
            "free": last_use[idx] + 1, "index": idx,
        })

    allocations.sort(key=lambda x: -x["size"])  # Largest first

    placements = []
    arena_planner = 0
    for alloc in allocations:
        size, a_start, a_end = alloc["size"], alloc["alloc"], alloc["free"]
        # Find conflicting placements (overlapping lifetimes)
        conflicts = sorted(
            [p for p in placements if not (a_end <= p["alloc"] or a_start >= p["free"])],
            key=lambda p: p["offset"],
        )
        candidate = 0
        for c in conflicts:
            if candidate + size <= c["offset"]:
                break
            candidate = ((c["offset"] + c["size"] + ALIGNMENT - 1) // ALIGNMENT) * ALIGNMENT
        placements.append({"offset": candidate, "size": size, "alloc": a_start, "free": a_end})
        arena_planner = max(arena_planner, candidate + size)

    # --- Apply overhead factor ---
    # TFLite Micro's arena also holds: per-tensor TfLiteTensor structs (~52 bytes
    # each, for ALL tensors including constants), per-op TfLiteNode structs
    # (~24 bytes each), op scratch buffers, and runtime bookkeeping.
    # Calibrated against v6 (5L ch32, 128-frame): planner=8192, actual=16384 → 2.0x.
    OVERHEAD_FACTOR = 2.0
    estimated = int(arena_planner * OVERHEAD_FACTOR)

    # Recommended: +10% margin, rounded up to nearest 1024 bytes
    recommended = ((int(estimated * 1.1) + 1023) // 1024) * 1024

    return estimated, recommended


def main():
    parser = argparse.ArgumentParser(description="Export model to TFLite INT8 C header")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--model", default="outputs/best_model.pt", help="PyTorch model path")
    parser.add_argument("--data-dir", default=None, help="Processed data dir (for calibration)")
    parser.add_argument("--output-dir", default="outputs", help="Output directory")
    parser.add_argument("--no-quantize", action="store_true", help="Skip INT8 quantization")
    parser.add_argument("--no-fuse-bn", action="store_true",
                        help="Don't fuse BatchNorm into Conv (keep separate ADD/MUL ops)")
    parser.add_argument("--inference-frames", type=int, default=None,
                        help="Context frames for device inference (default: use training chunk size). "
                             "Smaller = less device RAM. Must be >= receptive field (15 frames).")
    args = parser.parse_args()

    cfg = load_config(args.config)

    data_dir = Path(args.data_dir or cfg["data"]["processed_dir"])
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not Path(args.model).exists():
        print(f"Error: Model not found at {args.model}", file=sys.stderr)
        sys.exit(1)

    if args.inference_frames is not None and args.inference_frames < 15:
        print(f"Error: --inference-frames must be >= 15 (model receptive field)", file=sys.stderr)
        sys.exit(1)

    # Load PyTorch model
    print(f"Loading PyTorch model from {args.model}...")
    checkpoint = torch.load(args.model, map_location="cpu", weights_only=True)
    if isinstance(checkpoint, dict) and "state_dict" in checkpoint:
        pt_state = checkpoint["state_dict"]
        use_downbeat = checkpoint.get("use_downbeat", cfg["model"].get("downbeat", False))
    else:
        pt_state = checkpoint
        use_downbeat = cfg["model"].get("downbeat", False)

    # Build TF model and transfer weights
    inference_frames = args.inference_frames or cfg["training"]["chunk_frames"]
    dilations = cfg["model"]["dilations"]
    model_type = cfg["model"].get("type", "causal_cnn")
    residual = cfg["model"].get("residual", False)

    fuse_bn = not args.no_fuse_bn
    print(f"Building TF model (type={model_type}, inference_frames={inference_frames}, "
          f"downbeat={use_downbeat}, fuse_bn={fuse_bn}, residual={residual})...")
    tf_model = build_tf_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=dilations,
        chunk_frames=inference_frames,
        downbeat=use_downbeat,
        fuse_bn=fuse_bn,
        model_type=model_type,
        residual=residual,
    )
    dropout = cfg["model"].get("dropout", 0.1)
    transfer_pytorch_weights(tf_model, pt_state, dilations, dropout=dropout,
                             fuse_bn=fuse_bn, model_type=model_type)

    # Verify weight transfer (fused TF model should match unfused PyTorch model)
    print("Verifying weight transfer...")
    from models.beat_cnn import build_beat_cnn as build_pt_cnn
    pt_model = build_pt_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=dilations,
        dropout=cfg["model"].get("dropout", 0.1),
        downbeat=use_downbeat,
        model_type=model_type,
        residual=residual,
    )
    pt_model.load_state_dict(pt_state)
    pt_model.eval()

    test_input = np.random.randn(1, inference_frames, cfg["audio"]["n_mels"]).astype(np.float32)
    with torch.no_grad():
        pt_out = pt_model(torch.from_numpy(test_input)).numpy()
    tf_out = tf_model.predict(test_input, verbose=0)
    max_diff = np.abs(pt_out - tf_out).max()
    ok_thresh = 0.01 if fuse_bn else 0.001  # BN fusion has slightly larger float rounding
    print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < ok_thresh else 'WARNING'}")

    # Export TFLite
    max_size_kb = cfg["export"]["max_model_size_kb"]
    c_array_name = cfg["export"]["c_array_name"]
    header_path = cfg["export"]["output_header"]

    quantize = not args.no_quantize
    tflite_path = str(output_dir / ("beat_model_int8.tflite" if quantize else "beat_model_fp32.tflite"))

    suffix = f" (inference_frames={inference_frames})" if args.inference_frames else ""
    print(f"Exporting {'INT8' if quantize else 'FP32'} TFLite model{suffix}...")
    tflite_bytes = export_tflite(tf_model, data_dir, tflite_path,
                                  quantize_int8=quantize,
                                  inference_frames=args.inference_frames)

    size_kb = len(tflite_bytes) / 1024
    print(f"TFLite model: {size_kb:.1f} KB ({tflite_path})")

    if size_kb > max_size_kb:
        print(f"WARNING: Model ({size_kb:.1f} KB) exceeds budget ({max_size_kb} KB)!", file=sys.stderr)
        print("Consider reducing channels or layers.", file=sys.stderr)
        sys.exit(1)

    # Export C header
    Path(header_path).parent.mkdir(parents=True, exist_ok=True)
    tflite_to_c_header(tflite_bytes, c_array_name, header_path)
    print(f"C header: {header_path}")

    # Also save .tflite next to the header for verification/re-export
    tflite_copy = Path(header_path).parent / "beat_model.tflite"
    tflite_copy.write_bytes(tflite_bytes)
    print(f"TFLite copy: {tflite_copy}")

    # Validate by loading
    interpreter = tf.lite.Interpreter(model_path=tflite_path)
    interpreter.allocate_tensors()
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    print(f"\nModel validation:")
    print(f"  Input:  {input_details[0]['shape']} dtype={input_details[0]['dtype']}")
    print(f"  Output: {output_details[0]['shape']} dtype={output_details[0]['dtype']}")

    ctx_frames = inference_frames
    n_mels = cfg["audio"]["n_mels"]
    ctx_ram = ctx_frames * n_mels * 4
    print(f"  Context buffer: {ctx_frames} frames x {n_mels} mels = {ctx_ram / 1024:.1f} KB RAM")

    arena_bytes, arena_rec = estimate_tensor_arena(tflite_path)
    arena_kb = arena_bytes / 1024
    rec_kb = arena_rec / 1024
    print(f"  Estimated tensor arena: ~{arena_kb:.1f} KB")
    print(f"  Recommended TENSOR_ARENA_SIZE: {arena_rec:,} ({rec_kb:.0f} KB)")
    total_ram_kb = ctx_ram / 1024 + rec_kb
    print(f"  Device RAM (heap): ~{total_ram_kb:.0f} KB (context {ctx_ram / 1024:.0f} KB + arena {rec_kb:.0f} KB)")
    print(f"  Device flash: {size_kb:.1f} KB (model weights, read-only)")


if __name__ == "__main__":
    main()
