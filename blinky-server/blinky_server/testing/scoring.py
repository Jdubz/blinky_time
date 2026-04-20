"""Pure scoring functions for onset and PLP pattern evaluation.

Ported from blinky-serial-mcp/src/lib/scoring.ts — canonical implementation.
No side effects, no I/O. All rounding uses JS-compatible math.round pattern.

Only two things are scored:
1. Onset accuracy — do detected onsets match ground truth kicks/snares?
2. PLP pattern quality — does the PLP pattern lock on and show structure?
"""

from __future__ import annotations

import bisect
import logging
import math
import statistics
from collections import defaultdict
from typing import Any, NotRequired, TypedDict

from .types import (
    DeviceRunScore,
    Diagnostics,
    GroundTruth,
    OffsetStats,
    OnsetTracking,
    PlpMetrics,
    SignalGapStats,
    TestData,
)

log = logging.getLogger(__name__)


def _js_round(x: float, decimals: int = 3) -> float:
    """Round like JavaScript Math.round (rounds .5 up, not banker's)."""
    factor: float = 10.0**decimals
    return math.floor(x * factor + 0.5) / factor


def _js_round_int(x: float) -> int:
    """Round to nearest int like JavaScript Math.round (rounds .5 up)."""
    return math.floor(x + 0.5)


# ---------------------------------------------------------------------------
# Typed intermediates (eliminates dict[str, Any] type: ignore comments)
# ---------------------------------------------------------------------------


class _DetectionDict(TypedDict):
    timestamp_ms: float
    type: str
    strength: float


class _MusicStateDict(TypedDict):
    timestamp_ms: float
    active: bool
    confidence: float
    plp_pulse: float | None
    plp_period: int | None
    reliability: NotRequired[float | None]
    nn_agreement: NotRequired[float | None]


# ---------------------------------------------------------------------------
# Core matching
# ---------------------------------------------------------------------------


def match_events_f1(
    estimated: list[float],
    reference: list[float],
    tolerance_sec: float,
) -> dict[str, float]:
    """Greedy nearest-neighbor matching of estimated events against reference."""
    matched: set[int] = set()
    tp = 0
    for est in estimated:
        best_idx = -1
        best_dist = float("inf")
        for i, ref in enumerate(reference):
            if i in matched:
                continue
            dist = abs(est - ref)
            if dist < best_dist and dist <= tolerance_sec:
                best_dist = dist
                best_idx = i
        if best_idx >= 0:
            matched.add(best_idx)
            tp += 1
    precision = tp / len(estimated) if estimated else 0.0
    recall = tp / len(reference) if reference else 0.0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0.0
    return {"f1": f1, "precision": precision, "recall": recall, "tp": tp}


# ---------------------------------------------------------------------------
# Audio latency estimation
# ---------------------------------------------------------------------------

PLAUSIBLE_LATENCY_MIN = -50
PLAUSIBLE_LATENCY_MAX = 300
ONSET_TOLERANCE_SEC = 0.10
MIN_ONSET_STRENGTH = 0.6
ONSET_DEDUP_SEC = 0.070

# Half-width of the window used to classify an NN frame as "near a GT onset"
# when computing HybridMetrics (flatness/flux gap). Tight enough to isolate
# the transient attack, loose enough to absorb device clock jitter.
HYBRID_ONSET_WINDOW_SEC = 0.050


