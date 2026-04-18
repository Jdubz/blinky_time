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
LOG_EPSILON = 1e-7   # Matches firmware (1e-7 avoids ARM Cortex-M4 denormals)


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

    Chunks long audio to avoid CUDA OOM on tracks >3 minutes.

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

    # Guard: audio must be at least n_fft samples for STFT.
    # Short/corrupt files (some LOFI mp3s decode to < 256 samples) would crash torch.stft.
    if len(audio) < n_fft:
        raise ValueError(
            f"Audio too short for STFT: {len(audio)} samples < n_fft={n_fft}"
        )

    # Chunk long audio to avoid GPU OOM. 30s at 16kHz = 480K samples.
    # STFT of 480K samples needs ~75 MB GPU; safe even when augmentation
    # variants consume most of the 10 GB GPU memory.
    # Note: chunk boundaries lose n_fft samples of context (16ms at 16kHz).
    # Acceptable for training data; not suitable for frame-exact evaluation.
    # Each chunk's magnitudes are moved to CPU immediately to avoid accumulating
    # GPU memory — torch.cat on GPU with many chunks caused OOM on long tracks.
    max_samples = 480_000  # 30s at 16kHz
    if len(audio) > max_samples:
        chunks = []
        for start in range(0, len(audio), max_samples):
            chunk = audio[start:start + max_samples]
            if len(chunk) < n_fft:
                break  # Last chunk too short for STFT, discard
            stft = torch.stft(chunk, n_fft=n_fft, hop_length=hop_length,
                              window=window, center=False, return_complex=True)
            chunks.append(stft.abs().cpu())
        magnitudes = torch.cat(chunks, dim=1).to(audio.device)
    else:
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
        log_eps = cfg.get("audio", {}).get("log_epsilon", LOG_EPSILON)
        db_range = float(cfg.get("audio", {}).get("mel_db_range", 60))
        log_mel = 10.0 * torch.log10(mel_spec + log_eps)
        log_mel = (log_mel + db_range) / db_range
        log_mel = log_mel.clamp(0.0, 1.0)
        return log_mel.T.cpu().numpy()  # (n_frames, n_mels)


