"""A run's tempo against the track's ground truth, for the stable proposals (docs/DEV_LOOP.md,
"Stable candidates from scores").

The chip streams its ACF period (``m.per``, firmware analysis frames: 16 kHz / 256-sample hop =
62.5 Hz; ``MusicState.plp_period``). The run's tempo is the median period over the run's second
half (the first half is the tracker locking on), as BPM. The reference is the track's
``groundTruthBpm`` (blinky-corpus ``edm/<track>.pattern_gt.json``; a track without it has no
tempo score, reported as absent with the reason, never as zero).

The error is octave-tolerant, the evaluation convention for tempo (MIREX "Accuracy 2", Gouyon
et al. 2006): half, double and triple time are equally valid, so the error is the smallest
relative distance to the reference times 1, 2, 3, 1/2 or 1/3. ``acc2`` is that error within 4%.
This is not the removed ``bpmError`` (docs/TESTING_METHODOLOGY.md, "Removed metrics"): that
counted half and double time as wrong.

The period is an integer frame count, so the tempo moves in steps: at 120 BPM one frame is about
3%. The error therefore reads in steps of that size; the proposals judge it beyond its noise
like every other metric.
"""

from __future__ import annotations

import json
import logging
import statistics
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

FRAME_HZ = 62.5  # blinky-firmware's analysis frame rate (16 kHz, 256-sample hop)
FACTORS = (1.0, 2.0, 3.0, 0.5, 1.0 / 3.0)  # Accuracy 2's metrical levels
ACC2_TOL = 0.04
MIN_FRAMES = 20  # fewer music-state frames with a period than this: no tempo for the run


def ground_truth_bpm(audio_file: str | Path) -> float | None:
    """The track's ``groundTruthBpm`` from its pattern file next to the audio, else None."""
    p = Path(audio_file)
    gt = p.with_name(p.stem + ".pattern_gt.json")
    try:
        bpm = json.loads(gt.read_text()).get("groundTruthBpm")
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        log.error("[FALLBACK] tempo: %s unreadable (%s): no tempo score for the track", gt, e)
        return None
    return float(bpm) if isinstance(bpm, (int, float)) and bpm > 0 else None


def octave_error(bpm: float, reference: float) -> float:
    """The smallest relative error of ``bpm`` against the reference at any Accuracy 2 level."""
    return min(abs(bpm / (reference * f) - 1.0) for f in FACTORS)


def tempo_score(music_states: list[Any], reference_bpm: float | None) -> dict[str, Any]:
    """{bpm, gtBpm, err, acc2, frames}, or {bpm: None, why} when there is nothing to score."""
    if reference_bpm is None:
        return {"bpm": None, "why": "no ground-truth tempo for this track"}
    states = [s for s in music_states if getattr(s, "plp_period", None)]
    if states:
        t0, t1 = states[0].timestamp_ms, states[-1].timestamp_ms
        half = t0 + (t1 - t0) / 2
        states = [s for s in states if s.timestamp_ms >= half]
    periods = [float(s.plp_period) for s in states if float(s.plp_period) > 0]
    if len(periods) < MIN_FRAMES:
        return {"bpm": None, "why": f"{len(periods)} frames with a period (< {MIN_FRAMES})"}
    bpm = 60.0 * FRAME_HZ / statistics.median(periods)
    err = octave_error(bpm, reference_bpm)
    return {
        "bpm": round(bpm, 2),
        "gtBpm": reference_bpm,
        "err": round(err, 4),
        "acc2": err <= ACC2_TOL,
        "frames": len(periods),
    }
