"""Shared audio processing utilities for the ML training pipeline.

Single source of truth for the firmware-matching mel spectrogram computation.
Both GPU (torch) and CPU (numpy) implementations are provided here to avoid
duplication across scripts.

Firmware pipeline (SharedSpectralAnalysis::getRawMelBands, v56+):
  - 16 kHz sample rate
  - Hamming window (alpha=0.54, symmetric)
  - FFT-256, hop-256 (no overlap)
  - Spectral noise subtraction (Martin 2001 min-statistics, v56)
  - 26 mel bands (60-8000 Hz), HTK mel scale, no normalization
  - Log compression: 10*log10(x + 1e-10), mapped [-60, 0] dB -> [0, 1]
  - NO compressor or whitening (raw mel path for NN input)
"""

from __future__ import annotations

import numpy as np


# ---- Firmware audio constants (must match SharedSpectralAnalysis.h) ----
SAMPLE_RATE = 16000
N_FFT = 256
HOP_LENGTH = 256
N_MELS = 26
FMIN = 60
FMAX = 8000
FRAME_RATE = SAMPLE_RATE / HOP_LENGTH  # 62.5 Hz


def build_mel_filterbank_np() -> np.ndarray:
    """Build mel filterbank as numpy array (CPU). Shape: (n_mels, n_fft//2 + 1).

    Firmware uses weighted AVERAGE (divides by sum of triangle weights per band),
    not weighted SUM. We normalize each row by its weight sum so that
    ``mel_fb @ magnitudes`` produces the same result as firmware's
    ``sum(mag*weight) / sum(weight)`` loop.
    """
    import librosa
    fb = librosa.filters.mel(
        sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS,
        fmin=FMIN, fmax=FMAX, htk=True, norm=None,
    ).astype(np.float32)
    # Normalize: divide each band's weights by their sum → weighted average
    row_sums = fb.sum(axis=1, keepdims=True)
    row_sums = np.maximum(row_sums, 1e-10)  # avoid division by zero
    fb /= row_sums
    return fb


def build_mel_filterbank_torch(cfg: dict, device) -> "torch.Tensor":
    """Build mel filterbank as torch tensor on device. Shape: (n_mels, n_fft//2 + 1).

    Reads audio params from config dict for flexibility (supports custom n_mels, fmin, etc).
    Normalized to weighted average (matching firmware ``computeMelBandsFrom()``).
    """
    import librosa
    import torch

    mel_basis = librosa.filters.mel(
        sr=cfg["audio"]["sample_rate"],
        n_fft=cfg["audio"]["n_fft"],
        n_mels=cfg["audio"]["n_mels"],
        fmin=cfg["audio"]["fmin"],
        fmax=cfg["audio"]["fmax"],
        htk=True, norm=None,
    ).astype(np.float32)
    # Normalize: divide each band's weights by their sum → weighted average
    # Matches firmware SharedSpectralAnalysis::computeMelBandsFrom() which
    # computes sum(mag*weight)/sum(weight), not just sum(mag*weight).
    row_sums = mel_basis.sum(axis=1, keepdims=True)
    row_sums = np.maximum(row_sums, 1e-10)
    mel_basis /= row_sums
    return torch.from_numpy(mel_basis).to(device)


def apply_spectral_noise_subtraction(magnitudes: np.ndarray,
                                      smooth_alpha: float = 0.92,
                                      release_factor: float = 0.999,
                                      oversubtract: float = 1.5,
                                      floor_ratio: float = 0.02) -> np.ndarray:
    """Minimum-statistics noise estimation + spectral subtraction (Martin 2001).

    Mirrors firmware SharedSpectralAnalysis::estimateAndSubtractNoise().
    Per-bin smoothed power tracking -> running minimum -> oversubtraction.
    Applied to FFT magnitudes before mel projection.

    Args:
        magnitudes: (n_freqs, n_frames) FFT magnitude array
        smooth_alpha: power smoothing EMA coefficient (0.9-0.99)
        release_factor: noise floor release rate (0.99-0.9999)
        oversubtract: oversubtraction factor (1.0-3.0)
        floor_ratio: spectral floor as fraction of original (prevents zero-out)

    Returns: (n_freqs, n_frames) noise-subtracted magnitudes
    """
    mags = magnitudes.copy()
    n_freqs, n_frames = mags.shape

    # State arrays — skip DC bin 0 (matching firmware)
    smoothed_power = np.zeros(n_freqs - 1, dtype=np.float32)
    noise_floor_est = np.zeros(n_freqs - 1, dtype=np.float32)

    for t in range(n_frames):
        mag = mags[1:, t]
        power = mag * mag

        # Smooth power estimate (EMA)
        smoothed_power[:] = smooth_alpha * smoothed_power + (1.0 - smooth_alpha) * power

        # Noise floor: instant attack to new minimums, exponential release
        new_min = (smoothed_power < noise_floor_est) | (noise_floor_est == 0.0)
        noise_floor_est[:] = np.where(
            new_min,
            smoothed_power,
            release_factor * noise_floor_est + (1.0 - release_factor) * smoothed_power,
        )

        # Spectral subtraction
        noise_mag = oversubtract * np.sqrt(noise_floor_est)
        floor = floor_ratio * mag
        clean = mag - noise_mag
        mags[1:, t] = np.maximum(clean, floor)

    return mags


