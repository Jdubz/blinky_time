#!/usr/bin/env python3
"""Extract mel spectrograms and onset activation targets from labeled audio.

Replicates the firmware's SharedSpectralAnalysis mel pipeline (raw path, v56+):
  - 16 kHz sample rate
  - Hamming window (alpha=0.54, beta=0.46)
  - FFT-256, hop-256 (no overlap)
  - Spectral noise subtraction (Martin 2001 min-statistics, v56)
  - 26 mel bands (60-8000 Hz), HTK mel scale
  - Triangular filterbank (area-normalized)
  - Log compression: 10*log10(x + 1e-10), mapped [-60, 0] dB -> [0, 1]
  - NO compressor or whitening (matches getRawMelBands() used at inference)

GPU-accelerated: STFT, mel filterbank, augmentation (noise, filtering,
RIR convolution) run on CUDA. Audio loading uses librosa for resampling
consistency with the original pipeline.

Acoustic environment augmentation for robustness across venues:
  - Room impulse responses (RIR convolution)
  - Background noise (crowd, outdoor, industrial)
  - Volume variation (-20 to +6 dB)
  - Low-pass filter (simulates muffled bass-heavy rooms)

Usage:
    python scripts/prepare_dataset.py --config configs/default.yaml
    python scripts/prepare_dataset.py --config configs/default.yaml --augment
"""

import argparse
import atexit
import gc
import hashlib
import json
import math
import shutil
import sys
import tempfile
from pathlib import Path

# Track active shard temp dirs for crash cleanup. Shard dirs are created
# in output_dir (not /tmp) so they're easy to find if orphaned. The atexit
# handler cleans them up on normal exit and Python exceptions. Note: atexit
# does NOT run on SIGKILL (OOM killer) — orphaned dirs must be found and
# deleted manually in that case. Placing them in output_dir (not /tmp)
# makes this straightforward: `find /mnt/storage -name 'blinky_*' -type d`.
_active_shard_dirs: list[Path] = []


def _cleanup_shard_dirs() -> None:
    for d in _active_shard_dirs:
        if d.exists():
            try:
                shutil.rmtree(d)
            except Exception as e:
                print(f"WARNING: Failed to clean up shard dir {d}: {e}", file=sys.stderr)


atexit.register(_cleanup_shard_dirs)

import librosa
import numpy as np
import torch
import torchaudio
from tqdm import tqdm


# Ensure ml-training root is on sys.path for `from scripts.audio import ...`
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


def compute_mel_cache_key(cfg: dict, augment: bool, seed: int,
                          mic_profile_path: str | None = None,
                          rir_dir: str | None = None,
                          stems_dir: str | None = None,
                          stem_variants: list | None = None) -> str:
    """Compute a hash key for mel cache based on audio + augmentation config.

    Labels, targets, and training params are NOT included — those only affect
    target generation, not mel extraction. This allows reusing cached mels
    when only labels change.
    """
    cache_fields = {
        "audio": cfg.get("audio", {}),
        "features": cfg.get("features", {}),  # PCEN changes mel values
        "augmentation": cfg.get("augmentation", {}),
        "augment_enabled": augment,
        "seed": seed,
        "rir_dir": str(rir_dir) if rir_dir else None,
        "stems_dir": str(stems_dir) if stems_dir else None,
        "stem_variants": stem_variants,
    }
    # Hash mic profile content (not path) so moving the file doesn't invalidate
    if mic_profile_path and Path(mic_profile_path).exists():
        with open(mic_profile_path, "rb") as f:
            cache_fields["mic_profile_hash"] = hashlib.sha256(f.read()).hexdigest()[:12]
    canonical = json.dumps(cache_fields, sort_keys=True, default=str)
    return hashlib.sha256(canonical.encode()).hexdigest()[:12]


def _file_rng(seed: int, track_stem: str) -> np.random.Generator:
    """Create a deterministic per-file RNG from global seed + track name.

    This makes each track's augmentation reproducible regardless of processing
    order, enabling mel caching across runs.
    """
    track_hash = int(hashlib.md5(track_stem.encode()).hexdigest()[:8], 16)
    return np.random.default_rng(seed ^ track_hash)

from scripts.audio import (
    append_delta_features,
    append_band_flux_features,
    build_mel_filterbank_torch as _build_mel_filterbank,
    firmware_mel_spectrogram_torch as firmware_mel_spectrogram,
    load_config,
)


def apply_spectral_conditioning(mel: np.ndarray) -> np.ndarray:
    """Apply dynamic range compression + whitening as feature augmentation.

    Approximates the firmware's spectral conditioning (soft-knee compressor +
    per-band whitening). Compressor stage is vectorized; whitening stage
    requires sequential processing (running_max depends on previous frame).
    """
    mel = mel.copy()
    n_frames, n_mels = mel.shape

    threshold_db = -30.0
    ratio = 3.0
    knee_db = 15.0
    makeup_db = 6.0
    half_knee = knee_db * 0.5

    # Vectorized soft-knee compressor: compute per-frame RMS and gain
    frame_rms = np.sqrt(np.mean(mel ** 2, axis=1) + 1e-10)  # (n_frames,)
    rms_db = 20.0 * np.log10(frame_rms + 1e-10)
    diff = rms_db - threshold_db

    gain_db = np.full(n_frames, makeup_db, dtype=np.float64)
    # Below knee: gain_db = 0 + makeup
    # Above knee: full ratio compression
    above = diff >= half_knee
    gain_db[above] += (1.0 - 1.0 / ratio) * (threshold_db - rms_db[above])
    # Within soft knee: quadratic interpolation
    within = (~above) & (diff > -half_knee)
    x = diff[within] + half_knee
    gain_db[within] += (1.0 / ratio - 1.0) * x * x / (2.0 * knee_db)

    linear_gain = 10 ** (gain_db / 20.0)  # (n_frames,)
    mel *= linear_gain[:, np.newaxis]
    mel = np.clip(mel, 0.0, 1.0)

    # Per-band whitening (sequential — running_max depends on previous frame)
    decay = 0.97
    floor = 0.01
    running_max = np.full(n_mels, floor, dtype=np.float32)

    for t in range(n_frames):
        running_max = np.maximum(mel[t], running_max * decay)
        denom = np.maximum(running_max, floor)
        mel[t] /= denom

    return np.clip(mel, 0.0, 1.0)


def _gaussian_targets(times: np.ndarray, n_frames: int, frame_rate: float,
                      sigma: float) -> np.ndarray:
    """Create Gaussian-smoothed activation targets from event times."""
    targets = np.zeros(n_frames, dtype=np.float32)
    for t in times:
        frame_idx = t * frame_rate
        frame_range = max(0, int(frame_idx - 4 * sigma)), min(n_frames, int(frame_idx + 4 * sigma) + 1)
        for i in range(frame_range[0], frame_range[1]):
            targets[i] = max(targets[i], np.exp(-0.5 * ((i - frame_idx) / sigma) ** 2))
    return targets


def _binary_targets(times: np.ndarray, n_frames: int,
                    frame_rate: float,
                    strengths: np.ndarray | None = None,
                    neighbor_weight: float = 0.0,
                    label_shift_frames: int = 0,
                    early_neighbor_frames: int = 0,
                    early_neighbor_weight: float = 0.0) -> np.ndarray:
    """Create binary activation targets (at nearest frame to each event).

    If strengths is provided, uses per-beat strength values (0-1) instead of
    binary 1.0. This gives softer supervision for lower-confidence beats
    (e.g., 2/4 system agreement → 0.5 target vs 4/4 → 1.0 target).

    If neighbor_weight > 0, sets the frames immediately adjacent to each onset
    (onset-1 and onset+1) to neighbor_weight × onset_value. This provides
    gradient signal at neighboring frames (Rong Gong et al. 2018).

    label_shift_frames: shift all targets earlier by N frames (negative = shift
    onset labels backward in time). Compensates for median-of-systems delay
    in consensus labels. Schluter & Böck (2014) validated that models faithfully
    track label timing shifts.

    early_neighbor_frames + early_neighbor_weight: Asymmetric target shape.
    Sets frames BEFORE the onset (up to early_neighbor_frames) to a decaying
    value. Trains the model to prefer early firing (wider acceptance before
    onset, narrow after). sigma_early > sigma_late effect.
    """
    targets = np.zeros(n_frames, dtype=np.float32)
    for i, t in enumerate(times):
        frame_idx = round(t * frame_rate) - label_shift_frames
        if 0 <= frame_idx < n_frames:
            val = float(strengths[i]) if strengths is not None else 1.0
            targets[frame_idx] = max(targets[frame_idx], val)

            # Symmetric neighbor weighting (onset ± 1 frame)
            if neighbor_weight > 0:
                nval = val * neighbor_weight
                if frame_idx > 0:
                    targets[frame_idx - 1] = max(targets[frame_idx - 1], nval)
                if frame_idx < n_frames - 1:
                    targets[frame_idx + 1] = max(targets[frame_idx + 1], nval)

            # Asymmetric early-side weighting: wider acceptance BEFORE onset.
            # Decaying weight from onset backward: frame-1 gets full early_weight,
            # frame-2 gets early_weight * 0.5, etc.
            if early_neighbor_frames > 0 and early_neighbor_weight > 0:
                for ef in range(1, early_neighbor_frames + 1):
                    eidx = frame_idx - ef
                    if eidx >= 0:
                        decay = early_neighbor_weight * (1.0 - (ef - 1) / early_neighbor_frames)
                        targets[eidx] = max(targets[eidx], val * decay)
    return targets


