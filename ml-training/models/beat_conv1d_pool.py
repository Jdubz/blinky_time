"""Frame-level Conv1D with temporal pooling for downbeat/bar detection.

Designed for long-context windows (192 frames = 3.07s) where plain FC
flattening destroys temporal locality. Conv1D preserves local patterns,
AvgPool1D progressively compresses the time dimension.

Architecture:
  Conv1D layers with interleaved AvgPool1D -> per-pooled-timestep output
  Example: 192 frames -> pool(4) -> 48 -> pool(4) -> 12 output timesteps

Output time dimension is input_time // product(pool_sizes).
Labels must be downsampled to match (handled by train.py).

TFLite ops: Conv2D (Conv1D mapped), Pad, AveragePool2D (Pool1D mapped),
            Logistic, Quantize, Dequantize
"""

import torch
import torch.nn as nn


class FrameBeatConv1DPool(nn.Module):
    """Causal Conv1D with temporal pooling for long-context beat/downbeat.

    Input:  (batch, time, n_mels)
    Output: (batch, time // pool_factor, out_channels)
    """

    def __init__(self, n_mels: int = 26,
                 channels: list[int] = [32, 48, 32],
                 kernel_sizes: list[int] = [5, 5, 3],
                 pool_sizes: list[int] = [4, 4, 1],
                 dropout: float = 0.1,
                 downbeat: bool = True):
        super().__init__()
        assert len(channels) == len(kernel_sizes) == len(pool_sizes), \
            "channels, kernel_sizes, and pool_sizes must have same length"

        self.out_channels = 2 if downbeat else 1
        self.n_mels = n_mels
        self.channels = channels
        self.kernel_sizes = kernel_sizes
        self.pool_sizes = pool_sizes

        # Total temporal downsampling factor
        self.pool_factor = 1
        for p in pool_sizes:
            self.pool_factor *= p

        layers = []
        in_ch = n_mels
        for i, (ch, k, pool) in enumerate(zip(channels, kernel_sizes, pool_sizes)):
            pad = k - 1  # causal: pad left only
            layers.append(nn.ConstantPad1d((pad, 0), 0.0))
            layers.append(nn.Conv1d(in_ch, ch, k, stride=1, padding=0, bias=True))
            layers.append(nn.ReLU())
            if dropout > 0:
                layers.append(nn.Dropout(dropout))
            if pool > 1:
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


def build_beat_conv1d_pool(n_mels: int = 26,
                           channels: list[int] = [32, 48, 32],
                           kernel_sizes: list[int] = [5, 5, 3],
                           pool_sizes: list[int] = [4, 4, 1],
                           dropout: float = 0.1,
                           downbeat: bool = True) -> nn.Module:
    """Build a Conv1D+Pool beat/downbeat model."""
    return FrameBeatConv1DPool(
        n_mels=n_mels, channels=channels, kernel_sizes=kernel_sizes,
        pool_sizes=pool_sizes, dropout=dropout, downbeat=downbeat)
