"""Pure scoring functions for music test evaluation.

Ported from blinky-serial-mcp/src/lib/scoring.ts — canonical implementation.
No side effects, no I/O. All rounding uses JS-compatible math.round pattern.
"""

from __future__ import annotations

import math
from collections import defaultdict
from typing import Any

from .types import (
    BeatEvent,
    BeatTracking,
    DeviceRunScore,
    Diagnostics,
    GroundTruth,
    MusicMode,
    MusicState,
    OffsetStats,
    PlpMetrics,
    TestData,
    TransientEvent,
    TransientTracking,
)


def _js_round(x: float, decimals: int = 3) -> float:
    """Round like JavaScript Math.round (rounds .5 up, not banker's)."""
    factor = 10**decimals
    return math.floor(x * factor + 0.5) / factor


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
# Statistics
# ---------------------------------------------------------------------------


def compute_stats(values: list[float]) -> dict[str, float]:
    """Mean, std, min, max. Returns zeros for empty arrays."""
    if not values:
        return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0}
    mean = sum(values) / len(values)
    variance = sum((v - mean) ** 2 for v in values) / len(values)
    return {"mean": mean, "std": math.sqrt(variance), "min": min(values), "max": max(values)}


def round_stats(s: dict[str, float]) -> dict[str, float]:
    return {k: _js_round(v) for k, v in s.items()}


# ---------------------------------------------------------------------------
# Audio latency estimation
# ---------------------------------------------------------------------------


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
        h for h in gt_hits if h.get("expect_trigger", True) and h["time"] * 1000 <= audio_duration_ms
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
        bucket = round(o / BUCKET) * BUCKET
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
        if abs(round(o / BUCKET) * BUCKET - peak_bucket) <= BUCKET:
            sum_offset += o
            sum_weight += 1

    return sum_offset / sum_weight if sum_weight > 0 else float(peak_bucket)


# ---------------------------------------------------------------------------
# Main scoring
# ---------------------------------------------------------------------------

BEAT_TOLERANCE_SEC = 0.10
PLAUSIBLE_LATENCY_MIN = -50
PLAUSIBLE_LATENCY_MAX = 300
MIN_ONSET_STRENGTH = 0.6
ONSET_DEDUP_SEC = 0.070


