"""The chip's detection latency against the rig's calibration, from every scored box run.

A box run is scored at its measured lag: the DAC start plus the rig's fixed path,
``audio_player.AUDIO_PATH_MS`` (docs/TEST_BOX.md, "Audio start"). That fixed path includes the
chip's own detection latency on purpose: a firmware that detects later loses score instead of
having the lag searched away. The scorer still searches the music within +-100 ms of the
measured lag (blinky-corpus ``lag.py``, ``lagCheckMs``), so

    offset = lagCheckMs - lagMs

is how much later (positive) or sooner the chip detected than the calibrated firmware did. At
calibration (2026-11-25, 28 runs, firmware b211-dev-c5f4d51a) it was -6.1 to +6.6 ms with sd 2.9
on the 26 well-detected runs, median +0.55.

**Which runs count.** The music's check needs detections on the drums. On a track where the
chip catches few kicks it locks onto whatever few matches it finds, so its offset says nothing
about latency: at calibration dubstep-edm-halftime (kickR50 0.25, +57 ms) and edm-trap-electro
(kickR50 0.31, -16 ms) were the only runs outside +-7 ms, and every other run had kickR50 >= 0.58.
So a run counts when its lag was measured, the music gave a check, and its kickR50 is at least
``MIN_KICK_R50``. Every other run is listed with the reason, never dropped silently.

The blind spot this leaves: kickR50 is scored at the measured lag, so a drift past the +-50 ms
match tolerance would lower it and could leave the run out. Drifts that large also show as the
scorer's own ``lagNote`` ("disagrees with the measured one") and as falling drum scores; the
strict rule (``strict_rules`` ``detection_latency``, +-10 ms) warns long before that.
"""

from __future__ import annotations

import statistics
from typing import Any

# The fewest kicks the chip must catch (recall at +-50 ms) for the music's check to say anything
# about its latency (module docstring: the calibration runs split at 0.31 vs 0.58).
MIN_KICK_R50 = 0.5


def run_offset(drums: dict[str, Any] | None) -> dict[str, Any]:
    """One run's offset (ms, ``lagCheckMs - lagMs``), whether it counts, and why not."""
    if not drums:
        return {"offset_ms": None, "counted": False, "why": "no drum score"}
    if drums.get("lagSource") != "measured":
        return {"offset_ms": None, "counted": False, "why": "lag not measured"}
    check, lag = drums.get("lagCheckMs"), drums.get("lagMs")
    if check is None or lag is None:
        return {"offset_ms": None, "counted": False, "why": "the music gave no check"}
    offset = round(float(check) - float(lag), 1)
    k = drums.get("kickR50")
    if k is None or float(k) < MIN_KICK_R50:
        why = f"kickR50 {k} < {MIN_KICK_R50:g}: too few kicks detected for the music's check"
        return {"offset_ms": offset, "counted": False, "why": why}
    return {"offset_ms": offset, "counted": True, "why": None}


def summarize(results: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Per device, over a job's validation results ([{track, run?, scores: {device: summary}}]):
    the median offset of the runs that count (None when none does), how many counted, and every
    run's offset with its reason when it didn't count."""
    out: dict[str, dict[str, Any]] = {}
    for r in results:
        for dev, s in (r.get("scores") or {}).items():
            row = {"track": r.get("track"), **run_offset((s or {}).get("drums"))}
            if r.get("run") is not None:
                row["run"] = r["run"]
            out.setdefault(dev, {"runs": []})["runs"].append(row)
    for d in out.values():
        counted = [x["offset_ms"] for x in d["runs"] if x["counted"]]
        d["median_ms"] = round(statistics.median(counted), 1) if counted else None
        d["counted"] = len(counted)
    return out
