"""Beat-synchronous FC classifier for downbeat detection and CBSS correction.

Designed to run on XIAO nRF52840 Sense (Cortex-M4F @ 64 MHz, 256 KB RAM)
via TFLite Micro + CMSIS-NN.  Inference at beat rate (~2 Hz), not frame rate.

Input: last N beats of spectral summary features (N_beats × features_per_beat).
Output: per-beat classification (downbeat, beat confidence, tempo/phase corrections).

Phased output heads:
  Phase A: downbeat only
  Phase B: + beat confidence
  Phase C: + tempo factor (3-class) + phase offset

Size budget: ≤15 KB INT8 (target ~11 KB).
Inference: <0.5ms per beat on Cortex-M4F.
"""

import torch
import torch.nn as nn
import torch.nn.functional as F


class BeatSyncClassifier(nn.Module):
    """Beat-synchronous FC classifier with multi-head outputs.

    Input:  (batch, n_beats, features_per_beat)
    Output: dict of tensors, keys depend on phase.
    """

    def __init__(self, n_beats: int = 4, features_per_beat: int = 79,
                 hidden1: int = 32, hidden2: int = 16,
                 dropout: float = 0.15, phase: str = 'A'):
        super().__init__()
        self.n_beats = n_beats
        self.features_per_beat = features_per_beat
        self.phase = phase
        input_dim = n_beats * features_per_beat

        self.shared = nn.Sequential(
            nn.Linear(input_dim, hidden1),
            nn.ReLU(),
            nn.Dropout(dropout),
            nn.Linear(hidden1, hidden2),
            nn.ReLU(),
            nn.Dropout(dropout * 0.67),
        )

        # Phase A: downbeat detection (always present)
        self.downbeat_head = nn.Linear(hidden2, 1)

        # Phase B: beat confidence
        self.beat_conf_head = nn.Linear(hidden2, 1) if phase >= 'B' else None

        # Phase C: tempo correction (3-class: half, correct, double)
        #          + phase offset (continuous, [-0.5, 0.5])
        self.tempo_head = nn.Linear(hidden2, 3) if phase >= 'C' else None
        self.phase_head = nn.Linear(hidden2, 1) if phase >= 'C' else None

    def forward(self, x: torch.Tensor) -> dict[str, torch.Tensor]:
        """Forward pass.

        Args:
            x: (batch, n_beats, features_per_beat)
        Returns:
            dict with keys: 'downbeat', and optionally 'beat_confidence',
            'tempo_factor', 'phase_offset' depending on phase.
        """
        x = x.flatten(1)  # (batch, n_beats * features_per_beat)
        h = self.shared(x)

        outputs = {
            'downbeat': torch.sigmoid(self.downbeat_head(h)).squeeze(-1),
        }

        if self.beat_conf_head is not None:
            outputs['beat_confidence'] = torch.sigmoid(
                self.beat_conf_head(h)).squeeze(-1)

        if self.tempo_head is not None:
            outputs['tempo_factor'] = F.softmax(self.tempo_head(h), dim=-1)
            outputs['phase_offset'] = 0.5 * torch.tanh(
                self.phase_head(h)).squeeze(-1)

        return outputs

    @property
    def output_names(self) -> list[str]:
        names = ['downbeat']
        if self.beat_conf_head is not None:
            names.append('beat_confidence')
        if self.tempo_head is not None:
            names.extend(['tempo_factor', 'phase_offset'])
        return names


def build_beat_sync(n_beats: int = 4, features_per_beat: int = 79,
                    hidden1: int = 32, hidden2: int = 16,
                    dropout: float = 0.15, phase: str = 'A') -> nn.Module:
    """Build a beat-synchronous classifier."""
    return BeatSyncClassifier(
        n_beats=n_beats, features_per_beat=features_per_beat,
        hidden1=hidden1, hidden2=hidden2, dropout=dropout, phase=phase)


def model_summary(n_beats: int = 4, features_per_beat: int = 79,
                  hidden1: int = 32, hidden2: int = 16,
                  phase: str = 'A') -> None:
    """Print model summary and parameter count."""
    model = build_beat_sync(n_beats=n_beats, features_per_beat=features_per_beat,
                            hidden1=hidden1, hidden2=hidden2, phase=phase)
    total_params = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    print(f"BeatSyncClassifier (phase={phase}): {total_params} params ({trainable} trainable)")
    print(f"  Input: {n_beats} beats × {features_per_beat} features = {n_beats * features_per_beat}")
    print(f"  Hidden: [{hidden1}, {hidden2}]")
    print(f"  Outputs: {model.output_names}")

    int8_size_kb = total_params / 1024
    print(f"  INT8 model size estimate: {int8_size_kb:.1f} KB")

    # MACs estimate (FC layers)
    input_dim = n_beats * features_per_beat
    macs = input_dim * hidden1 + hidden1 * hidden2 + hidden2 * 1
    if phase >= 'B':
        macs += hidden2 * 1
    if phase >= 'C':
        macs += hidden2 * 3 + hidden2 * 1
    print(f"  MACs per inference: {macs:,}")

    # CMSIS-NN estimate: ~2 MACs/cycle with INT8 SIMD on Cortex-M4
    cycles_per_mac = 0.5  # optimistic with SIMD
    freq_mhz = 64
    est_us = macs * cycles_per_mac / freq_mhz
    print(f"  Estimated inference: ~{est_us:.0f} µs on Cortex-M4F @ {freq_mhz} MHz")

    print(f"\nArchitecture:")
    for name, p in model.named_parameters():
        print(f"  {name}: {list(p.shape)}")


if __name__ == "__main__":
    import sys
    phase = sys.argv[1] if len(sys.argv) > 1 else 'A'
    n_beats = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    model_summary(n_beats=n_beats, phase=phase)