def load_soft_teacher_labels(teacher_soft_dir: Path, track_stem: str,
                            n_frames: int, frame_rate: float,
                            speed: float = 1.0) -> np.ndarray | None:
    """Load pre-generated madmom CNN onset activations and resample to firmware frame rate.

    Madmom outputs continuous [0,1] activations at 100 Hz. We resample to the
    firmware frame rate (62.5 Hz) via linear interpolation, which preserves the
    smooth activation shape. For time-stretched variants, the teacher activations
    are stretched by the same factor (a 1.2x speed track has 1.2x compressed labels).

    Returns None if the label file doesn't exist for this track.
    """
    label_path = teacher_soft_dir / f"{track_stem}.onsets_teacher.json"
    if not label_path.exists():
        return None

    with open(label_path) as f:
        data = json.load(f)

    teacher_fps = data["fps"]  # 100 Hz
    activations = np.array(data["activations"], dtype=np.float32)

    # For time-stretched audio (speed != 1.0), the audio is compressed/expanded
    # in time. Teacher labels cover the original duration — resample to match
    # the stretched duration. At speed=1.2x, the audio is shorter, so we need
    # fewer teacher frames covering a proportionally shorter time span.
    teacher_duration = len(activations) / teacher_fps  # original duration in seconds
    stretched_duration = teacher_duration / speed       # duration after time-stretch

    # Create timestamp arrays for interpolation
    teacher_times = np.arange(len(activations)) / teacher_fps  # original timestamps
    target_times = np.arange(n_frames) / frame_rate             # student frame timestamps

    # For stretched audio, teacher timestamps are scaled by 1/speed
    # (a kick at t=1.0s in original lands at t=0.833s at 1.2x speed)
    stretched_teacher_times = teacher_times / speed

    # Linear interpolation from stretched teacher times to student frame times
    resampled = np.interp(target_times, stretched_teacher_times, activations,
                          left=0.0, right=0.0)
    return resampled.astype(np.float32)


def make_teacher_targets(beat_times: np.ndarray, n_frames: int, frame_rate: float,
                        strengths: np.ndarray | None = None,
                        sigma: float = 1.5) -> np.ndarray:
    """Create soft teacher targets for knowledge distillation.

    Each beat gets a Gaussian peak with amplitude = consensus strength (0-1).
    Beats with 7/7 system agreement get full amplitude; 2/7 gets 0.29.
    Sigma = 1.5 frames (~24ms at 62.5 Hz) — wider than binary (0) but much
    narrower than the broken sigma=3.0 that produced 52% positive ratio.

    These soft labels encode the uncertainty of the consensus labeling system,
    providing richer supervision than binary labels for distillation.
    """
    targets = np.zeros(n_frames, dtype=np.float32)
    for i, t in enumerate(beat_times):
        frame_idx = t * frame_rate
        amplitude = float(strengths[i]) if strengths is not None else 1.0
        frame_range = max(0, int(frame_idx - 4 * sigma)), min(n_frames, int(frame_idx + 4 * sigma) + 1)
        for j in range(frame_range[0], frame_range[1]):
            val = amplitude * np.exp(-0.5 * ((j - frame_idx) / sigma) ** 2)
            targets[j] = max(targets[j], val)
    return targets


def make_onset_targets(beat_times: np.ndarray, n_frames: int, frame_rate: float,
                      sigma: float, target_type: str = "gaussian",
                      strengths: np.ndarray | None = None,
                      neighbor_weight: float = 0.0,
                      label_shift_frames: int = 0,
                      early_neighbor_frames: int = 0,
                      early_neighbor_weight: float = 0.0) -> np.ndarray:
    """Create onset activation targets (binary or Gaussian-smoothed).

    If target_type is "binary" and strengths is provided, uses per-beat
    strength values as targets instead of 1.0.

    If neighbor_weight > 0, adjacent frames get onset_value × neighbor_weight
    (Rong Gong et al. 2018 — provides gradient at neighboring frames).

    label_shift_frames: shift targets earlier by N frames (for snappy detection).
    early_neighbor_frames/weight: asymmetric early-side weighting.
    """
    if target_type == "binary":
        return _binary_targets(beat_times, n_frames, frame_rate, strengths,
                               neighbor_weight=neighbor_weight,
                               label_shift_frames=label_shift_frames,
                               early_neighbor_frames=early_neighbor_frames,
                               early_neighbor_weight=early_neighbor_weight)
    return _gaussian_targets(beat_times, n_frames, frame_rate, sigma)



INSTRUMENT_CHANNELS = {"kick": 0, "snare": 1, "hihat": 2}


def make_instrument_targets(beat_times: np.ndarray, beat_types: list[str],
                            n_frames: int, frame_rate: float) -> np.ndarray:
    """Create 3-channel per-instrument binary targets (kick/snare/hihat).

    Returns (n_frames, 3) array where each channel is an independent binary
    target for one instrument type. Multiple channels can be 1 at the same
    frame if two instruments coincide (e.g., kick+snare on beat 1).
    """
    targets = np.zeros((n_frames, 3), dtype=np.float32)
    for t, inst_type in zip(beat_times, beat_types):
        frame_idx = round(t * frame_rate)
        ch = INSTRUMENT_CHANNELS.get(inst_type)
        if ch is not None and 0 <= frame_idx < n_frames:
            targets[frame_idx, ch] = 1.0
    return targets


def _pink_noise_gpu(n: int, rng: np.random.Generator,
                    device: torch.device) -> torch.Tensor:
    """Generate pink (1/f) noise on GPU, with CPU fallback for large sizes."""
    white = torch.from_numpy(rng.standard_normal(n).astype(np.float32))
    try:
        white_dev = white.to(device)
        fft = torch.fft.rfft(white_dev)
        freqs = torch.fft.rfftfreq(n, device=device)
        freqs[0] = 1
        fft *= 1.0 / torch.sqrt(freqs)
        pink = torch.fft.irfft(fft, n=n)
    except RuntimeError:
        # cuFFT can fail on large sizes; fall back to CPU
        fft = torch.fft.rfft(white)
        freqs = torch.fft.rfftfreq(n)
        freqs[0] = 1
        fft *= 1.0 / torch.sqrt(freqs)
        pink = torch.fft.irfft(fft, n=n).to(device)
    return pink / (pink.abs().max() + 1e-10)


def _design_butter_sos(order: int, cutoff, btype: str, fs: int) -> np.ndarray:
    """Design Butterworth filter coefficients (CPU)."""
    from scipy.signal import butter
    return butter(order, cutoff, btype=btype, fs=fs, output="sos")


def _sosfilt_gpu(sos: np.ndarray, x: torch.Tensor) -> torch.Tensor:
    """Apply SOS filter on GPU using torchaudio's lfilter (cascaded biquads)."""
    device = x.device
    sos_t = torch.from_numpy(sos.astype(np.float32)).to(device)

    for section in range(sos_t.shape[0]):
        b0, b1, b2 = sos_t[section, 0], sos_t[section, 1], sos_t[section, 2]
        a0, a1, a2 = sos_t[section, 3], sos_t[section, 4], sos_t[section, 5]
        b0, b1, b2 = b0 / a0, b1 / a0, b2 / a0
        a1, a2 = a1 / a0, a2 / a0

        b_coeffs = torch.tensor([b0, b1, b2], device=device)
        a_coeffs = torch.tensor([1.0, a1, a2], device=device)
        x = torchaudio.functional.lfilter(x.unsqueeze(0), a_coeffs, b_coeffs,
                                          clamp=False).squeeze(0)
    return x


