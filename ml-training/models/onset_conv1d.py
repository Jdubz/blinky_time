"""Frame-level 1D Conv model for onset activation.

Designed for XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro. Target inference: 3-8ms per frame for 32-frame window.

Architecture: causal Conv1D layers (no dilation, no BatchNorm)
  Input: (batch, time, n_mels) → per-frame output: (batch, time, out_channels)

No dilated convolutions — avoids SpaceToBatch/BatchToSpace TFLite overhead
that made the old CNN 79-98ms (SpaceToBatch alone was ~20ms).
No BatchNorm — eliminates BN fusion complexity in TFLite export.
Wider kernels (k=5,7) compensate for lack of dilation.

Receptive field with [k=5, k=5, k=3]: 11 frames (176ms at 62.5 Hz)

TFLite ops: Conv2D (Conv1D mapped), Pad, Reshape, Logistic, Quantize, Dequantize

Training: processes full 128-frame chunks with causal padding,
producing per-frame predictions — same interface as OnsetCNN/FrameOnsetFC.

Firmware: processes N-frame sliding window, takes last output timestep.
"""

import sys
from pathlib import Path

import torch
import torch.nn as nn


def _fake_quantize_int8(weight: torch.Tensor) -> torch.Tensor:
    """Simulate INT8 per-tensor quantization (round-trip through int8 range)."""
    scale = weight.detach().abs().max() / 127.0
    if scale < 1e-10:
        return weight
    return (torch.round(weight / scale).clamp(-128, 127)) * scale


class _QuantNoiseConv1d(nn.Module):
    """Wrapper that applies stochastic quantization noise to Conv1d weights.

    During training, a random fraction of weights are replaced with their
    INT8-quantized values. Gradients flow through via straight-through
    estimator (torch.where is differentiable). At eval, behaves identically
    to the wrapped Conv1d.

    Fan et al. 2020: "Training with Quantization Noise for Extreme Model Compression"
    """

    def __init__(self, conv: nn.Conv1d, ratio: float = 0.1):
        super().__init__()
        self.conv = conv
        self.ratio = ratio

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if self.training and self.ratio > 0:
            w = self.conv.weight
            mask = (torch.rand_like(w) < self.ratio).float()
            w_quant = _fake_quantize_int8(w)
            # STE: use quantized weights for masked subset, original for rest
            w_noisy = w + (w_quant - w).detach() * mask
            return nn.functional.conv1d(
                x, w_noisy, self.conv.bias,
                stride=self.conv.stride, padding=self.conv.padding,
                dilation=self.conv.dilation, groups=self.conv.groups)
        return self.conv(x)


def apply_quant_noise(model: nn.Module, ratio: float = 0.1):
    """Wrap all Conv1d layers with stochastic quantization noise.

    Args:
        model: the model to apply quant-noise to
        ratio: fraction of weights to quantize per forward pass (0.1 = 10%)

    Raises:
        ValueError: if ratio is not in [0, 1]
    """
    if not 0.0 <= ratio <= 1.0:
        raise ValueError(f"quant_noise ratio must be in [0, 1], got {ratio}")
    # Wrap Conv1d inside Sequential containers (backbone layers)
    for name, module in model.named_modules():
        if isinstance(module, nn.Sequential):
            for i, layer in enumerate(module):
                if isinstance(layer, nn.Conv1d):
                    module[i] = _QuantNoiseConv1d(layer, ratio)
    # Wrap standalone Conv1d attributes (e.g., output_conv)
    for name, attr in list(model.named_children()):
        if isinstance(attr, nn.Conv1d):
            setattr(model, name, _QuantNoiseConv1d(attr, ratio))


def unwrap_quant_noise_state_dict(state_dict: dict) -> dict:
    """Strip QuantNoise wrapper prefixes from state dict keys.

    QuantNoise wraps Conv1d in _QuantNoiseConv1d, adding '.conv.' to keys.
    This restores original key names for export compatibility.
    """
    cleaned = {}
    for key, value in state_dict.items():
        if key.endswith('.conv.weight'):
            cleaned_key = key[:-len('.conv.weight')] + '.weight'
        elif key.endswith('.conv.bias'):
            cleaned_key = key[:-len('.conv.bias')] + '.bias'
        else:
            cleaned_key = key
        cleaned[cleaned_key] = value
    return cleaned


