"""Python mirror of firmware pulse-detection logic (AudioTracker peak-picking).

**Parity disclaimer.** This is a pure-Python reimplementation of the
decision logic in `blinky-things/audio/AudioTracker.cpp
updatePulseDetection()`. It does NOT link the firmware C++ — any
future change to peak-picking on-device requires matching changes
here, or TP-vs-FP analysis will drift from device behavior. The
preferred approach is a C++ refactor that links the same code the
device runs (planned as harness v2 task "port peak-picking to harness",
gap 9 in docs/HYBRID_FEATURE_ANALYSIS_PLAN.md).

Until that refactor lands, prefer using **on-device captured transients**
(`transient` stream events in blinky-server validation captures) as the
authoritative "firing" set. This module is for offline experiments where
no device captures exist — e.g. held-out-corpus evaluations.

### What this replicates

One frame's worth of AudioTracker::updatePulseDetection, specifically
the NN-driven path. Inputs per frame:

  nn_activation   — raw NN activation (the model's per-frame output)
  bass_flux       — spectral_.getBassFlux()
  broadband_flux  — spectral_.getSpectralFlux() (compressed)
  plp_confidence  — plpConfidence_ (set to 0 to disable PLP bias)
  plp_pulse       — plpPulseValue_ (ignored if confidence below threshold)
  signal_presence — max(odfPeakHold_, cachedBassEnergy_) or a simple proxy
  now_ms          — monotonic ms for cooldown

State carried frame-to-frame: nnSmoothed_ (EMA), prevSignal_, prevPrevSignal_,
lastPulseMs_.

Decision: `fires` == True iff
  - signal_presence > pulse_min_level
  - prev_signal > prev_prev_signal  (local max on NN activation)
  - prev_signal > nn_activation
  - prev_signal > effective_floor (pulseOnsetFloor · bass_gate · plp_bias)
  - (now_ms - lastPulseMs_) > cooldown_ms

The cooldown uses tempo-adaptive formula: 40 + 110·(1 − bpm_norm) ms,
where bpm_norm = clamp((bpm-60)/140, 0, 1).

No crest gate in this initial port — crestGateMin sweep already showed
the gate is anti-selective (docs/HYBRID_FEATURE_ANALYSIS_PLAN.md Path A
null result). Add if a later experiment wants to re-test with different
crest-timing semantics.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class PeakPickerConfig:
    pulse_onset_floor: float = 0.30
    pulse_min_level: float = 0.03
    bass_gate_max_boost: float = 0.5
    bass_gate_min_ratio: float = 1.0 / 3.0
    plp_bias_max_suppression: float = 0.3
    plp_bias_min_confidence: float = 0.3
    nn_smoothing_alpha: float = 0.3  # EMA on NN activation (TODO: verify vs firmware)


@dataclass
class PeakPickerState:
    nn_smoothed: float = 0.0
    prev_signal: float = 0.0
    prev_prev_signal: float = 0.0
    last_pulse_ms: int = 0


def on_frame(
    cfg: PeakPickerConfig,
    state: PeakPickerState,
    *,
    nn_activation: float,
    bass_flux: float,
    broadband_flux: float,
    plp_confidence: float,
    plp_pulse: float,
    signal_presence: float,
    bpm: float,
    now_ms: int,
) -> tuple[bool, float]:
    """Apply one frame's worth of peak-picking. Returns (fires, strength).

    Mutates `state` in place — caller must persist it across calls for
    temporal continuity.
    """
    # EMA smoothing of NN activation. AudioTracker's exact smoothing factor
    # is not exposed; using a conservative default. TODO: confirm against
    # firmware `nnSmoothed_` update formula when porting to C++.
    state.nn_smoothed = (
        cfg.nn_smoothing_alpha * nn_activation
        + (1.0 - cfg.nn_smoothing_alpha) * state.nn_smoothed
    )
    nn = state.nn_smoothed

    # Bass gate: inverse-bass-ratio threshold boost.
    bass_ratio = bass_flux / broadband_flux if broadband_flux > 0.001 else 0.5
    clamped = max(0.0, min(1.0, 1.0 - bass_ratio / cfg.bass_gate_min_ratio))
    bass_gate = 1.0 + cfg.bass_gate_max_boost * clamped

    # PLP bias: off-beat threshold boost when pattern confidence is high.
    pattern_bias = 1.0
    if plp_confidence > cfg.plp_bias_min_confidence:
        pattern_val = max(0.0, min(1.0, plp_pulse))
        suppress = cfg.plp_bias_max_suppression * plp_confidence * (1.0 - pattern_val)
        pattern_bias = 1.0 + suppress

    effective_floor = cfg.pulse_onset_floor * bass_gate * pattern_bias

    # Tempo-adaptive cooldown: 40 ms at 200 BPM, 150 ms at 60 BPM.
    bpm_norm = max(0.0, min(1.0, (bpm - 60.0) / 140.0))
    cooldown_ms = 40.0 + 110.0 * (1.0 - bpm_norm)
    if now_ms < state.last_pulse_ms:
        state.last_pulse_ms = now_ms
    cooldown_ok = (now_ms - state.last_pulse_ms) > cooldown_ms

    # Local-max on NN activation (peak was prev frame if now decreasing).
    is_local_max = (
        state.prev_signal > state.prev_prev_signal
        and state.prev_signal > nn
        and state.prev_signal > effective_floor
    )

    fires = signal_presence > cfg.pulse_min_level and is_local_max and cooldown_ok
    strength = 0.0
    if fires:
        strength = max(0.0, min(1.0, state.prev_signal))
        state.last_pulse_ms = now_ms

    # Advance per-frame history
    state.prev_prev_signal = state.prev_signal
    state.prev_signal = nn

    return fires, strength
