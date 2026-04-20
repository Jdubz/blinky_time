"""Generate a synthetic tonal-impulse corpus for Phase 1 onset-vs-tonal analysis.

Creates ~8 mono 16 kHz WAV tracks of pure tonal content with known impulse
times written as `.onsets_consensus.json` sidecars. Each track simulates
a different NN-false-positive-inducing pattern:

  - sine impulses        — cleanest tonal attack (single frequency)
  - saw chord stabs      — EDM synth stabs (trance/house)
  - detuned leads        — aggressive monophonic synth attacks
  - harmonic stacks      — piano-like with rich overtones
  - bass note attacks    — deep low-frequency impulses (sub-bass stabs)
  - vocal formants       — bandpass noise bursts approximating consonants
  - sustained pad+stabs  — tonal impulses over a held chord (hard case)
  - two-speed variants   — 2/s and 4/s impulse rates per pattern

The corpus deliberately contains ZERO percussion, so any feature firing on
these tracks is by construction a tonal-impulse response. Used by
run_catalog.py as the negative class when computing the cross-corpus
Cohen's d of "percussion onset peak" vs "tonal impulse peak".
"""

from __future__ import annotations

import argparse
import json
import logging
from pathlib import Path

import numpy as np
import soundfile as sf

log = logging.getLogger("tonal_corpus")

SR = 16000
DURATION = 30.0  # seconds per track


def _adsr(n: int, attack_ms: float, decay_ms: float, sustain: float, release_ms: float) -> np.ndarray:
    """Simple ADSR envelope of total length n samples."""
    a = max(1, int(attack_ms * SR / 1000))
    d = max(1, int(decay_ms * SR / 1000))
    r = max(1, int(release_ms * SR / 1000))
    s = max(1, n - a - d - r)
    env = np.concatenate([
        np.linspace(0.0, 1.0, a, dtype=np.float32),
        np.linspace(1.0, sustain, d, dtype=np.float32),
        np.full(s, sustain, dtype=np.float32),
        np.linspace(sustain, 0.0, r, dtype=np.float32),
    ])
    return env[:n]


def _place_impulses(
    duration: float,
    onsets_per_sec: float,
    *,
    jitter: float = 0.02,
    seed: int = 0,
) -> np.ndarray:
    """Return sorted impulse times (seconds) evenly spaced with jitter."""
    rng = np.random.default_rng(seed)
    n = max(1, int(duration * onsets_per_sec))
    base = np.linspace(0.5, duration - 0.5, n, dtype=np.float64)
    jitter_s = rng.uniform(-jitter, jitter, n)
    return np.sort(np.clip(base + jitter_s, 0.0, duration - 0.1))


def sine_impulses(onsets: np.ndarray, freq_hz: float = 440.0, length_ms: float = 120.0) -> np.ndarray:
    """Pure sine tone impulses — cleanest possible tonal attack."""
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    n = int(length_ms * SR / 1000)
    env = _adsr(n, attack_ms=5, decay_ms=20, sustain=0.4, release_ms=80)
    t = np.arange(n, dtype=np.float32) / SR
    tone = np.sin(2 * np.pi * freq_hz * t).astype(np.float32) * env
    for t_sec in onsets:
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += tone[: end - start]
    return out


def saw_chord_stabs(onsets: np.ndarray, root_hz: float = 220.0) -> np.ndarray:
    """Detuned sawtooth chord stabs — trance/house EDM synth character."""
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    # Major triad + octave, each voice slightly detuned
    ratios = [1.0, 5 / 4, 3 / 2, 2.0]
    detunes = [1.0, 1.007, 0.993, 1.003]
    n = int(0.25 * SR)
    env = _adsr(n, attack_ms=8, decay_ms=120, sustain=0.3, release_ms=100)
    t = np.arange(n, dtype=np.float32) / SR
    stab = np.zeros(n, dtype=np.float32)
    for ratio in ratios:
        for detune in detunes:
            f = root_hz * ratio * detune
            # Band-limited saw via harmonic sum (first 6 partials)
            for k in range(1, 7):
                stab += np.sin(2 * np.pi * f * k * t).astype(np.float32) / k
    stab = (stab / stab.max()) * env * 0.6
    for t_sec in onsets:
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += stab[: end - start]
    return out


