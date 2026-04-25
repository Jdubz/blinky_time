"""Shared types for test scoring.

Ported from blinky-serial-mcp/src/lib/types.ts — canonical definitions.
Only onset accuracy and PLP pattern metrics. No beat/BPM scoring.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class GroundTruthHit:
    time: float  # seconds
    type: str
    strength: float
    expect_trigger: bool = True


@dataclass
class GroundTruthOnset:
    time: float  # seconds
    strength: float


@dataclass
class GroundTruth:
    pattern: str
    duration_ms: float
    hits: list[GroundTruthHit]
    onsets: list[GroundTruthOnset] | None = None


@dataclass
class TransientEvent:
    timestamp_ms: float
    type: str
    strength: float
    # T1.4/T1.5 — per-firing diagnostics (added 2026-04-24). Bits indicate
    # which gates were close to suppressing: 0x01 bass_gate boosted,
    # 0x02 pattern_bias boosted, 0x04 crest near threshold, 0x08 beat-grid
    # near threshold. Features is a snapshot at firing time (flat, rflux,
    # cent, crest, roll, hfc, bassR, plpP, plpC). Both None when the
    # firmware predates b145 (no gateMask/features fields in transient JSON).
    gate_mask: int | None = None
    features: dict[str, float] | None = None


@dataclass
class SignalFrame:
    """A single captured frame of per-feature signal values.

    `values` is a dict of signal_name → float. The keys used by the
    current firmware are documented in test_session.py's `SIGNAL_KEYS`
    mapping. A generic dict lets the catalog grow without server-type
    churn — adding a new firmware signal requires only a key addition
    in test_session.py and a corresponding feature in Python reference.

    `activation` is the NN's raw onset activation, kept as a dedicated
    field because it's used by non-signal metrics (e.g. activation_ms).
    """

    timestamp_ms: float
    activation: float
    values: dict[str, float]


@dataclass
class MusicState:
    timestamp_ms: float
    active: bool
    confidence: float
    plp_pulse: float | None = None
    plp_period: int | None = None  # ACF period in frames (~66Hz) — used for autocorr lag
    reliability: float | None = None  # Per-bin epoch consistency (debug stream only)
    nn_agreement: float | None = None  # Flux/NN fold agreement (debug stream only)


@dataclass
class TestData:
    """Raw recording buffers from a test session."""

    duration: float  # ms
    start_time: float  # ms (epoch)
    transients: list[TransientEvent] = field(default_factory=list)
    music_states: list[MusicState] = field(default_factory=list)
    signal_frames: list[SignalFrame] = field(default_factory=list)


@dataclass
class OffsetStats:
    median: int
    std_dev: int
    iqr: int


@dataclass
class OnsetTracking:
    f1: float
    precision: float
    recall: float
    count: int
    f1_50ms: float
    f1_70ms: float
    f1_100ms: float
    f1_150ms: float
    ref_onsets: int


@dataclass
class PlpMetrics:
    at_transient: float  # avg PLP pulse at GT onset times (raw)
    at_transient_norm: float  # at_transient / mean — >1.0 means pattern peaks at onsets
    gt_onsets_matched: int
    gt_onsets_total: int
    auto_corr: float  # autocorrelation at detected period lag (1.0 = periodic)
    peakiness: float  # peak/mean ratio (1.0 = flat, >2 = strong)
    mean: float  # avg PLP value
    reliability: float = 0.0  # mean per-bin epoch consistency (0=random, 1=identical every cycle)
    nn_agreement: float = 0.0  # cosine similarity between flux fold and NN fold (0-1)
    gt_pattern_corr: float = 0.0  # correlation between device pattern and GT-folded pattern


@dataclass
class SignalGapStats:
    """Per-signal onset-vs-non-onset statistics. One of these per streamed signal.

    Two `mode`s:
      - "frame": every captured frame is classified onset (within
        ±HYBRID_ONSET_WINDOW_SEC of any GT onset) or non-onset. Same method
        b132-b133 used. Dilutes sharp-attack signals because the onset
        window spans multiple frames around the peak.
      - "peak":  one sample per GT onset — max feature value within
        ±HYBRID_ONSET_WINDOW_SEC around each onset. Non-onset pool is still
        frame-level. Matches the offline Phase 1 methodology in
        run_catalog.py. Should give higher |d| for sharp-attack features.

    See docs/HYBRID_FEATURE_ANALYSIS_PLAN.md Phase 1.
    """

    signal: str
    mode: str  # "frame" or "peak"
    onset_mean: float
    onset_std: float
    non_mean: float
    non_std: float
    gap: float  # onset_mean - non_mean (raw difference)
    cohens_d: float  # gap / sqrt((onset_var + non_var) / 2) — pooled effect size
    n_onset: int
    n_non: int


@dataclass
class ActivationStats:
    """Summary statistics of raw NN activation across a test run.

    Captures the distribution shape so compressed-output pathologies (e.g.,
    v30_mel_only with std=0.15 and min=0.137 instead of v29's std=0.34 and
    min≈0) surface in the validation output rather than needing offline
    TFLite inference to diagnose. Populated from signal_frames[].activation.
    """

    min: float
    max: float
    mean: float
    std: float
    p5: float
    p50: float
    p95: float
    p99: float
    frames: int


@dataclass
class LatencyHistogram:
    """Distribution of per-detection onset offset (ms) in 20 ms bins.

    Mean latency hides jitter: +5 ms mean could be (all 5 ms tight) or
    (half -50, half +60). Histogram distinguishes the two modes.
    Bin edges at ±10, ±30, ±50, ±70, ±100, ±150 ms; anything beyond
    lumps into the outer bucket.
    """

    bins_ms: list[str]  # label for each bin, e.g., "[-30,-10)"
    counts: list[int]
    median: int
    p25: int
    p75: int


@dataclass
class Diagnostics:
    onset_rate: float
    onset_offset_stats: OffsetStats | None
    onset_offsets: list[int]
    activation_stats: ActivationStats | None = None
    latency_histogram: LatencyHistogram | None = None


@dataclass
class DeviceRunScore:
    audio_latency_ms: float | None
    audio_duration_sec: float
    timing_offset_ms: float
    onset_tracking: OnsetTracking
    plp: PlpMetrics
    avg_confidence: float
    activation_ms: float | None
    diagnostics: Diagnostics
    signals: list[SignalGapStats] = field(default_factory=list)
    signal_frames_captured: int = 0
