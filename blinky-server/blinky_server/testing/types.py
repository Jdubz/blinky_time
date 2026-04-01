"""Shared types for music test scoring.

Ported from blinky-serial-mcp/src/lib/types.ts — canonical definitions.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


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
    bpm: float | None = None
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
    bpm: float
    phase: float
    confidence: float
    plp_pulse: float | None = None


@dataclass
class BeatEvent:
    timestamp_ms: float
    bpm: float
    type: str
    predicted: bool | None = None


@dataclass
class TestData:
    """Raw recording buffers from a test session."""

    duration: float  # ms
    start_time: float  # ms (epoch)
    transients: list[TransientEvent] = field(default_factory=list)
    music_states: list[MusicState] = field(default_factory=list)
    beat_events: list[BeatEvent] = field(default_factory=list)


@dataclass
class OffsetStats:
    median: int
    std_dev: int
    iqr: int


@dataclass
class BeatTracking:
    f1: float
    precision: float
    recall: float
    cmlt: float
    cmlc: float
    amlt: float
    ref_beats: int
    est_beats: int


@dataclass
class TransientTracking:
    f1: float
    precision: float
    recall: float
    count: int
    f1_at_50ms: float
    f1_at_70ms: float
    f1_at_100ms: float
    f1_at_150ms: float
    ref_onsets: int


@dataclass
class MusicMode:
    avg_confidence: float
    phase_stability: float
    activation_ms: float | None
    detected_bpm: float  # informational only — not scored


@dataclass
class PlpMetrics:
    at_transient: float  # avg PLP pulse at GT onset times (1.0 = aligned)
    gt_onsets_matched: int
    gt_onsets_total: int
    auto_corr: float  # autocorrelation at detected period lag (1.0 = periodic)
    peakiness: float  # peak/mean ratio (1.0 = flat, >2 = strong)
    mean: float  # avg PLP value (0.5 = cosine fallback)


@dataclass
class Diagnostics:
    transient_rate: float
    expected_beat_rate: float
    beat_event_rate: float
    phase_offset_stats: OffsetStats | None
    beat_offset_stats: OffsetStats | None
    beat_offset_histogram: dict[str, int]
    beat_vs_reference: dict[str, int]  # matched, extra, missed
    prediction_ratio: dict[str, int] | None  # predicted, fallback, total
    transient_beat_offsets: list[int]
    beat_event_offsets: list[int]


@dataclass
class DeviceRunScore:
    audio_latency_ms: float | None
    audio_duration_sec: float
    timing_offset_ms: float
    beat_tracking: BeatTracking
    transient_tracking: TransientTracking
    music_mode: MusicMode
    plp: PlpMetrics
    diagnostics: Diagnostics
    adjusted_detections: list[dict[str, Any]]
    adjusted_beat_events: list[dict[str, Any]]
    adjusted_music_states: list[dict[str, Any]]
