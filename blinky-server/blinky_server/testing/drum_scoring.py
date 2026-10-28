"""Drum-hit scoring: the primary metric for the onset detector (#294, CLAUDE.md).

The firmware detects kicks and snares; hats and other changes are false positives. All-onset
F1 scored 0.68 while the hat looked "almost entirely random" (#280), so a run is scored on:

  kick_r50, kick_r30  share of kicks with a detection within +-50 / +-30 ms
  snare_r50           share of snares with a detection within +-50 ms (PROVISIONAL until #284)
  on_drum             share of detections within +-50 ms of a kick or snare
  drum_f              F1 of detections against kicks + snares at +-50 ms
  lone_hat            share of lone hats (no kick or snare within 50 ms) that fired a detection
  det_per_s           detections per second

Matching is one-to-one. A constant per-run lag is estimated against the drum labels and
removed first, as the #280 study did, so a fixed pipeline delay isn't scored as misses.

Labels are `kick_weighted` cleaned by `ml-training/scripts/clean_kick_weighted.py`'s rule:
an onset is kept only if a full-mix consensus onset backed by >= 2 systems lies within 70 ms
(P 0.65 -> 0.91 against hand-curated GT). `ml-training/tests/test_drum_label_sync.py`
checks this copy of the rule against that script.

Pure Python: blinky-server doesn't depend on numpy.
"""

from __future__ import annotations

import bisect
import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .scoring import PLAUSIBLE_LATENCY_MAX, PLAUSIBLE_LATENCY_MIN, estimate_audio_latency
from .types import DrumLabels

CLEAN_TOLERANCE_SEC = 0.070
CLEAN_MIN_SYSTEMS = 2
DRUM_TOL_SEC = 0.050
TIGHT_TOL_SEC = 0.030
LONE_HAT_SEC = 0.050


@dataclass
class DrumScore:
    kick_r50: float
    kick_r30: float
    snare_r50: float
    on_drum: float
    drum_f: float
    lone_hat: float
    det_per_s: float
    lag_ms: float | None
    n_kicks: int
    n_snares: int
    n_detections: int
    # False when the lag couldn't be estimated (too few detections near a label) and the
    # caller's fallback was used. The run is still scored: a silent detector scores 0.
    lag_estimated: bool = True
    provisional: list[str] = field(default_factory=lambda: ["snare_r50"])

    def as_dict(self) -> dict[str, Any]:
        def r(x: float) -> float | None:
            return None if math.isnan(x) else round(x, 4)

        return {
            "kickR50": r(self.kick_r50),
            "kickR30": r(self.kick_r30),
            "snareR50": r(self.snare_r50),
            "onDrum": r(self.on_drum),
            "drumF": r(self.drum_f),
            "loneHat": r(self.lone_hat),
            "detPerSec": r(self.det_per_s),
            "lagMs": None
            if self.lag_ms is None or math.isnan(self.lag_ms)
            else round(self.lag_ms, 1),
            "kicks": self.n_kicks,
            "snares": self.n_snares,
            "detections": self.n_detections,
            "lagEstimated": self.lag_estimated,
            "provisional": self.provisional,
        }


def clean_kick_weighted(kw: dict[str, Any], consensus: dict[str, Any]) -> list[dict[str, Any]]:
    """The kept `kick_weighted` onsets (see the module docstring for the rule)."""
    supported = sorted(
        o["time"] for o in consensus.get("onsets", []) if o.get("systems", 1) >= CLEAN_MIN_SYSTEMS
    )
    if not supported:
        raise ValueError(f"no consensus onsets backed by >= {CLEAN_MIN_SYSTEMS} systems")
    kept = []
    for o in kw.get("onsets", []):
        i = bisect.bisect_left(supported, o["time"])
        near = [abs(supported[j] - o["time"]) for j in (i - 1, i) if 0 <= j < len(supported)]
        if near and min(near) <= CLEAN_TOLERANCE_SEC:
            kept.append(o)
    return kept


