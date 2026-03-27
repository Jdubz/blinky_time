"""Frame-level Conv1D with temporal pooling for long-context detection.

Designed for long-context windows (192 frames = 3.07s) where plain FC
flattening destroys temporal locality. Conv1D preserves local patterns,
AvgPool1D or strided conv progressively compress the time dimension.

Architecture:
  Conv1D layers with stride or AvgPool1D -> per-pooled-timestep output
  Example: 192 frames -> stride(4) -> 48 -> stride(4) -> 12 output timesteps

Output time dimension is input_time // product(pool_sizes).
Labels must be downsampled to match (handled by train.py).

Strided conv mode (pool_sizes used as stride): replaces separate AvgPool ops
with strided convolutions. Fewer TFLite ops = less dispatch overhead on MCU.

TFLite ops: Conv2D (Conv1D mapped), Pad, [AveragePool2D if not strided],
            Logistic, Quantize, Dequantize
"""

import torch
import torch.nn as nn


class FrameOnsetConv1DPool(nn.Module):
    """Causal Conv1D with temporal pooling for long-context onset detection.

    Input:  (batch, time, n_mels)
    Output: (batch, time // pool_factor, out_channels)

    If use_stride=True, pool_sizes are applied as conv strides instead of
    separate AvgPool ops. This reduces TFLite op count and dispatch overhead.
    """

    def __init__(self, n_mels: int = 26,
                 channels: list[int] = [32, 48, 32],
                 kernel_sizes: list[int] = [5, 5, 3],
                 pool_sizes: list[int] = [4, 4, 1],
                 dropout: float = 0.1,
                 use_stride: bool = False):
        super().__init__()
        assert len(channels) == len(kernel_sizes) == len(pool_sizes), \
            "channels, kernel_sizes, and pool_sizes must have same length"

        self.out_channels = 1
        self.n_mels = n_mels
        self.channels = channels
        self.kernel_sizes = kernel_sizes
        self.pool_sizes = pool_sizes
        self.use_stride = use_stride

        # Total temporal downsampling factor
        self.pool_factor = 1
        for p in pool_sizes:
            self.pool_factor *= p

        layers = []
        in_ch = n_mels
        for i, (ch, k, pool) in enumerate(zip(channels, kernel_sizes, pool_sizes)):
            stride = pool if use_stride else 1
            pad = k - 1  # causal: pad left only (same for strided and non-strided)
            layers.append(nn.ConstantPad1d((pad, 0), 0.0))
            layers.append(nn.Conv1d(in_ch, ch, k, stride=stride, padding=0, bias=True))
            layers.append(nn.ReLU())
            if dropout > 0:
                layers.append(nn.Dropout(dropout))
            if not use_stride and pool > 1:
                layers.append(nn.AvgPool1d(pool, stride=pool))
            in_ch = ch

        self.backbone = nn.Sequential(*layers)
        self.output_conv = nn.Conv1d(in_ch, self.out_channels, 1, bias=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Forward pass.

        Args:
            x: (batch, time, n_mels)
        Returns:
            (batch, time // pool_factor, out_channels) with sigmoid
        """
        x = x.permute(0, 2, 1)  # (batch, n_mels, time)
        x = self.backbone(x)
        x = self.output_conv(x)
        x = torch.sigmoid(x)
        return x.permute(0, 2, 1)  # (batch, time_pooled, channels)


def build_onset_conv1d_pool(n_mels: int = 26,
                           channels: list[int] = [32, 48, 32],
                           kernel_sizes: list[int] = [5, 5, 3],
                           pool_sizes: list[int] = [4, 4, 1],
                           dropout: float = 0.1,
                           use_stride: bool = False) -> nn.Module:
    """Build a Conv1D+Pool onset model."""
    return FrameOnsetConv1DPool(
        n_mels=n_mels, channels=channels, kernel_sizes=kernel_sizes,
        pool_sizes=pool_sizes, dropout=dropout,
        use_stride=use_stride)
