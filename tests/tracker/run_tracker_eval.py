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
  f1, prec  Onset F1 and precision after warm-up, scored exactly as on a
            device (blinky-server's scoring: reference = onsets with strength
            >= 0.6, deduped at 70 ms; latency estimated and removed; greedy
            matching at +-100 ms; f1@50 at +-50 ms). Same numbers as
            `tools/device_eval.py` and `POST /api/test/validate`, so a host
            result and a device result can be compared directly.
  bpm       Median BPM after warm-up (look for loop-rate dependence).

Drum metrics, the PRIMARY score (#294, CLAUDE.md: the target is kicks and snares),
against cleaned `kick_weighted` labels (blinky_server/testing/drum_scoring.py), after
warm-up, one-to-one, with a constant per-run lag removed first:

  kR50 kR30 kick recall at +-50 / +-30 ms
  sR50      snare recall at +-50 ms (PROVISIONAL until the label fixes in #284)
  onDrum    share of detections within +-50 ms of a kick or snare
  drumF     F1 against kicks + snares at +-50 ms
  loneHat   share of lone hats (no kick/snare within 50 ms) that fired a detection
  det/s     detections per second

Usage:
    python tests/tracker/run_tracker_eval.py --harness build/tracker_harness.exe \
        [--compare build/tracker_old.exe] [--tracks techno-minimal-01 ...] \
        [--fps 35 60 90] [--ffmpeg path/to/ffmpeg]

ffmpeg is needed to decode the corpus mp3s (falls back to imageio_ffmpeg).
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "blinky-server"))
from blinky_server.testing.scoring import (  # noqa: E402
    MIN_ONSET_STRENGTH,
    ONSET_DEDUP_SEC,
    ONSET_TOLERANCE_SEC,
    PLAUSIBLE_LATENCY_MAX,
    PLAUSIBLE_LATENCY_MIN,
    estimate_audio_latency,
    match_events_f1,
)
from blinky_server.testing.drum_scoring import (  # noqa: E402
    drum_label_path,
    load_drum_labels,
    score_drums,
)

CORPUS = REPO / "blinky-test-player" / "music" / "edm"
WARMUP_S = 10.0
STRONG = 0.8
TOL_S = 0.05
# Playback level, dB re the decoded (mastered) file. The corpus peaks near
# full scale, but the device's mic does not: hat_v1 with music peaks at
# ~0.0028-0.004 FS, about -50 dB. Testing at file level hid that the
# first SuperFlux lambda produced no onsets at all on the device (#280), so
# the default is the device's level; the PCM is re-quantized to int16 after
# scaling, as the mic's samples are.
DEVICE_LEVEL_DB = -50.0


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


def decode(ffmpeg: str, src: Path, dst: Path, level_db: float = 0.0) -> None:
    subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(src), "-ac", "1", "-ar", "16000",
                    "-f", "s16le", str(dst)], check=True)
    if level_db != 0.0:
        x = np.fromfile(dst, dtype="<i2").astype(np.float64) * 10 ** (level_db / 20)
        np.clip(np.round(x), -32768, 32767).astype("<i2").tofile(dst)


def run_harness(exe: str, pcm: Path, out: Path, fps: float, jitter: float, seed: int,
                extra: list[str] | None = None) -> np.ndarray:
    subprocess.run([exe, str(pcm), str(out), str(fps), str(jitter), str(seed), *(extra or [])],
                   check=True)
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


