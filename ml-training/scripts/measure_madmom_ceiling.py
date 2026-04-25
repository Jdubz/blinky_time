"""Measure madmom CNN/RNN onset detector F1 on validation tracks.

Run with venv311 (madmom requires Python 3.11):
    venv311/bin/python scripts/measure_madmom_ceiling.py

Reads ground-truth onsets from .beats.json files (the same labels the
device validation pipeline scores against) and runs madmom's
CNNOnsetProcessor + RNNOnsetProcessor on each audio file. Reports F1 at
the same tolerances the device validation reports (50/70/100/150 ms) so
the numbers are directly comparable to v32's on-device F1=0.265.

Purpose: establish the algorithmic-detector ceiling on this corpus
before launching another training run. If madmom hits e.g. F1=0.85, the
gap to v32 (0.27) is real and closing it likely needs the input-
representation fixes from the 2026-04-25 synthesis. If madmom hits
e.g. F1=0.55, the validation labels themselves cap us near where we
already are.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import mir_eval
import numpy as np
from madmom.features.onsets import (
    CNNOnsetProcessor,
    OnsetPeakPickingProcessor,
    RNNOnsetProcessor,
)

TOLERANCES_S = [0.05, 0.07, 0.10, 0.15]
# Default to the corpus that lives in the repo, resolved relative to this
# script. Earlier versions hardcoded an absolute developer path which broke
# the moment the script ran on any other machine. Override by passing an
# explicit dir as argv[1].
DEFAULT_TRACK_DIR = (
    Path(__file__).resolve().parents[2] / "blinky-test-player/music/edm_holdout"
)


def load_ground_truth(beats_json_path: Path) -> np.ndarray:
    """Read .beats.json and return the array of expected-trigger onset times in seconds.

    Mirrors the validation pipeline: only `expectTrigger=true` hits count.
    """
    data = json.loads(beats_json_path.read_text())
    times = [
        h["time"] for h in data.get("hits", []) if h.get("expectTrigger", True)
    ]
    return np.asarray(sorted(times), dtype=np.float64)


def detect_madmom(audio_path: Path, processor, peak_picker) -> np.ndarray:
    """Run a pre-instantiated madmom onset processor on one track."""
    activation = processor(str(audio_path))
    onsets = peak_picker(activation)
    return np.asarray(onsets, dtype=np.float64)


def evaluate_track(detected: np.ndarray, reference: np.ndarray) -> dict:
    """Per-tolerance F1/P/R for one track using mir_eval.onset.f_measure."""
    out = {}
    for tol in TOLERANCES_S:
        if reference.size == 0 and detected.size == 0:
            f1 = p = r = float("nan")
        elif reference.size == 0:
            f1 = 0.0
            p = 0.0
            r = float("nan")
        elif detected.size == 0:
            f1 = 0.0
            p = float("nan")
            r = 0.0
        else:
            f1, p, r = mir_eval.onset.f_measure(reference, detected, window=tol)
        out[f"F1_{int(tol*1000)}ms"] = float(f1)
        out[f"P_{int(tol*1000)}ms"] = float(p) if not np.isnan(p) else None
        out[f"R_{int(tol*1000)}ms"] = float(r) if not np.isnan(r) else None
    out["count_detected"] = int(detected.size)
    out["count_reference"] = int(reference.size)
    return out


def main() -> int:
    track_dir = DEFAULT_TRACK_DIR if len(sys.argv) < 2 else Path(sys.argv[1])
    if not track_dir.exists():
        print(f"ERROR: track dir not found: {track_dir}", file=sys.stderr)
        print(
            "Usage: venv311/bin/python scripts/measure_madmom_ceiling.py [track_dir]",
            file=sys.stderr,
        )
        return 1
    audio_files = sorted(
        p
        for p in track_dir.iterdir()
        if p.suffix.lower() in {".wav", ".mp3", ".flac", ".ogg", ".m4a"}
    )
    if not audio_files:
        print(f"No audio files in {track_dir}", file=sys.stderr)
        return 1

    print(f"Track dir: {track_dir}")
    print(f"Found {len(audio_files)} audio files")
    print(f"Tolerances: {[f'{int(t*1000)}ms' for t in TOLERANCES_S]}\n")

    # Instantiate processors ONCE — each constructor loads DNN weights from
    # disk, and prior versions of this script accidentally re-instantiated
    # inside the per-track loop, adding ~5 minutes of redundant model loads
    # on a 25-track set.
    cnn = CNNOnsetProcessor()
    rnn = RNNOnsetProcessor()
    pp = OnsetPeakPickingProcessor(fps=100)

    per_track_results = []
    for i, audio_path in enumerate(audio_files, 1):
        beats_json = audio_path.with_name(audio_path.stem + ".beats.json")
        if not beats_json.exists():
            print(f"  [{i}/{len(audio_files)}] {audio_path.stem}: no .beats.json, skipping")
            continue
        reference = load_ground_truth(beats_json)
        if reference.size == 0:
            print(f"  [{i}/{len(audio_files)}] {audio_path.stem}: empty .beats.json, skipping")
            continue

        t0 = time.time()
        cnn_onsets = detect_madmom(audio_path, cnn, pp)
        rnn_onsets = detect_madmom(audio_path, rnn, pp)
        t = time.time() - t0

        cnn_score = evaluate_track(cnn_onsets, reference)
        rnn_score = evaluate_track(rnn_onsets, reference)
        per_track_results.append(
            {
                "track": audio_path.stem,
                "ref_count": int(reference.size),
                "cnn": cnn_score,
                "rnn": rnn_score,
                "elapsed_s": round(t, 2),
            }
        )
        print(
            f"  [{i}/{len(audio_files)}] {audio_path.stem}: "
            f"ref={reference.size:3d} "
            f"CNN F1@70={cnn_score['F1_70ms']:.3f} F1@100={cnn_score['F1_100ms']:.3f} "
            f"RNN F1@70={rnn_score['F1_70ms']:.3f} F1@100={rnn_score['F1_100ms']:.3f} "
            f"({t:.1f}s)"
        )

    # Aggregate
    print("\n=== Aggregate (mean across tracks) ===")
    print(f"{'detector':<10} {'F1@50ms':<10}{'F1@70ms':<10}{'F1@100ms':<10}{'F1@150ms':<10}{'P@100ms':<10}{'R@100ms':<10}")
    for det_key in ("cnn", "rnn"):
        scores_per_tol = {f"F1_{int(t*1000)}ms": [] for t in TOLERANCES_S}
        p100 = []
        r100 = []
        for r in per_track_results:
            for k in scores_per_tol:
                v = r[det_key].get(k)
                if v is not None and not np.isnan(v):
                    scores_per_tol[k].append(v)
            p = r[det_key].get("P_100ms")
            if p is not None and not np.isnan(p):
                p100.append(p)
            rec = r[det_key].get("R_100ms")
            if rec is not None and not np.isnan(rec):
                r100.append(rec)
        means = {k: float(np.mean(v)) for k, v in scores_per_tol.items() if v}
        print(
            f"{det_key:<10} "
            f"{means.get('F1_50ms', 0):<10.3f}"
            f"{means.get('F1_70ms', 0):<10.3f}"
            f"{means.get('F1_100ms', 0):<10.3f}"
            f"{means.get('F1_150ms', 0):<10.3f}"
            f"{(np.mean(p100) if p100 else 0):<10.3f}"
            f"{(np.mean(r100) if r100 else 0):<10.3f}"
        )

    out = {
        "track_dir": str(track_dir),
        "n_tracks": len(per_track_results),
        "tolerances_s": TOLERANCES_S,
        "results": per_track_results,
    }
    out_path = Path("outputs/madmom_ceiling_edm_holdout.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2))
    print(f"\nFull per-track results → {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
