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
import yaml


def build_tf_beat_cnn(n_mels: int, channels: int, kernel_size: int,
                      dilations: list[int], chunk_frames: int,
                      downbeat: bool) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch BeatCNN (for TFLite export only)."""
    out_channels = 2 if downbeat else 1
    inputs = keras.Input(shape=(chunk_frames, n_mels), name="mel_input")
    x = inputs

    for i, dilation in enumerate(dilations):
        pad_size = (kernel_size - 1) * dilation
        x = layers.ZeroPadding1D(padding=(pad_size, 0))(x)
        x = layers.Conv1D(channels, kernel_size, dilation_rate=dilation,
                          padding="valid", use_bias=True,
                          name=f"conv{i+1}_d{dilation}")(x)
        x = layers.BatchNormalization(name=f"bn{i+1}")(x)
        x = layers.ReLU(name=f"relu{i+1}")(x)

    x = layers.Conv1D(out_channels, 1, padding="valid", activation="sigmoid",
                      name="output_conv")(x)

    return keras.Model(inputs=inputs, outputs=x, name="beat_cnn")


def transfer_pytorch_weights(tf_model: keras.Model, pt_state_dict: dict,
                             dilations: list[int]):
    """Copy weights from PyTorch state_dict to equivalent TF/Keras model.

    Handles the Conv1D weight transposition (PyTorch: out,in,k → TF: k,in,out)
    and BatchNorm parameter mapping.
    """
    for i, dilation in enumerate(dilations):
        # Conv weights: PyTorch (out_ch, in_ch, kernel) → TF (kernel, in_ch, out_ch)
        conv_name = f"conv{i+1}_d{dilation}"
        pt_w = pt_state_dict[f"backbone.{i*5+1}.weight"].numpy()  # (out, in, k)
        pt_b = pt_state_dict[f"backbone.{i*5+1}.bias"].numpy()
        tf_w = np.transpose(pt_w, (2, 1, 0))  # (k, in, out)
        tf_model.get_layer(conv_name).set_weights([tf_w, pt_b])

        # BatchNorm: gamma, beta, running_mean, running_var
        bn_name = f"bn{i+1}"
        gamma = pt_state_dict[f"backbone.{i*5+2}.weight"].numpy()
        beta = pt_state_dict[f"backbone.{i*5+2}.bias"].numpy()
        mean = pt_state_dict[f"backbone.{i*5+2}.running_mean"].numpy()
        var = pt_state_dict[f"backbone.{i*5+2}.running_var"].numpy()
        tf_model.get_layer(bn_name).set_weights([gamma, beta, mean, var])

    # Output conv
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

alignas(8) const unsigned char {c_array_name}[] = {{
{chr(10).join(hex_lines)}
}};

const unsigned int {c_array_name}_len = {len(tflite_bytes)};

#endif // BEAT_MODEL_DATA_H
"""
    with open(output_path, "w") as f:
        f.write(header)


def main():
    parser = argparse.ArgumentParser(description="Export model to TFLite INT8 C header")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--model", default="outputs/best_model.pt", help="PyTorch model path")
    parser.add_argument("--data-dir", default=None, help="Processed data dir (for calibration)")
    parser.add_argument("--output-dir", default="outputs", help="Output directory")
    parser.add_argument("--no-quantize", action="store_true", help="Skip INT8 quantization")
    parser.add_argument("--inference-frames", type=int, default=None,
                        help="Context frames for device inference (default: use training chunk size). "
                             "Smaller = less device RAM. Must be >= receptive field (15 frames).")
    args = parser.parse_args()

    with open(args.config) as f:
        cfg = yaml.safe_load(f)

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

    print(f"Building TF model (inference_frames={inference_frames}, downbeat={use_downbeat})...")
    tf_model = build_tf_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=dilations,
        chunk_frames=inference_frames,
        downbeat=use_downbeat,
    )
    transfer_pytorch_weights(tf_model, pt_state, dilations)

    # Verify weight transfer
    print("Verifying weight transfer...")
    from models.beat_cnn import build_beat_cnn as build_pt_cnn
    pt_model = build_pt_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=dilations,
        dropout=cfg["model"].get("dropout", 0.1),
        downbeat=use_downbeat,
    )
    pt_model.load_state_dict(pt_state)
    pt_model.eval()

    test_input = np.random.randn(1, inference_frames, cfg["audio"]["n_mels"]).astype(np.float32)
    with torch.no_grad():
        pt_out = pt_model(torch.from_numpy(test_input)).numpy()
    tf_out = tf_model.predict(test_input, verbose=0)
    max_diff = np.abs(pt_out - tf_out).max()
    print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < 0.001 else 'WARNING'}")

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

    tensor_arena_kb = sum(
        np.prod(d["shape"]) * np.dtype(d["dtype"]).itemsize
        for d in interpreter.get_tensor_details()
    ) / 1024
    print(f"  Estimated tensor arena: ~{tensor_arena_kb:.1f} KB")
    print(f"  Total device RAM: ~{(ctx_ram / 1024 + tensor_arena_kb + size_kb):.1f} KB "
          f"(context + arena + model flash)")


if __name__ == "__main__":
    main()