def onset_f1(rows: np.ndarray, gt_onsets: list[dict]) -> dict:
    """Onset F1/precision after warm-up, with blinky-server's device scoring."""
    t = rows["t_ms"] / 1000.0
    on = rows["onset"] > 0
    # One detection per onset: a delivered onset can span consecutive loop rows.
    edge = on & ~np.concatenate([[False], on[:-1]])
    sel = edge & (t >= WARMUP_S)
    det = [{"timestamp_ms": float(ms), "strength": float(v)}
           for ms, v in zip(rows["t_ms"][sel], rows["onset"][sel], strict=True)]
    end = float(t[-1])
    hits = [{"time": o["time"], "strength": o.get("strength", 1.0), "expect_trigger": True}
            for o in gt_onsets if WARMUP_S <= o["time"] <= end]
    lat = estimate_audio_latency(det, hits, end * 1000)
    if lat is None or not PLAUSIBLE_LATENCY_MIN <= lat <= PLAUSIBLE_LATENCY_MAX:
        return {"f1": float("nan"), "prec": float("nan"), "f1_50": float("nan"), "lat": lat}
    ref: list[float] = []
    for x in sorted(h["time"] for h in hits if h["strength"] >= MIN_ONSET_STRENGTH):
        if not ref or x - ref[-1] > ONSET_DEDUP_SEC:
            ref.append(x)
    est = [(d["timestamp_ms"] - lat) / 1000 for d in det]
    r100 = match_events_f1(est, ref, ONSET_TOLERANCE_SEC)
    r50 = match_events_f1(est, ref, 0.05)
    return {"f1": r100["f1"], "prec": r100["precision"], "f1_50": r50["f1"], "lat": lat}


