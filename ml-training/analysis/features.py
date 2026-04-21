"""Candidate onset-discriminator features — Phase 1 catalog.

Ten spectral / transient / shape descriptors computed from a shared STFT.
All functions are vectorized over frames and return a (n_frames,) array.

Framing matches firmware (`SharedSpectralAnalysis`):
  - sr = 16000, frame_length = 256, hop = 256 (no overlap)
  - Hamming window (symmetric)
  - 129 complex bins; bin 0 (DC) is skipped by every feature below

The features do not target any specific label — discriminative power is
measured downstream against TP/FP/TN frame labels. See
`ml-training/analysis/run_catalog.py`.
"""

from __future__ import annotations

import numpy as np

# --- Framing constants (match firmware SharedSpectralAnalysis) ---
SR = 16000
N_FFT = 256
HOP = 256

# Band splits match firmware spectral flux bands (post-DC-skip indices).
# Firmware bins 1..6 → mags[:, 0:6]; 7..32 → mags[:, 6:32]; 33..127 → mags[:, 32:].
BASS_SLICE = slice(0, 6)   # ~62–375 Hz at sr/N_FFT = 62.5 Hz/bin
MID_SLICE = slice(6, 32)   # ~375–2000 Hz
HIGH_SLICE = slice(32, None)  # ~2000–8000 Hz
BASS_COUNT = 6.0
MID_COUNT = 26.0
HIGH_COUNT = 95.0  # 127 - 32

_EPS = 1e-10


# Firmware `SharedSpectralAnalysis` uses NUM_BINS = FFT_SIZE / 2 = 128 internal
# bins (DC at 0, positive frequencies at 1..127) and discards the Nyquist bin
# (rfft index 128). Feature loops iterate i=1..127, giving 127 active bins.
# We match that exactly: drop DC (index 0) and Nyquist (index 128), keep 127.
NUM_BINS = N_FFT // 2 - 1  # 127


