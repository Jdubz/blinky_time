"""Causal 1D CNN for beat activation detection.

Designed to run on XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro + CMSIS-NN.

Two architecture variants:
  BeatCNN: Standard dilated causal conv layers (v1-v8)
  DSTCNBeatCNN: Depthwise separable TCN with residual connections (v9+)

Receptive field depends on dilation config:
  [1,2,4] = 15 frames (240ms), [1,2,4,8,16] = 63 frames (1008ms).
Size budget: ≤50 KB INT8 (nRF52840 has ~700 KB flash free).
"""

import sys
from pathlib import Path

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


class DSConvBlock(nn.Module):
    """Depthwise separable conv block with optional residual connection.

    Each block: CausalPad → DepthwiseConv1D(k, dilation) → BN → ReLU →
                PointwiseConv1D(1×1) → BN → ReLU → [Dropout]
                + residual skip connection (if in_ch == out_ch)

    On Cortex-M4F, both DEPTHWISE_CONV_2D and CONV_2D have CMSIS-NN SIMD
    optimization (arm_depthwise_conv_wrapper_s8 / arm_convolve_wrapper_s8).

    MACs per timestep (k=3, ch=24):
      Depthwise:  k * ch     = 3 * 24  = 72
      Pointwise:  ch * ch    = 24 * 24 = 576
      Total:      648  (vs 1728 for standard Conv1D → 2.7x fewer)
    """

    def __init__(self, in_ch: int, out_ch: int, kernel_size: int,
                 dilation: int, dropout: float = 0.1, residual: bool = True):
        super().__init__()
        self.use_residual = residual and (in_ch == out_ch)
        pad_size = (kernel_size - 1) * dilation

        self.pad = nn.ConstantPad1d((pad_size, 0), 0.0)
        # Depthwise: each input channel convolved independently (groups=in_ch)
        self.dw_conv = nn.Conv1d(in_ch, in_ch, kernel_size, dilation=dilation,
                                 groups=in_ch, padding=0, bias=False)
        self.dw_bn = nn.BatchNorm1d(in_ch)
        self.dw_relu = nn.ReLU()
        # Pointwise: 1×1 conv mixes channels
        # TODO(next training run): change to bias=False — redundant before BN
        self.pw_conv = nn.Conv1d(in_ch, out_ch, 1, bias=True)
        self.pw_bn = nn.BatchNorm1d(out_ch)
        self.pw_relu = nn.ReLU()
        self.dropout = nn.Dropout(dropout) if dropout > 0 else nn.Identity()

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        residual = x
        x = self.pad(x)
        x = self.dw_conv(x)
        x = self.dw_bn(x)
        x = self.dw_relu(x)
        x = self.pw_conv(x)
        x = self.pw_bn(x)
        x = self.pw_relu(x)
        x = self.dropout(x)
        if self.use_residual:
            x = x + residual
        return x


