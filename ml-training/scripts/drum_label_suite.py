#!/usr/bin/env python3
"""Drum label regression suite (#284): score a kick/snare label generator before trusting it.

The kick_weighted labels are the NN's training targets and the drum metrics' ground truth, and
the audit on #284 found them late, with phantom snares, snares labeled as kicks and duplicate
kicks. Any change to the generator is scored here first; the current generator is the baseline
(`drum_label_baseline.json`), and a candidate must hold or improve every row.

Two kinds of evidence:

**Synthetic drums with known hit times** (`synth_corpus`), for what needs the truth: timing,
and kicks vs snares. The kit varies: punchy and 808 kicks, both with a beater click, and snares
whose body ranges 170-260 Hz, so some sit under the generator's 200 Hz split. Patterns include
co-hits, kick-snare flams and 16th-note kick doubles. `score_synthetic` matches labels to the
truth by type. Phantom snares don't reproduce in synthetic audio (the merge absorbs a click
inside its 30 ms window); the real-data metric measures them.

**Real tracks without drum ground truth** (`run_edm`): edm/'s 18 tracks against
their full-mix onset consensus. It measures timing against the consensus, phantom snares just
before a kick, duplicate kicks, density, and what the clean step (`clean_kick_weighted.py`)
keeps. These are proxies; no hand-annotated drum reference exists yet.

usage:
  python scripts/drum_label_suite.py synthetic        # score the current generator
  python scripts/drum_label_suite.py edm              # score the edm/ labels in the repo
  python scripts/drum_label_suite.py baseline         # both, written to drum_label_baseline.json
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

ML = Path(__file__).resolve().parents[1]
REPO = ML.parent
EDM = REPO / "blinky-test-player" / "music" / "edm"
BASELINE = Path(__file__).with_name("drum_label_baseline.json")

TYPES = ("kick", "snare")
MATCH_TOL_S = 0.050  # MIREX drum transcription tolerance: timing is reported separately
DUPLICATE_IOI_S = 0.080  # same-type labels closer than this are one hit labeled twice
PHANTOM_WINDOW_S = (0.030, 0.080)  # a snare this far BEFORE a kick: the kick's click
CONSENSUS_MIN_SYSTEMS = 2
CONSENSUS_TOL_S = 0.070  # clean_kick_weighted.py's tolerance, so retention means the same

Event = tuple[float, str]  # (time in s, "kick" | "snare" | "hihat")


# -- synthetic drums -------------------------------------------------------------------------

SYNTH_SR = 44100  # demucs stems are 44.1 kHz; the generator resamples, as it does for real stems


def _decay(n: int, tau_s: float, sr: int) -> np.ndarray:
    return np.exp(-np.arange(n) / (tau_s * sr))


def kick(sr: int, rng: np.random.Generator, vel: float) -> np.ndarray:
    """Half punchy (a 160 -> 45 Hz sweep at full level at once), half 808 (40-100 Hz with a
    25 ms soft attack and a long tail). Both carry a 6 ms 1.5-6 kHz beater click."""
    from scipy.signal import butter, sosfilt

    is808 = rng.uniform() < 0.5
    n = int((0.5 if is808 else 0.3) * sr)
    t = np.arange(n) / sr
    f = 40 + 60 * np.exp(-t / 0.05) if is808 else 45 + 115 * np.exp(-t / 0.03)
    env = _decay(n, 0.25 if is808 else 0.12, sr)
    if is808:
        a = int(0.025 * sr)
        env[:a] *= np.linspace(0, 1, a) ** 2
    body = np.sin(2 * np.pi * np.cumsum(f) / sr) * env
    m = int(0.006 * sr)
    sos = butter(2, [1500, 6000], btype="band", fs=sr, output="sos")
    c = sosfilt(sos, rng.standard_normal(m))
    click = np.zeros(n)
    click[:m] = c / (np.abs(c).max() + 1e-9) * np.exp(-np.arange(m) / (m / 3)) * 0.8
    return vel * (body + click)


def snare(sr: int, rng: np.random.Generator, vel: float) -> np.ndarray:
    """A 170-260 Hz body at a varying level (the low ones sit under the generator's 200 Hz
    split) plus a 1-8 kHz noise burst."""
    from scipy.signal import butter, sosfilt

    n = int(0.20 * sr)
    t = np.arange(n) / sr
    body = np.sin(2 * np.pi * rng.uniform(170, 260) * t) * _decay(n, 0.05, sr)
    sos = butter(2, [1000, 8000], btype="band", fs=sr, output="sos")
    noise = sosfilt(sos, rng.standard_normal(n)) * _decay(n, 0.06, sr)
    return vel * (rng.uniform(0.3, 0.7) * body + 1.2 * noise / (np.abs(noise).max() + 1e-9))


def hihat(sr: int, rng: np.random.Generator, vel: float) -> np.ndarray:
    from scipy.signal import butter, sosfilt

    n = int(0.06 * sr)
    sos = butter(4, 7000, btype="high", fs=sr, output="sos")
    noise = sosfilt(sos, rng.standard_normal(n)) * _decay(n, 0.012, sr)
    return vel * 0.4 * noise / (np.abs(noise).max() + 1e-9)


SOUNDS: dict[str, Callable[[int, np.random.Generator, float], np.ndarray]] = {
    "kick": kick, "snare": snare, "hihat": hihat,
}


@dataclass(frozen=True)
class SynthTrack:
    name: str
    audio: np.ndarray
    sr: int
    truth: list[Event]


def _pattern(bpm: float, bars: int, rng: np.random.Generator, style: str) -> list[Event]:
    """One bar = 16 sixteenths. Kicks on the beats, snares on 2 and 4, 8th hats, plus the
    cases the audit found broken: co-hits, flams (snare 25 ms after a kick), kick doubles."""
    six = 60.0 / bpm / 4
    ev: list[Event] = []
    for b in range(bars):
        t0 = 0.5 + b * 16 * six
        for s in range(16):
            t = t0 + s * six
            if s % 2 == 0:
                ev.append((t, "hihat"))
            if s % 4 == 0 and style != "breaks":
                ev.append((t, "kick"))
            if s in (4, 12):
                ev.append((t, "snare"))
        if style == "breaks":
            for s in (0, 6, 10):
                ev.append((t0 + s * six, "kick"))
        if b % 2 == 1:  # every other bar: a co-hit, a flam and a 16th kick double
            ev.append((t0 + 12 * six, "kick"))  # kick under the snare on 4: a co-hit
            ev.append((t0 + 8 * six + 0.025, "snare"))  # flam: snare 25 ms after the kick on 3
            ev.append((t0 + 15 * six, "kick"))  # 16th before the next bar's downbeat
    return sorted(ev)


def synth_corpus(seed: int = 0) -> list[SynthTrack]:
    rng = np.random.default_rng(seed)
    specs = [("house", 124, "four"), ("techno", 132, "four"), ("dnb", 174, "breaks"),
             ("hiphop", 92, "breaks"), ("trap", 140, "breaks"), ("disco", 118, "four")]
    tracks = []
    for name, bpm, style in specs:
        truth = _pattern(bpm, 8, rng, style)
        dur = truth[-1][0] + 1.0
        y = 0.003 * rng.standard_normal(int(dur * SYNTH_SR))  # a separation-residue floor
        for t, kind in truth:
            vel = float(rng.uniform(0.6, 1.0)) if kind != "hihat" else float(rng.uniform(0.3, 0.7))
            s = SOUNDS[kind](SYNTH_SR, rng, vel)
            i = int(round(t * SYNTH_SR))
            y[i : i + len(s)] += s[: len(y) - i]
        y = (0.9 * y / np.abs(y).max()).astype(np.float32)
        tracks.append(SynthTrack(name, y, SYNTH_SR, truth))
    return tracks


# -- scoring ---------------------------------------------------------------------------------


def _match(ref: list[float], est: list[float], tol: float) -> list[tuple[int, int]]:
    """One-to-one matches within tol, closest pairs first."""
    pairs = sorted(
        (abs(e - r), i, j) for i, r in enumerate(ref) for j, e in enumerate(est) if abs(e - r) <= tol
    )
    used_r: set[int] = set()
    used_e: set[int] = set()
    out = []
    for _, i, j in pairs:
        if i not in used_r and j not in used_e:
            used_r.add(i)
            used_e.add(j)
            out.append((i, j))
    return out


def _dist(xs: list[float]) -> dict[str, float | None]:
    if not xs:
        return {"median_ms": None, "p10_ms": None, "p90_ms": None, "within_10ms": None}
    a = np.asarray(xs) * 1000
    return {
        "median_ms": round(float(np.median(a)), 1),
        "p10_ms": round(float(np.percentile(a, 10)), 1),
        "p90_ms": round(float(np.percentile(a, 90)), 1),
        "within_10ms": round(float(np.mean(np.abs(a) <= 10)), 3),
    }


def duplicates(est: list[Event], kind: str) -> int:
    ts = sorted(t for t, k in est if k == kind)
    return int(sum(1 for a, b in zip(ts, ts[1:], strict=False) if b - a < DUPLICATE_IOI_S))


def phantom_snares(est: list[Event], kicks: list[float]) -> int:
    """Snare labels 30-80 ms BEFORE a kick: the kick's click firing the snare band."""
    lo, hi = PHANTOM_WINDOW_S
    k = np.asarray(sorted(kicks))
    n = 0
    for t, kind in est:
        if kind != "snare" or not len(k):
            continue
        i = int(np.searchsorted(k, t))
        if i < len(k) and lo <= k[i] - t <= hi:
            n += 1
    return n