def _load_noise_clip(path: Path, sr: int, n_samples: int,
                     rng: np.random.Generator, device: torch.device) -> torch.Tensor:
    """Load a noise clip, resample to sr, and tile/trim to n_samples."""
    noise_np, _ = librosa.load(str(path), sr=sr, mono=True)
    if len(noise_np) < n_samples:
        # Tile to fill
        reps = (n_samples // len(noise_np)) + 1
        noise_np = np.tile(noise_np, reps)
    # Random offset for variety
    max_start = len(noise_np) - n_samples
    start = 0 if max_start == 0 else rng.integers(0, max_start + 1)
    noise_np = noise_np[start:start + n_samples]
    return torch.from_numpy(noise_np.astype(np.float32)).to(device)


def _mix_at_snr(audio: torch.Tensor, noise: torch.Tensor,
                snr_db: float) -> torch.Tensor:
    """Mix noise into audio at a target SNR."""
    signal_power = (audio ** 2).mean() + 1e-10
    noise_power = (noise ** 2).mean() + 1e-10
    noise_scale = torch.sqrt(signal_power / (noise_power * 10 ** (snr_db / 10)))
    return (audio + noise * noise_scale).clamp(-1.0, 1.0)


# Speaker EQ profiles: approximate common playback systems.
# Each profile is a list of (filter_type, freq_hz, Q_or_bandwidth, gain_db).
# Peak filters use a Butterworth bandpass + additive gain, NOT a true
# parametric EQ (no constant-Q shape). This is intentional — the goal is
# approximate spectral coloring for augmentation diversity, not accurate
# speaker modeling. The Butterworth approximation is sufficient to teach
# the model that real audio can be spectrally colored.
SPEAKER_EQ_PROFILES = {
    "phone": [
        ("highpass", 200, 0.7, None),      # No bass below 200 Hz
        ("peak", 2000, 1.0, 6),            # Presence peak
        ("lowpass", 10000, 0.7, None),      # HF rolloff
    ],
    "bluetooth": [
        ("highpass", 80, 0.7, None),        # Gentle bass rolloff
        ("peak", 150, 1.0, 4),             # Bass boost
        ("lowpass", 12000, 0.7, None),      # Gentle HF rolloff
    ],
    "pa_system": [
        ("peak", 100, 1.0, 3),             # Sub-bass hump
        ("peak", 3000, 2.0, -2),           # Slight mid scoop
        ("lowpass", 14000, 0.7, None),      # PA HF limit
    ],
    "small_monitor": [
        ("highpass", 60, 0.7, None),        # Bass limit
        ("peak", 3000, 2.0, -4),           # Mid scoop
    ],
}


def _apply_speaker_eq(audio: torch.Tensor, sr: int, profile_name: str) -> torch.Tensor:
    """Apply a speaker EQ profile using cascaded biquad filters."""
    profile = SPEAKER_EQ_PROFILES[profile_name]
    result = audio
    nyquist = sr / 2 - 1
    for filt in profile:
        ftype = filt[0]
        freq = filt[1]
        Q = filt[2]
        if ftype in ("highpass", "lowpass"):
            sos = _design_butter_sos(2, min(freq, nyquist), ftype.replace("pass", ""), sr)
            result = _sosfilt_gpu(sos, result)
        elif ftype == "peak":
            gain_db = filt[3]
            # Bandwidth defined by Q: BW = freq/Q
            bw = freq / max(Q, 0.5)
            f_lo = max(20, freq - bw / 2)
            f_hi = min(nyquist, freq + bw / 2)
            if f_hi <= f_lo:
                continue
            sos_band = _design_butter_sos(1, [f_lo, f_hi], "band", sr)
            band = _sosfilt_gpu(sos_band, result)
            gain_lin = 10 ** (gain_db / 20.0) - 1.0
            result = (result + band * gain_lin).clamp(-1.0, 1.0)
    return result


def augment_audio(audio: torch.Tensor, sr: int, rir_dir: Path | None,
                  rng: np.random.Generator, device: torch.device,
                  noise_dir: Path | None = None) -> list[tuple[str, torch.Tensor]]:
    """Generate augmented versions of audio on GPU.

    Augmentation categories:
    - Gain variation: [-18, -12, -6, +6] dB
    - Pink noise: SNR [6, 12, 20] dB
    - Environmental noise: crowd/traffic/weather/hvac/ambient at random SNR
    - Low-pass filter: 4 kHz cutoff (muffled rooms)
    - Bass boost: 60-200 Hz resonance
    - Speaker EQ: phone/bluetooth/PA/monitor profiles
    - Distance attenuation: HF rolloff for 5-20m listening distances
    - Room impulse responses: synthetic venue acoustics
    """
    variants = [("clean", audio)]

    # Gain variation must cover device-realistic levels.
    # Device mic during music sees +13 dB above training calibration (mel mean
    # 0.74 vs 0.49). Previous range [-18, +6] never trained on device-like levels.
    # Extended to [-18, +18] dB based on April 10 mel distribution analysis.
    for gain_db in [-18, -12, -6, 6, 12, 18]:
        gain = 10 ** (gain_db / 20.0)
        clipped = (audio * gain).clamp(-1.0, 1.0)
        variants.append((f"gain{gain_db:+d}dB", clipped))

    for snr_db in [6, 12, 20]:
        noise = _pink_noise_gpu(len(audio), rng, device)
        noisy = _mix_at_snr(audio, noise, snr_db)
        variants.append((f"pink-snr{snr_db}dB", noisy))

    # Environmental noise (diverse real-world noise, not just pink)
    if noise_dir and noise_dir.exists():
        noise_categories = [d.name for d in noise_dir.iterdir() if d.is_dir()]
        for category in noise_categories:
            cat_files = list((noise_dir / category).glob("*.wav")) + \
                        list((noise_dir / category).glob("*.ogg"))
            if cat_files:
                chosen_file = rng.choice(cat_files)
                snr_db = rng.uniform(3, 15)
                try:
                    noise_clip = _load_noise_clip(chosen_file, sr, len(audio), rng, device)
                    noisy = _mix_at_snr(audio, noise_clip, snr_db)
                    variants.append((f"noise-{category}-snr{snr_db:.0f}dB", noisy))
                except (OSError, ValueError, RuntimeError) as e:
                    tqdm.write(f"  WARNING: noise clip {chosen_file.name}: {e}")

    # Low-pass filter
    sos_lp = _design_butter_sos(4, 4000, "low", sr)
    lp = _sosfilt_gpu(sos_lp, audio)
    variants.append(("lowpass-4k", lp))

    # Bass boost
    sos_bass = _design_butter_sos(2, [60, 200], "band", sr)
    bass = _sosfilt_gpu(sos_bass, audio)
    boosted = (audio + bass * 1.5).clamp(-1.0, 1.0)
    variants.append(("bass-boost", boosted))

    # Speaker EQ profiles (teach model about colored playback systems)
    eq_profile = rng.choice(list(SPEAKER_EQ_PROFILES.keys()))
    try:
        eq_audio = _apply_speaker_eq(audio, sr, eq_profile)
        variants.append((f"eq-{eq_profile}", eq_audio))
    except (ValueError, RuntimeError) as e:
        tqdm.write(f"  WARNING: speaker EQ '{eq_profile}': {e}")

    # Distance attenuation (HF rolloff for far-field listening)
    distance_m = rng.uniform(5, 20)
    cutoff = max(2000, 16000 / (1 + 0.3 * distance_m))
    sos_dist = _design_butter_sos(2, cutoff, "low", sr)
    dist_audio = _sosfilt_gpu(sos_dist, audio)
    variants.append((f"dist-{distance_m:.0f}m", dist_audio))

    # Speaker harmonic distortion (polynomial nonlinearity)
    # Speakers driven by kicks produce 1-8% THD, creating harmonics the model
    # hasn't seen in clean training audio. Even-order (H2) is asymmetric,
    # odd-order (H3) is symmetric — both affect transient shape.
    thd = rng.uniform(0.01, 0.08)
    even_coeff = thd * 0.7  # H2 (even-order, asymmetric)
    odd_coeff = thd * 0.3   # H3 (odd-order, symmetric)
    distorted = (audio + even_coeff * audio**2 + odd_coeff * audio**3).clamp(-1.0, 1.0)
    variants.append((f"spkr-thd{thd*100:.0f}pct", distorted))

    # Comb filter from early reflections (1-10ms delay = 0.3-3.4m reflector)
    # Creates notches in 100-1000 Hz that affect kick perception.
    # Uses causal zero-padded delay (not circular roll).
    delay = int(rng.integers(16, 160))  # 1-10ms at 16kHz
    comb_gain = rng.uniform(-0.4, 0.4)
    delayed = torch.cat([torch.zeros(delay, device=audio.device), audio[:-delay]])
    combed = (audio + comb_gain * delayed).clamp(-1.0, 1.0)
    variants.append((f"comb-{delay}smp", combed))

    # MEMS mic soft clipping (polynomial saturation at high SPL)
    # MEMS diaphragm nonlinearity is smooth and asymmetric, unlike digital clipping.
    clip_a3 = rng.uniform(0.05, 0.20)
    clip_a5 = rng.uniform(0.01, 0.05)
    soft_clipped = (audio - clip_a3 * audio**3 + clip_a5 * audio**5).clamp(-1.0, 1.0)
    variants.append(("mems-softclip", soft_clipped))

    # Room impulse responses
    if rir_dir and rir_dir.exists():
        rir_files = list(rir_dir.glob("*.wav")) + list(rir_dir.glob("*.npy"))
        if rir_files:
            chosen = rng.choice(rir_files, size=min(3, len(rir_files)), replace=False)
            for rir_path in chosen:
                if rir_path.suffix == ".npy":
                    rir_np = np.load(rir_path)
                    rir = torch.from_numpy(rir_np.astype(np.float32)).to(device)
                else:
                    rir, rir_sr = torchaudio.load(str(rir_path))
                    if rir.shape[0] > 1:
                        rir = rir.mean(dim=0)
                    else:
                        rir = rir.squeeze(0)
                    if rir_sr != sr:
                        rir = torchaudio.functional.resample(rir, rir_sr, sr)
                    rir = rir.to(device)

                rir = rir / (rir.abs().max() + 1e-10)
                # GPU convolution via FFT (CPU fallback for large sizes)
                n_conv = len(audio) + len(rir) - 1
                try:
                    convolved = torch.fft.irfft(
                        torch.fft.rfft(audio, n=n_conv) * torch.fft.rfft(rir, n=n_conv),
                        n=n_conv
                    )[:len(audio)]
                except RuntimeError:
                    a_cpu, r_cpu = audio.cpu(), rir.cpu()
                    convolved = torch.fft.irfft(
                        torch.fft.rfft(a_cpu, n=n_conv) * torch.fft.rfft(r_cpu, n=n_conv),
                        n=n_conv
                    )[:len(audio)].to(device)
                convolved = convolved / (convolved.abs().max() + 1e-10)
                variants.append((f"rir-{rir_path.stem}", convolved))

    return variants


def _apply_mic_profile(mel: np.ndarray, mic_profile: dict,
                       rng: np.random.Generator) -> np.ndarray:
    """Apply mic transfer function to simulate MEMS mic response.

    Scales per-band by measured gain ratio and adds noise floor.
    If gain-sweep data is available, randomly samples a HW gain level
    and uses the corresponding noise floor for that gain — this trains
    the model to be robust across the AGC's operating range.

    Uses the provided rng for reproducible noise generation.
    """
    n_mel = mel.shape[1]
    band_gain = mic_profile["band_gain"]
    # Interpolate mic profile if band count changed (e.g., 26→40 mel bands)
    if len(band_gain) != n_mel:
        from scipy.interpolate import interp1d
        x_old = np.linspace(0, 1, len(band_gain))
        x_new = np.linspace(0, 1, n_mel)
        band_gain = interp1d(x_old, band_gain, kind='linear')(x_new).astype(np.float32)

    # Select noise floor: gain-aware (random gain per example) or static
    if "noise_floor_by_gain" in mic_profile:
        nf_by_gain = mic_profile["noise_floor_by_gain"]  # (G, 26)
        hw_gains = mic_profile["hw_gain_levels"]           # (G,)
        # Sample a random gain level, weighted toward the recommended range
        if "recommended_gain_min" in mic_profile and "recommended_gain_max" in mic_profile:
            rec_min = mic_profile["recommended_gain_min"]
            rec_max = mic_profile["recommended_gain_max"]
            # 70% chance within recommended range, 30% full range
            if rng.random() < 0.7:
                valid = np.where((hw_gains >= rec_min) & (hw_gains <= rec_max))[0]
                if len(valid) > 0:
                    g_idx = rng.choice(valid)
                else:
                    g_idx = rng.integers(len(hw_gains))
            else:
                g_idx = rng.integers(len(hw_gains))
        else:
            g_idx = rng.integers(len(hw_gains))
        noise_floor = nf_by_gain[g_idx]
    else:
        noise_floor = mic_profile["noise_floor"]

    # Interpolate noise floor if band count changed
    if len(noise_floor) != n_mel:
        from scipy.interpolate import interp1d
        x_old = np.linspace(0, 1, len(noise_floor))
        x_new = np.linspace(0, 1, n_mel)
        noise_floor = interp1d(x_old, noise_floor, kind='linear')(x_new).astype(np.float32)

    mel_mic = mel * band_gain[np.newaxis, :]
    if noise_floor.max() > 0:
        noise = rng.normal(
            loc=noise_floor, scale=noise_floor * 0.3,
            size=mel.shape
        ).astype(np.float32)
        noise = np.maximum(noise, 0)
        mel_mic = np.maximum(mel_mic, noise)
    return np.clip(mel_mic, 0.0, 1.0)


def process_file(audio_path: Path, label_path: Path, cfg: dict,
                 augment: bool, rir_dir: Path | None,
                 rng: np.random.Generator,
                 device: torch.device,
                 mel_fb: torch.Tensor,
                 window: torch.Tensor,
                 mic_profile: dict | None = None,
                 stems_dir: Path | None = None,
                 stem_variants: list[str] | None = None,
                 mel_cache_dir: Path | None = None,
                 generate_teacher: bool = False,
                 teacher_soft_dir: Path | None = None,
                 noise_dir: Path | None = None) -> list[dict]:
    """Process one audio file into (features, targets) pairs.

    Audio loaded with librosa (resampling consistency), then moved to GPU
    for STFT, mel extraction, and augmentation.

    When augmenting, also generates time-stretched variants (resample-based,
    changes pitch — fine for beat detection). Stretched variants get clean
    audio only (no noise/gain/RIR augmentation) to keep dataset size reasonable.

    Stem augmentation (stems_dir + stem_variants):
    If stems_dir is provided and contains Demucs-separated stems for this track,
    generates additional training variants from stem combinations. Each variant
    uses the SAME beat labels (beats happen at the same times regardless of mix).
    Stem variants get clean audio only — the stems themselves are the augmentation.

    Supported stem_variants:
      - "drums": Isolated percussion (teaches kick/snare mel patterns)
      - "no_vocals": drums+bass+other (simulates instrumental sections)
      - "drums_bass": Rhythmic foundation (kick+bass for onset detection)
    """
    sr = cfg["audio"]["sample_rate"]
    sigma = cfg["labels"]["sigma_frames"]
    frame_rate = cfg["audio"]["frame_rate"]
    target_type = cfg["labels"].get("target_type", "gaussian")

    # Load audio with librosa for resampling consistency
    audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)

    # Normalize RMS to simulate firmware AGC level.
    # Mastered audio (~-8 dB RMS) produces mel values crushed to [0.95, 1.0].
    # Firmware mic+AGC operates at much lower levels where [-60, 0] dB mapping
    # uses the full [0, 1] range. Target -63 dB RMS gives mel mean ~0.52,
    # matching firmware AGC output (calibrated via tools/rms_mel_sweep.py).
    target_rms_db = cfg["audio"].get("target_rms_db", -63)
    rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
    target_rms = 10 ** (target_rms_db / 20)
    audio_np = audio_np * (target_rms / rms)

    audio_gpu = torch.from_numpy(audio_np).to(device)

    # Load labels — onset consensus, kick-weighted, or consensus beat labels.
    labels_type = cfg.get("labels", {}).get("labels_type", "consensus")
    kick_weighted_dir = cfg.get("labels", {}).get("kick_weighted_dir", "")
    onset_consensus_dir = cfg.get("labels", {}).get("onset_consensus_dir", "")
    neighbor_weight = cfg.get("labels", {}).get("neighbor_weight", 0.0)
    label_shift_frames = cfg.get("labels", {}).get("label_shift_frames", 0)
    early_neighbor_frames = cfg.get("labels", {}).get("early_neighbor_frames", 0)
    early_neighbor_weight = cfg.get("labels", {}).get("early_neighbor_weight", 0.0)

    if labels_type == "onset_consensus" and onset_consensus_dir:
        # Multi-system onset consensus labels (5 systems).
        # These are acoustic onset positions, not metrical beats — directly
        # matching the onset detection task. Each onset has a strength field
        # encoding the number of agreeing systems (1/5 to 5/5).
        onset_path = Path(onset_consensus_dir) / f"{audio_path.stem}.onsets.json"
        if not onset_path.exists():
            raise FileNotFoundError(
                f"Onset consensus labels missing for {audio_path.stem}: {onset_path}\n"
                f"Run: python scripts/generate_onset_consensus.py"
            )
        with open(onset_path) as f:
            onset_data = json.load(f)
        beat_times = np.array([o["time"] for o in onset_data["onsets"]])
        beat_strengths = np.array([o["strength"] for o in onset_data["onsets"]])
    elif labels_type == "instrument" and kick_weighted_dir:
        # Per-instrument onset targets: 3 channels (kick/snare/hihat).
        # Each channel gets an independent binary target from the kick_weighted
        # label type field. The model learns to classify onset type, and the
        # firmware can choose which channels to use for visual pulse.
        kw_path = Path(kick_weighted_dir) / f"{audio_path.stem}.kick_weighted.json"
        if not kw_path.exists():
            raise FileNotFoundError(
                f"Kick-weighted labels missing for {audio_path.stem}: {kw_path}\n"
                f"Run: python scripts/generate_kick_weighted_targets.py"
            )
        with open(kw_path) as f:
            kw_data = json.load(f)
        # Build per-instrument time/type arrays (used later by make_instrument_targets)
        beat_times = np.array([o["time"] for o in kw_data["onsets"]])
        beat_types = [o["type"] for o in kw_data["onsets"]]
        beat_strengths = None  # not used for instrument targets
    elif labels_type == "kick_weighted" and kick_weighted_dir:
        kw_path = Path(kick_weighted_dir) / f"{audio_path.stem}.kick_weighted.json"
        if not kw_path.exists():
            # Missing label (may be quarantined) — skip track silently
            return []
        with open(kw_path) as f:
            kw_data = json.load(f)
        # Quality gate: skip tracks with too few events or explicitly skipped
        if kw_data.get("skipped"):
            return []
        n_ks = kw_data.get("kick_count", 0) + kw_data.get("snare_count", 0)
        if n_ks < 10:
            print(f"  WARNING: Skipping {audio_path.stem}: only {n_ks} kick+snare events "
                  f"(likely failed drum separation)")
            return []
        beat_times = np.array([o["time"] for o in kw_data["onsets"]])
        beat_strengths = np.array([o["weight"] for o in kw_data["onsets"]])
    elif labels_type == "consensus_kick_weighted" and kick_weighted_dir:
        # Consensus beats for timing accuracy, kick_weighted for instrument type.
        # Each consensus beat is matched to the nearest kick_weighted onset within
        # ±70ms. Kicks/snares keep weight 1.0, hihats get weight 0.0 (suppress),
        # unmatched beats keep their consensus strength (musically important).
        with open(label_path) as f:
            labels = json.load(f)
        hits = [h for h in labels["hits"] if h.get("expectTrigger", True)]
        beat_times = np.array([h["time"] for h in hits])
        beat_strengths = np.array([h.get("strength", 1.0) for h in hits])

        kw_path = Path(kick_weighted_dir) / f"{audio_path.stem}.kick_weighted.json"
        if kw_path.exists() and len(beat_times) > 0:
            with open(kw_path) as f:
                kw_data = json.load(f)
            kw_times = np.array([o["time"] for o in kw_data["onsets"]])
            kw_types = [o["type"] for o in kw_data["onsets"]]
            if len(kw_times) > 0:
                for i, bt in enumerate(beat_times):
                    nearest_idx = int(np.argmin(np.abs(kw_times - bt)))
                    if abs(kw_times[nearest_idx] - bt) <= 0.07:
                        if kw_types[nearest_idx] == "hihat":
                            beat_strengths[i] = 0.0
    else:
        with open(label_path) as f:
            labels = json.load(f)
        hits = [h for h in labels["hits"] if h.get("expectTrigger", True)]
        beat_times = np.array([h["time"] for h in hits])
        beat_strengths = np.array([h.get("strength", 1.0) for h in hits])

    results = []

    # Time-stretch factors: original speed + stretched variants when augmenting.
    # Resample-based stretch changes pitch (fine for beat detection).
    # Diversifies BPM distribution — training data is 33.5% at 120-140 BPM.
    time_stretch_factors = [1.0]
    if augment:
        ts_factors = cfg.get("augmentation", {}).get("time_stretch_factors", [])
        time_stretch_factors.extend(ts_factors)

    # Pitch-shift semitones: key-invariant augmentation.
    # Beat This! ablation: removing pitch shift drops F1 by 4.3 points.
    # Applied to original speed audio only (pitch-shifted + time-stretched
    # would be redundant and triple the dataset size).
    pitch_shifts = [0]  # 0 = no shift (original)
    if augment:
        ps_semitones = cfg.get("augmentation", {}).get("pitch_shift_semitones", [])
        pitch_shifts.extend(ps_semitones)

    for speed in time_stretch_factors:
        if speed == 1.0:
            src_audio = audio_gpu
            src_beats = beat_times
            src_strengths = beat_strengths
        else:
            # Resample to simulate tempo change: speed > 1 = faster
            try:
                src_audio = torchaudio.functional.resample(
                    audio_gpu.unsqueeze(0), int(sr * speed), sr).squeeze(0)
            except Exception as e:
                import logging
                logging.warning(f"Time-stretch {speed:.2f}x failed for "
                                f"{audio_path.name}: {e}")
                continue
            src_beats = beat_times / speed
            src_strengths = beat_strengths  # Strengths don't change with speed

        # Full augmentation only for original speed; clean only for stretched
        if augment and speed == 1.0:
            variants = augment_audio(src_audio, sr, rir_dir, rng, device,
                                        noise_dir=noise_dir)
        else:
            tag = f"stretch{speed:.2f}" if speed != 1.0 else "clean"
            variants = [(tag, src_audio)]

        # Pitch-shifted variants (original speed only, clean audio)
        # Beat times don't change with pitch shift — only key/timbre changes.
        #
        # We avoid torchaudio.functional.pitch_shift because its internal
        # resample step builds a sinc kernel proportional to orig_freq * new_freq / GCD².
        # With exact semitone ratios the GCD is 1-2, creating 0.5-1.3 GB kernels
        # that OOM on 10 GB GPUs. Instead: phase_vocoder + resample with a rounded
        # frequency ratio (GCD increases ~1000x, kernel drops to < 2 KB, <0.35% error).
        if augment and speed == 1.0:
            for semitones in pitch_shifts:
                if semitones == 0:
                    continue
                try:
                    rate = 2 ** (semitones / 12)
                    # Run pitch shift on CPU to avoid GPU OOM on long tracks.
                    # The augmented variants list holds ~10 GPU tensors; adding
                    # STFT workspace on top exceeds 10 GB for tracks > 5 min.
                    # CPU pitch shift takes ~1s/track — negligible vs GPU savings.
                    src_cpu = src_audio.cpu()
                    n_fft = 512
                    hop = n_fft // 4
                    freq_bins = n_fft // 2 + 1
                    win = torch.hann_window(n_fft)
                    spec = torch.stft(src_cpu, n_fft=n_fft, hop_length=hop,
                                      window=win, return_complex=True)
                    phase_advance = torch.linspace(
                        0, math.pi * hop, freq_bins)[..., None]
                    stretched_spec = torchaudio.functional.phase_vocoder(
                        spec.unsqueeze(0), rate, phase_advance)
                    stretched = torch.istft(stretched_spec.squeeze(0), n_fft=n_fft,
                                           hop_length=hop, window=win)
                    # Resample back to original length (restores duration, shifts pitch).
                    # Round source freq to nearest 100 for large GCD → tiny sinc kernel.
                    orig_freq = round(int(sr / rate) / 100) * 100
                    if orig_freq < 100:
                        orig_freq = 100
                    shifted = torchaudio.functional.resample(
                        stretched.unsqueeze(0), orig_freq, sr).squeeze(0)
                    # Trim or pad to original length
                    if len(shifted) > len(src_audio):
                        shifted = shifted[:len(src_audio)]
                    elif len(shifted) < len(src_audio):
                        shifted = torch.nn.functional.pad(shifted, (0, len(src_audio) - len(shifted)))
                    variants.append((f"pitch{semitones:+d}st", shifted.to(device)))
                    del src_cpu, spec, stretched_spec, stretched
                except Exception as e:
                    import logging
                    logging.warning(f"Pitch shift {semitones:+d}st failed for "
                                    f"{audio_path.name}: {e}")

        for aug_name, aug_audio in variants:
            cache_key = f"{speed:.2f}_{aug_name}"
            cached_path = (mel_cache_dir / audio_path.stem / f"{cache_key}.npy"
                           if mel_cache_dir else None)

            if cached_path and cached_path.exists():
                mel = np.load(cached_path)
            else:
                mel = firmware_mel_spectrogram(aug_audio, cfg, mel_fb, window)
                # Apply mic transfer function if calibration profile provided.
                if mic_profile is not None:
                    mel = _apply_mic_profile(mel, mic_profile, rng)
                if cached_path:
                    cached_path.parent.mkdir(parents=True, exist_ok=True)
                    np.save(cached_path, mel)

            n_frames = mel.shape[0]
            if labels_type == "instrument":
                targets = make_instrument_targets(
                    src_beats, beat_types, n_frames, frame_rate)
            else:
                targets = make_onset_targets(src_beats, n_frames, frame_rate, sigma,
                                            target_type=target_type,
                                            strengths=src_strengths,
                                            neighbor_weight=neighbor_weight,
                                            label_shift_frames=label_shift_frames,
                                            early_neighbor_frames=early_neighbor_frames,
                                            early_neighbor_weight=early_neighbor_weight)

            result = {
                "mel": mel,
                "target": targets,
                "aug": aug_name,
                "source": audio_path.stem,
            }

            # Teacher targets for knowledge distillation
            if generate_teacher:
                if teacher_soft_dir is not None:
                    # Madmom CNN soft labels: continuous [0,1] activations resampled
                    # from 100 Hz to firmware frame rate. Preserves activation shape
                    # (rise/attack/decay) for MSE distillation.
                    soft = load_soft_teacher_labels(
                        teacher_soft_dir, audio_path.stem, n_frames, frame_rate,
                        speed=speed)
                    if soft is not None:
                        result["teacher"] = soft
                    else:
                        # Soft label missing for this track — fall back to Gaussians
                        result["teacher"] = make_teacher_targets(
                            src_beats, n_frames, frame_rate, strengths=src_strengths, sigma=1.5)
                else:
                    # Fallback: consensus-strength-weighted Gaussians
                    result["teacher"] = make_teacher_targets(
                        src_beats, n_frames, frame_rate, strengths=src_strengths, sigma=1.5)

            results.append(result)

            # Spectral conditioning variant (only for original speed, clean)
            if augment and aug_name == "clean" and speed == 1.0:
                cond_cache_key = f"{speed:.2f}_conditioned"
                cond_cached = (mel_cache_dir / audio_path.stem / f"{cond_cache_key}.npy"
                               if mel_cache_dir else None)
                if cond_cached and cond_cached.exists():
                    conditioned_mel = np.load(cond_cached)
                else:
                    conditioned_mel = apply_spectral_conditioning(mel)
                    if cond_cached:
                        np.save(cond_cached, conditioned_mel)
                cond_result = {
                    "mel": conditioned_mel,
                    "target": targets,
                    "aug": "conditioned",
                    "source": audio_path.stem,
                }
                if generate_teacher and "teacher" in result:
                    cond_result["teacher"] = result["teacher"]  # Same beats, different mel
                results.append(cond_result)

    # Stem augmentation: generate additional variants from Demucs-separated stems.
    # Uses the same beat labels — beats happen at the same times regardless of mix.
    # Only at original speed, clean audio (the stem itself is the augmentation).
    if stems_dir and stem_variants:
        # Check both {stems_dir}/htdemucs/{track}/ and {stems_dir}/{track}/
        stem_dir = stems_dir / "htdemucs" / audio_path.stem
        if not stem_dir.exists():
            stem_dir = stems_dir / audio_path.stem
        if stem_dir.exists():
            stem_mixes = {
                "drums": ["drums"],
                "no_vocals": ["drums", "bass", "other"],
                "drums_bass": ["drums", "bass"],
            }
            for variant_name in stem_variants:
                stem_names = stem_mixes.get(variant_name)
                if not stem_names:
                    continue
                # Load and sum the requested stems
                stem_audio = None
                all_found = True
                for sn in stem_names:
                    stem_path = stem_dir / f"{sn}.wav"
                    if not stem_path.exists():
                        all_found = False
                        break
                    sw, stem_sr = torchaudio.load(str(stem_path))
                    # Mix to mono
                    sw = sw.mean(dim=0)
                    # Resample to training sample rate if needed
                    if stem_sr != sr:
                        sw = torchaudio.functional.resample(sw, stem_sr, sr)
                    if stem_audio is None:
                        stem_audio = sw
                    else:
                        # Align lengths (stems should be same length but be safe)
                        min_len = min(len(stem_audio), len(sw))
                        stem_audio = stem_audio[:min_len] + sw[:min_len]

                if not all_found or stem_audio is None:
                    continue

                # Normalize RMS to match firmware AGC level (same as full mix)
                stem_np = stem_audio.numpy()
                rms = np.sqrt(np.mean(stem_np ** 2) + 1e-10)
                if rms > 1e-8:  # Skip near-silent stems
                    target_rms = 10 ** (target_rms_db / 20)
                    stem_np = stem_np * (target_rms / rms)

                    stem_cache_key = f"1.00_stem_{variant_name}"
                    stem_cached = (mel_cache_dir / audio_path.stem / f"{stem_cache_key}.npy"
                                   if mel_cache_dir else None)
                    if stem_cached and stem_cached.exists():
                        mel = np.load(stem_cached)
                    else:
                        stem_gpu = torch.from_numpy(stem_np).to(device)
                        mel = firmware_mel_spectrogram(stem_gpu, cfg, mel_fb, window)
                        if mic_profile is not None:
                            mel = _apply_mic_profile(mel, mic_profile, rng)
                        if stem_cached:
                            stem_cached.parent.mkdir(parents=True, exist_ok=True)
                            np.save(stem_cached, mel)
                    n_frames = mel.shape[0]
                    targets = make_onset_targets(beat_times, n_frames, frame_rate,
                                                sigma, target_type=target_type,
                                                strengths=beat_strengths,
                                                neighbor_weight=neighbor_weight,
                                                label_shift_frames=label_shift_frames,
                                                early_neighbor_frames=early_neighbor_frames,
                                                early_neighbor_weight=early_neighbor_weight)
                    stem_result = {
                        "mel": mel,
                        "target": targets,
                        "aug": f"stem_{variant_name}",
                        "source": audio_path.stem,
                    }
                    if generate_teacher:
                        if teacher_soft_dir is not None:
                            stem_result["teacher"] = load_soft_teacher_labels(
                                teacher_soft_dir, audio_path.stem, n_frames, frame_rate)
                        else:
                            stem_result["teacher"] = make_teacher_targets(
                                beat_times, n_frames, frame_rate,
                                strengths=beat_strengths, sigma=1.5)
                    results.append(stem_result)

    return results


