"""Causal 1D CNN for beat activation detection.

Designed to run on XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro + CMSIS-NN.

Architecture: 3 dilated causal conv layers → per-frame sigmoid output.
Receptive field: 21 frames = 336ms at 62.5 Hz (covers one beat at 180 BPM).
Size budget: ~3,500 params = ~14 KB INT8.
"""

import os
os.environ.setdefault("TF_USE_LEGACY_KERAS", "1")

import tf_keras as keras
from tf_keras import layers


def build_beat_cnn(n_mels: int = 26, channels: int = 32, kernel_size: int = 3,
                   dilations: list[int] = [1, 2, 4], dropout: float = 0.1,
                   chunk_frames: int = 128) -> keras.Model:
    """Build a causal 1D CNN for beat activation.

    Args:
        n_mels: Number of mel bands (must match firmware: 26)
        channels: Number of conv filters per layer
        kernel_size: Conv kernel size
        dilations: Dilation rates for each conv layer
        dropout: Dropout rate between layers
        chunk_frames: Input sequence length (for shape info only)

    Returns:
        Keras model: input (batch, time, n_mels) -> output (batch, time, 1)
    """
    inputs = keras.Input(shape=(chunk_frames, n_mels), name="mel_input")
    x = inputs

    for i, dilation in enumerate(dilations):
        # Causal padding: pad left side only
        pad_size = (kernel_size - 1) * dilation
        x = layers.ZeroPadding1D(padding=(pad_size, 0))(x)

        out_channels = channels if i < len(dilations) - 1 else channels
        x = layers.Conv1D(
            out_channels, kernel_size, dilation_rate=dilation,
            padding="valid", use_bias=True,
            name=f"conv{i+1}_d{dilation}",
        )(x)
        x = layers.BatchNormalization(name=f"bn{i+1}")(x)
        x = layers.ReLU(name=f"relu{i+1}")(x)
        if dropout > 0:
            x = layers.Dropout(dropout, name=f"drop{i+1}")(x)

    # Final 1x1 conv to single output channel
    x = layers.Conv1D(1, 1, padding="valid", activation="sigmoid", name="output_conv")(x)

    model = keras.Model(inputs=inputs, outputs=x, name="beat_cnn")
    return model


def model_summary(cfg: dict) -> None:
    """Print model summary and parameter count."""
    model = build_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=cfg["model"]["dilations"],
        dropout=cfg["model"]["dropout"],
        chunk_frames=cfg["training"]["chunk_frames"],
    )
    model.summary()

    total_params = model.count_params()
    int8_size_kb = total_params / 1024  # 1 byte per param in INT8
    print(f"\nINT8 model size estimate: {int8_size_kb:.1f} KB")
    print(f"Max allowed: {cfg['export']['max_model_size_kb']} KB")

    # Compute receptive field
    dilations = cfg["model"]["dilations"]
    kernel_size = cfg["model"]["kernel_size"]
    rf = 1
    for d in dilations:
        rf += (kernel_size - 1) * d
    frame_rate = cfg["audio"]["frame_rate"]
    print(f"Receptive field: {rf} frames = {rf / frame_rate * 1000:.0f} ms")


if __name__ == "__main__":
    import yaml
    with open("configs/default.yaml") as f:
        cfg = yaml.safe_load(f)
    model_summary(cfg)
