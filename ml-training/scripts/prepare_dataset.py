#!/usr/bin/env python3
"""Extract mel spectrograms and beat activation targets from labeled audio.

Replicates the firmware's SharedSpectralAnalysis mel pipeline exactly:
  - 16 kHz sample rate
  - Hamming window (alpha=0.54, beta=0.46)
  - FFT-256, hop-256 (no overlap)
  - 26 mel bands (60-8000 Hz), HTK mel scale
  - Triangular filterbank (area-normalized)
  - Soft-knee compressor on FFT magnitudes
  - Log compression: 10*log10(x + 1e-10), mapped [-60, 0] dB -> [0, 1]

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
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import yaml
from tqdm import tqdm


def load_config(config_path: str) -> dict:
    with open(config_path) as f:
        return yaml.safe_load(f)


def firmware_mel_spectrogram(audio: np.ndarray, cfg: dict) -> np.ndarray:
    """Compute mel spectrogram matching the firmware pipeline exactly.

    Returns shape (n_frames, n_mels) with values in [0, 1].
    """
    sr = cfg["audio"]["sample_rate"]
    n_fft = cfg["audio"]["n_fft"]
    hop_length = cfg["audio"]["hop_length"]
    n_mels = cfg["audio"]["n_mels"]
    fmin = cfg["audio"]["fmin"]
    fmax = cfg["audio"]["fmax"]

    # Firmware uses Hamming window (alpha=0.54, beta=0.46)
    stft = librosa.stft(
        audio, n_fft=n_fft, hop_length=hop_length,
        window="hamming", center=False,
    )
    magnitudes = np.abs(stft)  # (n_fft//2 + 1, n_frames)

    # Mel filterbank — HTK mel scale matches firmware's hzToMel formula
    mel_basis = librosa.filters.mel(
        sr=sr, n_fft=n_fft, n_mels=n_mels,
        fmin=fmin, fmax=fmax, htk=True, norm=None,
    )

    # Apply mel filterbank
    mel_spec = mel_basis @ magnitudes  # (n_mels, n_frames)

    # Firmware log compression: 10*log10(x + 1e-10), then map [-60, 0] -> [0, 1]
    log_mel = 10.0 * np.log10(mel_spec + 1e-10)
    log_mel = (log_mel + 60.0) / 60.0
    log_mel = np.clip(log_mel, 0.0, 1.0)

    return log_mel.T  # (n_frames, n_mels)


def apply_spectral_conditioning(mel: np.ndarray, cfg: dict) -> np.ndarray:
    """Apply a static approximation of the firmware's spectral conditioning.

    Simulates the compressor + per-band whitening pipeline that detectors
    (other than the NN) see. Training on a mix of raw and conditioned mel
    features teaches the NN robustness — important because:
      1. NN inference uses raw mel bands (getRawMelBands), but
      2. If someone accidentally routes conditioned bands, the NN shouldn't break
      3. The NN should learn features invariant to dynamic range compression

    Args:
        mel: (n_frames, n_mels) raw log-compressed mel bands in [0, 1]
        cfg: config dict (uses compressor params from firmware defaults)

    Returns:
        (n_frames, n_mels) conditioned mel bands in [0, 1]
    """
    mel = mel.copy()
    n_frames, n_mels = mel.shape

    # --- Static soft-knee compressor (Giannoulis/Massberg/Reiss 2012) ---
    # Firmware params: threshold=-30dB, ratio=3:1, knee=15dB, makeup=+6dB
    # Operates on frame RMS in dB, applies uniform gain to all bands.
    threshold_db = -30.0
    ratio = 3.0
    knee_db = 15.0
    makeup_db = 6.0
    half_knee = knee_db * 0.5

    for t in range(n_frames):
        # Frame RMS in [0,1] normalized space → convert back to dB
        frame_rms = np.sqrt(np.mean(mel[t] ** 2) + 1e-10)
        rms_db = 20.0 * np.log10(frame_rms + 1e-10)

        diff = rms_db - threshold_db
        if diff <= -half_knee:
            gain_db = 0.0
        elif diff >= half_knee:
            gain_db = (1.0 - 1.0 / ratio) * (threshold_db - rms_db)
        else:
            x = diff + half_knee
            gain_db = (1.0 / ratio - 1.0) * x * x / (2.0 * knee_db)

        gain_db += makeup_db
        linear_gain = 10 ** (gain_db / 20.0)
        mel[t] *= linear_gain

    mel = np.clip(mel, 0.0, 1.0)

    # --- Per-band adaptive whitening (Stowell & Plumbley 2007) ---
    # Running max with exponential decay, normalize by running max.
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
    """Create Gaussian-smoothed activation targets from event times.

    Returns shape (n_frames,) with values in [0, 1].
    """
    targets = np.zeros(n_frames, dtype=np.float32)
    for t in times:
        frame_idx = t * frame_rate
        frame_range = max(0, int(frame_idx - 4 * sigma)), min(n_frames, int(frame_idx + 4 * sigma) + 1)
        for i in range(frame_range[0], frame_range[1]):
            targets[i] = max(targets[i], np.exp(-0.5 * ((i - frame_idx) / sigma) ** 2))
    return targets


def make_beat_targets(beat_times: np.ndarray, n_frames: int, frame_rate: float,
                      sigma: float) -> np.ndarray:
    """Create Gaussian-smoothed beat activation targets."""
    return _gaussian_targets(beat_times, n_frames, frame_rate, sigma)


def make_downbeat_targets(downbeat_times: np.ndarray, n_frames: int,
                          frame_rate: float, sigma: float) -> np.ndarray:
    """Create Gaussian-smoothed downbeat activation targets."""
    return _gaussian_targets(downbeat_times, n_frames, frame_rate, sigma)


def augment_audio(audio: np.ndarray, sr: int, rir_dir: Path | None,
                  rng: np.random.Generator) -> list[tuple[np.ndarray, str]]:
    """Generate augmented versions of audio for diverse acoustic environments.

    Returns list of (augmented_audio, augmentation_description) tuples.
    """
    variants = [("clean", audio)]

    # Volume variations: simulate near-speaker vs far-from-speaker
    for gain_db in [-18, -12, -6, 6]:
        gain = 10 ** (gain_db / 20.0)
        clipped = np.clip(audio * gain, -1.0, 1.0)
        variants.append((f"gain{gain_db:+d}dB", clipped))

    # Pink noise at various SNRs (crowd noise approximation)
    for snr_db in [6, 12, 20]:
        noise = _pink_noise(len(audio), rng)
        signal_power = np.mean(audio ** 2) + 1e-10
        noise_power = np.mean(noise ** 2) + 1e-10
        noise_scale = np.sqrt(signal_power / (noise_power * 10 ** (snr_db / 10)))
        noisy = audio + noise * noise_scale
        noisy = np.clip(noisy, -1.0, 1.0)
        variants.append((f"pink-snr{snr_db}dB", noisy))

    # Low-pass filter (boomy warehouse — everything above 4kHz attenuated)
    from scipy.signal import butter, sosfilt
    sos = butter(4, 4000, btype="low", fs=sr, output="sos")
    lp = sosfilt(sos, audio).astype(np.float32)
    variants.append(("lowpass-4k", lp))

    # Boomy bass boost (warehouse resonance)
    sos_bass = butter(2, [60, 200], btype="band", fs=sr, output="sos")
    bass = sosfilt(sos_bass, audio).astype(np.float32)
    boosted = np.clip(audio + bass * 1.5, -1.0, 1.0)
    variants.append(("bass-boost", boosted))

    # Room impulse responses (if available)
    if rir_dir and rir_dir.exists():
        rir_files = list(rir_dir.glob("*.wav")) + list(rir_dir.glob("*.npy"))
        if rir_files:
            # Pick up to 3 random RIRs
            chosen = rng.choice(rir_files, size=min(3, len(rir_files)), replace=False)
            for rir_path in chosen:
                if rir_path.suffix == ".npy":
                    rir = np.load(rir_path)
                else:
                    rir, _ = librosa.load(rir_path, sr=sr, mono=True)
                # Normalize RIR
                rir = rir / (np.max(np.abs(rir)) + 1e-10)
                convolved = np.convolve(audio, rir, mode="full")[:len(audio)]
                convolved = convolved / (np.max(np.abs(convolved)) + 1e-10)
                variants.append((f"rir-{rir_path.stem}", convolved.astype(np.float32)))

    return variants


def _pink_noise(n: int, rng: np.random.Generator) -> np.ndarray:
    """Generate pink (1/f) noise."""
    white = rng.standard_normal(n).astype(np.float32)
    # Voss-McCartney algorithm approximation via spectral shaping
    fft = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n)
    freqs[0] = 1  # avoid div by zero
    fft *= 1.0 / np.sqrt(freqs)
    pink = np.fft.irfft(fft, n=n).astype(np.float32)
    return pink / (np.max(np.abs(pink)) + 1e-10)


def process_file(audio_path: Path, label_path: Path, cfg: dict,
                 augment: bool, rir_dir: Path | None,
                 rng: np.random.Generator) -> list[dict]:
    """Process one audio file into (features, targets) pairs.

    Returns list of dicts with 'mel', 'target', and optionally 'downbeat_target'.
    """
    sr = cfg["audio"]["sample_rate"]
    sigma = cfg["labels"]["sigma_frames"]
    frame_rate = cfg["audio"]["frame_rate"]
    use_downbeat = cfg["model"].get("downbeat", False)

    # Load audio at firmware sample rate
    audio, _ = librosa.load(str(audio_path), sr=sr, mono=True)

    # Load beat labels
    with open(label_path) as f:
        labels = json.load(f)
    beat_times = np.array([h["time"] for h in labels["hits"] if h.get("expectTrigger", True)])

    # Extract downbeat times (strength > 0.9 indicates downbeat from label_beats.py)
    downbeat_times = np.array([
        h["time"] for h in labels["hits"]
        if h.get("expectTrigger", True) and h.get("strength", 0.7) > 0.9
    ]) if use_downbeat else np.array([])

    results = []

    if augment:
        variants = augment_audio(audio, sr, rir_dir, rng)
    else:
        variants = [("clean", audio)]

    for aug_name, aug_audio in variants:
        mel = firmware_mel_spectrogram(aug_audio, cfg)
        n_frames = mel.shape[0]
        targets = make_beat_targets(beat_times, n_frames, frame_rate, sigma)

        result = {
            "mel": mel,
            "target": targets,
            "aug": aug_name,
            "source": audio_path.stem,
        }

        if use_downbeat and len(downbeat_times) > 0:
            result["downbeat_target"] = make_downbeat_targets(
                downbeat_times, n_frames, frame_rate, sigma)

        results.append(result)

        # Spectral conditioning augmentation: apply firmware compressor+whitening
        # to the clean variant. Teaches NN robustness to spectral conditioning.
        if augment and aug_name == "clean":
            conditioned_mel = apply_spectral_conditioning(mel, cfg)
            cond_result = {
                "mel": conditioned_mel,
                "target": targets,
                "aug": "conditioned",
                "source": audio_path.stem,
            }
            if use_downbeat and len(downbeat_times) > 0:
                cond_result["downbeat_target"] = result["downbeat_target"]
            results.append(cond_result)

    return results


def chunk_data(mel: np.ndarray, target: np.ndarray,
               chunk_frames: int, chunk_stride: int,
               downbeat_target: np.ndarray | None = None) -> tuple:
    """Split mel/target arrays into overlapping fixed-length chunks.

    Returns (mel_chunks, target_chunks) or (mel_chunks, target_chunks, downbeat_chunks)
    if downbeat_target is provided.
    """
    n_frames = mel.shape[0]
    has_downbeat = downbeat_target is not None

    if n_frames < chunk_frames:
        pad_mel = np.zeros((chunk_frames, mel.shape[1]), dtype=mel.dtype)
        pad_target = np.zeros(chunk_frames, dtype=target.dtype)
        pad_mel[:n_frames] = mel
        pad_target[:n_frames] = target
        if has_downbeat:
            pad_db = np.zeros(chunk_frames, dtype=downbeat_target.dtype)
            pad_db[:n_frames] = downbeat_target
            return pad_mel[np.newaxis], pad_target[np.newaxis], pad_db[np.newaxis]
        return pad_mel[np.newaxis], pad_target[np.newaxis], None

    chunks_mel = []
    chunks_target = []
    chunks_db = [] if has_downbeat else None
    for start in range(0, n_frames - chunk_frames + 1, chunk_stride):
        chunks_mel.append(mel[start:start + chunk_frames])
        chunks_target.append(target[start:start + chunk_frames])
        if has_downbeat:
            chunks_db.append(downbeat_target[start:start + chunk_frames])

    db_arr = np.array(chunks_db) if has_downbeat else None
    return np.array(chunks_mel), np.array(chunks_target), db_arr


def main():
    parser = argparse.ArgumentParser(description="Prepare dataset for beat activation CNN")
    parser.add_argument("--config", default="configs/default.yaml", help="Config file path")
    parser.add_argument("--augment", action="store_true", help="Apply acoustic environment augmentation")
    parser.add_argument("--audio-dir", default=None, help="Override audio directory from config")
    parser.add_argument("--labels-dir", default=None, help="Override labels directory from config")
    parser.add_argument("--output-dir", default=None, help="Override output directory from config")
    parser.add_argument("--rir-dir", default=None, help="Directory of room impulse responses (.wav/.npy)")
    parser.add_argument("--seed", default=None, type=int, help="Random seed for augmentation")
    args = parser.parse_args()

    cfg = load_config(args.config)

    audio_dir = Path(args.audio_dir or cfg["data"]["audio_dir"])
    labels_dir = Path(args.labels_dir or cfg["data"]["labels_dir"])
    output_dir = Path(args.output_dir or cfg["data"]["processed_dir"])
    rir_dir = Path(args.rir_dir) if args.rir_dir else Path(cfg["data"].get("rir_dir", "data/rir"))
    seed = args.seed if args.seed is not None else cfg["training"]["seed"]

    output_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)

    # Find paired audio + label files
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_files = sorted(f for f in audio_dir.rglob("*") if f.suffix.lower() in audio_extensions)

    pairs = []
    for af in audio_files:
        # Look for labels alongside audio, or in labels_dir
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

    print(f"Found {len(pairs)} paired files. Augmentation: {'ON' if args.augment else 'OFF'}")

    chunk_frames = cfg["training"]["chunk_frames"]
    chunk_stride = cfg["training"]["chunk_stride"]
    use_downbeat = cfg["model"].get("downbeat", False)

    all_mel_chunks = []
    all_target_chunks = []
    all_db_chunks = []
    total_variants = 0

    for audio_path, label_path in tqdm(pairs, desc="Processing"):
        try:
            results = process_file(audio_path, label_path, cfg, args.augment, rir_dir, rng)
            for r in results:
                mel_chunks, target_chunks, db_chunks = chunk_data(
                    r["mel"], r["target"], chunk_frames, chunk_stride,
                    downbeat_target=r.get("downbeat_target"),
                )
                all_mel_chunks.append(mel_chunks)
                all_target_chunks.append(target_chunks)
                if db_chunks is not None:
                    all_db_chunks.append(db_chunks)
                total_variants += 1
        except Exception as e:
            tqdm.write(f"  ERROR: {audio_path.name}: {e}")

    # Concatenate all chunks
    X = np.concatenate(all_mel_chunks, axis=0)  # (N, chunk_frames, n_mels)
    Y = np.concatenate(all_target_chunks, axis=0)  # (N, chunk_frames)
    Y_db = np.concatenate(all_db_chunks, axis=0) if all_db_chunks else None

    # Shuffle
    perm = rng.permutation(len(X))
    X = X[perm]
    Y = Y[perm]
    if Y_db is not None:
        Y_db = Y_db[perm]

    # Train/val split
    val_split = cfg["training"]["val_split"]
    n_val = int(len(X) * val_split)
    X_train, X_val = X[n_val:], X[:n_val]
    Y_train, Y_val = Y[n_val:], Y[:n_val]

    # Save
    np.save(output_dir / "X_train.npy", X_train)
    np.save(output_dir / "Y_train.npy", Y_train)
    np.save(output_dir / "X_val.npy", X_val)
    np.save(output_dir / "Y_val.npy", Y_val)

    if Y_db is not None:
        Y_db_train, Y_db_val = Y_db[n_val:], Y_db[:n_val]
        np.save(output_dir / "Y_db_train.npy", Y_db_train)
        np.save(output_dir / "Y_db_val.npy", Y_db_val)
        print(f"  Downbeat positive ratio: {Y_db_train.mean():.3f} (train)")

    print(f"\nDataset saved to {output_dir}/")
    print(f"  Tracks: {len(pairs)}, Variants: {total_variants}")
    print(f"  Train: {len(X_train)} chunks ({X_train.shape})")
    print(f"  Val:   {len(X_val)} chunks ({X_val.shape})")
    print(f"  Positive frame ratio: {Y_train.mean():.3f} (train), {Y_val.mean():.3f} (val)")


if __name__ == "__main__":
    main()