def chunk_data(mel: np.ndarray, target: np.ndarray,
               chunk_frames: int, chunk_stride: int,
               teacher_target: np.ndarray | None = None,
               use_delta: bool = False,
               use_band_flux: bool = False) -> tuple:
    """Split mel/target arrays into overlapping fixed-length chunks.

    If use_delta=True, appends first-order mel differences as additional
    channels: mel[t] - mel[t-1]. Output shape becomes (N, chunk_frames, 2*n_mels).
    If use_band_flux=True, appends 3 band-grouped HWR flux channels.
    Output shape becomes (N, chunk_frames, n_mels+3).
    """
    n_frames = mel.shape[0]

    if use_delta:
        mel = append_delta_features(mel)
    elif use_band_flux:
        mel = append_band_flux_features(mel)
    has_teacher = teacher_target is not None

    if n_frames < chunk_frames:
        pad_mel = np.zeros((chunk_frames, mel.shape[1]), dtype=mel.dtype)
        target_shape = (chunk_frames,) + target.shape[1:]
        pad_target = np.zeros(target_shape, dtype=target.dtype)
        pad_mel[:n_frames] = mel
        pad_target[:n_frames] = target
        pad_teacher = None
        if has_teacher:
            pad_teacher = np.zeros(chunk_frames, dtype=teacher_target.dtype)
            pad_teacher[:n_frames] = teacher_target
        return pad_mel[np.newaxis], pad_target[np.newaxis], \
               pad_teacher[np.newaxis] if pad_teacher is not None else None

    chunks_mel = []
    chunks_target = []
    chunks_teacher = [] if has_teacher else None
    for start in range(0, n_frames - chunk_frames + 1, chunk_stride):
        chunks_mel.append(mel[start:start + chunk_frames])
        chunks_target.append(target[start:start + chunk_frames])
        if has_teacher:
            chunks_teacher.append(teacher_target[start:start + chunk_frames])

    teacher_arr = np.array(chunks_teacher) if has_teacher else None
    return np.array(chunks_mel), np.array(chunks_target), teacher_arr


