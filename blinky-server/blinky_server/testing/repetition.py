"""A run's repetition consistency (repS), label-free, for the stable proposals (docs/DEV_LOOP.md,
"Stable candidates from scores"): does repeated audio get repeated light?

Each track's repeat lag and repeated regions come with the corpus (blinky-corpus
``edm/repetition/<track>.json``, computed off the box from the clean audio by its
``scripts/repetition.py``, a copy of blinky-firmware's ``tests/tracker/consistency.py``). Here the
run's detections are only matched against them, with blinky-corpus's standard-library scorer
(``blinky_scoring.repetition.consistency``): detections in a repeated region against the
detections one lag later, at ±32 ms, strengths compared.

The detections are the drum scorer's (every transient from the audio start on), put on the
track's clock with the same lag the drum row reports (``lagMs``: the measured one when the
playback measured its start), so the row's ``lagSource`` describes repS too. Scored over the
window played, [seek, seek + duration]. Fewer than MIN_SCORED detections in repeated audio, or a
track without a map, is absent with the reason, never a number.
"""

from __future__ import annotations

from typing import Any

from blinky_scoring.repetition import consistency, load_repetition


def repetition_score(
    transients: list[Any],
    audio_start_ms: float,
    track: dict[str, Any],
    duration_ms: float,
    lag_ms: float | None,
) -> dict[str, Any]:
    """{repS, rep, coverage, n, lagS}, or {repS: None, why} when there is nothing to score."""
    path = track.get("repetition")
    if not path:
        return {"repS": None, "why": "no repetition map for this track"}
    if lag_ms is None:
        return {"repS": None, "why": "no lag for the run"}
    rep = load_repetition(path)  # a malformed map raises: never a silent zero
    seek = float(track["seek"])
    times, strengths = [], []
    for d in transients:
        rel = d.timestamp_ms - audio_start_ms
        if rel >= 0:
            times.append(seek + (rel - lag_ms) / 1000.0)
            strengths.append(float(d.strength))
    end = min(seek + duration_ms / 1000.0, rep.n_frames * rep.frame_s)
    c = consistency(times, strengths, rep, seek, end)
    out: dict[str, Any] = {
        "repS": None if c["rep_s"] is None else round(c["rep_s"], 4),
        "rep": None if c["rep"] is None else round(c["rep"], 4),
        "coverage": None if c["coverage"] is None else round(c["coverage"], 4),
        "n": c["n"],
        "lagS": round(rep.lag_s, 4),
    }
    if c["rep_s"] is None:
        out["why"] = f"{c['n']} detections in repeated audio (too few to score)"
    return out
