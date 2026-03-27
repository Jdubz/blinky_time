"""Frame-level FC model for onset activation.

Designed for XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro. Runs every spectral frame (~62.5 Hz), ~60-200us
per inference.

Architecture: sliding window of N raw mel frames x 26 bands
  -> flatten -> FC hidden layers -> onset_activation

TFLite ops needed: FullyConnected, ReLU, Logistic, Quantize, Dequantize
(all already in FrameOnsetNN.h resolver — no Reshape needed since firmware
 feeds flat window buffer directly).

Training: unfolds 128-frame chunks into overlapping causal windows,
producing per-frame predictions (batch, time, out_channels) — same
interface as OnsetCNN, so loss functions and evaluation work unchanged.

Firmware: single flat window input -> single prediction.
"""

import sys
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F


class FrameOnsetFC(nn.Module):
    """Frame-level FC for onset activation.

    Input:  (batch, time, n_mels)
    Output: (batch, time, 1)

    Maintains a causal sliding window of `window_frames` mel frames.
    During training, all timesteps are processed in parallel via unfolding.
    Data layout matches firmware windowBuffer_: frame-major order
    [frame0_mel0, frame0_mel1, ..., frame0_mel25, frame1_mel0, ...].
    """

    def __init__(self, n_mels: int = 26, window_frames: int = 32,
                 hidden_dims: list[int] | None = None, dropout: float = 0.1):
        super().__init__()
        self.n_mels = n_mels
        self.window_frames = window_frames
        self.out_channels = 1

        if hidden_dims is None:
            hidden_dims = [64, 32]

        input_dim = window_frames * n_mels
        layers = []
        in_dim = input_dim
        for h in hidden_dims:
            layers.append(nn.Linear(in_dim, h))
            layers.append(nn.ReLU())
            if dropout > 0:
                layers.append(nn.Dropout(dropout))
            in_dim = h
        self.backbone = nn.Sequential(*layers)
        self.output_head = nn.Linear(in_dim, self.out_channels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Forward pass with causal sliding window.

        Args:
            x: (batch, time, n_mels)
        Returns:
            (batch, time, out_channels) with sigmoid activation
        """
        batch, time, mels = x.shape

        # Causal padding: prepend window_frames-1 zero frames so that
        # output[t] only depends on input[0..t] (no future information)
        x_padded = F.pad(x, (0, 0, self.window_frames - 1, 0))

        # Extract all windows: (batch, time, mels, window_frames)
        windows = x_padded.unfold(1, self.window_frames, 1)

        # Reorder to frame-major (batch, time, window_frames, mels) then flatten
        # This matches firmware windowBuffer_ layout for weight compatibility
        flat = windows.permute(0, 1, 3, 2).reshape(batch, time, -1)

        # FC layers (nn.Linear operates on last dim, broadcasts over batch+time)
        h = self.backbone(flat)
        out = self.output_head(h)
        return torch.sigmoid(out)


def build_onset_fc(n_mels: int = 26, window_frames: int = 32,
                  hidden_dims: list[int] | None = None, dropout: float = 0.1) -> nn.Module:
    """Build a frame-level FC onset activation model.

    Args:
        n_mels: Number of mel bands (must match firmware, default 26)
        window_frames: Sliding window size in frames (e.g. 32 = 512ms at 62.5 Hz)
        hidden_dims: List of hidden layer sizes (e.g. [64, 32])
        dropout: Dropout rate between hidden layers
    """
    return FrameOnsetFC(n_mels=n_mels, window_frames=window_frames,
                       hidden_dims=hidden_dims, dropout=dropout)


def fc_model_summary(cfg: dict) -> None:
    """Print FC model summary and parameter count."""
    model = build_onset_fc(
        n_mels=cfg["audio"]["n_mels"],
        window_frames=cfg["model"]["window_frames"],
        hidden_dims=cfg["model"]["hidden_dims"],
        dropout=cfg["model"]["dropout"],
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    out_ch = 1
    print(f"FrameOnsetFC: {total_params} params ({trainable} trainable)")

    int8_size_kb = total_params / 1024
    print(f"INT8 model size estimate: {int8_size_kb:.1f} KB")
    print(f"Max allowed: {cfg['export']['max_model_size_kb']} KB")

    window_frames = cfg["model"]["window_frames"]
    n_mels = cfg["audio"]["n_mels"]
    frame_rate = cfg["audio"]["frame_rate"]
    window_ms = window_frames / frame_rate * 1000
    print(f"Window: {window_frames} frames = {window_ms:.0f} ms")

    # MACs per inference (one window)
    input_dim = window_frames * n_mels
    hidden_dims = cfg["model"]["hidden_dims"]
    macs = 0
    in_d = input_dim
    for h in hidden_dims:
        macs += in_d * h
        in_d = h
    macs += in_d * out_ch
    print(f"MACs/inference: {macs:,}")

    # Device RAM estimate
    arena_est_kb = 2  # FC models need minimal arena (~1-2 KB)
    window_ram_kb = window_frames * n_mels * 4 / 1024  # float32 sliding window
    print(f"Device RAM estimate: ~{arena_est_kb + window_ram_kb:.1f} KB "
          f"(arena ~{arena_est_kb} KB + window {window_ram_kb:.1f} KB)")

    print(f"\nArchitecture (frame_fc):")
    for name, p in model.named_parameters():
        print(f"  {name}: {list(p.shape)}")


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from scripts.audio import load_config
    import sys as _sys
    config = _sys.argv[1] if len(_sys.argv) > 1 else "configs/frame_fc.yaml"
    cfg = load_config(config)
    fc_model_summary(cfg)
