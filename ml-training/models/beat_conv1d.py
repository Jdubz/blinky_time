"""Frame-level 1D Conv model for beat/downbeat activation.

Designed for XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro. Target inference: 3-8ms per frame for 32-frame window.

Architecture: causal Conv1D layers (no dilation, no BatchNorm)
  Input: (batch, time, n_mels) → per-frame output: (batch, time, 2)

No dilated convolutions — avoids SpaceToBatch/BatchToSpace TFLite overhead
that made the old CNN 79-98ms (SpaceToBatch alone was ~20ms).
No BatchNorm — eliminates BN fusion complexity in TFLite export.
Wider kernels (k=5,7) compensate for lack of dilation.

Receptive field with [k=5, k=5, k=3]: 11 frames (176ms at 62.5 Hz)

TFLite ops: Conv2D (Conv1D mapped), Pad, Reshape, Logistic, Quantize, Dequantize

Training: processes full 128-frame chunks with causal padding,
producing per-frame predictions — same interface as BeatCNN/FrameBeatFC.

Firmware: processes N-frame sliding window, takes last output timestep.
"""

import sys
from pathlib import Path

import torch
import torch.nn as nn


class FrameBeatConv1D(nn.Module):
    """Causal 1D Conv for beat (and optional downbeat) activation.

    No BatchNorm, no dilation. Variable channel widths and kernel sizes
    per layer for flexible architecture search.

    Input:  (batch, time, n_mels)
    Output: (batch, time, out_channels)  — 1 = beat only, 2 = beat + downbeat
    """

    def __init__(self, n_mels: int = 26,
                 channels: list[int] = [32, 48, 32],
                 kernel_sizes: list[int] = [5, 5, 3],
                 dropout: float = 0.1,
                 downbeat: bool = False):
        super().__init__()
        assert len(channels) == len(kernel_sizes), \
            f"channels ({len(channels)}) and kernel_sizes ({len(kernel_sizes)}) must match"

        self.out_channels = 2 if downbeat else 1
        self.n_mels = n_mels
        self.channels = channels
        self.kernel_sizes = kernel_sizes

        layers = []
        in_ch = n_mels
        for ch, k in zip(channels, kernel_sizes):
            pad = k - 1  # causal: pad left only
            layers.append(nn.ConstantPad1d((pad, 0), 0.0))
            layers.append(nn.Conv1d(in_ch, ch, k, stride=1, padding=0, bias=True))
            layers.append(nn.ReLU())
            if dropout > 0:
                layers.append(nn.Dropout(dropout))
            in_ch = ch

        self.backbone = nn.Sequential(*layers)
        self.output_conv = nn.Conv1d(in_ch, self.out_channels, 1, bias=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Forward pass with causal convolutions.

        Args:
            x: (batch, time, n_mels)
        Returns:
            (batch, time, out_channels) with sigmoid activation
        """
        x = x.permute(0, 2, 1)  # (batch, n_mels, time)
        x = self.backbone(x)
        x = self.output_conv(x)
        x = torch.sigmoid(x)
        return x.permute(0, 2, 1)  # (batch, time, channels)


def build_beat_conv1d(n_mels: int = 26,
                      channels: list[int] = [32, 48, 32],
                      kernel_sizes: list[int] = [5, 5, 3],
                      dropout: float = 0.1,
                      downbeat: bool = False) -> nn.Module:
    """Build a frame-level Conv1D beat activation model.

    Args:
        n_mels: Number of mel bands (must match firmware, default 26)
        channels: Per-layer channel widths (e.g. [32, 48, 32])
        kernel_sizes: Per-layer kernel sizes (e.g. [5, 5, 3])
        dropout: Dropout rate between layers
        downbeat: If True, output 2 channels (beat + downbeat)
    """
    return FrameBeatConv1D(
        n_mels=n_mels, channels=channels, kernel_sizes=kernel_sizes,
        dropout=dropout, downbeat=downbeat)


def conv1d_model_summary(cfg: dict) -> None:
    """Print Conv1D model summary and parameter count."""
    model = build_beat_conv1d(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_sizes=cfg["model"]["kernel_sizes"],
        dropout=cfg["model"]["dropout"],
        downbeat=cfg["model"].get("downbeat", False),
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    out_ch = model.out_channels
    print(f"FrameBeatConv1D: {total_params} params ({trainable} trainable)")

    int8_size_kb = total_params / 1024
    print(f"INT8 model size estimate: {int8_size_kb:.1f} KB")
    print(f"Max allowed: {cfg['export']['max_model_size_kb']} KB")

    # Receptive field
    kernel_sizes = cfg["model"]["kernel_sizes"]
    rf = 1
    for k in kernel_sizes:
        rf += k - 1
    frame_rate = cfg["audio"]["frame_rate"]
    print(f"Receptive field: {rf} frames = {rf / frame_rate * 1000:.0f} ms")

    # MACs per timestep
    n_mels = cfg["audio"]["n_mels"]
    channels = cfg["model"]["channels"]
    macs = 0
    in_ch = n_mels
    for ch, k in zip(channels, kernel_sizes):
        macs += in_ch * k * ch
        in_ch = ch
    macs += in_ch * out_ch  # output conv
    print(f"MACs/timestep: {macs:,}")

    # Device inference estimate (window_frames × MACs/timestep)
    window_frames = cfg["model"].get("window_frames", 32)
    total_macs = macs * window_frames
    est_ms = total_macs / 72000  # ~72K MACs/ms measured on Cortex-M4F
    print(f"Device inference ({window_frames} frames): ~{total_macs/1000:.0f}K MACs ≈ {est_ms:.1f}ms")

    # RAM estimate
    max_ch = max(channels)
    arena_est_kb = max(4, max_ch * window_frames * 2 // 1024 + 4)
    window_ram_kb = window_frames * n_mels * 4 / 1024
    print(f"Device RAM estimate: ~{arena_est_kb + window_ram_kb:.1f} KB "
          f"(arena ~{arena_est_kb} KB + window {window_ram_kb:.1f} KB)")

    print(f"\nArchitecture (frame_conv1d):")
    for name, p in model.named_parameters():
        print(f"  {name}: {list(p.shape)}")


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from scripts.audio import load_config
    import sys as _sys
    config = _sys.argv[1] if len(_sys.argv) > 1 else "configs/frame_conv1d.yaml"
    cfg = load_config(config)
    conv1d_model_summary(cfg)