def firmware_mel_spectrogram_torch(audio: "torch.Tensor", cfg: dict,
                                   mel_fb: "torch.Tensor",
                                   window: "torch.Tensor") -> np.ndarray:
    """Compute mel spectrogram matching firmware (GPU-accelerated torch).

    Args:
        audio: (samples,) tensor on GPU/CPU
        cfg: config dict with audio section
        mel_fb: precomputed mel filterbank (n_mels, n_freqs) on same device
        window: precomputed Hamming window on same device

    Returns: (n_frames, n_mels) numpy array with values in [0, 1].
    """
    import torch

    n_fft = cfg["audio"]["n_fft"]
    hop_length = cfg["audio"]["hop_length"]

    stft = torch.stft(audio, n_fft=n_fft, hop_length=hop_length,
                      window=window, center=False, return_complex=True)
    magnitudes = stft.abs()  # (n_freqs, n_frames)

    # Spectral noise subtraction (v56, Martin 2001 min-statistics)
    ns_cfg = cfg.get("audio", {}).get("noise_subtraction", {})
    if ns_cfg.get("enabled", False):
        mags_np = magnitudes.cpu().numpy()
        mags_np = apply_spectral_noise_subtraction(
            mags_np,
            smooth_alpha=ns_cfg.get("smooth_alpha", 0.92),
            release_factor=ns_cfg.get("release_factor", 0.999),
            oversubtract=ns_cfg.get("oversubtract", 1.5),
            floor_ratio=ns_cfg.get("floor_ratio", 0.02),
        )
        magnitudes = torch.from_numpy(mags_np).to(magnitudes.device)

    mel_spec = mel_fb @ magnitudes  # (n_mels, n_frames)

    use_pcen = cfg.get("features", {}).get("use_pcen", False)
    if use_pcen:
        # PCEN requires causal frame-by-frame IIR — run on CPU numpy
        mel_np = mel_spec.T.cpu().numpy()  # (n_frames, n_mels)
        pcen_cfg = cfg.get("audio", {}).get("pcen", {})
        return pcen_transform(mel_np,
                              s=pcen_cfg.get("s", PCEN_S),
                              delta=pcen_cfg.get("delta", PCEN_DELTA),
                              eps=pcen_cfg.get("eps", PCEN_EPS),
                              norm=pcen_cfg.get("norm", PCEN_NORM))
    else:
        log_mel = 10.0 * torch.log10(mel_spec + 1e-10)
        log_mel = (log_mel + 60.0) / 60.0
        log_mel = log_mel.clamp(0.0, 1.0)
        return log_mel.T.cpu().numpy()  # (n_frames, n_mels)


def firmware_mel_spectrogram_np(audio: np.ndarray,
                                noise_subtraction: bool = False,
                                use_pcen: bool = False) -> np.ndarray:
    """Compute mel spectrogram matching firmware (CPU-only numpy).

    For use in calibration scripts and tools that don't need GPU or config.

    Args:
        audio: mono audio samples at 16 kHz
        noise_subtraction: if True, apply min-statistics noise subtraction
        use_pcen: if True, use PCEN instead of log compression

    Returns: (n_frames, n_mels) numpy array with values in [0, 1].
    """
    from scipy.signal.windows import hamming

    window = hamming(N_FFT, sym=True).astype(np.float32)
    mel_fb = build_mel_filterbank_np()

    n_frames = len(audio) // HOP_LENGTH

    if noise_subtraction:
        # Build full magnitude spectrogram, apply noise subtraction, then mel
        n_freqs = N_FFT // 2 + 1
        all_mags = np.zeros((n_freqs, n_frames), dtype=np.float32)
        for i in range(n_frames):
            frame = audio[i * HOP_LENGTH:(i * HOP_LENGTH) + N_FFT]
            if len(frame) < N_FFT:
                break
            windowed = frame * window
            all_mags[:, i] = np.abs(np.fft.rfft(windowed))
        all_mags = apply_spectral_noise_subtraction(all_mags)
        mel_spec = mel_fb @ all_mags  # (n_mels, n_frames)
        if use_pcen:
            return pcen_transform(mel_spec.T)
        log_mel = 10.0 * np.log10(mel_spec + 1e-10)
        log_mel = (log_mel + 60.0) / 60.0
        return np.clip(log_mel.T, 0.0, 1.0)

    # Build mel spectrogram frame by frame
    n_freqs = N_FFT // 2 + 1
    all_mags = np.zeros((n_freqs, n_frames), dtype=np.float32)
    for i in range(n_frames):
        frame = audio[i * HOP_LENGTH:(i * HOP_LENGTH) + N_FFT]
        if len(frame) < N_FFT:
            break
        windowed = frame * window
        all_mags[:, i] = np.abs(np.fft.rfft(windowed))

    mel_spec = mel_fb @ all_mags  # (n_mels, n_frames)
    if use_pcen:
        return pcen_transform(mel_spec.T)
    log_mel = 10.0 * np.log10(mel_spec + 1e-10)
    log_mel = (log_mel + 60.0) / 60.0
    return np.clip(log_mel.T, 0.0, 1.0)


