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

# Ensure ml-training root is on path (for "from models.onset_cnn import ...")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import numpy as np
import tensorflow as tf
import tf_keras as keras
from tf_keras import layers
import torch

from scripts.audio import load_config


def build_tf_onset_cnn(n_mels: int, channels: int, kernel_size: int,
                      dilations: list[int], chunk_frames: int,
                      fuse_bn: bool = False,
                      model_type: str = "causal_cnn",
                      residual: bool = False) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch OnsetCNN or DSTCNOnsetCNN.

    If fuse_bn=True, BatchNorm is omitted (weights will be fused into Conv
    by transfer_pytorch_weights). This eliminates expensive unfused ADD/MUL
    ops in TFLite for dilated convolutions.
    """
    if model_type == "ds_tcn":
        return _build_tf_ds_tcn(n_mels, channels, kernel_size, dilations,
                                chunk_frames, fuse_bn, residual)

    out_channels = 1
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

    return keras.Model(inputs=inputs, outputs=x, name="onset_cnn")


def _build_tf_ds_tcn(n_mels: int, channels: int, kernel_size: int,
                     dilations: list[int], chunk_frames: int,
                     fuse_bn: bool, residual: bool) -> keras.Model:
    """Build TF/Keras equivalent of DSTCNOnsetCNN.

    Depthwise separable blocks: ZeroPad → DepthwiseConv1D → [BN] → ReLU →
                                Conv1D(1×1) → [BN] → ReLU → [+ residual]
    """
    out_channels = 1
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
    """Transfer DSTCNOnsetCNN weights to TF/Keras equivalent.

    DSTCNOnsetCNN PyTorch state_dict key layout:
      input_conv.weight, input_conv.bias  (bias redundant with BN, TODO: remove)
      input_bn.weight, input_bn.bias, input_bn.running_mean, input_bn.running_var
      blocks.0.dw_conv.weight          (no bias)
      blocks.0.dw_bn.weight/bias/running_mean/running_var
      blocks.0.pw_conv.weight, blocks.0.pw_conv.bias  (bias redundant with BN, TODO: remove)
      blocks.0.pw_bn.weight/bias/running_mean/running_var
      ...
      output_conv.weight, output_conv.bias
    """
    bn_eps = 1e-5

    # --- Input conv ---
    pt_w = pt_state_dict["input_conv.weight"].numpy()  # (out, in, k)
    pt_b = pt_state_dict.get("input_conv.bias", None)
    pt_b = pt_b.numpy() if pt_b is not None else np.zeros(pt_w.shape[0], dtype=np.float32)

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
        pw_b_tensor = pt_state_dict.get(f"blocks.{i}.pw_conv.bias", None)
        pw_b = pw_b_tensor.numpy() if pw_b_tensor is not None else np.zeros(pw_w.shape[0], dtype=np.float32)

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


def build_tf_frame_conv1d(n_mels: int, channels: list[int],
                          kernel_sizes: list[int], window_frames: int,
                          freq_pos_encoding: bool = False,
                          num_output_channels: int = 0) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch FrameOnsetConv1D.

    Causal Conv1D layers with ReLU fused into Conv ops (no separate ReLU op).
    No BatchNorm — no fusion needed. ZeroPadding1D for causal padding.

    If freq_pos_encoding: adds a learnable per-mel-band bias before convolutions
    (FAC positional encoding, helps discriminate kicks from hi-hats).

    TFLite ops: Conv2D (Conv1D mapped), Pad, Reshape, Logistic, Quantize, Dequantize.
    """
    out_channels = num_output_channels if num_output_channels > 0 else 1
    inputs = keras.Input(shape=(window_frames, n_mels), name="mel_input")
    x = inputs

    if freq_pos_encoding:
        # Learnable frequency position vector (1, 1, n_mels), broadcast-added
        # to input before convolutions. Folded into the first Conv1D bias at
        # weight transfer time. The folding is exact for all output timesteps
        # past the causal padding boundary (t >= k-1), which includes the
        # firmware's inference position (last frame of the window).
        #
        # For the first k-1 output timesteps, the folded bias slightly
        # over-adds freq_pos (assumes full kernel window sees freq_pos, but
        # padded zeros don't). This is a minor approximation that only affects
        # chunk-boundary frames in offline evaluation, not firmware inference.
        pass  # freq_pos folded into conv1 bias during _transfer_conv1d_weights

    for i, (ch, k) in enumerate(zip(channels, kernel_sizes)):
        pad = k - 1
        x = layers.ZeroPadding1D(padding=(pad, 0))(x)
        x = layers.Conv1D(ch, k, padding="valid", use_bias=True,
                          activation="relu", name=f"conv{i+1}")(x)

    x = layers.Conv1D(out_channels, 1, padding="valid", activation="sigmoid",
                      name="output_conv")(x)

    return keras.Model(inputs=inputs, outputs=x, name="frame_onset_conv1d")


