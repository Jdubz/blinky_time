"""Causal 1D CNN for beat activation detection.

Designed to run on XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro + CMSIS-NN.

Architecture: 3 dilated causal conv layers (shared backbone) with per-task
output heads. Beat activation is always present; downbeat is optional.
Receptive field depends on dilation config:
  [1,2,4] = 15 frames (240ms), [1,2,4,8,16] = 63 frames (1008ms).
Size budget: ≤50 KB INT8 (nRF52840 has ~700 KB flash free).
"""

import torch
import torch.nn as nn


class BeatCNN(nn.Module):
    """Causal 1D CNN for beat (and optional downbeat) activation.

    Input:  (batch, time, n_mels)
    Output: (batch, time, out_channels)  — 1 = beat only, 2 = beat + downbeat
    """

    def __init__(self, n_mels: int = 26, channels: int = 32, kernel_size: int = 3,
                 dilations: list[int] = [1, 2, 4], dropout: float = 0.1,
                 downbeat: bool = False):
        super().__init__()
        self.out_channels = 2 if downbeat else 1

        layers = []
        in_ch = n_mels
        for i, dilation in enumerate(dilations):
            pad_size = (kernel_size - 1) * dilation  # causal: pad left only
            layers.append(nn.ConstantPad1d((pad_size, 0), 0.0))
            layers.append(nn.Conv1d(in_ch, channels, kernel_size,
                                    dilation=dilation, padding=0, bias=True))
            layers.append(nn.BatchNorm1d(channels))
            layers.append(nn.ReLU())
            if dropout > 0:
                layers.append(nn.Dropout(dropout))
            in_ch = channels

        self.backbone = nn.Sequential(*layers)
        self.output_conv = nn.Conv1d(channels, self.out_channels, 1, bias=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Forward pass.

        Args:
            x: (batch, time, n_mels)
        Returns:
            (batch, time, out_channels) with sigmoid activation
        """
        # Conv1d expects (batch, channels, time)
        x = x.permute(0, 2, 1)
        x = self.backbone(x)
        x = self.output_conv(x)
        x = torch.sigmoid(x)
        # Back to (batch, time, channels)
        return x.permute(0, 2, 1)


def build_beat_cnn(n_mels: int = 26, channels: int = 32, kernel_size: int = 3,
                   dilations: list[int] = [1, 2, 4], dropout: float = 0.1,
                   chunk_frames: int = 128, downbeat: bool = False) -> BeatCNN:
    """Build a causal 1D CNN for beat (and optional downbeat) activation.

    chunk_frames is accepted for API compatibility but not used — PyTorch
    models are dynamic in the time dimension.
    """
    return BeatCNN(n_mels=n_mels, channels=channels, kernel_size=kernel_size,
                   dilations=dilations, dropout=dropout, downbeat=downbeat)


def model_summary(cfg: dict) -> None:
    """Print model summary and parameter count."""
    model = build_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=cfg["model"]["dilations"],
        dropout=cfg["model"]["dropout"],
        chunk_frames=cfg["training"]["chunk_frames"],
        downbeat=cfg["model"].get("downbeat", False),
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(f"BeatCNN: {total_params} params ({trainable} trainable)")

    int8_size_kb = total_params / 1024
    print(f"INT8 model size estimate: {int8_size_kb:.1f} KB")
    print(f"Max allowed: {cfg['export']['max_model_size_kb']} KB")

    dilations = cfg["model"]["dilations"]
    kernel_size = cfg["model"]["kernel_size"]
    rf = 1
    for d in dilations:
        rf += (kernel_size - 1) * d
    frame_rate = cfg["audio"]["frame_rate"]
    print(f"Receptive field: {rf} frames = {rf / frame_rate * 1000:.0f} ms")

    # Print layer details
    print(f"\nArchitecture:")
    for name, p in model.named_parameters():
        print(f"  {name}: {list(p.shape)}")


if __name__ == "__main__":
    from scripts.audio import load_config
    cfg = load_config("configs/default.yaml")
    model_summary(cfg)