def load_drum_labels(kw_path: Path, consensus_path: Path) -> DrumLabels:
    kw = json.loads(kw_path.read_text())
    if kw.get("skipped"):
        raise ValueError(f"{kw_path}: drum labels marked skipped (separation failed upstream)")
    kept = clean_kick_weighted(kw, json.loads(consensus_path.read_text()))

    def times(kind: str) -> list[float]:
        return sorted(o["time"] for o in kept if o["type"] == kind)

    return DrumLabels(
        times("kick"),
        times("snare"),
        times("hihat"),
        source=str(kw_path),
        dropped=len(kw.get("onsets", [])) - len(kept),
    )


def drum_label_path(corpus_dir: Path, stem: str) -> Path:
    return corpus_dir / "kick_weighted" / f"{stem}.kick_weighted.json"


def match_count(est: list[float], ref: list[float], tol: float) -> int:
    """Size of the maximum one-to-one matching between two sorted lists within +-tol. For
    events on a line with equal windows, greedy in time order is optimal."""
    j = n = 0
    for r in ref:
        while j < len(est) and est[j] < r - tol:
            j += 1
        if j < len(est) and est[j] <= r + tol:
            n += 1
            j += 1
    return n


def _near(sorted_xs: list[float], t: float, tol: float) -> bool:
    i = bisect.bisect_left(sorted_xs, t - tol)
    return i < len(sorted_xs) and sorted_xs[i] <= t + tol


def score_drums(
    detections: list[tuple[float, float]],
    labels: DrumLabels,
    duration_s: float,
    lag_ms: float | None = None,
    fallback_lag_ms: float = 0.0,
) -> DrumScore:
    """Score detections (time_s, strength) against drum labels on the same clock.

    `lag_ms` is estimated against kicks + snares when not given. When it can't be (too few
    detections near a label, e.g. a detector a tuning value has silenced), the run is scored
    at `fallback_lag_ms` and flagged, never dropped: dropping it would let a tuner prefer
    values that silence the detector (review of #307). Metrics are NaN only where the labels
    themselves are empty (e.g. no kicks in the segment)."""
    drums = sorted(labels.kicks + labels.snares)
    estimated = True  # a lag the caller gives (measured) is trusted as is
    if lag_ms is None:
        lag_ms = estimate_audio_latency(
            [{"timestamp_ms": t * 1000.0, "strength": s} for t, s in detections],
            [{"time": t, "strength": 1.0, "expect_trigger": True} for t in drums],
            duration_s * 1000.0,
        )
        if lag_ms is None or not PLAUSIBLE_LATENCY_MIN <= lag_ms <= PLAUSIBLE_LATENCY_MAX:
            lag_ms, estimated = fallback_lag_ms, False
    det_per_s = len(detections) / duration_s if duration_s > 0 else float("nan")
    est = sorted(t - lag_ms / 1000.0 for t, _ in detections)

    def recall(ref: list[float], tol: float) -> float:
        return match_count(est, ref, tol) / len(ref) if ref else float("nan")

    tp = match_count(est, drums, DRUM_TOL_SEC)
    p = tp / len(est) if est else 0.0
    r = tp / len(drums) if drums else float("nan")
    lone = [h for h in labels.hats if not _near(drums, h, LONE_HAT_SEC)]
    return DrumScore(
        kick_r50=recall(labels.kicks, DRUM_TOL_SEC),
        kick_r30=recall(labels.kicks, TIGHT_TOL_SEC),
        snare_r50=recall(labels.snares, DRUM_TOL_SEC),
        # No detections: on-drum is 0 (as precision is), so silencing can't look neutral.
        on_drum=(sum(_near(drums, t, DRUM_TOL_SEC) for t in est) / len(est)) if est else 0.0,
        drum_f=(2 * p * r / (p + r)) if (p + r) > 0 else 0.0,
        lone_hat=(sum(_near(est, h, DRUM_TOL_SEC) for h in lone) / len(lone))
        if lone
        else float("nan"),
        det_per_s=det_per_s,
        lag_ms=lag_ms,
        n_kicks=len(labels.kicks),
        n_snares=len(labels.snares),
        n_detections=len(est),
        lag_estimated=estimated,
    )