def detuned_lead(onsets: np.ndarray, freq_hz: float = 330.0) -> np.ndarray:
    """Aggressive monophonic synth lead — dubstep/riddim style."""
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    n = int(0.18 * SR)
    env = _adsr(n, attack_ms=3, decay_ms=40, sustain=0.5, release_ms=50)
    t = np.arange(n, dtype=np.float32) / SR
    v1 = np.sign(np.sin(2 * np.pi * freq_hz * t))  # square
    v2 = np.sign(np.sin(2 * np.pi * freq_hz * 1.01 * t))  # detuned sq
    v3 = np.sign(np.sin(2 * np.pi * freq_hz * 0.5 * t))  # sub-octave
    voice = (0.4 * v1 + 0.4 * v2 + 0.3 * v3) * env * 0.5
    for t_sec in onsets:
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += voice[: end - start].astype(np.float32)
    return out


def harmonic_stack(onsets: np.ndarray, root_hz: float = 261.6) -> np.ndarray:
    """Piano-like exponentially-decaying harmonic stack."""
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    n = int(0.6 * SR)
    t = np.arange(n, dtype=np.float32) / SR
    decay = np.exp(-5.0 * t)
    voice = np.zeros(n, dtype=np.float32)
    for k, amp in enumerate([1.0, 0.5, 0.35, 0.2, 0.15, 0.1, 0.08, 0.06], start=1):
        voice += amp * np.sin(2 * np.pi * root_hz * k * t).astype(np.float32)
    voice = (voice / np.abs(voice).max()) * decay * 0.5
    for t_sec in onsets:
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += voice[: end - start]
    return out


def bass_note_attacks(onsets: np.ndarray, freq_hz: float = 55.0) -> np.ndarray:
    """Deep sub-bass stabs — low-frequency tonal impulses."""
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    n = int(0.35 * SR)
    env = _adsr(n, attack_ms=15, decay_ms=80, sustain=0.5, release_ms=200)
    t = np.arange(n, dtype=np.float32) / SR
    # Sine with a bit of second harmonic
    voice = (np.sin(2 * np.pi * freq_hz * t) + 0.3 * np.sin(2 * np.pi * freq_hz * 2 * t)).astype(
        np.float32
    )
    voice = voice * env * 0.7
    for t_sec in onsets:
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += voice[: end - start]
    return out


def vocal_formants(onsets: np.ndarray, seed: int = 1) -> np.ndarray:
    """Bandpass-filtered noise bursts — approximate vocal consonants.

    These are technically broadband but have tonal resonance, which is
    exactly the kind of FP the NN confuses with snare hits.
    """
    from scipy.signal import butter, sosfilt

    rng = np.random.default_rng(seed)
    total = int(DURATION * SR)
    out = np.zeros(total, dtype=np.float32)
    n = int(0.15 * SR)
    env = _adsr(n, attack_ms=4, decay_ms=30, sustain=0.3, release_ms=80)
    # Vocal formants approx (vowel "ah")
    sos = butter(4, [500, 2500], btype="bandpass", fs=SR, output="sos")
    for t_sec in onsets:
        noise = rng.standard_normal(n).astype(np.float32)
        voice = sosfilt(sos, noise).astype(np.float32) * env * 0.5
        start = int(t_sec * SR)
        end = min(total, start + n)
        out[start:end] += voice[: end - start]
    return out