class FrameOnsetConv1D(nn.Module):
    """Causal 1D Conv for onset activation.

    No BatchNorm, no dilation. Variable channel widths and kernel sizes
    per layer for flexible architecture search.

    Input:  (batch, time, n_mels)
    Output: (batch, time, out_channels)  — 1 for single onset, or N for instrument models
            During training with tempo head: tuple of (predictions, tempo_logits)
    """

    def __init__(self, n_mels: int = 26,
                 channels: list[int] = [32, 48, 32],
                 kernel_sizes: list[int] = [5, 5, 3],
                 dropout: float = 0.1,
                 num_tempo_bins: int = 0,
                 freq_pos_encoding: bool = False,
                 num_output_channels: int = 0):
        super().__init__()
        assert len(channels) == len(kernel_sizes), \
            f"channels ({len(channels)}) and kernel_sizes ({len(kernel_sizes)}) must match"

        # num_output_channels > 0 for instrument-aware models (e.g., 3 channels: kick/snare/hihat).
        self.out_channels = num_output_channels if num_output_channels > 0 else 1
        self.n_mels = n_mels
        self.channels = channels
        self.kernel_sizes = kernel_sizes
        self.num_tempo_bins = num_tempo_bins
        self.freq_pos_encoding = freq_pos_encoding

        # Frequency positional encoding (FAC, ICASSP 2024)
        # Learnable per-band vector helps discriminate kicks (low) from hi-hats (high)
        if freq_pos_encoding:
            self.freq_pos = nn.Parameter(torch.zeros(1, 1, n_mels))

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

        # Tempo auxiliary head: training-only, regularizes hidden features
        # toward tempo-relevant patterns (Bock et al. 2019: +1-5% F1).
        # Stripped at export — zero inference cost on device.
        if num_tempo_bins > 0:
            self.tempo_head = nn.Linear(channels[-1], num_tempo_bins)

    def forward(self, x: torch.Tensor) -> torch.Tensor | tuple[torch.Tensor, torch.Tensor]:
        """Forward pass with causal convolutions.

        Args:
            x: (batch, time, n_mels)
        Returns:
            (batch, time, out_channels) with sigmoid activation.
            During training with tempo head: (predictions, tempo_logits)
        """
        if self.freq_pos_encoding:
            x = x + self.freq_pos  # Add frequency position encoding

        x = x.permute(0, 2, 1)  # (batch, n_mels, time)
        h = self.backbone(x)     # (batch, last_ch, time)
        logits = self.output_conv(h)  # (batch, out_channels, time)

        out = torch.sigmoid(logits)

        out = out.permute(0, 2, 1)  # (batch, time, channels)

        if self.num_tempo_bins > 0 and self.training:
            h_pooled = h.mean(dim=2)  # (batch, last_ch) — global average pool
            tempo_logits = self.tempo_head(h_pooled)
            return out, tempo_logits

        return out


def build_onset_conv1d(n_mels: int = 26,
                      channels: list[int] = [32, 48, 32],
                      kernel_sizes: list[int] = [5, 5, 3],
                      dropout: float = 0.1,
                      num_tempo_bins: int = 0,
                      freq_pos_encoding: bool = False,
                      num_output_channels: int = 0) -> nn.Module:
    """Build a frame-level Conv1D onset activation model.

    Args:
        n_mels: Number of mel bands (must match firmware, default 26)
        channels: Per-layer channel widths (e.g. [32, 48, 32])
        kernel_sizes: Per-layer kernel sizes (e.g. [5, 5, 3])
        dropout: Dropout rate between layers
        num_tempo_bins: If > 0, add training-only tempo auxiliary head
        freq_pos_encoding: If True, add learnable frequency position vector
        num_output_channels: If > 0, override output channel count (e.g., 3 for kick/snare/hihat)
    """
    return FrameOnsetConv1D(
        n_mels=n_mels, channels=channels, kernel_sizes=kernel_sizes,
        dropout=dropout,
        num_tempo_bins=num_tempo_bins, freq_pos_encoding=freq_pos_encoding,
        num_output_channels=num_output_channels)


def conv1d_model_summary(cfg: dict) -> None:
    """Print Conv1D model summary and parameter count."""
    model = build_onset_conv1d(
        n_mels=cfg["audio"]["n_mels"],
        channels=cfg["model"]["channels"],
        kernel_sizes=cfg["model"]["kernel_sizes"],
        dropout=cfg["model"]["dropout"],
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    out_ch = model.out_channels
    print(f"FrameOnsetConv1D: {total_params} params ({trainable} trainable)")

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
