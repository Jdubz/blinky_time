"""Enhanced frame-level FC model with optional architectural improvements.

All improvements are independently toggleable via config:
  1. SE block: channel attention on mel bands (learns to weight kick vs hi-hat)
  2. Conv1D front-end: local temporal feature extraction before FC
  3. Multi-window: dual-path (short onset + long rhythm context)
  4. Tempo auxiliary head: regularizes hidden features (training only, not exported)

Each feature adds minimal parameters and stays within Cortex-M4F budget.
TFLite ops needed beyond baseline FC (FullyConnected, ReLU, Logistic, Q/DQ):
  - SE: Mean, Mul, Reshape (already in resolver except Mean, Mul)
  - Conv1D: Conv2D, Pad, Reshape (already in resolver)
  - Multi-window: StridedSlice, Concatenation
  - Tempo: training only, not exported

Firmware resolver needs: Mean, Mul, StridedSlice, Concatenation (4 new ops).
Expand MicroMutableOpResolver<8> to <12>.
"""

import torch
import torch.nn as nn
import torch.nn.functional as F


class SqueezeExcitation(nn.Module):
    """Channel attention on mel bands.

    Pools over the window/time dimension, then learns per-band importance.
    Encourages the model to weight kick/bass bands higher and suppress
    hi-hat/cymbal bands — matching the design goal of triggering on
    kicks and snares only.

    ~2 * (n_mels * n_mels/ratio) params. With n_mels=26, ratio=4: ~338 params.
    """

    def __init__(self, n_mels: int = 26, ratio: int = 4):
        super().__init__()
        mid = max(1, n_mels // ratio)
        self.fc1 = nn.Linear(n_mels, mid)
        self.fc2 = nn.Linear(mid, n_mels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Apply channel attention.

        Args:
            x: (batch, window_frames, n_mels) — a single window
        Returns:
            (batch, window_frames, n_mels) — scaled by attention weights
        """
        # Pool over time/window dimension → per-band statistics
        s = x.mean(dim=-2)  # (batch, n_mels)
        s = F.relu(self.fc1(s))  # (batch, mid)
        s = torch.sigmoid(self.fc2(s))  # (batch, n_mels)
        return x * s.unsqueeze(-2)  # broadcast over time


class EnhancedBeatFC(nn.Module):
    """Enhanced frame-level FC with optional SE, Conv1D, multi-window, and tempo head.

    Input:  (batch, time, n_mels)
    Output: (batch, time, out_channels) beat/downbeat activations
            Optionally also returns tempo logits for auxiliary loss.

    All enhancements are optional and independently configured.
    """

    def __init__(self, n_mels: int = 26, window_frames: int = 32,
                 hidden_dims: list[int] | None = None, dropout: float = 0.1,
                 downbeat: bool = False,
                 # SE block
                 se_ratio: int = 0,  # 0 = disabled, 4 = squeeze 26→6→26
                 # Conv1D front-end
                 conv_channels: int = 0,  # 0 = disabled
                 conv_kernel: int = 5,
                 # Multi-window
                 short_window: int = 0,  # 0 = disabled, e.g. 16
                 short_hidden: int = 0,  # hidden dim for short path
                 # Tempo auxiliary head
                 num_tempo_bins: int = 0,  # 0 = disabled, e.g. 20
                 ):
        super().__init__()
        self.n_mels = n_mels
        self.window_frames = window_frames
        self.out_channels = 2 if downbeat else 1
        self.short_window = short_window
        self.num_tempo_bins = num_tempo_bins
        self.use_se = se_ratio > 0
        self.use_conv = conv_channels > 0
        self.use_multiwindow = short_window > 0

        if hidden_dims is None:
            hidden_dims = [64, 32]

        # --- Conv1D front-end (optional) ---
        # Extracts local temporal features before windowing.
        # No BatchNorm (avoids TFLite fusion complexity).
        self.conv_channels = conv_channels
        if self.use_conv:
            self.conv_pad = conv_kernel - 1  # causal padding
            self.conv1d = nn.Conv1d(n_mels, conv_channels, conv_kernel, bias=True)
            self.conv_relu = nn.ReLU()
            feature_dim = conv_channels
        else:
            feature_dim = n_mels

        # --- SE block (optional) ---
        # Applied AFTER Conv1D and window unfolding, on each window independently.
        # This matches TFLite inference where SE operates on the single input window.
        # SE dimension matches the feature dim (conv_channels or n_mels).
        self.se = SqueezeExcitation(feature_dim, se_ratio) if self.use_se else None

        # --- FC paths ---
        # Long path: full window
        long_input_dim = window_frames * feature_dim
        long_layers = []
        in_dim = long_input_dim
        for h in hidden_dims:
            long_layers.append(nn.Linear(in_dim, h))
            long_layers.append(nn.ReLU())
            if dropout > 0:
                long_layers.append(nn.Dropout(dropout))
            in_dim = h
        self.long_backbone = nn.Sequential(*long_layers)
        self.long_hidden_dim = in_dim

        # Short path (optional): shorter window for onset sharpness
        self.short_hidden_dim = 0
        if self.use_multiwindow:
            assert short_window < window_frames, \
                f"short_window ({short_window}) must be < window_frames ({window_frames})"
            short_hidden = short_hidden or hidden_dims[-1]
            short_input_dim = short_window * feature_dim
            self.short_backbone = nn.Sequential(
                nn.Linear(short_input_dim, short_hidden),
                nn.ReLU(),
                nn.Dropout(dropout) if dropout > 0 else nn.Identity(),
            )
            self.short_hidden_dim = short_hidden

        # --- Output head ---
        merge_dim = self.long_hidden_dim + self.short_hidden_dim
        self.output_head = nn.Linear(merge_dim, self.out_channels)

        # --- Tempo auxiliary head (optional, training only) ---
        if self.num_tempo_bins > 0:
            self.tempo_head = nn.Linear(merge_dim, num_tempo_bins)

    def forward(self, x: torch.Tensor) -> torch.Tensor | tuple[torch.Tensor, torch.Tensor]:
        """Forward pass with optional enhancements.

        Processing order matches TFLite inference exactly:
          1. Conv1D on full sequence (TFLite: on single window)
          2. Unfold windows from Conv1D output
          3. SE on each unfolded window (TFLite: on single window)
          4. Flatten → FC

        For multi-window: both paths share the same SE-processed window,
        with the short path using the last short_window frames.

        Args:
            x: (batch, time, n_mels)
        Returns:
            If num_tempo_bins == 0: (batch, time, out_channels) activations
            If num_tempo_bins > 0: ((batch, time, out_channels), (batch, num_tempo_bins))
        """
        batch, time, mels = x.shape

        # --- 1. Conv1D front-end: extract local temporal features ---
        if self.use_conv:
            # (batch, time, mels) → (batch, mels, time) for Conv1d
            xc = x.permute(0, 2, 1)
            xc = F.pad(xc, (self.conv_pad, 0))  # causal pad
            xc = self.conv_relu(self.conv1d(xc))
            x = xc.permute(0, 2, 1)  # (batch, time, conv_channels)

        feature_dim = x.shape[-1]  # n_mels or conv_channels

        # --- 2. Unfold long windows ---
        x_padded = F.pad(x, (0, 0, self.window_frames - 1, 0))
        windows = x_padded.unfold(1, self.window_frames, 1)
        # (batch, time, feature_dim, window_frames) → (batch, time, window_frames, feature_dim)
        windows = windows.permute(0, 1, 3, 2)

        # --- 3. SE block: channel attention per window ---
        # Applied after unfolding so each window is processed independently,
        # matching TFLite where SE operates on the single input window.
        if self.use_se:
            bt = batch * time
            se_in = windows.reshape(bt, self.window_frames, feature_dim)
            se_out = self.se(se_in)  # (bt, window_frames, feature_dim)
            windows = se_out.reshape(batch, time, self.window_frames, feature_dim)

        # --- 4. Long window FC path ---
        flat_long = windows.reshape(batch, time, -1)
        h = self.long_backbone(flat_long)  # (batch, time, long_hidden)

        # --- 5. Short window path (optional) ---
        # Extracts the last short_window frames from the SAME SE-processed
        # long window. This matches TFLite where Cropping1D takes from the
        # SE+Conv1D output tensor.
        if self.use_multiwindow:
            short_windows = windows[:, :, -self.short_window:, :]
            flat_short = short_windows.reshape(batch, time, -1)
            h_short = self.short_backbone(flat_short)  # (batch, time, short_hidden)
            h = torch.cat([h, h_short], dim=-1)  # (batch, time, long+short)

        # --- Output ---
        out = torch.sigmoid(self.output_head(h))

        # --- Tempo auxiliary (training only) ---
        if self.num_tempo_bins > 0 and self.training:
            # Pool hidden features over time → global tempo prediction
            h_pooled = h.mean(dim=1)  # (batch, merge_dim)
            tempo_logits = self.tempo_head(h_pooled)  # (batch, num_tempo_bins)
            return out, tempo_logits

        return out


def build_beat_fc_enhanced(n_mels: int = 26, window_frames: int = 32,
                            hidden_dims: list[int] | None = None,
                            dropout: float = 0.1, downbeat: bool = False,
                            se_ratio: int = 0, conv_channels: int = 0,
                            conv_kernel: int = 5, short_window: int = 0,
                            short_hidden: int = 0,
                            num_tempo_bins: int = 0) -> nn.Module:
    """Build an enhanced frame-level FC beat activation model."""
    return EnhancedBeatFC(
        n_mels=n_mels, window_frames=window_frames,
        hidden_dims=hidden_dims, dropout=dropout, downbeat=downbeat,
        se_ratio=se_ratio, conv_channels=conv_channels,
        conv_kernel=conv_kernel, short_window=short_window,
        short_hidden=short_hidden, num_tempo_bins=num_tempo_bins,
    )


def enhanced_model_summary(cfg: dict) -> None:
    """Print enhanced model summary and parameter count."""
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

    model = build_beat_fc_enhanced(
        n_mels=cfg["audio"]["n_mels"],
        window_frames=cfg["model"]["window_frames"],
        hidden_dims=cfg["model"]["hidden_dims"],
        dropout=cfg["model"]["dropout"],
        downbeat=cfg["model"].get("downbeat", False),
        se_ratio=cfg["model"].get("se_ratio", 0),
        conv_channels=cfg["model"].get("conv_channels", 0),
        conv_kernel=cfg["model"].get("conv_kernel", 5),
        short_window=cfg["model"].get("short_window", 0),
        short_hidden=cfg["model"].get("short_hidden", 0),
        num_tempo_bins=cfg["model"].get("num_tempo_bins", 0),
    )
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)

    features = []
    if model.use_se:
        features.append(f"SE(ratio={cfg['model'].get('se_ratio', 0)})")
    if model.use_conv:
        features.append(f"Conv1D({cfg['model'].get('conv_channels', 0)}ch, k={cfg['model'].get('conv_kernel', 5)})")
    if model.use_multiwindow:
        features.append(f"MultiWindow(short={cfg['model'].get('short_window', 0)})")
    if model.num_tempo_bins > 0:
        features.append(f"Tempo({model.num_tempo_bins} bins)")

    print(f"EnhancedBeatFC: {total_params:,} params ({trainable:,} trainable)")
    print(f"  Features: {', '.join(features) if features else 'none (baseline FC)'}")

    int8_size_kb = total_params / 1024
    print(f"  INT8 estimate: {int8_size_kb:.1f} KB")
    max_kb = cfg["export"]["max_model_size_kb"]
    print(f"  Budget: {max_kb} KB {'OK' if int8_size_kb <= max_kb else 'OVER BUDGET'}")

    wf = cfg["model"]["window_frames"]
    fr = cfg["audio"]["frame_rate"]
    print(f"  Long window: {wf} frames ({wf / fr * 1000:.0f} ms)")
    if model.use_multiwindow:
        sw = cfg["model"]["short_window"]
        print(f"  Short window: {sw} frames ({sw / fr * 1000:.0f} ms)")

    print(f"\n  Architecture:")
    for name, p in model.named_parameters():
        print(f"    {name}: {list(p.shape)}")

    # Test forward pass
    x = torch.randn(2, 128, cfg["audio"]["n_mels"])
    model.eval()
    with torch.no_grad():
        out = model(x)
    if isinstance(out, tuple):
        print(f"\n  Output: activations {out[0].shape}, tempo {out[1].shape}")
    else:
        print(f"\n  Output: {out.shape}")


if __name__ == "__main__":
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from scripts.audio import load_config
    config = sys.argv[1] if len(sys.argv) > 1 else "configs/frame_fc_enhanced.yaml"
    cfg = load_config(config)
    enhanced_model_summary(cfg)
