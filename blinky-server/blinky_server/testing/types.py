"""Shared types for test scoring.

Ported from blinky-serial-mcp/src/lib/types.ts — canonical definitions.
Only onset accuracy and PLP pattern metrics. No beat/BPM scoring.
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
    phase: float
    confidence: float
    period_steps: int | None = None  # ACF period in stream steps (for PLP autocorr)
    oss: float | None = None
    plp_pulse: float | None = None


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
    at_transient: float  # avg PLP pulse at GT onset times (1.0 = aligned)
    gt_onsets_matched: int
    gt_onsets_total: int
    auto_corr: float  # autocorrelation at detected period lag (1.0 = periodic)
    peakiness: float  # peak/mean ratio (1.0 = flat, >2 = strong)
    mean: float  # avg PLP value (0.5 = cosine fallback)


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
    adjusted_detections: list[dict[str, Any]]
    adjusted_music_states: list[dict[str, Any]]