def compute_stft(audio: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Compute per-frame magnitude + phase spectra.

    Returns (mags, phases), each shape (n_frames, NUM_BINS) — DC and Nyquist
    dropped, so column index 0 corresponds to firmware bin 1 and column 126 to
    firmware bin 127. This matches `SharedSpectralAnalysis` exactly.
    """
    if audio.ndim != 1:
        raise ValueError(f"audio must be 1D, got shape {audio.shape}")
    n_frames = max(0, (len(audio) - N_FFT) // HOP + 1)
    if n_frames == 0:
        return (
            np.zeros((0, NUM_BINS), dtype=np.float32),
            np.zeros((0, NUM_BINS), dtype=np.float32),
        )
    starts = np.arange(n_frames) * HOP
    idx = starts[:, None] + np.arange(N_FFT)
    frames = audio[idx] * np.hamming(N_FFT).astype(np.float32)
    spectra = np.fft.rfft(frames, axis=1)
    # Slice [1:N_FFT//2] → bins 1..127, dropping DC and Nyquist to match firmware.
    mags = np.abs(spectra[:, 1 : N_FFT // 2]).astype(np.float32)
    phases = np.angle(spectra[:, 1 : N_FFT // 2]).astype(np.float32)
    return mags, phases


# -------------------------------------------------------------------- spectral shape


def spectral_flatness(mags: np.ndarray) -> np.ndarray:
    """Wiener entropy: geo_mean / arith_mean over bins.

    ~0.1–0.3 for tonal (pitched) content, ~0.5–0.8 for noise (drums, cymbals).
    """
    safe = np.maximum(mags, _EPS)
    log_mean = np.log(safe).mean(axis=1)
    geo = np.exp(log_mean)
    ari = safe.mean(axis=1)
    return np.where(ari > _EPS, geo / ari, 0.0).astype(np.float32)


def spectral_centroid(mags: np.ndarray) -> np.ndarray:
    """Centre-of-mass bin index. Drums shift centroid per hit; steady tones do not.

    Uses firmware bin indices (mags[:, 0] corresponds to firmware bin 1, so
    k starts at 1) — this matches `SharedSpectralAnalysis::computeShapeFeaturesRaw`
    exactly and keeps the parity harness bit-perfect.
    """
    k = np.arange(1, mags.shape[1] + 1, dtype=np.float32)
    num = (mags * k).sum(axis=1)
    den = np.maximum(mags.sum(axis=1), _EPS)
    return (num / den).astype(np.float32)


def spectral_rolloff(mags: np.ndarray, percentile: float = 0.85) -> np.ndarray:
    """Bin index below which `percentile` of energy lies. Low for narrow-band tones."""
    energy = mags**2
    cumulative = np.cumsum(energy, axis=1)
    total = cumulative[:, -1:]
    thresh = percentile * np.maximum(total, _EPS)
    # argmax on a boolean picks the first True; zero-energy frames → idx 0.
    idx = (cumulative >= thresh).argmax(axis=1)
    return idx.astype(np.float32)


def crest_factor(mags: np.ndarray) -> np.ndarray:
    """Peak-to-RMS ratio. High for transients, low for sustained tones.

    Near-silence frames (where total energy is below 1e-10) return 0.0 to
    match `SharedSpectralAnalysis::computeShapeFeaturesRaw`'s early-zero
    branch and keep the parity harness bit-perfect.
    """
    peak = mags.max(axis=1)
    energy = (mags**2).sum(axis=1)
    rms = np.sqrt(energy / float(mags.shape[1]))
    out = np.zeros_like(peak)
    valid = (energy > 1e-10) & np.isfinite(energy) & (rms > 1e-10)
    out[valid] = peak[valid] / rms[valid]
    return out.astype(np.float32)


def renyi_entropy_alpha2(mags: np.ndarray) -> np.ndarray:
    """Rényi entropy of order 2 on power-normalized spectrum.

    Sharper than Shannon entropy for near-deterministic signals; more
    robust than flatness under INT8 quantization (no log of tiny values).
    """
    p = mags**2
    p = p / np.maximum(p.sum(axis=1, keepdims=True), _EPS)
    p2 = np.maximum((p**2).sum(axis=1), _EPS)
    return (-np.log(p2)).astype(np.float32)


def kick_band_ratio(mags: np.ndarray) -> np.ndarray:
    """Bass-band energy as a fraction of total. Generalizes the on-device bass gate."""
    energy = mags**2
    bass = energy[:, BASS_SLICE].sum(axis=1)
    total = np.maximum(energy.sum(axis=1), _EPS)
    return (bass / total).astype(np.float32)


def high_frequency_content(mags: np.ndarray) -> np.ndarray:
    """Masri 1996: sum of bin-weighted energy.

    HFC = Σ k · |X[k]|². Percussion (especially snares) has broadband
    high-frequency content; tonal impulses concentrate at low bins.

    Uses firmware bin indices (k = 1..N, not 0..N-1) — matches
    `SharedSpectralAnalysis::computeShapeFeaturesRaw` exactly.
    """
    k = np.arange(1, mags.shape[1] + 1, dtype=np.float32)
    return ((mags**2) * k).sum(axis=1).astype(np.float32)


# ------------------------------------------------------------------- transient / phase


def raw_superflux(mags: np.ndarray) -> np.ndarray:
    """Bock & Widmer 2013 SuperFlux with 3-wide frequency max filter.

    Mirrors firmware `SharedSpectralAnalysis` raw flux loop exactly:
      bass=0.5, mid=0.2, high=0.3 band weights; each band normalized by bin count.
    Leading frame is 0 (no reference).
    """
    n_frames, n_bins = mags.shape
    out = np.zeros(n_frames, dtype=np.float32)
    if n_frames < 2:
        return out

    prev = mags[:-1]
    ref = prev.copy()
    ref[:, 1:] = np.maximum(ref[:, 1:], prev[:, :-1])
    ref[:, :-1] = np.maximum(ref[:, :-1], prev[:, 1:])
    diff = np.maximum(mags[1:] - ref, 0.0)
    bass = diff[:, BASS_SLICE].sum(axis=1) / BASS_COUNT
    mid = diff[:, MID_SLICE].sum(axis=1) / MID_COUNT
    high = diff[:, HIGH_SLICE].sum(axis=1) / HIGH_COUNT
    out[1:] = 0.5 * bass + 0.2 * mid + 0.3 * high
    return out


def complex_spectral_difference(mags: np.ndarray, phases: np.ndarray) -> np.ndarray:
    """Bello 2004 complex domain ODF.

    Each bin's expected next value (assuming constant frequency) is
    compared against the observed complex value; the magnitude of the
    residual summed over bins is the novelty. Suppresses sustained
    tones with stable magnitude + phase.
    """
    n_frames, n_bins = mags.shape
    out = np.zeros(n_frames, dtype=np.float32)
    if n_frames < 3:
        return out

    # Principal-value phase differences: wrap to (-π, π]
    dphi = phases[1:] - phases[:-1]
    dphi = (dphi + np.pi) % (2.0 * np.pi) - np.pi  # (n-1, bins)

    # Predicted phase at frame t: phases[t-1] + (phases[t-1] - phases[t-2])
    predicted_phase = phases[1:-1] + dphi[:-1]  # (n-2, bins)
    observed_phase = phases[2:]  # (n-2, bins)

    predicted_mag = mags[1:-1]  # assume constant magnitude
    observed_mag = mags[2:]

    # |observed - predicted| in complex plane = sqrt(|a|² + |b|² − 2|a||b|cos(Δφ))
    cos_delta = np.cos(observed_phase - predicted_phase)
    sq = (
        observed_mag**2 + predicted_mag**2 - 2.0 * observed_mag * predicted_mag * cos_delta
    )
    residual = np.sqrt(np.maximum(sq, 0.0))
    out[2:] = residual.sum(axis=1)
    return out


def weighted_phase_deviation(mags: np.ndarray, phases: np.ndarray) -> np.ndarray:
    """Dixon 2006 WPD: magnitude-weighted second phase derivative.

    A stable tone has linear phase progression → small 2nd derivative.
    A transient scatters phase randomly → large 2nd derivative.
    Weighting by magnitude suppresses phase noise in quiet bins.
    """
    n_frames = mags.shape[0]
    out = np.zeros(n_frames, dtype=np.float32)
    if n_frames < 3:
        return out

    dphi = np.diff(phases, axis=0)
    dphi = (dphi + np.pi) % (2.0 * np.pi) - np.pi  # wrap
    d2phi = np.diff(dphi, axis=0)  # (n-2, bins)
    d2phi = (d2phi + np.pi) % (2.0 * np.pi) - np.pi  # wrap again

    # Magnitude weighting: use current-frame magnitude at the aligned index.
    weights = mags[2:]
    weighted = np.abs(d2phi) * weights
    denom = np.maximum(weights.sum(axis=1), _EPS)
    out[2:] = weighted.sum(axis=1) / denom
    return out


# ------------------------------------------------------------------- orchestration


FEATURE_FNS = {
    "flatness": lambda m, p: spectral_flatness(m),
    "raw_flux": lambda m, p: raw_superflux(m),
    "complex_sd": lambda m, p: complex_spectral_difference(m, p),
    "hfc": lambda m, p: high_frequency_content(m),
    "wpd": lambda m, p: weighted_phase_deviation(m, p),
    "kick_ratio": lambda m, p: kick_band_ratio(m),
    "crest": lambda m, p: crest_factor(m),
    "renyi": lambda m, p: renyi_entropy_alpha2(m),
    "centroid": lambda m, p: spectral_centroid(m),
    "rolloff": lambda m, p: spectral_rolloff(m),
}


def compute_all_features(audio: np.ndarray) -> dict[str, np.ndarray]:
    """Run every catalog feature against a single audio clip.

    Returns a dict of {name: (n_frames,) float32} aligned to the same
    framing as `compute_stft`. Guaranteed to return every key in
    FEATURE_FNS even if a feature produces zeros for edge cases.
    """
    mags, phases = compute_stft(audio)
    return {name: fn(mags, phases) for name, fn in FEATURE_FNS.items()}


def frame_times(n_frames: int) -> np.ndarray:
    """Time in seconds for the centre of each frame."""
    return (np.arange(n_frames, dtype=np.float64) * HOP + HOP / 2.0) / SR