def build_tf_frame_conv1d_pool(n_mels: int, channels: list[int],
                               kernel_sizes: list[int], pool_sizes: list[int],
                               window_frames: int,
                               use_stride: bool = False) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch FrameOnsetConv1DPool.

    Conv1D with interleaved AveragePooling1D or strided conv for temporal compression.
    Strided mode uses fewer TFLite ops (no separate AvgPool), reducing dispatch overhead.
    """
    out_channels = 1
    inputs = keras.Input(shape=(window_frames, n_mels), name="mel_input")
    x = inputs

    for i, (ch, k, pool) in enumerate(zip(channels, kernel_sizes, pool_sizes)):
        stride = pool if use_stride else 1
        pad = k - 1  # causal: pad left only
        x = layers.ZeroPadding1D(padding=(pad, 0))(x)
        x = layers.Conv1D(ch, k, strides=stride, padding="valid", use_bias=True,
                          activation="relu", name=f"conv{i+1}")(x)
        if not use_stride and pool > 1:
            x = layers.AveragePooling1D(pool_size=pool, strides=pool,
                                        padding="valid", name=f"pool{i+1}")(x)

    x = layers.Conv1D(out_channels, 1, padding="valid", activation="sigmoid",
                      name="output_conv")(x)

    return keras.Model(inputs=inputs, outputs=x, name="frame_onset_conv1d_pool")


def _transfer_conv1d_weights(tf_model: keras.Model, pt_state_dict: dict,
                              channels: list[int],
                              freq_pos_encoding: bool = False):
    """Transfer FrameOnsetConv1D weights from PyTorch to TF/Keras.

    No BatchNorm to worry about — just transpose Conv1D weights.
    Extracts Conv1d layers by name pattern (backbone.N.weight) rather than
    assuming a fixed stride, robust to changes in non-parametric layers.

    Bias folding for freq_pos_encoding:
        The learnable frequency position vector (1, 1, n_mels) is added to the
        input before convolutions in PyTorch: conv(x + fp). This is equivalent
        to conv(x) + conv(fp), where conv(fp) is a constant per output channel.
        We fold conv(fp) into the first Conv1D's bias, eliminating the extra op.

        This folding is exact for timesteps where the full kernel window sees
        real input (t >= k-1). For the first k-1 timesteps, causal zero-padding
        means the freq_pos contribution is partial, so the folded bias slightly
        over-estimates. This is acceptable because:
          - Firmware inference uses only the LAST frame of the sliding window
            (always t=15 for W16, well past the k-1=4 boundary)
          - Offline evaluation uses overlapping chunks where boundary frames
            contribute minimally to the averaged activation

        Do NOT use this folding for sequential/streaming inference where every
        frame's output matters equally — use a separate addition op instead.
    """
    # Frequency position encoding: fold into first Conv1D bias.
    # conv(x + fp) = conv(x) + conv(fp). Since fp is constant across time,
    # conv(fp) is a constant per output channel: sum_k sum_in W[c,in,k]*fp[in].
    # This is exact for all timesteps past the causal padding boundary.
    freq_pos_bias_delta = None
    if freq_pos_encoding and "freq_pos" in pt_state_dict:
        freq_pos_val = pt_state_dict["freq_pos"].numpy().squeeze()  # (n_mels,)
        # Get first conv's PT weights: (out_ch, in_ch, k)
        first_conv_key = sorted(
            [k for k in pt_state_dict if k.startswith("backbone.") and k.endswith(".weight")],
            key=lambda k: int(k.split(".")[1])
        )[0]
        first_w = pt_state_dict[first_conv_key].numpy()  # (out_ch, n_mels, k)
        # Bias delta: for each output channel, sum W * fp over all kernel positions and input channels
        # W shape: (out_ch, in_ch, k), fp shape: (in_ch,)
        # delta[c] = sum_k sum_in W[c, in, k] * fp[in] = sum_k (W[c,:,k] @ fp)
        freq_pos_bias_delta = np.einsum('oik,i->o', first_w, freq_pos_val)
    # Find all Conv1d layers in backbone by scanning for .weight keys
    backbone_conv_keys = sorted(
        [k for k in pt_state_dict if k.startswith("backbone.") and k.endswith(".weight")],
        key=lambda k: int(k.split(".")[1])
    )
    assert len(backbone_conv_keys) == len(channels), (
        f"Expected {len(channels)} Conv1d layers in backbone, "
        f"found {len(backbone_conv_keys)}: {backbone_conv_keys}"
    )

    for i, w_key in enumerate(backbone_conv_keys):
        b_key = w_key.replace(".weight", ".bias")
        pt_w = pt_state_dict[w_key].numpy()  # (out, in, k)
        pt_b = pt_state_dict[b_key].numpy()
        # Fold freq_pos into first conv bias
        if i == 0 and freq_pos_bias_delta is not None:
            pt_b = pt_b + freq_pos_bias_delta
        tf_w = np.transpose(pt_w, (2, 1, 0))  # (k, in, out)
        tf_layer = tf_model.get_layer(f"conv{i+1}")
        expected_shape = tuple(tf_layer.get_weights()[0].shape)
        assert tf_w.shape == expected_shape, (
            f"Shape mismatch for conv{i+1}: PT weight {tf_w.shape} vs TF kernel {expected_shape}"
        )
        tf_layer.set_weights([tf_w, pt_b])

    # Output conv (1×1)
    pt_w = pt_state_dict["output_conv.weight"].numpy()  # (out, in, 1)
    pt_b = pt_state_dict["output_conv.bias"].numpy()
    tf_w = np.transpose(pt_w, (2, 1, 0))  # (1, in, out)
    tf_layer = tf_model.get_layer("output_conv")
    expected_shape = tuple(tf_layer.get_weights()[0].shape)
    assert tf_w.shape == expected_shape, (
        f"Shape mismatch for output_conv: PT weight {tf_w.shape} vs TF kernel {expected_shape}"
    )
    tf_layer.set_weights([tf_w, pt_b])


def conv1d_representative_dataset_gen(data_path: Path, window_frames: int,
                                       n_mels: int = 26, n_samples: int = 200):
    """Calibration data generator for Conv1D model INT8 quantization.

    Extracts random windows from training chunks as 3D tensors
    (1, window_frames, n_mels) — matching the TFLite model's input shape.
    """
    X = np.load(data_path / "X_train.npy", mmap_mode='r')
    chunk_frames = X.shape[1]
    rng = np.random.RandomState(42)
    indices = rng.choice(len(X), size=min(n_samples, len(X)), replace=False)
    for i in indices:
        chunk = X[i].astype(np.float32)  # (chunk_frames, n_mels)
        start = rng.randint(0, chunk_frames - window_frames + 1)
        window = chunk[start:start + window_frames]  # (W, n_mels)
        yield [window.reshape(1, window_frames, n_mels)]


def build_tf_frame_fc(n_mels: int, window_frames: int, hidden_dims: list[int]) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch FrameOnsetFC.

    Takes flat input (1, window_frames * n_mels) — no Reshape op needed in
    TFLite. Firmware writes window buffer as flat array, so shapes match.

    TFLite ops: FullyConnected (with fused ReLU/Logistic), Quantize, Dequantize.
    All already in FrameOnsetNN.h resolver (5 slots).
    """
    out_channels = 1
    input_dim = window_frames * n_mels

    inputs = keras.Input(shape=(input_dim,), name="flat_input")
    x = inputs

    for i, h in enumerate(hidden_dims):
        x = layers.Dense(h, activation="relu", name=f"fc{i+1}")(x)

    x = layers.Dense(out_channels, activation="sigmoid", name="output")(x)

    return keras.Model(inputs=inputs, outputs=x, name="frame_onset_fc")