def firmware_mel_spectrogram_np(audio: np.ndarray,
                                noise_subtraction: bool = False,
                                use_pcen: bool = False,
                                mel_db_range: float = 60.0) -> np.ndarray:
    """Compute mel spectrogram matching firmware (CPU-only numpy).

    For use in calibration scripts and tools that don't need GPU or config.

    Args:
        audio: mono audio samples at 16 kHz
        noise_subtraction: if True, apply min-statistics noise subtraction
        use_pcen: if True, use PCEN instead of log compression
        mel_db_range: log-mel dB range (must match firmware MEL_DB_RANGE, default 60)

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
        log_mel = 10.0 * np.log10(mel_spec + LOG_EPSILON)
        log_mel = (log_mel + mel_db_range) / mel_db_range
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
    log_mel = 10.0 * np.log10(mel_spec + LOG_EPSILON)
    log_mel = (log_mel + mel_db_range) / mel_db_range
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


def append_hybrid_features(mel: np.ndarray, audio: np.ndarray | None = None,
                           sr: int = 16000, n_fft: int = 256, hop: int = 256,
                           mel_db_range: float = 60.0) -> np.ndarray:
    """Append spectral flatness + SuperFlux spectral flux to mel features.

    These deterministic features directly encode the discrimination the
    model struggles to learn from mel bands alone:
      - Spectral flatness (Wiener entropy): drums=noisy ~0.5-0.8, tones ~0.1-0.3
      - SuperFlux spectral flux: high at broadband transients, zero during sustain

    When audio waveform is provided, both features are computed from the STFT
    magnitude spectrum, matching firmware's SharedSpectralAnalysis exactly:
      - Flatness: Wiener entropy from bins 1..NUM_BINS-1 (computeDerivedFeatures)
      - Flux: SuperFlux with 3-wide frequency max filter + band weighting (process)

    Without audio, falls back to mel-based approximations (less accurate).
    All STFT operations are vectorized (batch FFT, no Python per-frame loops).

    Input shape: (n_frames, n_mels)
    Output shape: (n_frames, n_mels + 2)
    """
    n_frames, n_mels = mel.shape
    extra = np.zeros((n_frames, 2), dtype=np.float32)

    if audio is not None and len(audio) >= n_fft:
        # --- STFT-based features matching firmware SharedSpectralAnalysis ---
        # Firmware applies Hamming window → FFT → magnitudes, then computes
        # both flatness (computeDerivedFeatures) and flux (SuperFlux) from the
        # same magnitude spectrum. We replicate this with batch FFT.
        n_valid = min(n_frames, (len(audio) - n_fft) // hop + 1)
        if n_valid > 0:
            # Batch STFT: reshape audio into (n_valid, n_fft), apply window, FFT
            starts = np.arange(n_valid) * hop
            indices = starts[:, np.newaxis] + np.arange(n_fft)
            frames = audio[indices] * np.hamming(n_fft).astype(np.float32)
            spectra = np.fft.rfft(frames, axis=1)        # (n_valid, n_fft/2+1)
            mags = np.abs(spectra[:, 1:])                 # Skip DC → (n_valid, 127)
            mags = np.maximum(mags, 1e-10)

            # Feature 0: Spectral flatness (Wiener entropy)
            # Matches firmware computeDerivedFeatures(): exp(mean(log(mags))) / mean(mags)
            log_mean = np.log(mags).mean(axis=1)
            geo_mean = np.exp(log_mean)
            ari_mean = mags.mean(axis=1)
            flatness = np.where(ari_mean > 1e-10, geo_mean / ari_mean, 0.0)
            extra[:n_valid, 0] = np.clip(flatness, 0.0, 1.0)

            # Feature 1: SuperFlux spectral flux (Bock & Widmer 2013)
            # Matches firmware process() exactly:
            #   1. 3-wide frequency max filter on previous frame's magnitudes
            #   2. HWR difference: max(0, mag[t] - maxfiltered_prev[t])
            #   3. Band-weighted sum: bass(1-6)*0.5 + mid(7-32)*0.2 + high(33-127)*0.3
            #   4. Each band normalized by bin count
            #
            # Firmware bin ranges (0-indexed after DC skip, so bin 0 here = firmware bin 1):
            #   Bass: firmware bins 1-6  → mags[:, 0:6]
            #   Mid:  firmware bins 7-32 → mags[:, 6:32]
            #   High: firmware bins 33-127 → mags[:, 32:]
            n_bins = mags.shape[1]  # 127
            BASS_COUNT = 6.0
            MID_COUNT = 26.0
            HIGH_COUNT = float(n_bins - 32)  # 95

            # Vectorized 3-wide max filter on previous frames: (n_valid-1, n_bins)
            prev = mags[:-1]  # frames 0..n_valid-2
            ref = prev.copy()
            ref[:, 1:] = np.maximum(ref[:, 1:], prev[:, :-1])   # left neighbor
            ref[:, :-1] = np.maximum(ref[:, :-1], prev[:, 1:])  # right neighbor

            # HWR difference: current - maxfiltered_prev, clipped to >= 0
            diff = np.maximum(mags[1:] - ref, 0.0)  # (n_valid-1, n_bins)

            # Band-weighted sum (firmware weights: bass=0.5, mid=0.2, high=0.3)
            bass_flux = diff[:, 0:6].sum(axis=1) / BASS_COUNT
            mid_flux = diff[:, 6:32].sum(axis=1) / MID_COUNT
            high_flux = diff[:, 32:].sum(axis=1) / HIGH_COUNT
            flux = 0.5 * bass_flux + 0.2 * mid_flux + 0.3 * high_flux
            extra[1:n_valid, 1] = flux
    else:
        # Fallback: approximate both features from mel bands.
        # WARNING: mel-based features diverge from STFT-based values. This fallback
        # should NOT be used for v27+ hybrid training — pass audio_np instead.

        # Flatness: Wiener entropy on mel energy (reverse log compression)
        linear = np.power(10.0, (mel * mel_db_range - mel_db_range) / 10.0)
        linear = np.maximum(linear, 1e-10)
        log_mean = np.log(linear).mean(axis=1)
        geo_mean = np.exp(log_mean)
        ari_mean = linear.mean(axis=1)
        flatness = np.where(ari_mean > 1e-10, geo_mean / ari_mean, 0.0)
        extra[:, 0] = np.clip(flatness, 0.0, 1.0)

        # Flux: simple HWR mel diff (no SuperFlux max filter, no band weighting)
        diff = np.maximum(mel[1:] - mel[:-1], 0.0)
        extra[1:, 1] = diff.sum(axis=1) / n_mels

    return np.concatenate([mel, extra], axis=-1)