def main():
    parser = argparse.ArgumentParser(description="Prepare dataset for onset activation CNN")
    parser.add_argument("--config", default="configs/default.yaml", help="Config file path")
    parser.add_argument("--augment", action="store_true", help="Apply acoustic environment augmentation")
    parser.add_argument("--audio-dir", default=None, help="Override audio directory from config")
    parser.add_argument("--labels-dir", default=None, help="Override labels directory from config")
    parser.add_argument("--output-dir", default=None, help="Override output directory from config")
    parser.add_argument("--rir-dir", default=None, help="Directory of room impulse responses (.wav/.npy)")
    parser.add_argument("--noise-dir", default=None,
                        help="Directory of environmental noise clips organized by category "
                             "(e.g., crowd/, traffic/, weather/). Mixed at random SNR 3-15 dB.")
    parser.add_argument("--mic-profile", default=None,
                        help="Mic calibration profile (.npz from calibrate_mic.py). "
                             "Applied to all mel spectrograms to simulate mic response.")
    parser.add_argument("--exclude-dir", default=None,
                        help="Directory of audio files to exclude from training (e.g., test set). "
                             "Files with matching stems are filtered out to prevent data leakage.")
    parser.add_argument("--stems-dir", default=None,
                        help="Directory of Demucs-separated stems (from batch_demucs_separate.py). "
                             "Generates additional training variants from stem combinations.")
    parser.add_argument("--stem-variants", default=None,
                        help="Comma-separated stem variants to generate. "
                             "Options: drums, no_vocals, drums_bass (default: drums)")
    parser.add_argument("--teacher", action="store_true",
                        help="Generate consensus-strength-weighted teacher labels (Y_teacher_*.npy) for distillation")
    parser.add_argument("--teacher-soft-dir", default=None,
                        help="Directory of madmom soft onset labels (.onsets_teacher.json at 100 Hz). "
                             "Resampled to firmware frame rate for MSE distillation. Implies --teacher.")
    parser.add_argument("--delta", action="store_true",
                        help="Append first-order mel differences as additional input channels (26→52 features)")
    parser.add_argument("--band-flux", action="store_true", dest="band_flux",
                        help="Append 3 band-grouped HWR mel flux channels (26→29 features)")
    parser.add_argument("--seed", default=None, type=int, help="Random seed for augmentation")
    parser.add_argument("--device", default=None, help="Device: cuda, cpu, or auto (default: auto)")
    parser.add_argument("--no-cache", action="store_true",
                        help="Disable mel spectrogram caching (recompute everything)")
    parser.add_argument("--clear-cache", action="store_true",
                        help="Clear existing mel cache before starting")
    args = parser.parse_args()

    cfg = load_config(args.config)

    audio_dir = Path(args.audio_dir or cfg["data"]["audio_dir"])
    labels_dir = Path(args.labels_dir or cfg["data"]["labels_dir"])
    output_dir = Path(args.output_dir or cfg["data"]["processed_dir"])
    rir_dir = Path(args.rir_dir) if args.rir_dir else Path(cfg["data"].get("rir_dir", "data/rir"))
    _noise = args.noise_dir or cfg["data"].get("noise_dir")
    noise_dir = Path(_noise) if _noise else None
    stems_dir = Path(args.stems_dir) if args.stems_dir else None
    stem_variant_list = None
    if stems_dir:
        valid_variants = {"drums", "no_vocals", "drums_bass"}
        stem_variant_list = [v.strip() for v in (args.stem_variants or "drums").split(",")]
        unknown = set(stem_variant_list) - valid_variants
        if unknown:
            print(f"ERROR: Unknown stem variants: {unknown}. "
                  f"Valid options: {sorted(valid_variants)}", file=sys.stderr)
            sys.exit(1)
    seed = args.seed if args.seed is not None else cfg["training"]["seed"]

    if args.device is None or args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    output_dir.mkdir(parents=True, exist_ok=True)

    # --- Disk space pre-check ---
    # Processed datasets are 80-130 GB. Fail fast instead of dying mid-run.
    MIN_FREE_GB = 50  # Minimum free space to even start
    disk_stat = shutil.disk_usage(output_dir)
    free_gb = disk_stat.free / (1024 ** 3)
    if free_gb < MIN_FREE_GB:
        print(f"ERROR: Only {free_gb:.1f} GB free on {output_dir}. "
              f"Need at least {MIN_FREE_GB} GB to start dataprep.\n"
              f"  Tip: delete old processed_v* dirs or stale mel_cache entries.",
              file=sys.stderr)
        sys.exit(1)

    # --- Clean up old processed dirs ---
    # Each version creates a new processed_v{N} dir but old ones were never
    # auto-cleaned, causing 100+ GB of dead data to accumulate.
    output_parent = output_dir.parent
    output_name = output_dir.name
    old_processed = sorted(
        d for d in output_parent.iterdir()
        if d.is_dir() and d.name.startswith("processed_") and d.name != output_name
    )
    if old_processed:
        old_processed_sizes = [
            (d, sum(f.stat().st_size for f in d.rglob("*") if f.is_file()))
            for d in old_processed
        ]
        old_total = sum(s for _, s in old_processed_sizes)
        old_total_gb = old_total / (1024 ** 3)
        print(f"\nFound {len(old_processed)} old processed dir(s) "
              f"({old_total_gb:.1f} GB total):")
        for d, d_size in old_processed_sizes:
            print(f"  {d.name}: {d_size / (1024**3):.1f} GB")
        if sys.stdin.isatty():
            resp = input("Delete old processed dirs to free space? [y/N] ").strip().lower()
            if resp == "y":
                for d in old_processed:
                    shutil.rmtree(d)
                    print(f"  Deleted {d.name}")
                # Re-check free space
                free_gb = shutil.disk_usage(output_dir).free / (1024 ** 3)
                print(f"  Free space now: {free_gb:.1f} GB")
        else:
            print("  (Non-interactive — skipping auto-delete. "
                  "Run interactively or delete manually.)")

    # --- Prune stale mel cache entries ---
    mel_cache_base = Path(cfg.get("data", {}).get("mel_cache_dir", "data/mel_cache"))
    if mel_cache_base.exists():
        current_cache_key = compute_mel_cache_key(
            cfg, args.augment, seed,
            mic_profile_path=getattr(args, 'mic_profile', None),
            rir_dir=str(rir_dir) if rir_dir else None,
            stems_dir=str(stems_dir) if stems_dir else None,
            stem_variants=stem_variant_list,
        )
        stale_caches = [
            d for d in mel_cache_base.iterdir()
            if d.is_dir() and d.name != current_cache_key
        ]
        if stale_caches:
            stale_cache_sizes = {
                d: sum(f.stat().st_size for f in d.rglob("*") if f.is_file())
                for d in stale_caches
            }
            stale_total = sum(stale_cache_sizes.values())
            stale_gb = stale_total / (1024 ** 3)
            if stale_gb > 1.0:
                print(f"\nFound {len(stale_caches)} stale mel cache dir(s) "
                      f"({stale_gb:.1f} GB, config hash mismatch):")
                for d in stale_caches:
                    d_size = stale_cache_sizes[d]
                    print(f"  {d.name}: {d_size / (1024**3):.1f} GB")
                if sys.stdin.isatty():
                    resp = input("Delete stale mel caches? [y/N] ").strip().lower()
                    if resp == "y":
                        for d in stale_caches:
                            shutil.rmtree(d)
                            print(f"  Deleted {d.name}")
                else:
                    print("  (Non-interactive — skipping. Delete manually or "
                          "run with --clear-cache.)")

    rng = np.random.default_rng(seed)

    # Load mic calibration profile if provided
    mic_profile = None
    if args.mic_profile:
        profile_path = Path(args.mic_profile)
        if not profile_path.exists():
            print(f"ERROR: Mic profile not found: {profile_path}", file=sys.stderr)
            sys.exit(1)
        data = np.load(profile_path)
        mic_profile = {
            "band_gain": data["band_gain"].astype(np.float32),
            "noise_floor": data["noise_floor"].astype(np.float32),
        }
        # Load gain-aware fields if present
        if "noise_floor_by_gain" in data:
            mic_profile["noise_floor_by_gain"] = data["noise_floor_by_gain"].astype(np.float32)
            mic_profile["hw_gain_levels"] = data["hw_gain_levels"]
            if "recommended_gain_min" in data:
                mic_profile["recommended_gain_min"] = int(data["recommended_gain_min"])
                mic_profile["recommended_gain_max"] = int(data["recommended_gain_max"])
        print(f"Mic profile loaded: {profile_path}")
        print(f"  Band gain range: [{mic_profile['band_gain'].min():.3f}, {mic_profile['band_gain'].max():.3f}]")
        print(f"  Noise floor range: [{mic_profile['noise_floor'].min():.4f}, {mic_profile['noise_floor'].max():.4f}]")
        # Override recommended gain max with config hw_gain_max if set
        # This must agree with firmware AdaptiveMic.h hwGainMaxSignal
        hw_gain_max = cfg.get("audio", {}).get("hw_gain_max")
        if hw_gain_max is not None and "noise_floor_by_gain" in mic_profile:
            mic_profile["recommended_gain_max"] = int(hw_gain_max)
            if "recommended_gain_min" not in mic_profile:
                mic_profile["recommended_gain_min"] = 0
        if "noise_floor_by_gain" in mic_profile:
            g = mic_profile["hw_gain_levels"]
            print(f"  Gain-aware: {len(g)} levels ({g[0]}-{g[-1]})")
            if "recommended_gain_min" in mic_profile:
                print(f"  AGC range: [{mic_profile['recommended_gain_min']} - {mic_profile['recommended_gain_max']}]"
                      f"{' (from config hw_gain_max)' if hw_gain_max is not None else ''}")

    # Precompute mel filterbank and window on device (reused for every file)
    mel_fb = _build_mel_filterbank(cfg, device)
    n_fft = cfg["audio"]["n_fft"]
    window = torch.hamming_window(n_fft, periodic=False).to(device)

    # Mel cache: reuse computed mels when only labels change
    mel_cache_dir = None
    if not args.no_cache:
        cache_key = compute_mel_cache_key(
            cfg, args.augment, seed,
            mic_profile_path=args.mic_profile,
            rir_dir=str(rir_dir) if rir_dir else None,
            stems_dir=str(stems_dir) if stems_dir else None,
            stem_variants=stem_variant_list,
        )
        mel_cache_base = Path(cfg.get("data", {}).get("mel_cache_dir", "data/mel_cache"))
        mel_cache_dir = mel_cache_base / cache_key
        if args.clear_cache and mel_cache_dir.exists():
            shutil.rmtree(mel_cache_dir)
            print(f"Cleared mel cache: {mel_cache_dir}")
        mel_cache_dir.mkdir(parents=True, exist_ok=True)
        cached_count = sum(1 for d in mel_cache_dir.iterdir() if d.is_dir())
        print(f"Mel cache: {mel_cache_dir} ({cached_count} tracks cached)")

    # Find paired audio + label files (non-recursive — audio_dir should be flat)
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    subdirs = [d for d in audio_dir.iterdir() if d.is_dir()]
    if subdirs:
        print(f"WARNING: audio_dir has {len(subdirs)} subdirectories that will be ignored "
              f"(iterdir is non-recursive). Use audio/combined/ for flat namespace.",
              file=sys.stderr)
    audio_files = sorted(f for f in audio_dir.iterdir() if f.is_file() and f.suffix.lower() in audio_extensions)

    pairs = []
    seen_stems = set()
    for af in audio_files:
        if af.stem in seen_stems:
            print(f"WARNING: Duplicate stem '{af.stem}' — skipping {af}", file=sys.stderr)
            continue
        seen_stems.add(af.stem)
        label_candidates = [
            af.parent / f"{af.stem}.beats.json",
            labels_dir / f"{af.stem}.beats.json",
        ]
        for lf in label_candidates:
            if lf.exists():
                pairs.append((af, lf))
                break

    if not pairs:
        print(f"No paired audio+label files found.\n"
              f"  Audio dir: {audio_dir}\n"
              f"  Labels dir: {labels_dir}", file=sys.stderr)
        sys.exit(1)

    # Exclude test tracks to prevent data leakage
    if args.exclude_dir:
        exclude_path = Path(args.exclude_dir)
        audio_exts = {".mp3", ".wav", ".flac", ".ogg"}
        exclude_stems = {f.stem for f in exclude_path.rglob("*")
                         if f.suffix.lower() in audio_exts}
        before = len(pairs)
        pairs = [(a, l) for a, l in pairs if a.stem not in exclude_stems]
        excluded = before - len(pairs)
        if excluded > 0:
            print(f"Excluded {excluded} test tracks from training data "
                  f"(from {exclude_path})")

    # Filter by label quality score (v2 consensus labels include quality_score)
    min_quality = cfg.get("training", {}).get("min_quality", 0.0)
    if min_quality > 0:
        before = len(pairs)
        filtered = []
        for a, l in pairs:
            with open(l) as f:
                q = json.load(f).get("quality_score", 1.0)
            if q >= min_quality:
                filtered.append((a, l))
        pairs = filtered
        dropped = before - len(pairs)
        if dropped > 0:
            print(f"Filtered {dropped} tracks below quality {min_quality} "
                  f"({len(pairs)} remaining)")

    # File-level train/val split (prevents data leakage between splits)
    pairs_shuffled = list(pairs)
    rng.shuffle(pairs_shuffled)
    val_split = cfg["training"]["val_split"]
    n_val_files = max(1, int(len(pairs_shuffled) * val_split))
    val_pairs = pairs_shuffled[:n_val_files]
    train_pairs = pairs_shuffled[n_val_files:]

    teacher_soft_dir = Path(args.teacher_soft_dir) if args.teacher_soft_dir else None
    generate_teacher = getattr(args, 'teacher', False) or teacher_soft_dir is not None
    use_delta = getattr(args, 'delta', False) or cfg.get("features", {}).get("use_delta", False)
    use_band_flux = getattr(args, 'band_flux', False) or cfg.get("features", {}).get("use_band_flux", False)
    if use_delta and use_band_flux:
        print("WARNING: Both delta and band_flux enabled. Using delta (takes precedence).")
        use_band_flux = False
    teacher_src = "madmom soft" if teacher_soft_dir else "consensus Gaussian" if generate_teacher else None
    feat_str = " + Delta features" if use_delta else " + Band flux (3ch)" if use_band_flux else ""
    print(f"Found {len(pairs)} paired files. Augmentation: {'ON' if args.augment else 'OFF'}"
          f"{f' + Teacher labels ({teacher_src})' if generate_teacher else ''}"
          f"{feat_str}")
    if stems_dir:
        # Count how many tracks have stems available
        stems_found = sum(
            1 for a, _ in pairs
            if (stems_dir / "htdemucs" / a.stem).exists() or (stems_dir / a.stem).exists()
        )
        stems_missing = len(pairs) - stems_found
        print(f"Stem augmentation: {stem_variant_list} from {stems_dir}")
        print(f"  Stems available: {stems_found}/{len(pairs)} tracks"
              f"{f' ({stems_missing} missing — will use full mix only)' if stems_missing else ''}")
    print(f"File-level split: {len(train_pairs)} train, {len(val_pairs)} val")

    chunk_frames = cfg["training"]["chunk_frames"]
    chunk_stride = cfg["training"]["chunk_stride"]
    n_mels = cfg["audio"]["n_mels"]
    SHARD_BATCH = 500  # files per shard (limits RAM to ~3 GB)

    for split_name, split_pairs in [("train", train_pairs), ("val", val_pairs)]:
        shard_dir = Path(tempfile.mkdtemp(prefix=f"blinky_{split_name}_", dir=str(output_dir)))
        _active_shard_dirs.append(shard_dir)
        shard_idx = 0
        shard_counts = []
        batch_X, batch_Y, batch_T = [], [], []
        total_variants = 0
        errors = 0

        for i, (audio_path, label_path) in enumerate(tqdm(split_pairs, desc=split_name)):
            try:
                # Per-file deterministic RNG for reproducible augmentation
                # (enables mel caching across runs regardless of processing order)
                file_rng = _file_rng(seed, audio_path.stem)
                results = process_file(audio_path, label_path, cfg, args.augment,
                                       rir_dir, file_rng, device, mel_fb, window,
                                       mic_profile=mic_profile,
                                       stems_dir=stems_dir,
                                       stem_variants=stem_variant_list,
                                       mel_cache_dir=mel_cache_dir,
                                       generate_teacher=generate_teacher,
                                       teacher_soft_dir=teacher_soft_dir,
                                       noise_dir=noise_dir)
                for r in results:
                    mel_chunks, target_chunks, teacher_chunks = chunk_data(
                        r["mel"], r["target"], chunk_frames, chunk_stride,
                        teacher_target=r.get("teacher"),
                        use_delta=use_delta,
                        use_band_flux=use_band_flux,
                    )
                    batch_X.append(mel_chunks)
                    batch_Y.append(target_chunks)
                    if generate_teacher and teacher_chunks is not None:
                        batch_T.append(teacher_chunks)
                    total_variants += 1
            except Exception as e:
                tqdm.write(f"  ERROR: {audio_path.name}: {e}")
                errors += 1

            # Flush batch to disk periodically to limit RAM usage
            if (i + 1) % SHARD_BATCH == 0 or i == len(split_pairs) - 1:
                if batch_X:
                    X_s = np.concatenate(batch_X)
                    Y_s = np.concatenate(batch_Y)
                    np.save(shard_dir / f"X_{shard_idx}.npy", X_s)
                    np.save(shard_dir / f"Y_{shard_idx}.npy", Y_s)
                    if batch_T:
                        T_s = np.concatenate(batch_T)
                        np.save(shard_dir / f"T_{shard_idx}.npy", T_s)
                        del T_s
                    shard_counts.append(len(X_s))
                    del X_s, Y_s
                    shard_idx += 1
                batch_X, batch_Y, batch_T = [], [], []
                gc.collect()
                torch.cuda.empty_cache()

        # Merge shards into final .npy using memmap (never holds full dataset in RAM)
        total = sum(shard_counts)
        if total == 0:
            print(f"  WARNING: No data for {split_name} split!")
            shutil.rmtree(shard_dir)
            continue

        # Detect X feature dimension from first shard (26 for mel, 52 with delta features)
        x_first = np.load(shard_dir / "X_0.npy", mmap_mode='r')
        x_features = x_first.shape[2]  # n_mels or n_mels*2 with deltas
        del x_first
        X_out = np.lib.format.open_memmap(
            str(output_dir / f"X_{split_name}.npy"), mode='w+',
            dtype=np.float32, shape=(total, chunk_frames, x_features))
        # Detect Y shape from first shard: 1D (chunk_frames,) or 2D (chunk_frames, channels)
        y_first = np.load(shard_dir / "Y_0.npy", mmap_mode='r')
        y_shape = (total,) + y_first.shape[1:]  # (total, chunk_frames) or (total, chunk_frames, 3)
        del y_first
        Y_out = np.lib.format.open_memmap(
            str(output_dir / f"Y_{split_name}.npy"), mode='w+',
            dtype=np.float32, shape=y_shape)

        has_teacher = (shard_dir / "T_0.npy").exists()
        T_out = None
        if has_teacher:
            T_out = np.lib.format.open_memmap(
                str(output_dir / f"Y_teacher_{split_name}.npy"), mode='w+',
                dtype=np.float32, shape=(total, chunk_frames))

        offset = 0
        for s in range(shard_idx):
            X_s = np.load(shard_dir / f"X_{s}.npy")
            Y_s = np.load(shard_dir / f"Y_{s}.npy")
            n = len(X_s)
            X_out[offset:offset + n] = X_s
            Y_out[offset:offset + n] = Y_s
            if has_teacher and (shard_dir / f"T_{s}.npy").exists():
                T_s = np.load(shard_dir / f"T_{s}.npy")
                T_out[offset:offset + n] = T_s
                del T_s
            offset += n
            del X_s, Y_s

        X_out.flush()
        Y_out.flush()
        if T_out is not None:
            T_out.flush()
            pos_ratio = (T_out > 0.1).mean()
            print(f"  Teacher labels: {T_out.shape}, pos_ratio={pos_ratio:.3f}")

        del X_out, Y_out
        if T_out is not None:
            del T_out

        shutil.rmtree(shard_dir)
        _active_shard_dirs.remove(shard_dir)
        print(f"\n  {split_name}: {total} chunks from {total_variants} variants ({errors} errors)")

    # Print summary
    X_train = np.load(output_dir / "X_train.npy", mmap_mode='r')
    Y_train = np.load(output_dir / "Y_train.npy", mmap_mode='r')
    X_val = np.load(output_dir / "X_val.npy", mmap_mode='r')
    Y_val = np.load(output_dir / "Y_val.npy", mmap_mode='r')

    print(f"\nDataset saved to {output_dir}/")
    print(f"  Tracks: {len(pairs)}")
    print(f"  Train: {len(X_train)} chunks {X_train.shape}")
    print(f"  Val:   {len(X_val)} chunks {X_val.shape}")

    # === Post-prep validation ===
    # Catches silent data corruption before burning GPU hours on training.
    print("\n--- Post-prep validation ---")
    issues = []

    # 1. Positive ratio sanity check
    sample_size = min(50000, len(Y_train))
    val_sample_size = min(50000, len(Y_val))
    sample_rng = np.random.default_rng(42)
    train_idx = sample_rng.choice(len(Y_train), size=sample_size, replace=False)
    val_idx = sample_rng.choice(len(Y_val), size=val_sample_size, replace=False)
    train_pos = (Y_train[train_idx] > 0.5).mean()
    val_pos = (Y_val[val_idx] > 0.5).mean()
    print(f"  Positive ratio: train={train_pos:.4f}, val={val_pos:.4f}")
    if train_pos < 0.005:
        issues.append(f"Train positive ratio suspiciously low ({train_pos:.4f}) — labels may be wrong")
    if train_pos > 0.30:
        issues.append(f"Train positive ratio suspiciously high ({train_pos:.4f}) — labels may be wrong")
    if abs(train_pos - val_pos) > train_pos * 0.5:
        issues.append(f"Train/val positive ratio mismatch: {train_pos:.4f} vs {val_pos:.4f}")

    # 2. Mel range check (only first n_mels columns — delta/band-flux can be negative)
    mel_sample = X_train[train_idx[:1000], :, :n_mels]  # Only mel bands, not delta/flux
    mel_min, mel_max = mel_sample.min(), mel_sample.max()
    mel_mean = mel_sample.mean()
    print(f"  Mel range: [{mel_min:.3f}, {mel_max:.3f}], mean={mel_mean:.3f}")
    if mel_min < -0.1 or mel_max > 1.1:
        issues.append(f"Mel values out of [0,1] range: [{mel_min:.3f}, {mel_max:.3f}]")
    if mel_mean < 0.01 or mel_mean > 0.95:
        issues.append(f"Mel mean suspicious ({mel_mean:.3f}) — may be clipped or empty")

    # 3. Check augmentation variant count (detect silently skipped augmentations)
    expected_variants_per_track = 1  # clean only
    if args.augment:
        # clean + 4 gain + 3 noise + lowpass + bass-boost + conditioned = 11
        expected_variants_per_track = 11
        ts = cfg.get("augmentation", {}).get("time_stretch_factors", [])
        ps = cfg.get("augmentation", {}).get("pitch_shift_semitones", [])
        expected_variants_per_track += len(ps)  # pitch shifts
        expected_variants_per_track += len(ts)  # time stretches (clean only each)
    actual_per_track = len(X_train) / max(len(train_pairs), 1) / (chunk_frames / chunk_stride)
    print(f"  Variants/track: expected~{expected_variants_per_track}, "
          f"actual~{actual_per_track:.1f} chunks/track/stride")

    # 4. Check for NaN/Inf
    if np.any(~np.isfinite(mel_sample)):
        issues.append("NaN or Inf detected in mel spectrograms!")

    if issues:
        print(f"\n  VALIDATION FAILED — {len(issues)} issue(s):")
        for issue in issues:
            print(f"    ✗ {issue}")
        print("\n  Fix the issues above before training. Use --no-cache to force recompute.")
        sys.exit(1)
    else:
        print("  All checks passed.")


if __name__ == "__main__":
    main()