def drum_metrics(rows: np.ndarray, labels) -> dict:
    """Drum scores after warm-up, on the same onset edges as `onset_f1`."""
    t = rows["t_ms"] / 1000.0
    on = rows["onset"] > 0
    edge = on & ~np.concatenate([[False], on[:-1]]) & (t >= WARMUP_S)
    end = float(t[-1])
    det = [(float(ms) / 1000.0, float(v)) for ms, v in zip(rows["t_ms"][edge], rows["onset"][edge],
                                                          strict=True)]
    d = score_drums(det, labels.window(WARMUP_S, end), end - WARMUP_S)
    return {"kick_r50": d.kick_r50, "kick_r30": d.kick_r30, "snare_r50": d.snare_r50,
            "on_drum": d.on_drum, "drum_f": d.drum_f, "lone_hat": d.lone_hat,
            "det_per_s": d.det_per_s, "drum_lag": float("nan") if d.lag_ms is None else d.lag_ms}


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
    ap.add_argument("--level-db", type=float, default=DEVICE_LEVEL_DB,
                    help=f"playback level, dB re the decoded file (default {DEVICE_LEVEL_DB:g}: "
                         "the device mic's level with music; 0 = file level)")
    ap.add_argument("--harness-args", nargs="*", default=[],
                    help="extra positional args after the seed: plpPhaseGain tempoPriorWidth "
                         "syncopatedCandidateScale (tuning sweeps; the new build only)")
    ap.add_argument("--harness-opt", action="append", default=[],
                    help="a --name=value option for the new build's harness, e.g. "
                         "--harness-opt=--onset-pattern-bias=0 (repeatable)")
    ap.add_argument("--min-excess", type=float, help="fail if the new build's mean excess is below this")
    ap.add_argument("--min-tempo", type=float, help="fail if the new build's mean tempo score is below this")
    ap.add_argument("--min-recall", type=float, help="fail if the new build's mean recall is below this")
    ap.add_argument("--min-f1", type=float, help="fail if the new build's mean onset F1 is below this")
    ap.add_argument("--min-kick-recall", type=float,
                    help="fail if the new build's mean kick recall at +-50 ms is below this")
    ap.add_argument("--min-on-drum", type=float,
                    help="fail if the new build's mean on-drum share is below this")
    ap.add_argument("--min-drum-f", type=float, help="fail if the new build's mean drum F is below this")
    args = ap.parse_args()

    ffmpeg = find_ffmpeg(args.ffmpeg)
    stems = args.tracks or sorted(p.name[:-len(".onsets_consensus.json")]
                                  for p in CORPUS.glob("*.onsets_consensus.json"))
    builds = [("new", args.harness)] + ([("old", args.compare)] if args.compare else [])

    summary: dict[str, list] = {b: [] for b, _ in builds}
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        print(f"{'track':32s} {'fps':>5s} " + "  ".join(
            f"{b+':kR50':>8s} {b+':onDrum':>9s} {b+':drumF':>8s} {b+':sR50':>7s} "
            f"{b+':exc':>8s} {b+':tmp':>8s} {b+':worst':>9s} {b+':rec':>7s} {b+':f1':>7s} "
            f"{b+':prec':>8s} {b+':bpm':>8s}" for b, _ in builds))
        for stem in stems:
            mp3 = CORPUS / f"{stem}.mp3"
            gt = json.loads((CORPUS / f"{stem}.onsets_consensus.json").read_text())
            onsets = np.array(sorted(o["time"] for o in gt["onsets"] if o["strength"] >= STRONG))
            pgt = CORPUS / f"{stem}.pattern_gt.json"
            gt_bpm = json.loads(pgt.read_text()).get("groundTruthBpm") if pgt.exists() else None
            drums = load_drum_labels(drum_label_path(CORPUS, stem),
                                     CORPUS / f"{stem}.onsets_consensus.json")
            pcm = tmpd / f"{stem}.pcm"
            decode(ffmpeg, mp3, pcm, args.level_db)
            for fps in args.fps:
                cells = []
                for b, exe in builds:
                    rows = run_harness(exe, pcm, tmpd / f"{stem}_{b}.csv", fps, args.jitter, args.seed,
                                       (args.harness_args + args.harness_opt) if b == "new" else None)
                    r = evaluate(rows, onsets)
                    r.update(onset_f1(rows, gt["onsets"]))
                    r.update(drum_metrics(rows, drums))
                    r["tempo"] = tempo_score(rows, gt_bpm)
                    r["has_gt_bpm"] = bool(gt_bpm)
                    r["label"] = f"{stem}@{fps:.0f}fps"
                    summary[b].append(r)
                    cells.append(f"{r['kick_r50']:8.2f} {r['on_drum']:9.2f} {r['drum_f']:8.3f} "
                                 f"{r['snare_r50']:7.2f} {r['excess']:8.2f} {r['tempo']:8.2f} {r['worst20s']:9.2f} {r['recall']:7.2f} "
                                 f"{r['f1']:7.3f} {r['prec']:8.3f} {r['bpm']:8.1f}")
                print(f"{stem:32s} {fps:5.0f} " + "  ".join(cells), flush=True)
    print()
    for b, rs in summary.items():
        arr = lambda k: np.nanmean([r[k] for r in rs])
        print(f"{b}: DRUMS mean kickR@50 {arr('kick_r50'):.3f}  kickR@30 {arr('kick_r30'):.3f}  "
              f"onDrum {arr('on_drum'):.3f}  drumF {arr('drum_f'):.3f}  "
              f"snareR@50 {arr('snare_r50'):.3f} (provisional, #284)  loneHat {arr('lone_hat'):.3f}  "
              f"det/s {arr('det_per_s'):.2f}  lag {arr('drum_lag'):.0f} ms")
        print(f"{b}: mean lock {arr('lock'):.3f}  mean excess {arr('excess'):.3f}  "
              f"mean tempo {arr('tempo'):.3f}  mean worst20s {arr('worst20s'):.3f}  "
              f"mean recall {arr('recall'):.3f}  mean F1 {arr('f1'):.3f}  mean prec {arr('prec'):.3f}  "
              f"mean F1@50 {arr('f1_50'):.3f}")

    # Regression gate (CI): floors apply to the new build's corpus means. Any
    # non-finite per-run value fails outright, so a broken run can't hide inside
    # the mean. The one legitimate NaN is tempo on a track with no annotated BPM.
    failed = []
    for key, floor in (("excess", args.min_excess), ("tempo", args.min_tempo), ("recall", args.min_recall),
                       ("f1", args.min_f1), ("kick_r50", args.min_kick_recall),
                       ("on_drum", args.min_on_drum), ("drum_f", args.min_drum_f)):
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
