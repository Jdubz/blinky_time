#!/usr/bin/env python3
"""Does a hit's position inside its spectral frame decide whether it fires? (#178, #340)

Spectral frames are 256 samples (16 ms) with hop = frame size and a Hamming
window, so the same hit lands early, mid or late in a frame depending on where
the frame boundaries fall. This study shifts each track by s in {0, 32, ..., 224}
samples (one frame = 8 shifts) by prepending silence, runs the same harness on
each, and maps onsets back to the audio clock. Nothing differs between the runs
but the frame phase.

Per kick or snare label (cleaned drum labels, after removing the run's lag):

  fires     how many of the 8 shifts detect it within 50 ms (blinky_server
            drum_scoring tolerance). 8/8 = always, 0/8 = never, 1..7 = phase-sensitive
  strength  the onset strength across the shifts that fire: its coefficient of
            variation, for hits that fire in all 8 (how much the flash wobbles)

And per repeated pair (a drum label and a same-kind label one repeat lag later,
both in repeated audio, tests/tracker/consistency.py): the pair is inconsistent at
a shift when exactly one of the two fires. The study reports how many inconsistent
pairs involve a phase-sensitive hit, i.e. how much of the "same hit, different
bar, different result" comes from frame phase.

--control keeps the frame phase fixed (no shift) and varies the harness's
loop-jitter seed instead (1..8). Whatever varies there comes from the loop
timing and the tracker's adaptive state, not from frame phase; subtract it.

Usage:
    python tests/tracker/frame_phase_study.py --harness build/tracker_harness [--fps 60] [--control]
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
    run_harness,
)
from blinky_server.testing.drum_scoring import (  # noqa: E402
    DRUM_TOL_SEC,
    drum_label_path,
    load_drum_labels,
    score_drums,
)

SAMPLE_RATE = 16000
FRAME = 256
SHIFTS = list(range(0, FRAME, 32))


def edges(rows: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    t = rows["t_ms"] / 1000.0
    on = rows["onset"] > 0
    e = on & ~np.concatenate([[False], on[:-1]])
    return t[e], rows["onset"][e]


def nearest(xs: np.ndarray, t: float) -> int:
    """Index of the element of sorted xs closest to t (-1 if xs is empty)."""
    if not len(xs):
        return -1
    j = int(np.searchsorted(xs, t))
    cands = [k for k in (j - 1, j) if 0 <= k < len(xs)]
    return min(cands, key=lambda k: abs(xs[k] - t))


def study_track(stem: str, exe: str, fps: float, ffmpeg: str, tmpd: Path, control: bool) -> dict:
    pcm = tmpd / f"{stem}.pcm"
    rep = find_repetition(decode(ffmpeg, CORPUS / f"{stem}.mp3", pcm, DEVICE_LEVEL_DB))
    base = np.fromfile(pcm, dtype="<i2")
    labels = load_drum_labels(drum_label_path(CORPUS, stem), CORPUS / f"{stem}.onsets_consensus.json")
    runs = []  # per shift: (onset times on the audio clock, strengths)
    for i, shift in enumerate(SHIFTS):
        s, seed = (0, i + 1) if control else (shift, 1)
        p = tmpd / f"{stem}_{s}.pcm"
        np.concatenate([np.zeros(s, dtype="<i2"), base]).tofile(p)
        rows = run_harness(exe, p, tmpd / f"{stem}_{i}.csv", fps, 0.25, seed)
        t, v = edges(rows)
        runs.append((t - s / SAMPLE_RATE, v))
    t0 = runs[0][0]
    end = float(t0[-1]) if len(t0) else WARMUP_S
    lag_s = (score_drums(list(zip(t0[t0 >= WARMUP_S], np.ones(int((t0 >= WARMUP_S).sum())), strict=True)),
                         labels.window(WARMUP_S, end), end - WARMUP_S).lag_ms or 0.0) / 1000.0

    hits = []  # (kind, label time, fires per shift, strength per shift)
    for kind, xs in (("kick", labels.kicks), ("snare", labels.snares)):
        for lt in xs:
            if lt + lag_s < WARMUP_S or lt + lag_s > end:
                continue
            fires, strs = [], []
            for t, v in runs:
                k = nearest(t, lt + lag_s)
                ok = k >= 0 and abs(t[k] - (lt + lag_s)) <= DRUM_TOL_SEC
                fires.append(ok)
                strs.append(float(v[k]) if ok else float("nan"))
            hits.append((kind, float(lt), fires, strs))

    # Repeated pairs: same-kind label one repeat lag later, both in repeated audio.
    L = rep.lag_s
    pairs = incons = incons_phase = 0
    for kind in ("kick", "snare"):
        hk = [h for h in hits if h[0] == kind]
        times = np.array([h[1] for h in hk])
        for i, h in enumerate(hk):
            j = nearest(times, h[1] + L)
            if j < 0 or j == i or abs(times[j] - (h[1] + L)) > MATCH_TOL_S:
                continue
            if not rep.repeated(np.array([h[1], times[j]])).all():
                continue
            a, b = h[2], hk[j][2]
            pairs += 1
            for s in range(len(SHIFTS)):
                if a[s] != b[s]:
                    incons += 1
                    miss = b if a[s] else a
                    if 0 < sum(miss) < len(SHIFTS):
                        incons_phase += 1

    n = len(hits)
    counts = np.array([sum(h[2]) for h in hits]) if n else np.zeros(0)
    always = int((counts == len(SHIFTS)).sum())
    never = int((counts == 0).sum())
    cvs = [np.std(h[3]) / np.mean(h[3]) for h in hits if sum(h[2]) == len(SHIFTS) and np.mean(h[3]) > 0]
    out = {
        "track": stem, "hits": n, "always": always, "never": never, "sensitive": n - always - never,
        "recall_s0": float(np.mean([h[2][0] for h in hits])) if n else float("nan"),
        "recall_mean": float(counts.mean() / len(SHIFTS)) if n else float("nan"),
        "recall_any": float((counts > 0).mean()) if n else float("nan"),
        "strength_cv": float(np.median(cvs)) if cvs else float("nan"),
        "pairs": pairs, "pair_shifts": pairs * len(SHIFTS),
        "inconsistent": incons, "inconsistent_phase": incons_phase,
        "counts": counts.tolist(),
    }
    print(f"{stem:28s} hits {n:4d}  always {always / max(n, 1):5.1%}  phase-sensitive "
          f"{out['sensitive'] / max(n, 1):5.1%}  never {never / max(n, 1):5.1%}  "
          f"recall mean {out['recall_mean']:.3f} (any shift {out['recall_any']:.3f})  "
          f"strength CV {out['strength_cv']:.2f}  repeat pairs {pairs:4d}  "
          f"inconsistent {incons / max(pairs * len(SHIFTS), 1):5.1%} "
          f"(phase {incons_phase / max(incons, 1):5.1%})", flush=True)
    return out


def summarize(rows: list[dict], control: bool) -> None:
    n = sum(r["hits"] for r in rows)
    if not n:
        print("no hits")
        return
    al, nv, se = (sum(r[k] for r in rows) for k in ("always", "never", "sensitive"))
    counts = np.concatenate([np.asarray(r["counts"]) for r in rows])
    ps = sum(r["pair_shifts"] for r in rows)
    inc = sum(r["inconsistent"] for r in rows)
    incp = sum(r["inconsistent_phase"] for r in rows)
    what = f"{len(SHIFTS)} jitter seeds, no shift (CONTROL)" if control else \
        f"{len(SHIFTS)} shifts of {FRAME // len(SHIFTS)} samples"
    print(f"\nFRAME PHASE ({what}, kicks + snares, {n} hits)")
    print(f"  always fire {al / n:5.1%}   phase-sensitive {se / n:5.1%}   never fire {nv / n:5.1%}")
    print(f"  recall: mean over shifts {counts.mean() / len(SHIFTS):.3f}, if every phase-sensitive hit "
          f"fired {(counts > 0).mean():.3f}")
    print("  hits by shifts fired: " + "  ".join(f"{k}:{int((counts == k).sum())}" for k in range(len(SHIFTS) + 1)))
    cv = np.array([r["strength_cv"] for r in rows], dtype=float)
    print(f"  strength CV across shifts at always-firing hits: median of tracks {np.nanmedian(cv):.2f}")
    print(f"  repeated pairs: {ps // len(SHIFTS)} ; inconsistent (exactly one fires) {inc / max(ps, 1):5.1%} "
          f"of pair-shifts; of those, the missing hit is phase-sensitive in {incp / max(inc, 1):5.1%}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--tracks", nargs="*")
    ap.add_argument("--ffmpeg")
    ap.add_argument("--json", help="write the per-track results here")
    ap.add_argument("--control", action="store_true", help="vary the jitter seed, not the frame phase")
    args = ap.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    stems = args.tracks or sorted(p.name[:-len(".onsets_consensus.json")]
                                  for p in CORPUS.glob("*.onsets_consensus.json"))
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems:
            rows.append(study_track(stem, args.harness, args.fps, ffmpeg, Path(tmp), args.control))
    summarize(rows, args.control)
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
