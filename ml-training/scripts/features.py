"""Shared feature computation utilities for onset detection.

Ensures band-flux computation matches firmware FrameOnsetNN::computeBandFlux()
exactly. Used by both evaluate.py and replay_device_capture.py.
"""

import numpy as np


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
    diff = np.concatenate([np.zeros((1, mel.shape[1]), dtype=np.float32),
                           mel[1:] - mel[:-1]])
    pos = np.maximum(diff, 0.0)
    flux = np.empty((mel.shape[0], 3), dtype=np.float32)
    flux[:, 0] = pos[:, 0:6].sum(axis=1) / 6.0
    flux[:, 1] = pos[:, 6:14].sum(axis=1) / 8.0
    flux[:, 2] = pos[:, 14:26].sum(axis=1) / 12.0
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