def score_synthetic(truth: list[Event], est: list[Event]) -> dict[str, Any]:
    """Per type: precision/recall/F at MATCH_TOL_S and the timing of matched labels. Across
    types: true hits labeled as the other type, phantom snares, same-type duplicates."""
    out: dict[str, Any] = {}
    for kind in TYPES:
        ref = [t for t, k in truth if k == kind]
        got = [t for t, k in est if k == kind]
        m = _match(ref, got, MATCH_TOL_S)
        tp = len(m)
        p = tp / len(got) if got else 0.0
        r = tp / len(ref) if ref else 0.0
        out[kind] = {
            "ref": len(ref), "est": len(got), "tp": tp,
            "precision": round(p, 3), "recall": round(r, 3),
            "f1": round(2 * p * r / (p + r), 3) if p + r else 0.0,
            "timing": _dist([got[j] - ref[i] for i, j in m]),
            "duplicates": duplicates(est, kind),
        }
    # A true hit of one type whose only label within tolerance is the other type.
    for kind, other in (("snare", "kick"), ("kick", "snare")):
        ref = [t for t, k in truth if k == kind]
        same = [t for t, k in est if k == kind]
        wrong = [t for t, k in est if k == other]
        missed = [ref[i] for i in range(len(ref))
                  if i not in {a for a, _ in _match(ref, same, MATCH_TOL_S)}]
        out[f"{kind}_as_{other}"] = len(_match(missed, wrong, MATCH_TOL_S))
    out["phantom_snares"] = phantom_snares(est, [t for t, k in truth if k == "kick"])
    return out