def _transfer_fc_weights(tf_model: keras.Model, pt_state_dict: dict,
                         hidden_dims: list[int]):
    """Transfer FrameOnsetFC weights from PyTorch to TF/Keras.

    Extracts Linear layer weights by name pattern (backbone.N.weight/bias)
    rather than assuming a fixed stride through the state_dict. This is
    robust to changes in non-parametric layers (ReLU, Dropout, etc.).

    PyTorch Linear: weight (out_features, in_features), bias (out_features)
    TF Dense: kernel (in_features, out_features), bias (out_features)
    """
    # Find all Linear layers in backbone by scanning for .weight keys
    backbone_linear_keys = sorted(
        [k for k in pt_state_dict if k.startswith("backbone.") and k.endswith(".weight")],
        key=lambda k: int(k.split(".")[1])
    )
    assert len(backbone_linear_keys) == len(hidden_dims), (
        f"Expected {len(hidden_dims)} Linear layers in backbone, "
        f"found {len(backbone_linear_keys)}: {backbone_linear_keys}"
    )

    for i, w_key in enumerate(backbone_linear_keys):
        b_key = w_key.replace(".weight", ".bias")
        pt_w = pt_state_dict[w_key].numpy()  # (out, in)
        pt_b = pt_state_dict[b_key].numpy()
        tf_w = pt_w.T  # (in, out)
        tf_layer = tf_model.get_layer(f"fc{i+1}")
        expected_shape = tuple(tf_layer.get_weights()[0].shape)
        assert tf_w.shape == expected_shape, (
            f"Shape mismatch for fc{i+1}: PT weight {tf_w.shape} vs TF kernel {expected_shape}"
        )
        tf_layer.set_weights([tf_w, pt_b])

    # Output head
    pt_w = pt_state_dict["output_head.weight"].numpy()
    pt_b = pt_state_dict["output_head.bias"].numpy()
    tf_w = pt_w.T
    tf_layer = tf_model.get_layer("output")
    expected_shape = tuple(tf_layer.get_weights()[0].shape)
    assert tf_w.shape == expected_shape, (
        f"Shape mismatch for output: PT weight {tf_w.shape} vs TF kernel {expected_shape}"
    )
    tf_layer.set_weights([tf_w, pt_b])


