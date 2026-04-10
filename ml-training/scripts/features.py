"""Shared feature computation utilities for onset detection.

Ensures band-flux computation matches firmware FrameOnsetNN::computeBandFlux()
exactly. Used by both evaluate.py and replay_device_capture.py.
"""

import numpy as np


# Band boundaries matching firmware (FrameOnsetNN.h computeBandFlux)
BASS_BANDS = slice(0, 6)    # bands 0-5
MID_BANDS = slice(6, 14)    # bands 6-13
HIGH_BANDS = slice(14, 26)  # bands 14-25


def compute_band_flux(mel: np.ndarray) -> np.ndarray:
    """Compute 3-channel HWR band-flux from mel frame differences.

    Matches firmware FrameOnsetNN::computeBandFlux() exactly:
      bass  = sum(max(0, mel[t] - mel[t-1]) for bands 0-5)  / 6
      mid   = sum(max(0, mel[t] - mel[t-1]) for bands 6-13) / 8
      high  = sum(max(0, mel[t] - mel[t-1]) for bands 14-25) / 12

    Args:
        mel: (n_frames, 26) mel band values

    Returns: (n_frames, 3) band-flux features [bass, mid, high]
    """
    n_frames = mel.shape[0]
    flux = np.zeros((n_frames, 3), dtype=np.float32)
    for t in range(1, n_frames):
        diff = mel[t] - mel[t - 1]
        pos = np.maximum(diff, 0.0)
        flux[t, 0] = pos[BASS_BANDS].sum() / 6.0
        flux[t, 1] = pos[MID_BANDS].sum() / 8.0
        flux[t, 2] = pos[HIGH_BANDS].sum() / 12.0
    return flux


def append_features(mel: np.ndarray, use_delta: bool = False,
                    use_band_flux: bool = False) -> np.ndarray:
    """Append delta or band-flux features to mel data, matching firmware logic.

    Args:
        mel: (n_frames, 26) mel band values
        use_delta: append first-order difference (26 → 52 features)
        use_band_flux: append 3-channel HWR flux (26 → 29 features)

    Returns: (n_frames, features) array
    """
    if use_delta:
        delta = np.zeros_like(mel)
        delta[1:] = mel[1:] - mel[:-1]
        return np.concatenate([mel, delta], axis=1)
    elif use_band_flux:
        flux = compute_band_flux(mel)
        return np.concatenate([mel, flux], axis=1)
    return mel