def score_device_run(
    test_data: TestData,
    audio_start_time: float,
    gt_data: GroundTruth,
) -> DeviceRunScore:
    """Score a single device's test recording against ground truth."""
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
            "bpm": s.bpm,
            "phase": s.phase,
            "confidence": s.confidence,
            "plp_pulse": s.plp_pulse,
        }
        for s in test_data.music_states
        if s.timestamp_ms - timing_offset_ms >= 0
    ]
    beat_events = [
        {
            "timestamp_ms": b.timestamp_ms - timing_offset_ms,
            "bpm": b.bpm,
            "type": b.type,
            "predicted": b.predicted,
        }
        for b in test_data.beat_events
        if b.timestamp_ms - timing_offset_ms >= 0
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

    # Reference beats
    ref_beats = [
        h.time
        for h in gt_data.hits
        if h.expect_trigger and h.time <= audio_duration_sec
    ]

    # Estimated beats
    est_beats = [(b["timestamp_ms"] - latency_correction_ms) / 1000 for b in beat_events]

    # Beat F1
    beat_result = match_events_f1(est_beats, ref_beats, BEAT_TOLERANCE_SEC)

    # Transient F1
    est_transients = [(d["timestamp_ms"] - latency_correction_ms) / 1000 for d in detections]
    if gt_data.onsets:
        filtered = sorted(
            [o.time for o in gt_data.onsets if o.time <= audio_duration_sec and o.strength >= MIN_ONSET_STRENGTH]
        )
        deduped: list[float] = []
        for t in filtered:
            if not deduped or t - deduped[-1] > ONSET_DEDUP_SEC:
                deduped.append(t)
        ref_onsets = deduped
    else:
        ref_onsets = ref_beats

    transient_result = match_events_f1(est_transients, ref_onsets, BEAT_TOLERANCE_SEC)

    # CMLt: continuity metric
    correct = [any(abs(est - ref) <= BEAT_TOLERANCE_SEC for est in est_beats) for ref in ref_beats]
    total_correct, longest, current = 0, 0, 0
    for c in correct:
        if c:
            current += 1
        else:
            if current > 0:
                total_correct += current
                longest = max(longest, current)
                current = 0
    if current > 0:
        total_correct += current
        longest = max(longest, current)
    cmlt = total_correct / len(ref_beats) if ref_beats else 0.0
    cmlc = longest / len(ref_beats) if ref_beats else 0.0

    # AMLt: also check half-time and double-time
    double_time: list[float] = []
    for i, b in enumerate(est_beats):
        double_time.append(b)
        if i < len(est_beats) - 1:
            double_time.append((b + est_beats[i + 1]) / 2)
    half_time = [b for i, b in enumerate(est_beats) if i % 2 == 0]

    best_aml_correct = correct
    for alt_est in [double_time, half_time]:
        alt_correct = [any(abs(est - ref) <= BEAT_TOLERANCE_SEC for est in alt_est) for ref in ref_beats]
        if sum(alt_correct) > sum(best_aml_correct):
            best_aml_correct = alt_correct

    aml_total, aml_longest, aml_current = 0, 0, 0
    for c in best_aml_correct:
        if c:
            aml_current += 1
        else:
            if aml_current > 0:
                aml_total += aml_current
                aml_longest = max(aml_longest, aml_current)
                aml_current = 0
    if aml_current > 0:
        aml_total += aml_current
        aml_longest = max(aml_longest, aml_current)
    amlt = aml_total / len(ref_beats) if ref_beats else 0.0

    # Music mode metrics
    active_states = [s for s in music_states if s["active"]]
    avg_bpm = (
        sum(s["bpm"] for s in active_states) / len(active_states) if active_states else 0.0
    )
    avg_conf = (
        sum(s["confidence"] for s in active_states) / len(active_states) if active_states else 0.0
    )

    # Phase stability
    phase_stability = 0.0
    if len(active_states) > 1:
        phase_diffs: list[float] = []
        for i in range(1, len(active_states)):
            diff = active_states[i]["phase"] - active_states[i - 1]["phase"]
            if diff < -0.5:
                diff += 1.0
            if diff > 0.5:
                diff -= 1.0
            phase_diffs.append(diff)
        if phase_diffs:
            mean_diff = sum(phase_diffs) / len(phase_diffs)
            variance = sum((d - mean_diff) ** 2 for d in phase_diffs) / len(phase_diffs)
            phase_stability = max(0.0, 1.0 - math.sqrt(variance) * 10)

    # PLP metrics
    plp_values = [s["plp_pulse"] for s in active_states if s.get("plp_pulse") is not None]
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
                dist = abs(active_states[si]["timestamp_ms"] - onset_ms)
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

        # PLP autocorrelation at detected BPM lag
        if avg_bpm > 0 and len(plp_values) > 10:
            stream_rate = len(plp_values) / (audio_duration_sec or 1)
            bpm_lag = round(stream_rate * 60 / avg_bpm)
            if 0 < bpm_lag < len(plp_values) // 2:
                sum_xy = 0.0
                sum_x2 = 0.0
                n = len(plp_values) - bpm_lag
                for i in range(n):
                    x = plp_values[i] - plp_mean
                    y = plp_values[i + bpm_lag] - plp_mean
                    sum_xy += x * y
                    sum_x2 += x * x
                plp_auto_corr = sum_xy / sum_x2 if sum_x2 > 0 else 0.0

    # Diagnostics: transient-beat offsets
    transient_beat_offsets: list[int] = []
    for det in detections:
        det_sec = (det["timestamp_ms"] - latency_correction_ms) / 1000
        best_offset = float("inf")
        for ref in ref_beats:
            offset = det_sec - ref
            if abs(offset) < abs(best_offset):
                best_offset = offset
        if abs(best_offset) < 0.5:
            transient_beat_offsets.append(round(best_offset * 1000))

    phase_offset_stats = _compute_offset_stats(transient_beat_offsets)

    # Diagnostics: beat event offsets
    beat_event_offsets: list[int] = []
    for est in est_beats:
        best_offset = float("inf")
        for ref in ref_beats:
            offset = est - ref
            if abs(offset) < abs(best_offset):
                best_offset = offset
        if abs(best_offset) < 0.5:
            beat_event_offsets.append(round(best_offset * 1000))

    beat_offset_stats = _compute_offset_stats(beat_event_offsets)

    beat_offset_histogram: dict[str, int] = {}
    for offset in beat_event_offsets:
        bucket = round(offset / 10) * 10
        key = str(bucket)
        beat_offset_histogram[key] = beat_offset_histogram.get(key, 0) + 1

    predicted_beats = sum(1 for b in beat_events if b.get("predicted") is True)
    fallback_beats = sum(
        1 for b in beat_events if b.get("predicted") is not True
    )

    return DeviceRunScore(
        audio_latency_ms=audio_latency_ms,
        audio_duration_sec=audio_duration_sec,
        timing_offset_ms=timing_offset_ms,
        beat_tracking=BeatTracking(
            f1=_js_round(beat_result["f1"]),
            precision=_js_round(beat_result["precision"]),
            recall=_js_round(beat_result["recall"]),
            cmlt=_js_round(cmlt),
            cmlc=_js_round(cmlc),
            amlt=_js_round(amlt),
            ref_beats=len(ref_beats),
            est_beats=len(est_beats),
        ),
        transient_tracking=TransientTracking(
            f1=_js_round(transient_result["f1"]),
            precision=_js_round(transient_result["precision"]),
            recall=_js_round(transient_result["recall"]),
            count=len(detections),
            f1_at_50ms=_js_round(match_events_f1(est_transients, ref_onsets, 0.050)["f1"]),
            f1_at_70ms=_js_round(match_events_f1(est_transients, ref_onsets, 0.070)["f1"]),
            f1_at_100ms=_js_round(transient_result["f1"]),
            f1_at_150ms=_js_round(match_events_f1(est_transients, ref_onsets, 0.150)["f1"]),
            ref_onsets=len(ref_onsets),
        ),
        music_mode=MusicMode(
            avg_confidence=_js_round(avg_conf, 2),
            phase_stability=_js_round(phase_stability),
            activation_ms=active_states[0]["timestamp_ms"] if active_states else None,
            detected_bpm=_js_round(avg_bpm, 1),
        ),
        plp=PlpMetrics(
            at_transient=_js_round(plp_at_transient),
            gt_onsets_matched=len(gt_onset_plp_values),
            gt_onsets_total=len(ref_onsets),
            auto_corr=_js_round(plp_auto_corr),
            peakiness=_js_round(plp_peakiness, 2),
            mean=_js_round(plp_mean),
        ),
        diagnostics=Diagnostics(
            transient_rate=len(detections) / audio_duration_sec if audio_duration_sec > 0 else 0.0,
            expected_beat_rate=len(ref_beats) / audio_duration_sec if audio_duration_sec > 0 else 0.0,
            beat_event_rate=len(est_beats) / audio_duration_sec if audio_duration_sec > 0 else 0.0,
            phase_offset_stats=phase_offset_stats,
            beat_offset_stats=beat_offset_stats,
            beat_offset_histogram=beat_offset_histogram,
            beat_vs_reference={
                "matched": int(beat_result["tp"]),
                "extra": len(est_beats) - int(beat_result["tp"]),
                "missed": len(ref_beats) - int(beat_result["tp"]),
            },
            prediction_ratio=(
                {"predicted": predicted_beats, "fallback": fallback_beats, "total": len(beat_events)}
                if beat_events
                else None
            ),
            transient_beat_offsets=transient_beat_offsets,
            beat_event_offsets=beat_event_offsets,
        ),
        adjusted_detections=[dict(d) for d in detections],
        adjusted_beat_events=[dict(b) for b in beat_events],
        adjusted_music_states=[dict(s) for s in music_states],
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
    return OffsetStats(median=round(median), std_dev=round(std_dev), iqr=round(q3 - q1))


# ---------------------------------------------------------------------------
# Summary formatting
# ---------------------------------------------------------------------------


def format_score_summary(score: DeviceRunScore) -> dict[str, Any]:
    """Convert DeviceRunScore to compact JSON summary (removes raw data)."""
    bt = score.beat_tracking
    tt = score.transient_tracking
    mm = score.music_mode
    d = score.diagnostics
    return {
        "beatTracking": {
            "f1": bt.f1,
            "precision": bt.precision,
            "recall": bt.recall,
            "refBeats": bt.ref_beats,
            "estBeats": bt.est_beats,
        },
        "transientTracking": {
            "f1": tt.f1,
            "precision": tt.precision,
            "recall": tt.recall,
            "count": tt.count,
            "f1_at_50ms": tt.f1_at_50ms,
            "f1_at_70ms": tt.f1_at_70ms,
            "f1_at_100ms": tt.f1_at_100ms,
            "f1_at_150ms": tt.f1_at_150ms,
            "refOnsets": tt.ref_onsets,
        },
        "musicMode": {
            "avgConfidence": mm.avg_confidence,
            "phaseStability": mm.phase_stability,
            "activationMs": mm.activation_ms,
            "detectedBpm": mm.detected_bpm,
        },
        "plp": {
            "atTransient": score.plp.at_transient,
            "gtOnsetsMatched": score.plp.gt_onsets_matched,
            "gtOnsetsTotal": score.plp.gt_onsets_total,
            "autoCorr": score.plp.auto_corr,
            "peakiness": score.plp.peakiness,
            "mean": score.plp.mean,
        },
        "diagnostics": {
            "transientRate": _js_round(d.transient_rate, 1),
            "expectedBeatRate": _js_round(d.expected_beat_rate, 1),
            "beatEventRate": _js_round(d.beat_event_rate, 1),
            "transientOffsetMs": (
                {"median": d.phase_offset_stats.median, "stdDev": d.phase_offset_stats.std_dev, "iqr": d.phase_offset_stats.iqr}
                if d.phase_offset_stats
                else None
            ),
            "beatOffsetMs": (
                {"median": d.beat_offset_stats.median, "stdDev": d.beat_offset_stats.std_dev, "iqr": d.beat_offset_stats.iqr}
                if d.beat_offset_stats
                else None
            ),
            "beatOffsetHistogram": d.beat_offset_histogram,
            "predictionRatio": d.prediction_ratio,
            "matched": d.beat_vs_reference["matched"],
            "extra": d.beat_vs_reference["extra"],
            "missed": d.beat_vs_reference["missed"],
        },
        "timing": {
            "latencyMs": round(score.audio_latency_ms) if score.audio_latency_ms is not None else None,
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
