"""Shared audio processing utilities for the ML training pipeline.

Single source of truth for the firmware-matching mel spectrogram computation.
Both GPU (torch) and CPU (numpy) implementations are provided here to avoid
duplication across scripts.

Firmware pipeline (SharedSpectralAnalysis::getRawMelBands):
  - 16 kHz sample rate
  - Hamming window (alpha=0.54, symmetric)
  - FFT-256, hop-256 (no overlap)
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
    """Build mel filterbank as numpy array (CPU). Shape: (n_mels, n_fft//2 + 1)."""
    import librosa
    return librosa.filters.mel(
        sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS,
        fmin=FMIN, fmax=FMAX, htk=True, norm=None,
    ).astype(np.float32)


def build_mel_filterbank_torch(cfg: dict, device) -> "torch.Tensor":
    """Build mel filterbank as torch tensor on device. Shape: (n_mels, n_fft//2 + 1).

    Reads audio params from config dict for flexibility (supports custom n_mels, fmin, etc).
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
    )
    return torch.from_numpy(mel_basis.astype(np.float32)).to(device)


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

    mel_spec = mel_fb @ magnitudes
    log_mel = 10.0 * torch.log10(mel_spec + 1e-10)
    log_mel = (log_mel + 60.0) / 60.0
    log_mel = log_mel.clamp(0.0, 1.0)

    return log_mel.T.cpu().numpy()  # (n_frames, n_mels)


def firmware_mel_spectrogram_np(audio: np.ndarray) -> np.ndarray:
    """Compute mel spectrogram matching firmware (CPU-only numpy).

    For use in calibration scripts and tools that don't need GPU or config.

    Returns: (n_frames, n_mels) numpy array with values in [0, 1].
    """
    from scipy.signal.windows import hamming

    window = hamming(N_FFT, sym=True).astype(np.float32)
    mel_fb = build_mel_filterbank_np()

    n_frames = len(audio) // HOP_LENGTH
    mels = np.zeros((n_frames, N_MELS), dtype=np.float32)

    for i in range(n_frames):
        frame = audio[i * HOP_LENGTH:(i * HOP_LENGTH) + N_FFT]
        if len(frame) < N_FFT:
            break
        windowed = frame * window
        spectrum = np.abs(np.fft.rfft(windowed))
        mel_spec = mel_fb @ spectrum
        log_mel = 10.0 * np.log10(mel_spec + 1e-10)
        log_mel = (log_mel + 60.0) / 60.0
        mels[i] = np.clip(log_mel, 0.0, 1.0)

    return mels


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
