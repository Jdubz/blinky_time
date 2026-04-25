"""Investigate the 4-system signal anomaly noted in the audit (T4-tier
follow-up).

Background: per-strength mel-diff signal-to-baseline ratio across 20
tracks (audit doc, 2026-04-24):
    1/5 sys: 0.43×  (sub-random)
    2/5 sys: 0.46×  (sub-random)
    3/5 sys: 1.65×
    4/5 sys: 1.11×  ← anomalously low
    5/5 sys: 1.54×

Hypotheses to test:
  (A) Merge-tolerance artifact: 4-system events have higher per-system
      time variance, putting the median-time consensus at a poor frame.
  (B) Detector subset bias: 4-system events over-represent detector
      combinations that fire on harmonic / steady-state content.
      (Cannot test without per-system membership in label JSON; defer.)
  (C) Noise from small n; with more tracks the anomaly disappears.
  (D) 4-system events are at high mel-energy steady-state frames, not
      energy-change frames.

Tests run here: (A) requires per-system times; the consensus JSON only
records the merged time, so we can't compute per-system variance from
the label files alone. We focus on (C) — aggregate over many more
tracks — and (D) — test whether 4-system events are at high mel-energy
steady-state vs other strengths.

Output: per-strength signal ratios (extends audit's 20-track sample to
50+ tracks) and the energy-vs-energy-change comparison.
"""

from __future__ import annotations

import json
import os
import random
from pathlib import Path

import numpy as np

OC_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_consensus")
AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio/combined")
N_TRACKS = 60
SR = 16000
HOP = 256
FRAME_RATE = SR / HOP
TOL = 3


def get_audio_path(stem: str) -> Path | None:
    for ext in (".wav", ".mp3", ".flac"):
        p = AUDIO_DIR / f"{stem}{ext}"
        if p.exists():
            return p
    return None


def main() -> int:
    random.seed(0)
    files = sorted(p for p in OC_DIR.iterdir() if p.suffix == ".json")
    random.shuffle(files)

    # Per-strength accumulators
    sig_ratio: dict[int, list[float]] = {s: [] for s in range(1, 6)}
    energy_ratio: dict[int, list[float]] = {s: [] for s in range(1, 6)}
    counts: dict[int, int] = {s: 0 for s in range(1, 6)}

    import librosa  # heavy import; deferred until needed

    n = 0
    for fpath in files:
        if n >= N_TRACKS:
            break
        stem = fpath.name.replace(".onsets.json", "")
        audio = get_audio_path(stem)
        if not audio:
            continue
        try:
            y, sr = librosa.load(str(audio), sr=SR, mono=True)
        except Exception:
            continue
        mel = librosa.feature.melspectrogram(
            y=y, sr=sr, n_mels=30, hop_length=HOP, fmin=40, fmax=4000
        )
        mel_log = np.log(mel + 1e-9)
        energy = mel_log.sum(axis=0)
        diff = np.diff(energy, prepend=energy[0])
        diff_pos = np.maximum(diff, 0)
        n_frames = len(diff_pos)

        # Random baseline for this track (same for all strengths).
        rng = np.random.default_rng(42)
        rand_idx = rng.integers(TOL, n_frames - TOL, size=400)
        rand_max_diff = np.array([diff_pos[max(0, i - TOL):i + TOL + 1].max() for i in rand_idx])
        rand_mean_energy = np.array([energy[max(0, i - TOL):i + TOL + 1].mean() for i in rand_idx])
        baseline_diff = float(rand_max_diff.mean())
        baseline_energy = float(rand_mean_energy.mean())
        if baseline_diff < 1e-6 or abs(baseline_energy) < 1e-6:
            continue

        with open(fpath) as f:
            data = json.load(f)
        for o in data.get("onsets", []):
            s = int(o.get("systems", 0))
            if not 1 <= s <= 5:
                continue
            fr = int(round(float(o["time"]) * FRAME_RATE))
            if fr < TOL or fr >= n_frames - TOL:
                continue
            window_diff = float(diff_pos[fr - TOL:fr + TOL + 1].max())
            window_energy = float(energy[fr - TOL:fr + TOL + 1].mean())
            sig_ratio[s].append(window_diff / baseline_diff)
            energy_ratio[s].append(window_energy / baseline_energy)
            counts[s] += 1
        n += 1

    print(f"\n=== 4-system anomaly investigation, {n} tracks ===\n")
    print(f"{'sys':<8}{'count':<10}{'mel-diff sig':<18}{'mel-energy ratio':<20}{'observation':<25}")
    print("-" * 80)
    for s in range(1, 6):
        if counts[s] == 0:
            continue
        sig_mean = float(np.mean(sig_ratio[s]))
        sig_med = float(np.median(sig_ratio[s]))
        en_mean = float(np.mean(energy_ratio[s]))
        # Compare to 3-sys as baseline
        if s == 3:
            obs = "(reference)"
        elif sig_mean < 1.3:
            obs = "below noise floor"
        elif s == 4 and sig_mean < float(np.mean(sig_ratio[3])) - 0.2:
            obs = "ANOMALY confirmed"
        else:
            obs = "ok"
        print(f"{s}/5    {counts[s]:<10}{sig_mean:.2f}× (med {sig_med:.2f})   {en_mean:.3f}                {obs}")

    # Energy-vs-difference dimension: is the 4-system bucket at higher static
    # energy (suggesting steady-state events) than the 3- or 5-system bucket?
    print()
    print("Static-energy hypothesis (D):")
    print("  Higher energy_ratio at the 4-system bucket vs others → these events")
    print("  are at LOUD steady-state frames, not at energy-change frames.")
    print()
    if all(counts[s] for s in (3, 4, 5)):
        e3, e4, e5 = (float(np.mean(energy_ratio[s])) for s in (3, 4, 5))
        print(f"  energy_ratio: 3-sys={e3:.3f}  4-sys={e4:.3f}  5-sys={e5:.3f}")
        if e4 > e3 + 0.05 and e4 > e5 + 0.05:
            print("  → 4-system events ARE at higher-energy frames. (D) supported.")
        elif abs(e4 - e3) < 0.05 and abs(e4 - e5) < 0.05:
            print("  → energy levels comparable. (D) not supported; check other hypotheses.")
        else:
            print("  → mixed signal; investigate per-track variance.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