def estimate_audio_latency(
    detections: list[dict[str, Any]],
    gt_hits: list[dict[str, Any]],
    audio_duration_ms: float,
) -> float | None:
    """Robust histogram-peak latency estimation.

    Matches strong detections against strong GT hits, builds a 10ms-bucket
    histogram, and returns the refined peak offset.
    """
    strong_det = [d for d in detections if d["strength"] > 0.5]
    all_expected = [
        h
        for h in gt_hits
        if h.get("expect_trigger", True) and h["time"] * 1000 <= audio_duration_ms
    ]
    strong_expected = [h for h in all_expected if h["strength"] >= 0.8]

    use_det = strong_det if len(strong_det) >= 5 else detections
    use_exp = strong_expected if len(strong_expected) >= 3 else all_expected

    offsets: list[float] = []
    for det in use_det:
        best_signed = float("inf")
        for hit in use_exp:
            hit_ms = hit["time"] * 1000
            offset = det["timestamp_ms"] - hit_ms
            if abs(offset) < abs(best_signed):
                best_signed = offset
        if abs(best_signed) < 350:
            offsets.append(best_signed)

    if len(offsets) < 3:
        return None

    BUCKET = 10
    histogram: dict[int, int] = defaultdict(int)
    for o in offsets:
        bucket = _js_round_int(o / BUCKET) * BUCKET
        histogram[bucket] += 1

    peak_bucket = 0
    peak_count = 0
    for bucket, count in histogram.items():
        if count > peak_count or (count == peak_count and abs(bucket) < abs(peak_bucket)):
            peak_count = count
            peak_bucket = bucket

    sum_weight = 0
    sum_offset = 0.0
    for o in offsets:
        if abs(_js_round_int(o / BUCKET) * BUCKET - peak_bucket) <= BUCKET:
            sum_offset += o
            sum_weight += 1

    return sum_offset / sum_weight if sum_weight > 0 else float(peak_bucket)


# ---------------------------------------------------------------------------
# Main scoring
# ---------------------------------------------------------------------------