def sustained_pad_with_stabs(onsets: np.ndarray, stab_hz: float = 440.0) -> np.ndarray:
    """Chord pad held throughout, with synth stabs on top at onset times.

    Hardest case: the "non-onset" frames here still have tonal content.
    Between stabs, the pad is present. A feature has to pick out the
    stab attacks despite continuous harmonic content.
    """
    total = int(DURATION * SR)
    t_full = np.arange(total, dtype=np.float32) / SR
    pad = (
        0.3 * np.sin(2 * np.pi * 220 * t_full)
        + 0.25 * np.sin(2 * np.pi * 330 * t_full)
        + 0.2 * np.sin(2 * np.pi * 440 * t_full)
    ).astype(np.float32) * 0.3
    # Slow amplitude modulation to avoid static spectrum
    pad *= 0.7 + 0.3 * np.sin(2 * np.pi * 0.2 * t_full)
    stabs = saw_chord_stabs(onsets, root_hz=stab_hz)
    return (pad + stabs).astype(np.float32)


GENERATORS = {
    "sine_impulses_2hz": lambda: (
        sine_impulses(_place_impulses(DURATION, 2.0, seed=1)),
        _place_impulses(DURATION, 2.0, seed=1),
    ),
    "sine_impulses_4hz": lambda: (
        sine_impulses(_place_impulses(DURATION, 4.0, seed=2), freq_hz=660.0),
        _place_impulses(DURATION, 4.0, seed=2),
    ),
    "saw_chord_stabs_2hz": lambda: (
        saw_chord_stabs(_place_impulses(DURATION, 2.0, seed=3)),
        _place_impulses(DURATION, 2.0, seed=3),
    ),
    "saw_chord_stabs_4hz": lambda: (
        saw_chord_stabs(_place_impulses(DURATION, 4.0, seed=4), root_hz=165.0),
        _place_impulses(DURATION, 4.0, seed=4),
    ),
    "detuned_lead_3hz": lambda: (
        detuned_lead(_place_impulses(DURATION, 3.0, seed=5)),
        _place_impulses(DURATION, 3.0, seed=5),
    ),
    "harmonic_stack_2hz": lambda: (
        harmonic_stack(_place_impulses(DURATION, 2.0, seed=6)),
        _place_impulses(DURATION, 2.0, seed=6),
    ),
    "bass_note_attacks_2hz": lambda: (
        bass_note_attacks(_place_impulses(DURATION, 2.0, seed=7)),
        _place_impulses(DURATION, 2.0, seed=7),
    ),
    "vocal_formants_3hz": lambda: (
        vocal_formants(_place_impulses(DURATION, 3.0, seed=8)),
        _place_impulses(DURATION, 3.0, seed=8),
    ),
    "pad_with_stabs_2hz": lambda: (
        sustained_pad_with_stabs(_place_impulses(DURATION, 2.0, seed=9)),
        _place_impulses(DURATION, 2.0, seed=9),
    ),
}


def write_track(name: str, audio: np.ndarray, onsets: np.ndarray, out_dir: Path) -> None:
    """Write audio + GT sidecar. Normalizes peak to 0.9 to avoid clipping."""
    peak = float(np.abs(audio).max())
    if peak > 0:
        audio = audio * (0.9 / peak)
    out_dir.mkdir(parents=True, exist_ok=True)
    wav_path = out_dir / f"{name}.wav"
    gt_path = out_dir / f"{name}.onsets_consensus.json"
    sf.write(str(wav_path), audio.astype(np.float32), SR, subtype="FLOAT")
    gt_path.write_text(
        json.dumps(
            {
                "onsets": [{"time": float(t), "strength": 0.8, "systems": 1} for t in onsets],
                "count": len(onsets),
                "systems_succeeded": 1,
                "total_systems": 1,
                "tolerance_ms": 50,
                "source": "synthetic (analysis/generate_tonal_corpus.py)",
            },
            indent=2,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="Output directory for .wav + .json")
    parser.add_argument(
        "--tracks",
        type=str,
        default=None,
        help="Comma-separated subset (defaults to all)",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    names = list(GENERATORS)
    if args.tracks:
        wanted = set(args.tracks.split(","))
        names = [n for n in names if n in wanted]
    for name in names:
        log.info("Generating %s", name)
        audio, onsets = GENERATORS[name]()
        write_track(name, audio, onsets, args.out)
    log.info("Wrote %d tracks to %s", len(names), args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