# -- aggregation -----------------------------------------------------------------------------


def _sum_scores(per_track: list[dict[str, Any]]) -> dict[str, Any]:
    """Pool counts across tracks; F and rates recomputed from the pooled counts."""
    out: dict[str, Any] = {}
    for kind in TYPES:
        ref = sum(s[kind]["ref"] for s in per_track)
        est = sum(s[kind]["est"] for s in per_track)
        tp = sum(s[kind]["tp"] for s in per_track)
        p, r = (tp / est if est else 0.0), (tp / ref if ref else 0.0)
        out[kind] = {"ref": ref, "est": est, "tp": tp, "precision": round(p, 3),
                     "recall": round(r, 3), "f1": round(2 * p * r / (p + r), 3) if p + r else 0.0,
                     "duplicates": sum(s[kind]["duplicates"] for s in per_track)}
    for k in ("snare_as_kick", "kick_as_snare", "phantom_snares"):
        out[k] = sum(s[k] for s in per_track)
    return out


Labeler = Callable[[np.ndarray, int], list[Event]]


def run_synthetic(label: Labeler, seed: int = 0) -> dict[str, Any]:
    per_track, offsets = [], {k: [] for k in TYPES}
    for tr in synth_corpus(seed):
        est = label(tr.audio, tr.sr)
        s = score_synthetic(tr.truth, est)
        per_track.append(s)
        for kind in TYPES:  # pool raw offsets for the timing distribution
            ref = [t for t, k in tr.truth if k == kind]
            got = [t for t, k in est if k == kind]
            offsets[kind] += [got[j] - ref[i] for i, j in _match(ref, got, MATCH_TOL_S)]
    out = _sum_scores(per_track)
    for kind in TYPES:
        out[kind]["timing"] = _dist(offsets[kind])
    return out


def run_edm(kw_dir: Path = EDM / "kick_weighted", cons_dir: Path = EDM) -> dict[str, Any]:
    kws = sorted(kw_dir.glob("*.kick_weighted.json"))
    if not kws:
        raise SystemExit(f"no *.kick_weighted.json in {kw_dir}")
    offs: dict[str, list[float]] = {k: [] for k in TYPES}
    totals = {"labels": 0, "phantom_snares": 0, "kick_dup": 0, "kept": 0, "tracks": 0,
              "minutes": 0.0}
    labels_by_kind = {k: 0 for k in TYPES}
    for kw_path in kws:
        stem = kw_path.name[: -len(".kick_weighted.json")]
        kw = json.loads(kw_path.read_text())
        cons = json.loads((cons_dir / f"{stem}.onsets_consensus.json").read_text())
        est: list[Event] = [(o["time"], o["type"]) for o in kw["onsets"] if o.get("type") in TYPES]
        c = np.asarray(sorted(o["time"] for o in cons["onsets"]
                              if o.get("systems", 1) >= CONSENSUS_MIN_SYSTEMS))
        for t, k in est:
            i = int(np.searchsorted(c, t))
            near = [c[j] for j in (i - 1, i) if 0 <= j < len(c)]
            if near:
                d = t - min(near, key=lambda x: abs(x - t))
                if abs(d) <= CONSENSUS_TOL_S:
                    offs[k].append(d)
                    totals["kept"] += 1
            labels_by_kind[k] += 1
        totals["labels"] += len(est)
        totals["phantom_snares"] += phantom_snares(est, [t for t, k in est if k == "kick"])
        totals["kick_dup"] += duplicates(est, "kick")
        totals["tracks"] += 1
        span = max((t for t, _ in est), default=0.0)
        totals["minutes"] += span / 60
    return {
        "tracks": totals["tracks"],
        **{k: {"labels": labels_by_kind[k],
               "per_minute": round(labels_by_kind[k] / max(totals["minutes"], 1e-9), 1),
               "timing_vs_consensus": _dist(offs[k])} for k in TYPES},
        "phantom_snares": totals["phantom_snares"],
        "phantom_snare_rate": round(totals["phantom_snares"] / max(labels_by_kind["snare"], 1), 3),
        "kick_duplicates": totals["kick_dup"],
        "kick_duplicate_rate": round(totals["kick_dup"] / max(labels_by_kind["kick"], 1), 3),
        "clean_retention": round(totals["kept"] / max(totals["labels"], 1), 3),
    }


