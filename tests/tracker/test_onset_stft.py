"""The firmware's SuperFlux on the onset STFT matches an independent numpy one (#178).

The firmware computes SuperFlux on its own overlapped frames (Hann 512, hop 128,
right-aligned at the newest sample; SharedSpectralAnalysis::computeOnsetFrame).
This runs the host harness on a synthetic signal and recomputes the same ODF
here from the published definition (Boeck & Widmer, DAFx-13; the CPJKU
reference code's np.hanning window and filterbank, with our band edges), so a
ring-index, window-alignment, FFT or filterbank slip in the C++ shows up as a
mismatch. Needs the harness: TRACKER_HARNESS=build/tracker_harness.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

SR = 16000
N = 512
HOP = 128
LAMBDA = 32768.0
ONSET_BANDS = 85
HARNESS = os.environ.get("TRACKER_HARNESS")


def edges() -> np.ndarray:
    c = 62.5 * 2.0 ** (np.arange(0, np.log2(8000 / 62.5) * 24 + 1) / 24)
    return np.unique(np.clip(np.round(c / 31.25).astype(int), 1, 255))


def filterbank() -> np.ndarray:
    e = edges()
    fb = np.zeros((len(e) - 2, N // 2))
    for m in range(len(e) - 2):
        lo, c, hi = e[m], e[m + 1], e[m + 2]
        for k in range(lo + 1, hi):
            fb[m, k] = (k - lo) / (c - lo) if k <= c else (hi - k) / (hi - c)
    return fb


def reference_superflux(x: np.ndarray) -> dict[int, float]:
    """SuperFlux of every onset frame, keyed by its end sample (exclusive)."""
    fb = filterbank()
    win = np.hanning(N)
    padded = np.concatenate([np.zeros(N), x])
    out, prev = {}, None
    for end in range(HOP, len(x) + 1, HOP):
        frame = padded[end:end + N] * win  # padded index end + N - 1 = sample end - 1
        mag = np.abs(np.fft.rfft(frame))[: N // 2]
        logb = np.log10(LAMBDA * (fb @ mag) + 1.0)
        if prev is None:
            sf = 0.0
        else:
            ref = np.maximum(prev, np.concatenate([prev[1:], [0.0]]))
            ref[1:] = np.maximum(ref[1:], prev[:-1])
            d = logb[:ONSET_BANDS] - ref[:ONSET_BANDS]
            sf = float(d[d > 0].sum())
        out[end] = sf
        prev = logb
    return out


def signal() -> np.ndarray:
    rng = np.random.default_rng(0)
    t = np.arange(SR * 8) / SR
    x = 0.002 * rng.standard_normal(len(t)) + 0.01 * np.sin(2 * np.pi * 220 * t)
    # decaying noise bursts at arbitrary sub-frame positions
    for start in rng.uniform(0.5, 7.5, 24):
        i = int(start * SR)
        n = int(0.08 * SR)
        x[i:i + n] += 0.3 * rng.standard_normal(n) * np.exp(-np.arange(n) / (0.015 * SR))
    return np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")


@pytest.mark.skipif(not HARNESS, reason="set TRACKER_HARNESS to the built tracker harness")
def test_superflux_matches_reference(tmp_path: Path) -> None:
    pcm = signal()
    pcm.tofile(tmp_path / "x.pcm")
    subprocess.run([HARNESS, str(tmp_path / "x.pcm"), str(tmp_path / "out.csv"), "250", "0", "1",
                    f"--frame-features={tmp_path / 'frames.csv'}"], check=True)
    rows = np.genfromtxt(tmp_path / "frames.csv", delimiter=",", names=True)
    ref = reference_superflux(pcm.astype(np.float64) / 32768.0)
    ends = np.round(rows["t"] * SR).astype(int)
    got, want = [], []
    for e, sf in zip(ends, rows["superflux"], strict=True):
        if e in ref and e >= 4 * N:  # past the start-up frames
            got.append(sf)
            want.append(ref[e])
    got, want = np.array(got), np.array(want)
    assert len(got) > 0.9 * (len(pcm) // 256 - 8), "frame ends don't line up with the reference"
    assert want.max() > 5.0, "the bursts should produce clear onsets"
    # float32 firmware vs float64 reference; the dump prints 5 decimals
    np.testing.assert_allclose(got, want, rtol=1e-3, atol=2e-3)