def build_tf_frame_fc_enhanced(n_mels: int, window_frames: int,
                                hidden_dims: list[int],
                                se_ratio: int = 0, conv_channels: int = 0,
                                conv_kernel: int = 5, short_window: int = 0,
                                short_hidden: int = 0) -> keras.Model:
    """Build TF/Keras equivalent of PyTorch EnhancedOnsetFC.

    Input is flat (1, window_frames * n_mels) to match firmware buffer.
    Reshapes internally when needed for SE/Conv1D operations.

    Additional TFLite ops beyond baseline FC:
      - SE: Reshape, Mean (ReduceMean), Mul
      - Conv1D: Reshape, ZeroPad, Conv1D (→Conv2D), Reshape
      - Multi-window: StridedSlice (or Lambda), Concatenate
    """
    out_channels = 1
    use_se = se_ratio > 0
    use_conv = conv_channels > 0
    use_multiwindow = short_window > 0

    input_dim = window_frames * n_mels
    inputs = keras.Input(shape=(input_dim,), name="flat_input")

    # Reshape to 2D for Conv1D and/or SE operations
    if use_se or use_conv:
        x = layers.Reshape((window_frames, n_mels), name="reshape_2d")(inputs)
    else:
        x = inputs

    # --- Conv1D front-end (applied before SE, matching PyTorch order) ---
    if use_conv:
        pad = conv_kernel - 1
        x = layers.ZeroPadding1D(padding=(pad, 0), name="conv_pad")(x)
        x = layers.Conv1D(conv_channels, conv_kernel, padding="valid",
                          use_bias=True, activation="relu", name="conv_front")(x)

    feature_dim = conv_channels if use_conv else n_mels

    # --- SE block (applied after Conv1D, on the window) ---
    if use_se:
        mid = max(1, feature_dim // se_ratio)
        # Pool over time dimension → (1, feature_dim)
        se = layers.GlobalAveragePooling1D(name="se_pool")(x)
        se = layers.Dense(mid, activation="relu", name="se_fc1")(se)
        se = layers.Dense(feature_dim, activation="sigmoid", name="se_fc2")(se)
        # Broadcast multiply: (1, W, feature_dim) * (1, 1, feature_dim)
        se = layers.Reshape((1, feature_dim), name="se_reshape")(se)
        x = layers.Multiply(name="se_mul")([x, se])
        # Need reshape for SE-only case (no Conv1D means we need to stay 2D)
        if not use_conv:
            pass  # Already 2D from reshape_2d

    # Flatten for FC layers
    if use_se or use_conv:
        x_flat = layers.Reshape((window_frames * feature_dim,), name="flatten_long")(x)
    else:
        x_flat = x

    # --- Long window FC path ---
    h = x_flat
    for i, dim in enumerate(hidden_dims):
        h = layers.Dense(dim, activation="relu", name=f"fc{i+1}")(h)

    # --- Short window FC path (optional) ---
    if use_multiwindow:
        short_hidden = short_hidden or hidden_dims[-1]
        # Extract last short_window * feature_dim elements from flat representation
        short_dim = short_window * feature_dim
        if use_se or use_conv:
            # Slice from 2D tensor: last short_window frames (after SE+Conv1D)
            x_short = layers.Cropping1D(cropping=(window_frames - short_window, 0),
                                         name="crop_short")(x)
            x_short_flat = layers.Reshape((short_dim,), name="flatten_short")(x_short)
        else:
            # Slice from flat input (capture offset via default arg for closure safety)
            _offset = (window_frames - short_window) * feature_dim
            x_short_flat = layers.Lambda(
                lambda t, o=_offset: t[:, o:], output_shape=(short_dim,),
                name="slice_short")(inputs)
        h_short = layers.Dense(short_hidden, activation="relu",
                                name="fc_short")(x_short_flat)
        h = layers.Concatenate(name="merge")([h, h_short])

    # --- Output ---
    out = layers.Dense(out_channels, activation="sigmoid", name="output")(h)

    return keras.Model(inputs=inputs, outputs=out, name="enhanced_onset_fc")


def _transfer_enhanced_fc_weights(tf_model: keras.Model, pt_state_dict: dict,
                                   cfg: dict):
    """Transfer EnhancedOnsetFC weights from PyTorch to TF/Keras."""
    # SE block
    if cfg["model"].get("se_ratio", 0) > 0:
        for name_pt, name_tf in [("se.fc1", "se_fc1"), ("se.fc2", "se_fc2")]:
            pt_w = pt_state_dict[f"{name_pt}.weight"].numpy().T  # (in, out)
            pt_b = pt_state_dict[f"{name_pt}.bias"].numpy()
            tf_model.get_layer(name_tf).set_weights([pt_w, pt_b])

    # Conv1D front-end
    if cfg["model"].get("conv_channels", 0) > 0:
        pt_w = pt_state_dict["conv1d.weight"].numpy()  # (out, in, k)
        pt_b = pt_state_dict["conv1d.bias"].numpy()
        tf_w = np.transpose(pt_w, (2, 1, 0))  # (k, in, out)
        tf_model.get_layer("conv_front").set_weights([tf_w, pt_b])

    # Long backbone FC layers
    hidden_dims = cfg["model"]["hidden_dims"]
    backbone_linear_keys = sorted(
        [k for k in pt_state_dict if k.startswith("long_backbone.") and k.endswith(".weight")],
        key=lambda k: int(k.split(".")[1])
    )
    for i, w_key in enumerate(backbone_linear_keys):
        b_key = w_key.replace(".weight", ".bias")
        pt_w = pt_state_dict[w_key].numpy().T  # (in, out)
        pt_b = pt_state_dict[b_key].numpy()
        tf_model.get_layer(f"fc{i+1}").set_weights([pt_w, pt_b])

    # Short path FC (if multi-window)
    if cfg["model"].get("short_window", 0) > 0:
        # Find the Linear layer in short_backbone
        short_w_keys = sorted(
            [k for k in pt_state_dict if k.startswith("short_backbone.") and k.endswith(".weight")],
            key=lambda k: int(k.split(".")[1])
        )
        if short_w_keys:
            w_key = short_w_keys[0]
            b_key = w_key.replace(".weight", ".bias")
            pt_w = pt_state_dict[w_key].numpy().T
            pt_b = pt_state_dict[b_key].numpy()
            tf_model.get_layer("fc_short").set_weights([pt_w, pt_b])

    # Output head
    pt_w = pt_state_dict["output_head.weight"].numpy().T
    pt_b = pt_state_dict["output_head.bias"].numpy()
    tf_model.get_layer("output").set_weights([pt_w, pt_b])


def fc_representative_dataset_gen(data_path: Path, window_frames: int,
                                  n_mels: int = 26, n_samples: int = 200):
    """Calibration data generator for FC model INT8 quantization.

    Extracts random windows from training chunks and flattens them
    to match the TFLite model's flat input shape (1, window_frames * n_mels).
    """
    X = np.load(data_path / "X_train.npy", mmap_mode='r')
    chunk_frames = X.shape[1]
    rng = np.random.RandomState(42)
    indices = rng.choice(len(X), size=min(n_samples, len(X)), replace=False)
    for i in indices:
        chunk = X[i].astype(np.float32)  # (chunk_frames, n_mels)
        start = rng.randint(0, chunk_frames - window_frames + 1)
        window = chunk[start:start + window_frames]  # (W, n_mels)
        yield [window.reshape(1, -1)]  # (1, W * n_mels)


def representative_dataset_gen(data_path: Path, inference_frames: int = None,
                                n_samples: int = 200):
    """Generator for calibration data (required for full INT8 quantization)."""
    X = np.load(data_path / "X_train.npy", mmap_mode='r')
    rng = np.random.RandomState(42)  # Deterministic for reproducible quantization
    indices = rng.choice(len(X), size=min(n_samples, len(X)), replace=False)
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

    guard_name = c_array_name.upper() + "_H"
    macro_prefix = c_array_name.upper()

    header = f"""// Auto-generated by export_tflite.py — do not edit
// Model size: {len(tflite_bytes)} bytes ({len(tflite_bytes)/1024:.1f} KB)
// SHA256 prefix: {model_hash}
// Exported: {timestamp}

#ifndef {guard_name}
#define {guard_name}

#define {macro_prefix}_HASH "{model_hash}"
#define {macro_prefix}_SIZE {len(tflite_bytes)}

alignas(8) static const unsigned char {c_array_name}[] = {{
{chr(10).join(hex_lines)}
}};

static const unsigned int {c_array_name}_len = {len(tflite_bytes)};

#endif // {guard_name}
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
    # Note: DS-TCN v9 has a different op mix (DepthwiseConv2D + ADD residuals);
    # re-calibrate once v9 runs on-device and update with measured arena usage.
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
    else:
        pt_state = checkpoint

    model_type = cfg["model"].get("type", "causal_cnn")
    use_delta = cfg.get("features", {}).get("use_delta", False)
    n_mels = cfg["audio"]["n_mels"] * (2 if use_delta else 1)

    if model_type == "frame_fc_enhanced":
        # --- Enhanced FC model ---
        window_frames = cfg["model"]["window_frames"]
        hidden_dims = cfg["model"]["hidden_dims"]

        features = []
        if cfg["model"].get("se_ratio", 0) > 0:
            features.append(f"SE(ratio={cfg['model']['se_ratio']})")
        if cfg["model"].get("conv_channels", 0) > 0:
            features.append(f"Conv1D({cfg['model']['conv_channels']}ch)")
        if cfg["model"].get("short_window", 0) > 0:
            features.append(f"MultiWindow(short={cfg['model']['short_window']})")
        print(f"Building TF model (type=frame_fc_enhanced, window={window_frames}, "
              f"hidden={hidden_dims}, features=[{', '.join(features)}])...")

        tf_model = build_tf_frame_fc_enhanced(
            n_mels=n_mels,
            window_frames=window_frames,
            hidden_dims=hidden_dims,
            se_ratio=cfg["model"].get("se_ratio", 0),
            conv_channels=cfg["model"].get("conv_channels", 0),
            conv_kernel=cfg["model"].get("conv_kernel", 5),
            short_window=cfg["model"].get("short_window", 0),
            short_hidden=cfg["model"].get("short_hidden", 0),
        )
        _transfer_enhanced_fc_weights(tf_model, pt_state, cfg)

        # Verify weight transfer
        print("Verifying weight transfer...")
        from models.onset_fc_enhanced import build_onset_fc_enhanced
        pt_model = build_onset_fc_enhanced(
            n_mels=n_mels, window_frames=window_frames,
            hidden_dims=hidden_dims,
            dropout=cfg["model"].get("dropout", 0.1),
            se_ratio=cfg["model"].get("se_ratio", 0),
            conv_channels=cfg["model"].get("conv_channels", 0),
            conv_kernel=cfg["model"].get("conv_kernel", 5),
            short_window=cfg["model"].get("short_window", 0),
            short_hidden=cfg["model"].get("short_hidden", 0),
        )
        pt_model.load_state_dict(pt_state, strict=False)  # strict=False: skip tempo_head
        pt_model.eval()

        test_window = np.random.randn(1, window_frames, n_mels).astype(np.float32)
        test_flat = test_window.reshape(1, -1)
        with torch.no_grad():
            pt_out = pt_model(torch.from_numpy(test_window))
            if isinstance(pt_out, tuple):
                pt_out = pt_out[0]
            pt_out = pt_out[:, -1, :].numpy()
        tf_out = tf_model.predict(test_flat, verbose=0)
        max_diff = np.abs(pt_out - tf_out).max()
        print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < 0.01 else 'WARNING'}")

        inference_frames = window_frames
    elif model_type == "frame_fc":
        # --- Frame-level FC model ---
        window_frames = cfg["model"]["window_frames"]
        hidden_dims = cfg["model"]["hidden_dims"]

        print(f"Building TF model (type=frame_fc, window={window_frames}, "
              f"hidden={hidden_dims})...")

        tf_model = build_tf_frame_fc(
            n_mels=n_mels,
            window_frames=window_frames,
            hidden_dims=hidden_dims,
        )
        _transfer_fc_weights(tf_model, pt_state, hidden_dims)

        # Verify weight transfer
        print("Verifying weight transfer...")
        from models.onset_fc import build_onset_fc
        pt_model = build_onset_fc(
            n_mels=n_mels,
            window_frames=window_frames,
            hidden_dims=hidden_dims,
            dropout=cfg["model"].get("dropout", 0.1),
        )
        pt_model.load_state_dict(pt_state)
        pt_model.eval()

        # PT takes (1, W, 26); TF takes (1, W*26). Compare at last timestep.
        test_window = np.random.randn(1, window_frames, n_mels).astype(np.float32)
        test_flat = test_window.reshape(1, -1)
        with torch.no_grad():
            pt_out = pt_model(torch.from_numpy(test_window))[:, -1, :].numpy()
        tf_out = tf_model.predict(test_flat, verbose=0)
        max_diff = np.abs(pt_out - tf_out).max()
        print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < 0.001 else 'WARNING'}")

        inference_frames = window_frames  # For RAM estimate
    elif model_type == "frame_conv1d":
        # --- Frame-level Conv1D model ---
        channels = cfg["model"]["channels"]
        kernel_sizes = cfg["model"]["kernel_sizes"]
        window_frames = cfg["model"]["window_frames"]
        dropout = cfg["model"].get("dropout", 0.1)
        freq_pos_encoding = cfg["model"].get("freq_pos_encoding", False)

        print(f"Building TF model (type=frame_conv1d, channels={channels}, "
              f"kernels={kernel_sizes}, window={window_frames}, "
              f"freq_pos={freq_pos_encoding})...")
        num_output_channels = cfg["model"].get("num_output_channels", 0)
        tf_model = build_tf_frame_conv1d(
            n_mels=n_mels,
            channels=channels,
            kernel_sizes=kernel_sizes,
            window_frames=window_frames,
            freq_pos_encoding=freq_pos_encoding,
            num_output_channels=num_output_channels,
        )
        _transfer_conv1d_weights(tf_model, pt_state, channels,
                                 freq_pos_encoding=freq_pos_encoding)

        # Verify weight transfer
        print("Verifying weight transfer...")
        from models.onset_conv1d import build_onset_conv1d
        pt_model = build_onset_conv1d(
            n_mels=n_mels,
            channels=channels,
            kernel_sizes=kernel_sizes,
            dropout=dropout,
            freq_pos_encoding=freq_pos_encoding,
            num_output_channels=num_output_channels,
        )
        pt_model.load_state_dict(pt_state)
        pt_model.eval()

        test_input = np.random.randn(1, window_frames, n_mels).astype(np.float32)
        with torch.no_grad():
            pt_out = pt_model(torch.from_numpy(test_input)).numpy()
        tf_out = tf_model.predict(test_input, verbose=0)
        max_diff = np.abs(pt_out - tf_out).max()
        # When freq_pos is folded into conv bias, early timesteps (within the
        # causal padding boundary) have a small approximation error. Compare
        # the last frame (firmware inference position) separately.
        last_diff = np.abs(pt_out[:, -1:, :] - tf_out[:, -1:, :]).max()
        if freq_pos_encoding and max_diff > 0.001:
            print(f"  PT vs TF max diff: {max_diff:.6f} (freq_pos bias folding, "
                  f"last frame diff: {last_diff:.6f} {'OK' if last_diff < 0.001 else 'WARNING'})")
        else:
            print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < 0.001 else 'WARNING'}")

        inference_frames = window_frames
    elif model_type == "frame_conv1d_pool":
        # --- Frame-level Conv1D with temporal pooling ---
        channels = cfg["model"]["channels"]
        kernel_sizes = cfg["model"]["kernel_sizes"]
        pool_sizes = cfg["model"]["pool_sizes"]
        window_frames = cfg["model"]["window_frames"]
        dropout = cfg["model"].get("dropout", 0.1)
        use_stride = cfg["model"].get("use_stride", False)

        print(f"Building TF model (type=frame_conv1d_pool, channels={channels}, "
              f"kernels={kernel_sizes}, pools={pool_sizes}, window={window_frames}, "
              f"stride={use_stride})...")
        tf_model = build_tf_frame_conv1d_pool(
            n_mels=n_mels,
            channels=channels,
            kernel_sizes=kernel_sizes,
            pool_sizes=pool_sizes,
            window_frames=window_frames,
            use_stride=use_stride,
        )
        # Reuse conv1d weight transfer — pooling layers are parameter-free
        _transfer_conv1d_weights(tf_model, pt_state, channels)

        # Verify weight transfer
        print("Verifying weight transfer...")
        from models.onset_conv1d_pool import build_onset_conv1d_pool
        pt_model = build_onset_conv1d_pool(
            n_mels=n_mels,
            channels=channels,
            kernel_sizes=kernel_sizes,
            pool_sizes=pool_sizes,
            dropout=dropout,
            use_stride=use_stride,
        )
        pt_model.load_state_dict(pt_state)
        pt_model.eval()

        test_input = np.random.randn(1, window_frames, n_mels).astype(np.float32)
        with torch.no_grad():
            pt_out = pt_model(torch.from_numpy(test_input)).numpy()
        tf_out = tf_model.predict(test_input, verbose=0)
        max_diff = np.abs(pt_out - tf_out).max()
        print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < 0.001 else 'WARNING'}")

        inference_frames = window_frames
    else:
        # --- CNN model (causal_cnn or ds_tcn) ---
        inference_frames = args.inference_frames or cfg["training"]["chunk_frames"]
        dilations = cfg["model"]["dilations"]
        residual = cfg["model"].get("residual", False)

        fuse_bn = not args.no_fuse_bn
        print(f"Building TF model (type={model_type}, inference_frames={inference_frames}, "
              f"fuse_bn={fuse_bn}, residual={residual})...")
        tf_model = build_tf_onset_cnn(
            n_mels=n_mels,
            channels=cfg["model"]["channels"],
            kernel_size=cfg["model"]["kernel_size"],
            dilations=dilations,
            chunk_frames=inference_frames,
            fuse_bn=fuse_bn,
            model_type=model_type,
            residual=residual,
        )
        dropout = cfg["model"].get("dropout", 0.1)
        transfer_pytorch_weights(tf_model, pt_state, dilations, dropout=dropout,
                                 fuse_bn=fuse_bn, model_type=model_type)

        # Verify weight transfer
        print("Verifying weight transfer...")
        from models.onset_cnn import build_onset_cnn as build_pt_cnn
        pt_model = build_pt_cnn(
            n_mels=n_mels,
            channels=cfg["model"]["channels"],
            kernel_size=cfg["model"]["kernel_size"],
            dilations=dilations,
            dropout=cfg["model"].get("dropout", 0.1),
            model_type=model_type,
            residual=residual,
        )
        pt_model.load_state_dict(pt_state)
        pt_model.eval()

        test_input = np.random.randn(1, inference_frames, n_mels).astype(np.float32)
        with torch.no_grad():
            pt_out = pt_model(torch.from_numpy(test_input)).numpy()
        tf_out = tf_model.predict(test_input, verbose=0)
        max_diff = np.abs(pt_out - tf_out).max()
        ok_thresh = 0.01 if fuse_bn else 0.001
        print(f"  PT vs TF max diff: {max_diff:.6f} {'OK' if max_diff < ok_thresh else 'WARNING'}")

    # Export TFLite
    max_size_kb = cfg["export"]["max_model_size_kb"]
    c_array_name = cfg["export"]["c_array_name"]
    header_path = cfg["export"]["output_header"]

    quantize = not args.no_quantize
    tflite_name = f"{c_array_name}_int8.tflite" if quantize else f"{c_array_name}_fp32.tflite"
    tflite_path = str(output_dir / tflite_name)

    print(f"Exporting {'INT8' if quantize else 'FP32'} TFLite model...")
    if model_type in ("frame_fc", "frame_fc_enhanced", "frame_conv1d", "frame_conv1d_pool") and quantize:
        # Frame models need window-based calibration samples
        window_frames = cfg["model"]["window_frames"]
        converter = tf.lite.TFLiteConverter.from_keras_model(tf_model)
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        if model_type in ("frame_fc", "frame_fc_enhanced"):
            converter.representative_dataset = lambda: fc_representative_dataset_gen(
                data_dir, window_frames=window_frames, n_mels=n_mels)
            # CRITICAL: Disable per-channel weight quantization for Dense layers.
            # TFLite Micro's CMSIS-NN FullyConnected kernel (arm_fully_connected_s8)
            # only supports per-tensor requantization (single multiplier/shift).
            # Per-channel weights cause filter->params.scale=0, making the
            # requantization multiplier=0 and producing constant -128 output.
            converter._experimental_disable_per_channel = True
        else:
            converter.representative_dataset = lambda: conv1d_representative_dataset_gen(
                data_dir, window_frames=window_frames, n_mels=n_mels)
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        converter.inference_input_type = tf.int8
        converter.inference_output_type = tf.int8
        tflite_bytes = converter.convert()
        with open(tflite_path, "wb") as f:
            f.write(tflite_bytes)
    else:
        tflite_bytes = export_tflite(tf_model, data_dir, tflite_path,
                                      quantize_int8=quantize,
                                      inference_frames=args.inference_frames)

    size_kb = len(tflite_bytes) / 1024
    print(f"TFLite model: {size_kb:.1f} KB ({tflite_path})")

    if size_kb > max_size_kb:
        print(f"WARNING: Model ({size_kb:.1f} KB) exceeds budget ({max_size_kb} KB)!", file=sys.stderr)
        if model_type == "frame_fc":
            print("Consider reducing hidden_dims or window_frames.", file=sys.stderr)
        else:
            print("Consider reducing channels, layers, or context size.", file=sys.stderr)
        sys.exit(1)

    # Export C header
    Path(header_path).parent.mkdir(parents=True, exist_ok=True)
    tflite_to_c_header(tflite_bytes, c_array_name, header_path)
    print(f"C header: {header_path}")

    # Also save .tflite next to the header for verification/re-export
    tflite_copy = Path(header_path).parent / f"{c_array_name}.tflite"
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