class DSTCNBeatCNN(nn.Module):
    """Depthwise Separable Temporal Convolutional Network for beat activation.

    Replaces standard Conv1D layers with depthwise separable convolutions for
    ~2.7x fewer MACs. Optional residual skip connections improve gradient flow.

    TFLite mapping:
      DepthwiseConv1D → DEPTHWISE_CONV_2D (CMSIS-NN: arm_depthwise_conv_wrapper_s8)
      PointwiseConv1D → CONV_2D with k=1 (CMSIS-NN: arm_convolve_1x1_s8_fast)
      Residual ADD    → ADD (CMSIS-NN: arm_elementwise_add_s8)

    Input:  (batch, time, n_mels)
    Output: (batch, time, out_channels)
    """

    def __init__(self, n_mels: int = 26, channels: int = 24, kernel_size: int = 3,
                 dilations: list[int] = [1, 2, 4, 8, 16], dropout: float = 0.1,
                 downbeat: bool = False, residual: bool = True):
        super().__init__()
        self.out_channels = 2 if downbeat else 1
        if dilations[0] != 1:
            raise ValueError(f"First dilation must be 1 (got {dilations[0]})")

        # Input projection: standard conv to go from n_mels → channels
        # (can't use depthwise here since in_ch != out_ch)
        self.input_pad = nn.ConstantPad1d(((kernel_size - 1) * dilations[0], 0), 0.0)
        # TODO(next training run): change to bias=False — redundant before BN
        self.input_conv = nn.Conv1d(n_mels, channels, kernel_size,
                                    dilation=dilations[0], padding=0, bias=True)
        self.input_bn = nn.BatchNorm1d(channels)
        self.input_relu = nn.ReLU()
        self.input_dropout = nn.Dropout(dropout) if dropout > 0 else nn.Identity()

        # DS-TCN blocks for remaining dilations
        self.blocks = nn.ModuleList()
        for dilation in dilations[1:]:
            self.blocks.append(DSConvBlock(
                channels, channels, kernel_size, dilation,
                dropout=dropout, residual=residual))

        self.output_conv = nn.Conv1d(channels, self.out_channels, 1, bias=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # Conv1d expects (batch, channels, time)
        x = x.permute(0, 2, 1)

        # Input projection
        x = self.input_pad(x)
        x = self.input_conv(x)
        x = self.input_bn(x)
        x = self.input_relu(x)
        x = self.input_dropout(x)

        # DS-TCN blocks
        for block in self.blocks:
            x = block(x)

        x = self.output_conv(x)
        x = torch.sigmoid(x)
        return x.permute(0, 2, 1)


def build_beat_cnn(n_mels: int = 26, channels: int = 32, kernel_size: int = 3,
                   dilations: list[int] = [1, 2, 4], dropout: float = 0.1,
                   chunk_frames: int = 128, downbeat: bool = False,
                   model_type: str = "causal_cnn",
                   residual: bool = False) -> nn.Module:
    """Build a beat activation model.

    Args:
        model_type: "causal_cnn" (v1-v8) or "ds_tcn" (v9+, depthwise separable)
        residual: Enable residual skip connections (ds_tcn only)

    chunk_frames is accepted for API compatibility but not used — PyTorch
    models are dynamic in the time dimension.
    """
    if model_type == "ds_tcn":
        return DSTCNBeatCNN(n_mels=n_mels, channels=channels, kernel_size=kernel_size,
                            dilations=dilations, dropout=dropout, downbeat=downbeat,
                            residual=residual)
    return BeatCNN(n_mels=n_mels, channels=channels, kernel_size=kernel_size,
                   dilations=dilations, dropout=dropout, downbeat=downbeat)


def model_summary(cfg: dict) -> None:
    """Print model summary and parameter count."""
    model_type = cfg["model"].get("type", "causal_cnn")
    model = build_beat_cnn(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_size=cfg["model"]["kernel_size"],
        dilations=cfg["model"]["dilations"],
        dropout=cfg["model"]["dropout"],
        chunk_frames=cfg["training"]["chunk_frames"],
        downbeat=cfg["model"].get("downbeat", False),
        model_type=model_type,
        residual=cfg["model"].get("residual", False),
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(f"{model.__class__.__name__}: {total_params} params ({trainable} trainable)")

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

    # MACs estimate
    channels = cfg["model"]["channels"]
    n_mels = cfg["audio"]["n_mels"]
    if model_type == "ds_tcn":
        # Input conv: standard
        macs = n_mels * channels * kernel_size
        # DS blocks: depthwise + pointwise
        for d in dilations[1:]:
            macs += channels * kernel_size + channels * channels
        print(f"MACs/timestep: {macs:,} (DS-TCN)")
    else:
        macs = n_mels * channels * kernel_size
        for d in dilations[1:]:
            macs += channels * channels * kernel_size
        print(f"MACs/timestep: {macs:,} (standard conv)")

    # Print layer details
    print(f"\nArchitecture ({model_type}):")
    for name, p in model.named_parameters():
        print(f"  {name}: {list(p.shape)}")


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from scripts.audio import load_config
    import sys as _sys
    config = _sys.argv[1] if len(_sys.argv) > 1 else "configs/default.yaml"
    cfg = load_config(config)
    model_summary(cfg)
