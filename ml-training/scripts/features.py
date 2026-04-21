"""Shared feature computation and inference utilities for onset detection.

Ensures band-flux computation matches firmware FrameOnsetNN::computeBandFlux()
exactly. Used by both evaluate.py and replay_device_capture.py.
"""

import numpy as np
import torch


def compute_band_flux(mel: np.ndarray) -> np.ndarray:
    """Compute 3-channel HWR band-flux from mel frame differences (vectorized).

    Matches firmware FrameOnsetNN::computeBandFlux() exactly:
      bass  = sum(max(0, mel[t] - mel[t-1]) for bands 0-5)  / 6
      mid   = sum(max(0, mel[t] - mel[t-1]) for bands 6-13) / 8
      high  = sum(max(0, mel[t] - mel[t-1]) for bands 14-25) / 12

    Args:
        mel: (n_frames, n_mels) mel band values (n_mels >= 26)

    Returns: (n_frames, 3) band-flux features [bass, mid, high]
    """
    assert mel.shape[1] >= 26, f"compute_band_flux requires >=26 mel bands, got {mel.shape[1]}"
    diff = np.concatenate([np.zeros((1, mel.shape[1]), dtype=np.float32),
                           mel[1:] - mel[:-1]])
    pos = np.maximum(diff, 0.0)
    flux = np.empty((mel.shape[0], 3), dtype=np.float32)
    flux[:, 0] = pos[:, 0:6].sum(axis=1) / 6.0
    flux[:, 1] = pos[:, 6:14].sum(axis=1) / 8.0
    flux[:, 2] = pos[:, 14:26].sum(axis=1) / 12.0
    return flux


def append_features(mel: np.ndarray, use_delta: bool = False,
                    use_band_flux: bool = False,
                    use_hybrid: bool = False,
                    audio: np.ndarray | None = None,
                    mel_db_range: float = 60.0) -> np.ndarray:
    """Append delta, band-flux, or hybrid features to mel data.

    Args:
        mel: (n_frames, n_mels) mel band values
        use_delta: append first-order difference (n_mels → 2*n_mels features)
        use_band_flux: append 3-channel HWR flux (n_mels → n_mels+3 features)
        use_hybrid: append spectral flatness + flux (n_mels → n_mels+2 features)
        audio: raw audio waveform (for STFT-based flatness in hybrid mode)
        mel_db_range: dB range for mel log compression reversal (fallback path)

    Returns: (n_frames, features) array
    """
    if use_delta:
        delta = np.zeros_like(mel)
        delta[1:] = mel[1:] - mel[:-1]
        return np.concatenate([mel, delta], axis=1)
    elif use_band_flux:
        flux = compute_band_flux(mel)
        return np.concatenate([mel, flux], axis=1)
    elif use_hybrid:
        # Legacy API: use_hybrid=True maps to the v27-era [flatness, raw_flux]
        # pair. New callers should use append_hybrid_features directly with
        # an explicit features= list (v28 adds crest + hfc).
        from scripts.audio import append_hybrid_features
        return append_hybrid_features(
            mel, audio=audio, mel_db_range=mel_db_range,
            features=["flatness", "raw_flux"],
        )
    return mel


def sliding_window_inference(mel_features: np.ndarray, model,
                             window_frames: int,
                             device: torch.device) -> np.ndarray:
    """Run model inference on mel features with overlapping sliding window.

    Uses 50% overlapping chunks with averaging at overlap regions.
    Same logic as firmware's circular buffer inference but batched offline.

    Note: the final partial window is zero-padded when it extends beyond the
    signal. Frames near the end may have biased activations due to padding
    context.

    Args:
        mel_features: (n_frames, n_features) input features
        model: PyTorch model in eval mode
        window_frames: model's expected input window size
        device: torch device

    Returns: (n_frames,) onset activation array
    """
    n_frames = mel_features.shape[0]
    mel_tensor = torch.from_numpy(mel_features).to(device)

    activations = np.zeros(n_frames, dtype=np.float32)
    counts = np.zeros(n_frames, dtype=np.float32)
    stride = window_frames // 2

    with torch.no_grad():
        for start in range(0, n_frames, stride):
            end = start + window_frames
            if end > n_frames:
                chunk = torch.zeros(window_frames, mel_features.shape[1],
                                    device=device)
                chunk[:n_frames - start] = mel_tensor[start:n_frames]
                actual_len = n_frames - start
            else:
                chunk = mel_tensor[start:end]
                actual_len = window_frames

            pred = model(chunk.unsqueeze(0))[0]  # (time, channels)
            activations[start:start + actual_len] += pred[:actual_len, 0].cpu().numpy()
            counts[start:start + actual_len] += 1

    activations /= np.maximum(counts, 1)
    return activations