# ---- PCEN constants (must match firmware SharedSpectralAnalysis) ----
PCEN_S = 0.025       # IIR smoother coefficient (~0.64s time constant at 62.5 Hz)
PCEN_DELTA = 2.0     # Stabilizer bias
PCEN_EPS = 1e-6      # Numerical floor
PCEN_NORM = 4.0      # Output normalization divisor


def pcen_transform(mel_linear: np.ndarray,
                   s: float = PCEN_S,
                   delta: float = PCEN_DELTA,
                   eps: float = PCEN_EPS,
                   norm: float = PCEN_NORM) -> np.ndarray:
    """Per-Channel Energy Normalization (Lostanlen & Salamon 2019).

    Replaces log compression with adaptive gain normalization. Processes
    frames causally (matching firmware order). More robust to mic gain
    and room variation than static log compression.

    With alpha=1.0 and r=0.5, the formula simplifies to:
      M[f,t] = s * E + (1-s) * M[f,t-1]   (IIR smoother)
      P[f,t] = sqrt(E/(eps+M) + delta) - sqrt(delta)

    Args:
        mel_linear: (n_frames, n_mels) raw mel energies (linear, NOT log-compressed)

    Returns: (n_frames, n_mels) PCEN-normalized values clipped to [0, 1].
    """
    n_frames, n_mels = mel_linear.shape
    out = np.zeros_like(mel_linear)
    sqrt_delta = np.sqrt(delta)

    # Initialize IIR state from first frame (avoids startup transient)
    M = mel_linear[0].copy()

    for t in range(n_frames):
        E = mel_linear[t]
        M = s * E + (1.0 - s) * M
        agc = E / (eps + M)
        out[t] = np.sqrt(agc + delta) - sqrt_delta

    # Normalize to [0, 1]
    return np.clip(out / norm, 0.0, 1.0)


def load_config(config_path: str) -> dict:
    """Load a YAML config file, merging with base.yaml if present.

    Model configs (default.yaml, wider_rf.yaml) only contain overrides.
    base.yaml in the same directory provides shared audio/training/data settings.
    The model config's values take precedence over base.yaml on conflict.
    """
    import yaml
    from pathlib import Path

    config_dir = Path(config_path).parent
    base_path = config_dir / "base.yaml"

    # Start with base config if it exists
    if base_path.exists() and Path(config_path).name != "base.yaml":
        with open(base_path) as f:
            cfg = yaml.safe_load(f) or {}
    else:
        cfg = {}

    # Merge model-specific overrides
    with open(config_path) as f:
        overrides = yaml.safe_load(f) or {}

    _deep_merge(cfg, overrides)
    return cfg


def _deep_merge(base: dict, overrides: dict) -> None:
    """Recursively merge overrides into base dict (mutates base)."""
    for key, value in overrides.items():
        if key in base and isinstance(base[key], dict) and isinstance(value, dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value


def append_delta_features(mel: np.ndarray) -> np.ndarray:
    """Append first-order mel differences as additional input channels.

    Input shape: (n_frames, n_mels)
    Output shape: (n_frames, 2*n_mels)

    Delta features explicitly provide spectral flux -- the #1 traditional onset
    detection signal (Rong Gong et al. 2018, Schluter & Bock 2014).
    """
    delta = np.zeros_like(mel)
    delta[1:] = mel[1:] - mel[:-1]
    return np.concatenate([mel, delta], axis=-1)


def append_band_flux_features(mel: np.ndarray) -> np.ndarray:
    """Append 3 band-grouped HWR mel flux channels.

    Compact alternative to 26 per-band delta channels. Computes
    half-wave-rectified (positive-only) mel differences within 3 frequency
    bands matching firmware's bass/mid/high grouping:
      - Bass: mel bands 0-5   (roughly 60-375 Hz, kicks)
      - Mid:  mel bands 6-13  (roughly 400-2000 Hz, vocals/pads)
      - High: mel bands 14-25 (roughly 2-8 kHz, snares/hi-hats)

    Each band's flux is normalized by the number of mel bands in that group.

    Input shape: (n_frames, 26)
    Output shape: (n_frames, 29)
    """
    n_frames = mel.shape[0]
    flux = np.zeros((n_frames, 3), dtype=np.float32)

    # Band boundaries (mel band indices)
    BASS = slice(0, 6)    # 6 bands
    MID = slice(6, 14)    # 8 bands
    HIGH = slice(14, 26)  # 12 bands

    for t in range(1, n_frames):
        diff = mel[t] - mel[t - 1]
        pos_diff = np.maximum(diff, 0.0)  # HWR: only positive changes (onsets)
        flux[t, 0] = pos_diff[BASS].sum() / 6.0
        flux[t, 1] = pos_diff[MID].sum() / 8.0
        flux[t, 2] = pos_diff[HIGH].sum() / 12.0

    return np.concatenate([mel, flux], axis=-1)
