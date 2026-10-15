#!/usr/bin/env python3
"""Evaluate firmware rhythm tracking with the host tracker harness.

Runs tracker_harness on each track at several simulated main-loop rates and
reports, per run:

  lock      Phase consistency at ground-truth onsets (strength >= 0.8) after
            warm-up: max over m in {1,2,4} of |mean(exp(2*pi*i*m*phase))|.
            1 = every strong onset lands at the same place in the rhythm
            cycle (or on a fixed 2-/4-subdivision of it); ~0 = phase unrelated
            to the music. Onsets, not beats, are the ground truth here
            (CLAUDE.md: onset task) — strong onsets in this corpus sit on the
            rhythm grid, so a locked phase concentrates them.
  excess    lock minus the median lock of the same phase trace sampled at
            uniformly random times (same count). A phase trace that sits in
            some range more than others scores high against ANY onset set;
            `excess` removes that bias. ~0 = no real alignment. Use this,
            not raw `lock`, to compare builds.
  worst20s  Minimum `lock` over 20 s windows — catches drift that a
            whole-track average hides.
  tempo     Fraction of post-warm-up time where the tracked rhythm period is
            a musically valid multiple (1/2, 1, 2 or 4 beats, +-4%) of the
            track's tempo from `.pattern_gt.json`. Used only to judge the
            visualizer's period choice — never as an onset target.
  recall    Fraction of strong GT onsets with a fired onset within +-50 ms.
  bpm       Median BPM after warm-up (look for loop-rate dependence).

Usage:
    python tests/tracker/run_tracker_eval.py --harness build/tracker_harness.exe \
        [--compare build/tracker_old.exe] [--tracks techno-minimal-01 ...] \
        [--fps 35 60 90] [--ffmpeg path/to/ffmpeg]

ffmpeg is needed to decode the corpus mp3s (falls back to imageio_ffmpeg).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
CORPUS = REPO / "blinky-test-player" / "music" / "edm"
WARMUP_S = 10.0
STRONG = 0.8
TOL_S = 0.05


def find_ffmpeg(explicit: str | None) -> str:
    if explicit:
        return explicit
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg  # type: ignore
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        sys.exit("ERROR: ffmpeg not found (install it, pass --ffmpeg, or pip install imageio-ffmpeg)")


def decode(ffmpeg: str, src: Path, dst: Path) -> None:
    subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(src), "-ac", "1", "-ar", "16000",
                    "-f", "s16le", str(dst)], check=True)


def run_harness(exe: str, pcm: Path, out: Path, fps: float, jitter: float, seed: int) -> np.ndarray:
    subprocess.run([exe, str(pcm), str(out), str(fps), str(jitter), str(seed)], check=True)
    return np.genfromtxt(out, delimiter=",", names=True)


def lock_score(phases: np.ndarray) -> float:
    if len(phases) < 8:
        return float("nan")
    return max(abs(np.exp(2j * np.pi * m * phases).mean()) for m in (1, 2, 4))


def tempo_score(rows: np.ndarray, gt_bpm: float | None) -> float:
    if not gt_bpm:
        return float("nan")
    t = rows["t_ms"] / 1000.0
    p = rows["period_f"][t >= WARMUP_S]
    beat = 62.5 * 60.0 / gt_bpm
    ok = np.zeros(len(p), dtype=bool)
    for m in (0.5, 1.0, 2.0, 4.0):
        ok |= np.abs(p / (beat * m) - 1.0) <= 0.04
    return float(ok.mean()) if len(p) else float("nan")


def evaluate(rows: np.ndarray, onsets: np.ndarray) -> dict:
    t = rows["t_ms"] / 1000.0
    strong = onsets[onsets >= WARMUP_S]
    strong = strong[strong <= t[-1]]
    idx = np.clip(np.searchsorted(t, strong), 0, len(t) - 1)
    ph = rows["phase"][idx]
    windows = []
    for w0 in np.arange(WARMUP_S, t[-1] - 20.0, 10.0):
        sel = (strong >= w0) & (strong < w0 + 20.0)
        if sel.sum() >= 8:
            windows.append(lock_score(ph[sel]))
    rng = np.random.default_rng(0)
    nulls = [lock_score(rows["phase"][np.clip(np.searchsorted(
                 t, np.sort(rng.uniform(WARMUP_S, t[-1], len(strong)))), 0, len(t) - 1)])
             for _ in range(40)]
    fired = t[rows["onset"] > 0]
    hits = 0
    for o in strong:
        j = np.searchsorted(fired, o)
        near = [abs(fired[k] - o) for k in (j - 1, j) if 0 <= k < len(fired)]
        if near and min(near) <= TOL_S:
            hits += 1
    return {
        "lock": lock_score(ph),
        "excess": lock_score(ph) - float(np.median(nulls)),
        "worst20s": min(windows) if windows else float("nan"),
        "recall": hits / len(strong) if len(strong) else float("nan"),
        "bpm": float(np.median(rows["bpm"][t >= WARMUP_S])),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--compare", help="second harness binary (e.g. built from the previous firmware)")
    ap.add_argument("--tracks", nargs="*", help="track stems (default: whole corpus)")
    ap.add_argument("--fps", nargs="*", type=float, default=[35.0, 60.0, 90.0])
    ap.add_argument("--jitter", type=float, default=0.25)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ffmpeg")
    ap.add_argument("--min-excess", type=float, help="fail if the new build's mean excess is below this")
    ap.add_argument("--min-tempo", type=float, help="fail if the new build's mean tempo score is below this")
    ap.add_argument("--min-recall", type=float, help="fail if the new build's mean recall is below this")
    args = ap.parse_args()

    ffmpeg = find_ffmpeg(args.ffmpeg)
    stems = args.tracks or sorted(p.name[:-len(".onsets_consensus.json")]
                                  for p in CORPUS.glob("*.onsets_consensus.json"))
    builds = [("new", args.harness)] + ([("old", args.compare)] if args.compare else [])

    summary: dict[str, list] = {b: [] for b, _ in builds}
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        print(f"{'track':32s} {'fps':>5s} " + "  ".join(
            f"{b+':exc':>8s} {b+':tmp':>8s} {b+':worst':>9s} {b+':rec':>7s} {b+':bpm':>8s}" for b, _ in builds))
        for stem in stems:
            mp3 = CORPUS / f"{stem}.mp3"
            gt = json.loads((CORPUS / f"{stem}.onsets_consensus.json").read_text())
            onsets = np.array(sorted(o["time"] for o in gt["onsets"] if o["strength"] >= STRONG))
            pgt = CORPUS / f"{stem}.pattern_gt.json"
            gt_bpm = json.loads(pgt.read_text()).get("groundTruthBpm") if pgt.exists() else None
            pcm = tmpd / f"{stem}.pcm"
            decode(ffmpeg, mp3, pcm)
            for fps in args.fps:
                cells = []
                for b, exe in builds:
                    rows = run_harness(exe, pcm, tmpd / f"{stem}_{b}.csv", fps, args.jitter, args.seed)
                    r = evaluate(rows, onsets)
                    r["tempo"] = tempo_score(rows, gt_bpm)
                    r["has_gt_bpm"] = bool(gt_bpm)
                    r["label"] = f"{stem}@{fps:.0f}fps"
                    summary[b].append(r)
                    cells.append(f"{r['excess']:8.2f} {r['tempo']:8.2f} {r['worst20s']:9.2f} {r['recall']:7.2f} {r['bpm']:8.1f}")
                print(f"{stem:32s} {fps:5.0f} " + "  ".join(cells), flush=True)
    print()
    for b, rs in summary.items():
        arr = lambda k: np.nanmean([r[k] for r in rs])
        print(f"{b}: mean lock {arr('lock'):.3f}  mean excess {arr('excess'):.3f}  "
              f"mean tempo {arr('tempo'):.3f}  mean worst20s {arr('worst20s'):.3f}  "
              f"mean recall {arr('recall'):.3f}")

    # Regression gate (CI): floors apply to the new build's corpus means. Any
    # non-finite per-run value fails outright, so a broken run can't hide inside
    # the mean. The one legitimate NaN is tempo on a track with no annotated BPM.
    failed = []
    for key, floor in (("excess", args.min_excess), ("tempo", args.min_tempo), ("recall", args.min_recall)):
        if floor is None:
            continue
        runs = [r for r in summary["new"] if key != "tempo" or r["has_gt_bpm"]]
        bad = [r["label"] for r in runs if not np.isfinite(r[key])]
        if bad:
            failed.append(f"non-finite {key} in {len(bad)} run(s): {', '.join(bad)}")
            continue
        mean = float(np.mean([r[key] for r in runs])) if runs else float("nan")
        if not np.isfinite(mean) or mean < floor:
            failed.append(f"mean {key} {mean:.3f} < floor {floor:.3f}")
    for msg in failed:
        print(f"[GATE FAIL] {msg}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