def score_device_run(
    test_data: TestData,
    audio_start_time: float,
    gt_data: GroundTruth,
) -> DeviceRunScore:
    """Score a single device's test recording against ground truth.

    Computes onset F1 (at multiple tolerances) and PLP pattern metrics.
    """
    raw_duration = test_data.duration
    # All timestamps from TestSession are system clock (epoch ms).
    # Subtract audio_start_time to get relative-to-audio timestamps
    # (0ms = audio playback begins). Ground truth is in seconds from
    # audio start, so these relative ms are divided by 1000 for matching.
    audio_epoch_ms = audio_start_time

    # Adjust timestamps relative to audio start
    detections: list[_DetectionDict] = [
        _DetectionDict(
            timestamp_ms=d.timestamp_ms - audio_epoch_ms,
            type=d.type,
            strength=d.strength,
        )
        for d in test_data.transients
        if d.timestamp_ms - audio_epoch_ms >= 0
    ]
    music_states: list[_MusicStateDict] = [
        _MusicStateDict(
            timestamp_ms=s.timestamp_ms - audio_epoch_ms,
            active=s.active,
            confidence=s.confidence,
            plp_pulse=s.plp_pulse,
            plp_period=s.plp_period,
            reliability=s.reliability,
            nn_agreement=s.nn_agreement,
        )
        for s in test_data.music_states
        if s.timestamp_ms - audio_epoch_ms >= 0
    ]

    # Latency estimation
    audio_duration_ms = raw_duration
    gt_hits_dicts = [
        {"time": h.time, "strength": h.strength, "expect_trigger": h.expect_trigger}
        for h in gt_data.hits
    ]
    det_dicts: list[dict[str, Any]] = [dict(d) for d in detections]
    audio_latency_ms = estimate_audio_latency(det_dicts, gt_hits_dicts, audio_duration_ms)
    if (
        audio_latency_ms is not None
        and PLAUSIBLE_LATENCY_MIN <= audio_latency_ms <= PLAUSIBLE_LATENCY_MAX
    ):
        latency_correction_ms = audio_latency_ms
    else:
        latency_correction_ms = 100.0

    audio_duration_sec = audio_duration_ms / 1000

    # Reference onsets
    if gt_data.onsets:
        filtered = sorted(
            [
                o.time
                for o in gt_data.onsets
                if o.time <= audio_duration_sec and o.strength >= MIN_ONSET_STRENGTH
            ]
        )
        deduped: list[float] = []
        for t in filtered:
            if not deduped or t - deduped[-1] > ONSET_DEDUP_SEC:
                deduped.append(t)
        ref_onsets = deduped
    else:
        ref_onsets = [
            h.time for h in gt_data.hits if h.expect_trigger and h.time <= audio_duration_sec
        ]

    # Onset F1 at multiple tolerances
    est_onsets: list[float] = [
        (d["timestamp_ms"] - latency_correction_ms) / 1000 for d in detections
    ]
    onset_result = match_events_f1(est_onsets, ref_onsets, ONSET_TOLERANCE_SEC)

    # Rhythm tracking diagnostics (confidence, activation)
    active_states = [s for s in music_states if s["active"]]
    avg_conf: float = (
        sum(s["confidence"] for s in active_states) / len(active_states) if active_states else 0.0
    )
    activation_ms: float | None = active_states[0]["timestamp_ms"] if active_states else None

    # PLP metrics
    plp_values: list[float] = [s["plp_pulse"] for s in active_states if s["plp_pulse"] is not None]
    plp_at_transient = 0.0
    plp_auto_corr = 0.0
    plp_peakiness = 0.0
    gt_onset_plp_values: list[float] = []
    plp_mean = 0.0
    period_lag = 0  # Computed inside `if plp_values:` from median plp_period
    stream_rate = 0.0  # Computed inside `if plp_values:` from state count / duration

    if plp_values:
        plp_mean = sum(plp_values) / len(plp_values)
        plp_max = max(plp_values)
        plp_peakiness = plp_max / plp_mean if plp_mean > 0.01 else 0.0

        # PLP at ground truth onset times (sliding search start for O(n))
        search_start = 0
        latency_offset_sec = latency_correction_ms / 1000
        for onset_sec in ref_onsets:
            onset_ms = (onset_sec + latency_offset_sec) * 1000
            best_state: _MusicStateDict | None = None
            best_dist = float("inf")
            for si in range(search_start, len(active_states)):
                dist = abs(active_states[si]["timestamp_ms"] - onset_ms)
                if dist < best_dist:
                    best_dist = dist
                    best_state = active_states[si]
                elif dist > best_dist:
                    search_start = max(0, si - 2)
                    break
            if best_state and best_state["plp_pulse"] is not None and best_dist < 75:
                gt_onset_plp_values.append(best_state["plp_pulse"])

        if gt_onset_plp_values:
            plp_at_transient = sum(gt_onset_plp_values) / len(gt_onset_plp_values)

        # PLP autocorrelation at detected period lag.
        # plp_period is in firmware analysis frames (~62.5 Hz = 16kHz / 256-sample hop).
        # plp_values are sampled at the stream rate (~20 Hz normal, ~100 Hz fast).
        # Must convert firmware frames to stream-rate samples for the lag index.
        FIRMWARE_ANALYSIS_HZ = 62.5  # 16 kHz sample rate / 256-sample FFT hop
        period_values = [
            s["plp_period"]
            for s in active_states
            if s["plp_period"] is not None and s["plp_period"] > 0
        ]
        stream_rate = len(plp_values) / (audio_duration_sec or 1)
        if period_values and stream_rate > 0:
            # Median is more robust than mean: immune to warm-up outliers
            # and tempo-change transients. Period is quantized (integer
            # frames), so median preserves the dominant locked value.
            median_period = statistics.median(period_values)
            # Convert from firmware frames to stream-rate samples
            period_lag = _js_round_int(median_period * stream_rate / FIRMWARE_ANALYSIS_HZ)

        if 0 < period_lag < len(plp_values) / 2:
            # Search a window of lags around the detected period (±10%) and
            # take the best. Handles period drift and quantization mismatch
            # between firmware frame rate and stream sample rate.
            # Pre-calculate centered values to avoid redundant subtraction
            # in the inner loop (O(M*N) → same complexity, lower constant).
            centered = [v - plp_mean for v in plp_values]
            margin = max(1, period_lag // 10)
            lo = max(1, period_lag - margin)
            hi = min(len(plp_values) // 2, period_lag + margin + 1)
            best_ac = -2.0
            for lag in range(lo, hi):
                sum_xy = 0.0
                sum_x2 = 0.0
                n_samples = len(centered) - lag
                for i in range(n_samples):
                    sum_xy += centered[i] * centered[i + lag]
                    sum_x2 += centered[i] * centered[i]
                ac = sum_xy / sum_x2 if sum_x2 > 0 else 0.0
                if ac > best_ac:
                    best_ac = ac
            if best_ac > -2.0:
                plp_auto_corr = best_ac

    # PLP reliability and NN agreement from debug stream (if available)
    plp_reliability = 0.0
    plp_nn_agreement = 0.0
    rel_values: list[float] = [
        float(v) for s in active_states if (v := s.get("reliability")) is not None
    ]
    nna_values: list[float] = [
        float(v) for s in active_states if (v := s.get("nn_agreement")) is not None
    ]
    if rel_values:
        plp_reliability = sum(rel_values) / len(rel_values)
    if nna_values:
        plp_nn_agreement = sum(nna_values) / len(nna_values)

    # GT pattern correlation: fold GT onsets at detected period, compare to device PLP.
    # This measures whether consistent events (kick on 1) land in the same bin.
    gt_pattern_corr = 0.0
    if period_lag > 0 and ref_onsets and plp_values and audio_duration_sec > 0:
        period_sec = period_lag / stream_rate if stream_rate > 0 else 0
        if period_sec > 0.1:
            # Build GT pattern: fold onset times into bins within one period
            n_bins = max(8, min(32, period_lag))  # match stream-rate resolution
            gt_pattern = [0.0] * n_bins
            for onset_sec in ref_onsets:
                phase = (onset_sec % period_sec) / period_sec
                bin_idx = int(phase * n_bins) % n_bins
                gt_pattern[bin_idx] += 1.0
            # Normalize GT pattern
            gt_max = max(gt_pattern) if gt_pattern else 1.0
            if gt_max > 0:
                gt_pattern = [v / gt_max for v in gt_pattern]

            # Build device pattern: fold PLP pulse values at same period
            dev_pattern = [0.0] * n_bins
            dev_counts = [0] * n_bins
            for i, pv in enumerate(plp_values):
                t_sec = i / stream_rate if stream_rate > 0 else 0
                phase = (t_sec % period_sec) / period_sec
                bin_idx = int(phase * n_bins) % n_bins
                dev_pattern[bin_idx] += pv
                dev_counts[bin_idx] += 1
            for b in range(n_bins):
                if dev_counts[b] > 0:
                    dev_pattern[b] /= dev_counts[b]
            # Normalize device pattern
            dev_max = max(dev_pattern) if dev_pattern else 1.0
            if dev_max > 0:
                dev_pattern = [v / dev_max for v in dev_pattern]

            # Cosine similarity
            dot = sum(a * b for a, b in zip(gt_pattern, dev_pattern, strict=True))
            mag_a = math.sqrt(sum(a * a for a in gt_pattern))
            mag_b = math.sqrt(sum(b * b for b in dev_pattern))
            if mag_a > 0 and mag_b > 0:
                gt_pattern_corr = dot / (mag_a * mag_b)

    # Diagnostics: onset-to-GT offsets
    onset_offsets: list[int] = []
    for det in detections:
        det_sec: float = (det["timestamp_ms"] - latency_correction_ms) / 1000
        best_offset = float("inf")
        for ref in ref_onsets:
            offset = det_sec - ref
            if abs(offset) < abs(best_offset):
                best_offset = offset
        if abs(best_offset) < 0.5:
            onset_offsets.append(_js_round_int(best_offset * 1000))

    onset_offset_stats = _compute_offset_stats(onset_offsets)

    # Per-signal gap metrics: compare every streamed signal's value at GT-onset
    # frames against its value at non-onset frames. Runs over whatever signals
    # the firmware streamed — no hardcoded field list here.
    signal_gaps: list[SignalGapStats] = []
    signal_frames = test_data.signal_frames
    if ref_onsets and not signal_frames:
        log.debug(
            "No signal frames captured — per-signal gaps unavailable. "
            "Check that firmware emits the debug block (flat/rflux/cent/crest/roll/hfc)."
        )
    if signal_frames and ref_onsets:
        # Classify each frame as onset (within ±HYBRID_ONSET_WINDOW_SEC of any
        # GT onset) or non-onset, then accumulate per-signal sums. ref_onsets
        # is time-ordered by construction (dedup branch sorts explicitly;
        # hits branch loads pre-sorted beat files), so bisect is safe.
        signal_names = list(signal_frames[0].values.keys())
        onset_sums: dict[str, float] = {n: 0.0 for n in signal_names}
        non_sums: dict[str, float] = {n: 0.0 for n in signal_names}
        onset_count = 0
        non_count = 0

        for f in signal_frames:
            ts_ms = f.timestamp_ms - audio_epoch_ms
            if ts_ms < 0:
                continue
            t_sec = (ts_ms - latency_correction_ms) / 1000
            idx = bisect.bisect_left(ref_onsets, t_sec)
            near_onset = False
            for j in (idx - 1, idx):
                if (
                    0 <= j < len(ref_onsets)
                    and abs(t_sec - ref_onsets[j]) < HYBRID_ONSET_WINDOW_SEC
                ):
                    near_onset = True
                    break
            if near_onset:
                onset_count += 1
                for name in signal_names:
                    onset_sums[name] += f.values.get(name, 0.0)
            else:
                non_count += 1
                for name in signal_names:
                    non_sums[name] += f.values.get(name, 0.0)

        if onset_count > 0 and non_count > 0:
            for name in signal_names:
                o_mean = onset_sums[name] / onset_count
                n_mean = non_sums[name] / non_count
                signal_gaps.append(
                    SignalGapStats(
                        signal=name,
                        onset_mean=_js_round(o_mean, 4),
                        non_mean=_js_round(n_mean, 4),
                        gap=_js_round(o_mean - n_mean, 4),
                        n_onset=onset_count,
                        n_non=non_count,
                    )
                )

    return DeviceRunScore(
        audio_latency_ms=audio_latency_ms,
        audio_duration_sec=audio_duration_sec,
        timing_offset_ms=audio_epoch_ms - test_data.start_time,
        onset_tracking=OnsetTracking(
            f1=_js_round(onset_result["f1"]),
            precision=_js_round(onset_result["precision"]),
            recall=_js_round(onset_result["recall"]),
            count=len(detections),
            f1_50ms=_js_round(match_events_f1(est_onsets, ref_onsets, 0.050)["f1"]),
            f1_70ms=_js_round(match_events_f1(est_onsets, ref_onsets, 0.070)["f1"]),
            f1_100ms=_js_round(onset_result["f1"]),
            f1_150ms=_js_round(match_events_f1(est_onsets, ref_onsets, 0.150)["f1"]),
            ref_onsets=len(ref_onsets),
        ),
        plp=PlpMetrics(
            at_transient=_js_round(plp_at_transient),
            at_transient_norm=_js_round(plp_at_transient / plp_mean, 2) if plp_mean > 0.01 else 0.0,
            gt_onsets_matched=len(gt_onset_plp_values),
            gt_onsets_total=len(ref_onsets),
            auto_corr=_js_round(plp_auto_corr),
            peakiness=_js_round(plp_peakiness, 2),
            mean=_js_round(plp_mean),
            reliability=_js_round(plp_reliability),
            nn_agreement=_js_round(plp_nn_agreement),
            gt_pattern_corr=_js_round(gt_pattern_corr),
        ),
        avg_confidence=_js_round(avg_conf, 2),
        activation_ms=activation_ms,
        diagnostics=Diagnostics(
            onset_rate=len(detections) / audio_duration_sec if audio_duration_sec > 0 else 0.0,
            onset_offset_stats=onset_offset_stats,
            onset_offsets=onset_offsets,
        ),
        signals=signal_gaps,
        signal_frames_captured=len(signal_frames),
    )


def _compute_offset_stats(offsets: list[int]) -> OffsetStats | None:
    if len(offsets) < 3:
        return None
    sorted_offsets = sorted(offsets)
    n = len(sorted_offsets)
    median = sorted_offsets[n // 2]
    mean = sum(sorted_offsets) / n
    std_dev = math.sqrt(sum((v - mean) ** 2 for v in sorted_offsets) / n)
    q1 = sorted_offsets[int(n * 0.25)]
    q3 = sorted_offsets[int(n * 0.75)]
    return OffsetStats(
        median=_js_round_int(median),
        std_dev=_js_round_int(std_dev),
        iqr=_js_round_int(q3 - q1),
    )


# ---------------------------------------------------------------------------
# Summary formatting
# ---------------------------------------------------------------------------


def format_score_summary(score: DeviceRunScore) -> dict[str, Any]:
    """Convert DeviceRunScore to compact JSON summary (removes raw data)."""
    ot = score.onset_tracking
    d = score.diagnostics
    return {
        "onsetTracking": {
            "f1": ot.f1,
            "precision": ot.precision,
            "recall": ot.recall,
            "count": ot.count,
            "f1_50ms": ot.f1_50ms,
            "f1_70ms": ot.f1_70ms,
            "f1_100ms": ot.f1_100ms,
            "f1_150ms": ot.f1_150ms,
            "refOnsets": ot.ref_onsets,
        },
        "plp": {
            "atTransient": score.plp.at_transient,
            "atTransientNorm": score.plp.at_transient_norm,
            "gtOnsetsMatched": score.plp.gt_onsets_matched,
            "gtOnsetsTotal": score.plp.gt_onsets_total,
            "autoCorr": score.plp.auto_corr,
            "peakiness": score.plp.peakiness,
            "mean": score.plp.mean,
            "reliability": score.plp.reliability,
            "nnAgreement": score.plp.nn_agreement,
            "gtPatternCorr": score.plp.gt_pattern_corr,
        },
        "rhythm": {
            "avgConfidence": score.avg_confidence,
            "activationMs": score.activation_ms,
        },
        "diagnostics": {
            "onsetRate": _js_round(d.onset_rate, 1),
            "onsetOffsetMs": (
                {
                    "median": d.onset_offset_stats.median,
                    "stdDev": d.onset_offset_stats.std_dev,
                    "iqr": d.onset_offset_stats.iqr,
                }
                if d.onset_offset_stats
                else None
            ),
        },
        "timing": {
            "latencyMs": round(score.audio_latency_ms)
            if score.audio_latency_ms is not None
            else None,
        },
        "signals": {
            "frames": score.signal_frames_captured,
            "gaps": [
                {
                    "signal": g.signal,
                    "onsetMean": g.onset_mean,
                    "nonMean": g.non_mean,
                    "gap": g.gap,
                    "nOnset": g.n_onset,
                    "nNon": g.n_non,
                }
                for g in score.signals
            ],
        },
    }


# ---------------------------------------------------------------------------
# Phase alignment (ported from param_sweep_multidev.cjs)
# ---------------------------------------------------------------------------


def analyze_phase_alignment(
    readings: list[dict[str, float]],
    settle_ms: float = 12000,
    tolerance_phase: float = 0.10,
) -> dict[str, Any]:
    """Fraction of strong pulses landing near an 8th-note grid subdivision.

    Octave errors don't affect this metric (half/double time still grid-aligned).
    """
    if not readings:
        return {"on_grid": 0, "total": 0, "pct": 0.0}

    start_time = readings[0]["time"] + settle_ms
    settled = [r for r in readings if r["time"] >= start_time]
    if not settled:
        return {"on_grid": 0, "total": 0, "pct": 0.0}

    pulse_frames = [r for r in settled if r.get("p", 0) > 0.1]
    if not pulse_frames:
        return {"on_grid": 0, "total": 0, "pct": 0.0}

    on_grid = 0
    for r in pulse_frames:
        subdiv_phase = (r.get("ph", 0) * 2) % 1.0
        grid_dist = subdiv_phase if subdiv_phase < 0.5 else (1.0 - subdiv_phase)
        if grid_dist < tolerance_phase:
            on_grid += 1

    return {
        "on_grid": on_grid,
        "total": len(pulse_frames),
        "pct": 100 * on_grid / len(pulse_frames) if pulse_frames else 0.0,
    }
