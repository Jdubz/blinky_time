"""The firmware's mel front end, built from its spec (#295).

The NN runs on the firmware's `SharedSpectralAnalysis::getRawMelBands()`, which reads the
integer table `MEL_BANDS[]`, not an HTK filterbank. Training must build the same features,
so the table and the arithmetic come from one spec, `ml-training/spec/mel_frontend_v1.json`.
`analysis/test_mel_parity.py` checks the spec against the firmware source, and the
arithmetic here against the firmware itself on golden frames.
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path

import numpy as np

SPEC_PATH = Path(__file__).resolve().parents[1] / "spec" / "mel_frontend_v1.json"


@lru_cache(maxsize=None)
def load_spec(path: str | None = None) -> dict:
    with open(path or SPEC_PATH, encoding="utf-8") as f:
        spec = json.load(f)
    if len(spec["bands"]) != spec["n_mels"]:
        raise ValueError(f"{path or SPEC_PATH}: {len(spec['bands'])} bands, n_mels {spec['n_mels']}")
    return spec


def band_weights(start: int, center: int, end: int, num_bins: int) -> dict[int, float]:
    """One band's per-bin weights, exactly as `computeMelBandsFrom` builds them."""
    w: dict[int, float] = {}
    for b in range(start, min(center, num_bins - 1) + 1):
        w[b] = (b - start) / (center - start) if center > start else 1.0
    for b in range(center + 1, min(end, num_bins - 1) + 1):
        w[b] = 1.0 - (b - center) / (end - center) if end > center else 1.0
    return w


def filterbank(spec: dict | None = None, n_freqs: int | None = None) -> np.ndarray:
    """(n_mels, n_freqs) weights, each row divided by its weight sum, so `fb @ mags` is the
    firmware's weighted average. `n_freqs` defaults to n_fft//2 + 1 (the rFFT's bins); columns
    from `num_bins` on (the Nyquist bin) are zero, as the firmware never reads them."""
    spec = spec or load_spec()
    n_freqs = n_freqs or spec["n_fft"] // 2 + 1
    fb = np.zeros((spec["n_mels"], n_freqs), dtype=np.float64)
    for m, (start, center, end) in enumerate(spec["bands"]):
        w = band_weights(start, center, end, spec["num_bins"])
        total = sum(w.values())
        if total <= 0:
            # The firmware asserts the same thing at begin(): a band that reads nothing is a
            # corrupt table, never a band to fill with zeros.
            raise ValueError(f"mel band {m} {[start, center, end]} has zero weight")
        for b, v in w.items():
            fb[m, b] = v / total
    return fb


def log_map(band_energy: np.ndarray, spec: dict | None = None) -> np.ndarray:
    """Band energies -> the NN's [0, 1] input: exact 0 below the silence threshold, else
    the firmware's log map."""
    spec = spec or load_spec()
    e = np.asarray(band_energy, dtype=np.float64)
    db = 10.0 * np.log10(e + spec["log_epsilon"])
    out = np.clip((db + spec["db_range"]) / spec["db_range"], 0.0, 1.0)
    return np.where(e < spec["silence_threshold"], 0.0, out)


def mel_from_magnitudes(mags: np.ndarray, spec: dict | None = None) -> np.ndarray:
    """(n_frames, n_freqs) magnitudes -> (n_frames, n_mels) firmware mel."""
    spec = spec or load_spec()
    fb = filterbank(spec, mags.shape[1])
    return log_map(mags.astype(np.float64) @ fb.T, spec)