# -- the gate ---------------------------------------------------------------------------------

# Every gated row and which way is better. A candidate generator must hold or improve each one
# against the baseline (`compare`). Timing is gated on |median| and on the share within 10 ms,
# so a constant offset can't pass for accuracy and a tight spread can't hide a lag.
HIGHER, LOWER, CLOSER_TO_0 = "higher", "lower", "closer_to_0"
GATED: dict[str, str] = {
    "synthetic.kick.f1": HIGHER, "synthetic.snare.f1": HIGHER,
    "synthetic.kick.timing.median_ms": CLOSER_TO_0, "synthetic.snare.timing.median_ms": CLOSER_TO_0,
    "synthetic.kick.timing.within_10ms": HIGHER, "synthetic.snare.timing.within_10ms": HIGHER,
    "synthetic.snare_as_kick": LOWER, "synthetic.kick_as_snare": LOWER,
    "synthetic.kick.duplicates": LOWER, "synthetic.snare.duplicates": LOWER,
    "edm.kick.timing_vs_consensus.median_ms": CLOSER_TO_0,
    "edm.snare.timing_vs_consensus.median_ms": CLOSER_TO_0,
    "edm.kick.timing_vs_consensus.within_10ms": HIGHER,
    "edm.snare.timing_vs_consensus.within_10ms": HIGHER,
    "edm.phantom_snare_rate": LOWER, "edm.kick_duplicate_rate": LOWER,
    "edm.clean_retention": HIGHER,
}


def _get(d: dict[str, Any], path: str) -> Any:
    for part in path.split("."):
        d = d[part]
    return d


def compare(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    """The gated rows where the candidate is worse than the baseline. Empty: it may replace it."""
    worse = []
    for path, better in GATED.items():
        b, c = _get(baseline, path), _get(candidate, path)
        if c is None:
            worse.append(f"{path}: no value (baseline {b})")
            continue
        if b is None:
            continue
        ok = {HIGHER: c >= b, LOWER: c <= b, CLOSER_TO_0: abs(c) <= abs(b)}[better]
        if not ok:
            worse.append(f"{path}: {c} vs baseline {b} ({better} is better)")
    return worse


def current_generator(y: np.ndarray, sr: int) -> list[Event]:
    """The generator as it is (generate_kick_weighted_targets.label_audio), as a Labeler."""
    sys.path.insert(0, str(ML))
    from scripts.generate_kick_weighted_targets import label_audio

    return [(o["time"], o["type"]) for o in label_audio(y, sr)["onsets"]]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=["synthetic", "edm", "baseline"])
    a = ap.parse_args(argv)
    if a.what == "synthetic":
        print(json.dumps(run_synthetic(current_generator), indent=2))
    elif a.what == "edm":
        print(json.dumps(run_edm(), indent=2))
    else:
        import librosa

        sys.path.insert(0, str(ML))
        from scripts.generate_kick_weighted_targets import GENERATOR

        base = {
            "generator": f"{GENERATOR['algorithm']} v{GENERATOR['version']}",
            # The synthetic rows depend on librosa's onset detector; edm's on nothing but files.
            "librosa": librosa.__version__,
            "synthetic": run_synthetic(current_generator),
            # The labels committed in edm/kick_weighted, which an OLDER parameterization made
            # (snare weight 0.5, bands 30-200/200-4000/4000-8000 Hz): what the drum metrics and
            # the edm/ studies have used, not a run of the current generator.
            "edm_labels": "blinky-test-player/music/edm/kick_weighted (older parameterization)",
            "edm": run_edm(),
        }
        BASELINE.write_text(json.dumps(base, indent=2) + "\n")
        print(f"wrote {BASELINE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
