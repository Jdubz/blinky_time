#!/usr/bin/env python3
"""Which hits does the rhythm-pattern bias suppress, and why? (#340)

The bias (`onsetPatternBias`, 0.3) raises the picker's delta where the PLP pattern
predicts no hit. It is wanted: triggers prefer the music's pattern. This study finds
where it is wrong or too strict, to choose which improvement to try first
(docs/ML_IMPROVEMENT_PLAN.md, pattern-bias item).

Each track runs twice through the same harness: default bias ("on") and
--onset-pattern-bias=0 ("off"). A suppressed hit is an "off" onset with no "on"
onset within +-32 ms. For each suppressed hit:

  label     kick / snare / hat / other: the nearest cleaned drum label within 50 ms,
            after removing the run's lag (blinky_server drum_scoring)
  repeat    the hit lies in repeated audio (tests/tracker/consistency.py) and the
            "on" run fires its counterpart one repeat earlier or later: the bias
            made a repeat inconsistent
  lock      the tracker's local phase lock at that moment: max over m in {1,2,4}
            of |mean exp(2 pi i m phase)| over kick labels within +-4 s (>= 8 kicks).
            Compared with the same lock at the kicks the "on" run did fire
  near      a timing near-miss: the pattern at the hit's phase is below 0.5 but
            reaches 0.5 within +-2 frames (tolerance would have let it through)
  conf/val  PLP confidence and pattern value there (bias = 1 + 0.3 * conf * (1 - val))
  base val  the pattern value at the kicks the "on" run fired. `lock` can't see a
            constant phase offset (an offset tracker is still consistent); this can.
            High here and low at the suppressed kicks = the pattern is aligned and
            those kicks are off it; low at both = the pattern or phase is off.

Usage:
    python tests/tracker/pattern_bias_study.py --harness build/tracker_harness [--fps 60]
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from consistency import MATCH_TOL_S, find_repetition  # noqa: E402
from run_tracker_eval import (  # noqa: E402
    CORPUS,
    DEVICE_LEVEL_DB,
    WARMUP_S,
    decode,
    find_ffmpeg,
    lock_score,
    run_harness,
)
from blinky_server.testing.drum_scoring import (  # noqa: E402
    DRUM_TOL_SEC,
    drum_label_path,
    load_drum_labels,
    score_drums,
)

LOCK_WINDOW_S = 4.0
NEAR_FRAMES = 2
FRAME_S = 0.016


def edges(rows: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    t = rows["t_ms"] / 1000.0
    on = rows["onset"] > 0
    e = on & ~np.concatenate([[False], on[:-1]]) & (t >= WARMUP_S)
    return t[e], rows["onset"][e]


def has_near(xs: np.ndarray, t: float, tol: float) -> bool:
    j = np.searchsorted(xs, t)
    return any(0 <= k < len(xs) and abs(xs[k] - t) <= tol for k in (j - 1, j))


def nearest_label(t: float, labels) -> str:
    """kick / snare (whichever is closer) within 50 ms, else hat within 50 ms, else other."""
    def dist(xs) -> float:
        arr = np.asarray(xs)
        return float(np.min(np.abs(arr - t))) if len(arr) else float("inf")
    dk, ds, dh = dist(labels.kicks), dist(labels.snares), dist(labels.hats)
    if min(dk, ds) <= DRUM_TOL_SEC:
        return "kick" if dk <= ds else "snare"
    return "hat" if dh <= DRUM_TOL_SEC else "other"


def local_lock(t: float, kicks_h: np.ndarray, rt: np.ndarray, phase: np.ndarray) -> float:
    k = kicks_h[np.abs(kicks_h - t) <= LOCK_WINDOW_S]
    if len(k) < 8:
        return float("nan")
    idx = np.clip(np.searchsorted(rt, k), 0, len(rt) - 1)
    return lock_score(phase[idx])


def study_track(stem: str, exe: str, fps: float, ffmpeg: str, tmpd: Path) -> list[dict]:
    pcm = tmpd / f"{stem}.pcm"
    rep = find_repetition(decode(ffmpeg, CORPUS / f"{stem}.mp3", pcm, DEVICE_LEVEL_DB))
    labels = load_drum_labels(drum_label_path(CORPUS, stem), CORPUS / f"{stem}.onsets_consensus.json")
    on = run_harness(exe, pcm, tmpd / f"{stem}_on.csv", fps, 0.25, 1)
    off = run_harness(exe, pcm, tmpd / f"{stem}_off.csv", fps, 0.25, 1, ["--onset-pattern-bias=0"])
    t_on, _ = edges(on)
    t_off, _ = edges(off)
    end = float(on["t_ms"][-1] / 1000.0)
    lag = score_drums(list(zip(t_off, np.ones(len(t_off)), strict=True)), labels.window(WARMUP_S, end),
                      end - WARMUP_S).lag_ms or 0.0
    lag_s = lag / 1000.0
    rt = on["t_ms"] / 1000.0
    kicks_h = np.asarray(labels.kicks) + lag_s  # label times on the harness clock
    out = []
    for t in t_off:
        if has_near(t_on, t, MATCH_TOL_S):
            continue
        i = min(int(np.searchsorted(rt, t)), len(rt) - 1)
        val, conf = float(on["plp_value"][i]), float(on["plp_conf"][i])
        w = (rt >= t - NEAR_FRAMES * FRAME_S) & (rt <= t + NEAR_FRAMES * FRAME_S)
        L = rep.lag_s
        repeated = bool(rep.repeated(np.array([t]))[0] or rep.repeated(np.array([t - L]))[0])
        counterpart = has_near(t_on, t + L, MATCH_TOL_S) or has_near(t_on, t - L, MATCH_TOL_S)
        out.append({
            "track": stem, "t": float(t),
            "label": nearest_label(t - lag_s, labels),
            "breaks_repeat": repeated and counterpart,
            "lock": local_lock(t, kicks_h, rt, on["phase"]),
            "near": val < 0.5 and float(on["plp_value"][w].max()) >= 0.5,
            "conf": conf, "val": val,
        })
    # Baseline lock: at the kicks the "on" run fired.
    fired_kicks = [k for k in kicks_h if k >= WARMUP_S and has_near(t_on, k, DRUM_TOL_SEC)]
    base = [local_lock(k, kicks_h, rt, on["phase"]) for k in fired_kicks]
    vals = [float(on["plp_value"][min(int(np.searchsorted(rt, k)), len(rt) - 1)]) for k in fired_kicks]
    # Does the pattern know where kicks are? Its value at every kick vs at every frame
    # (after warm-up, while the bias can act: conf > 0.3).
    act = (rt >= WARMUP_S) & (on["plp_conf"] > 0.3)
    all_k = [k for k in kicks_h if k >= WARMUP_S]
    ki = np.clip(np.searchsorted(rt, all_k), 0, len(rt) - 1)
    ki = ki[act[ki]]
    v_k = float(np.median(on["plp_value"][ki])) if len(ki) else float("nan")
    v_all = float(np.median(on["plp_value"][act])) if act.any() else float("nan")
    # Max within +-2 frames: a narrow peak a frame or two off the kick (phase offset,
    # label timing) reads low at the kick itself but high here.
    rows_per = max(1, int(round(NEAR_FRAMES * FRAME_S / float(np.median(np.diff(rt))))))
    pv = on["plp_value"]
    v_k2 = float(np.median([pv[max(0, i - rows_per):i + rows_per + 1].max() for i in ki])) if len(ki) else float("nan")
    print(f"#   {stem}: pattern value median at kicks {v_k:.2f} (max +-2 frames {v_k2:.2f}) "
          f"vs all frames {v_all:.2f}  (bias active {act[rt >= WARMUP_S].mean():.0%} of the time)", flush=True)
    for r in out:
        r["base_lock"] = float(np.nanmedian(base)) if np.isfinite(base).any() else float("nan")
        r["base_val"] = float(np.median(vals)) if vals else float("nan")
    n_off = len(t_off)
    print(f"{stem:28s} off {n_off:5d}  suppressed {len(out):4d}  "
          + "  ".join(f"{k} {sum(r['label'] == k for r in out):3d}" for k in ("kick", "snare", "hat", "other"))
          + f"  breaksRepeat {sum(r['breaks_repeat'] for r in out):3d}  near {sum(r['near'] for r in out):3d}",
          flush=True)
    return out


def summarize(rows: list[dict]) -> None:
    n = len(rows)
    if not n:
        print("no suppressed hits")
        return
    print(f"\nSUPPRESSED HITS: {n}")
    for k in ("kick", "snare", "hat", "other"):
        sub = [r for r in rows if r["label"] == k]
        if not sub:
            continue
        lk = np.array([r["lock"] for r in sub], dtype=float)
        print(f"  {k:6s} {len(sub):5d} ({len(sub) / n:5.1%})  breaks a repeat {np.mean([r['breaks_repeat'] for r in sub]):5.1%}"
              f"  timing near-miss {np.mean([r['near'] for r in sub]):5.1%}"
              f"  lock median {np.nanmedian(lk):.2f}  conf {np.mean([r['conf'] for r in sub]):.2f}"
              f"  val {np.mean([r['val'] for r in sub]):.2f}")
    drums = [r for r in rows if r["label"] in ("kick", "snare")]
    if drums:
        lk = np.array([r["lock"] for r in drums], dtype=float)
        base = np.array([r["base_lock"] for r in drums], dtype=float)
        print(f"  suppressed drums: tracker lock median {np.nanmedian(lk):.2f} vs {np.nanmedian(base):.2f} "
              f"at fired kicks (lower = the tracker was off there)")
        print(f"  pattern value: {np.median([r['val'] for r in drums]):.2f} at suppressed drums vs "
              f"{np.nanmedian([r['base_val'] for r in drums]):.2f} at fired kicks (same tracks)")
        print(f"  suppressed drums that are timing near-misses: {np.mean([r['near'] for r in drums]):.1%}; "
              f"that break a repeat: {np.mean([r['breaks_repeat'] for r in drums]):.1%}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--tracks", nargs="*")
    ap.add_argument("--ffmpeg")
    ap.add_argument("--json", help="write every suppressed hit here")
    args = ap.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    stems = args.tracks or sorted(p.name[:-len(".onsets_consensus.json")]
                                  for p in CORPUS.glob("*.onsets_consensus.json"))
    rows: list[dict] = []
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems:
            rows += study_track(stem, args.harness, args.fps, ffmpeg, Path(tmp))
    summarize(rows)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
