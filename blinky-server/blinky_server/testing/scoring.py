"""Pure scoring functions for onset and PLP pattern evaluation.

Ported from blinky-serial-mcp/src/lib/scoring.ts — canonical implementation.
No side effects, no I/O. All rounding uses JS-compatible math.round pattern.

Only two things are scored:
1. Onset accuracy — do detected onsets match ground truth kicks/snares?
2. PLP pattern quality — does the PLP pattern lock on and show structure?
"""

from __future__ import annotations

import math
from collections import defaultdict
from typing import Any

from .types import (
    DeviceRunScore,
    Diagnostics,
    GroundTruth,
    OffsetStats,
    OnsetTracking,
    PlpMetrics,
    TestData,
)


def _js_round(x: float, decimals: int = 3) -> float:
    """Round like JavaScript Math.round (rounds .5 up, not banker's)."""
    factor: float = 10.0**decimals
    return math.floor(x * factor + 0.5) / factor


def _js_round_int(x: float) -> int:
    """Round to nearest int like JavaScript Math.round (rounds .5 up)."""
    return math.floor(x + 0.5)


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
    timing_offset_ms = audio_start_time - test_data.start_time

    # Adjust timestamps relative to audio start
    detections = [
        {"timestamp_ms": d.timestamp_ms - timing_offset_ms, "type": d.type, "strength": d.strength}
        for d in test_data.transients
        if d.timestamp_ms - timing_offset_ms >= 0
    ]
    music_states = [
        {
            "timestamp_ms": s.timestamp_ms - timing_offset_ms,
            "active": s.active,
            "phase": s.phase,
            "confidence": s.confidence,
            "oss": s.oss,
            "plp_pulse": s.plp_pulse,
            "bpm_internal": s.bpm_internal,
        }
        for s in test_data.music_states
        if s.timestamp_ms - timing_offset_ms >= 0
    ]

    # Latency estimation
    audio_duration_ms = raw_duration - timing_offset_ms
    gt_hits_dicts = [
        {"time": h.time, "strength": h.strength, "expect_trigger": h.expect_trigger}
        for h in gt_data.hits
    ]
    audio_latency_ms = estimate_audio_latency(detections, gt_hits_dicts, audio_duration_ms)
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
        (d["timestamp_ms"] - latency_correction_ms) / 1000  # type: ignore[operator]
        for d in detections
    ]
    onset_result = match_events_f1(est_onsets, ref_onsets, ONSET_TOLERANCE_SEC)

    # Rhythm tracking diagnostics (confidence, activation)
    active_states = [s for s in music_states if s["active"]]
    avg_conf: float = (
        sum(s["confidence"] for s in active_states) / len(active_states) if active_states else 0.0  # type: ignore[misc]
    )
    activation_ms: float | None = active_states[0]["timestamp_ms"] if active_states else None

    # PLP metrics
    plp_values: list[float] = [
        s["plp_pulse"]  # type: ignore[misc]
        for s in active_states
        if s.get("plp_pulse") is not None
    ]
    plp_at_transient = 0.0
    plp_auto_corr = 0.0
    plp_peakiness = 0.0
    gt_onset_plp_values: list[float] = []
    plp_mean = 0.0

    if plp_values:
        plp_mean = sum(plp_values) / len(plp_values)
        plp_max = max(plp_values)
        plp_peakiness = plp_max / plp_mean if plp_mean > 0.01 else 0.0

        # PLP at ground truth onset times (sliding search start for O(n))
        search_start = 0
        latency_offset_sec = latency_correction_ms / 1000
        for onset_sec in ref_onsets:
            onset_ms = (onset_sec + latency_offset_sec) * 1000
            best_state: dict[str, Any] | None = None
            best_dist = float("inf")
            for si in range(search_start, len(active_states)):
                dist = abs(active_states[si]["timestamp_ms"] - onset_ms)  # type: ignore[operator]
                if dist < best_dist:
                    best_dist = dist
                    best_state = active_states[si]
                elif dist > best_dist:
                    search_start = max(0, si - 2)
                    break
            if best_state and best_state.get("plp_pulse") is not None and best_dist < 150:
                gt_onset_plp_values.append(best_state["plp_pulse"])

        if gt_onset_plp_values:
            plp_at_transient = sum(gt_onset_plp_values) / len(gt_onset_plp_values)

        # PLP autocorrelation at detected period lag
        # Derive lag from streamed BPM (internal, not scored) and stream rate.
        bpm_values = [
            s["bpm_internal"]
            for s in active_states
            if s.get("bpm_internal", 0) > 0  # type: ignore[operator]
        ]
        avg_bpm: float = (
            sum(bpm_values) / len(bpm_values)  # type: ignore[arg-type]
            if bpm_values
            else 0.0
        )
        if avg_bpm > 0 and len(plp_values) > 10:
            stream_rate = len(plp_values) / (audio_duration_sec or 1)
            period_lag = _js_round_int(stream_rate * 60 / avg_bpm)
        else:
            period_lag = 0

        if 0 < period_lag < len(plp_values) / 2:
            sum_xy = 0.0
            sum_x2 = 0.0
            n = len(plp_values) - period_lag
            for i in range(n):
                x = plp_values[i] - plp_mean
                y = plp_values[i + period_lag] - plp_mean
                sum_xy += x * y
                sum_x2 += x * x
            plp_auto_corr = sum_xy / sum_x2 if sum_x2 > 0 else 0.0

    # Diagnostics: onset-to-GT offsets
    onset_offsets: list[int] = []
    for det in detections:
        det_sec: float = (det["timestamp_ms"] - latency_correction_ms) / 1000  # type: ignore[operator]
        best_offset = float("inf")
        for ref in ref_onsets:
            offset = det_sec - ref
            if abs(offset) < abs(best_offset):
                best_offset = offset
        if abs(best_offset) < 0.5:
            onset_offsets.append(_js_round_int(best_offset * 1000))

    onset_offset_stats = _compute_offset_stats(onset_offsets)

    return DeviceRunScore(
        audio_latency_ms=audio_latency_ms,
        audio_duration_sec=audio_duration_sec,
        timing_offset_ms=timing_offset_ms,
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
            gt_onsets_matched=len(gt_onset_plp_values),
            gt_onsets_total=len(ref_onsets),
            auto_corr=_js_round(plp_auto_corr),
            peakiness=_js_round(plp_peakiness, 2),
            mean=_js_round(plp_mean),
        ),
        avg_confidence=_js_round(avg_conf, 2),
        activation_ms=activation_ms,
        diagnostics=Diagnostics(
            onset_rate=len(detections) / audio_duration_sec if audio_duration_sec > 0 else 0.0,
            onset_offset_stats=onset_offset_stats,
            onset_offsets=onset_offsets,
        ),
        adjusted_detections=[dict(d) for d in detections],
        adjusted_music_states=[
            {k: v for k, v in s.items() if k != "bpm_internal"} for s in music_states
        ],
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
            "gtOnsetsMatched": score.plp.gt_onsets_matched,
            "gtOnsetsTotal": score.plp.gt_onsets_total,
            "autoCorr": score.plp.auto_corr,
            "peakiness": score.plp.peakiness,
            "mean": score.plp.mean,
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
