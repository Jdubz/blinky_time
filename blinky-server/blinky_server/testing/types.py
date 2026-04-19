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
class Diagnostics:
    onset_rate: float
    onset_offset_stats: OffsetStats | None
    onset_offsets: list[int]


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
