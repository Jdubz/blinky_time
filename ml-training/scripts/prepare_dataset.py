#!/usr/bin/env python3
"""Extract mel spectrograms and beat activation targets from labeled audio.

Replicates the firmware's SharedSpectralAnalysis mel pipeline (raw path):
  - 16 kHz sample rate
  - Hamming window (alpha=0.54, beta=0.46)
  - FFT-256, hop-256 (no overlap)
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
import gc
import json
import shutil
import sys
import tempfile
from pathlib import Path

import librosa
import numpy as np
import torch
import torchaudio
import yaml
from tqdm import tqdm


def load_config(config_path: str) -> dict:
    with open(config_path) as f:
        return yaml.safe_load(f)


def _build_mel_filterbank(cfg: dict, device: torch.device) -> torch.Tensor:
    """Build mel filterbank using librosa (for exact consistency), on GPU.

    Returns shape (n_mels, n_fft//2 + 1) on the specified device.
    """
    sr = cfg["audio"]["sample_rate"]
    n_fft = cfg["audio"]["n_fft"]
    n_mels = cfg["audio"]["n_mels"]
    fmin = cfg["audio"]["fmin"]
    fmax = cfg["audio"]["fmax"]

    mel_basis = librosa.filters.mel(
        sr=sr, n_fft=n_fft, n_mels=n_mels,
        fmin=fmin, fmax=fmax, htk=True, norm=None,
    )
    return torch.from_numpy(mel_basis.astype(np.float32)).to(device)


def firmware_mel_spectrogram(audio: torch.Tensor, cfg: dict,
                             mel_fb: torch.Tensor,
                             window: torch.Tensor) -> np.ndarray:
    """Compute mel spectrogram matching the firmware pipeline exactly.

    Args:
        audio: (samples,) tensor on GPU/CPU
        cfg: config dict
        mel_fb: precomputed mel filterbank (n_mels, n_freqs) on same device
        window: precomputed Hamming window on same device

    Returns shape (n_frames, n_mels) numpy array with values in [0, 1].
    """
    n_fft = cfg["audio"]["n_fft"]
    hop_length = cfg["audio"]["hop_length"]

    # STFT on GPU — center=False matches firmware (no padding)
    stft = torch.stft(audio, n_fft=n_fft, hop_length=hop_length,
                      window=window, center=False, return_complex=True)
    magnitudes = stft.abs()  # (n_freqs, n_frames)

    # Apply mel filterbank: (n_mels, n_freqs) @ (n_freqs, n_frames) = (n_mels, n_frames)
    mel_spec = mel_fb @ magnitudes

    # Log compression: 10*log10(x + 1e-10), map [-60, 0] -> [0, 1]
    log_mel = 10.0 * torch.log10(mel_spec + 1e-10)
    log_mel = (log_mel + 60.0) / 60.0
    log_mel = log_mel.clamp(0.0, 1.0)

    return log_mel.T.cpu().numpy()  # (n_frames, n_mels)


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


def make_beat_targets(beat_times: np.ndarray, n_frames: int, frame_rate: float,
                      sigma: float) -> np.ndarray:
    """Create Gaussian-smoothed beat activation targets."""
    return _gaussian_targets(beat_times, n_frames, frame_rate, sigma)


def make_downbeat_targets(downbeat_times: np.ndarray, n_frames: int,
                          frame_rate: float, sigma: float) -> np.ndarray:
    """Create Gaussian-smoothed downbeat activation targets."""
    return _gaussian_targets(downbeat_times, n_frames, frame_rate, sigma)


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


def augment_audio(audio: torch.Tensor, sr: int, rir_dir: Path | None,
                  rng: np.random.Generator, device: torch.device) -> list[tuple[str, torch.Tensor]]:
    """Generate augmented versions of audio on GPU."""
    variants = [("clean", audio)]

    for gain_db in [-18, -12, -6, 6]:
        gain = 10 ** (gain_db / 20.0)
        clipped = (audio * gain).clamp(-1.0, 1.0)
        variants.append((f"gain{gain_db:+d}dB", clipped))

    for snr_db in [6, 12, 20]:
        noise = _pink_noise_gpu(len(audio), rng, device)
        signal_power = (audio ** 2).mean() + 1e-10
        noise_power = (noise ** 2).mean() + 1e-10
        noise_scale = torch.sqrt(signal_power / (noise_power * 10 ** (snr_db / 10)))
        noisy = (audio + noise * noise_scale).clamp(-1.0, 1.0)
        variants.append((f"pink-snr{snr_db}dB", noisy))

    # Low-pass filter
    sos_lp = _design_butter_sos(4, 4000, "low", sr)
    lp = _sosfilt_gpu(sos_lp, audio)
    variants.append(("lowpass-4k", lp))

    # Bass boost
    sos_bass = _design_butter_sos(2, [60, 200], "band", sr)
    bass = _sosfilt_gpu(sos_bass, audio)
    boosted = (audio + bass * 1.5).clamp(-1.0, 1.0)
    variants.append(("bass-boost", boosted))

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


def process_file(audio_path: Path, label_path: Path, cfg: dict,
                 augment: bool, rir_dir: Path | None,
                 rng: np.random.Generator,
                 device: torch.device,
                 mel_fb: torch.Tensor,
                 window: torch.Tensor) -> list[dict]:
    """Process one audio file into (features, targets) pairs.

    Audio loaded with librosa (resampling consistency), then moved to GPU
    for STFT, mel extraction, and augmentation.
    """
    sr = cfg["audio"]["sample_rate"]
    sigma = cfg["labels"]["sigma_frames"]
    frame_rate = cfg["audio"]["frame_rate"]
    use_downbeat = cfg["model"].get("downbeat", False)

    # Load audio with librosa for resampling consistency
    audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)

    # Normalize RMS to simulate firmware AGC level.
    # Mastered audio (~-8 dB RMS) produces mel values crushed to [0.95, 1.0].
    # Firmware mic+AGC operates at much lower levels where [-60, 0] dB mapping
    # uses the full [0, 1] range. Target -35 dB RMS gives mean ~0.81, std ~0.12.
    target_rms_db = cfg["audio"].get("target_rms_db", -35)
    rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
    target_rms = 10 ** (target_rms_db / 20)
    audio_np = audio_np * (target_rms / rms)

    audio_gpu = torch.from_numpy(audio_np).to(device)

    # Load beat labels
    with open(label_path) as f:
        labels = json.load(f)
    beat_times = np.array([h["time"] for h in labels["hits"] if h.get("expectTrigger", True)])

    downbeat_times = np.array([
        h["time"] for h in labels["hits"]
        if h.get("expectTrigger", True)
        and (h.get("isDownbeat", False) or h.get("strength", 0.7) > 0.9)
    ]) if use_downbeat else np.array([])

    results = []

    if augment:
        variants = augment_audio(audio_gpu, sr, rir_dir, rng, device)
    else:
        variants = [("clean", audio_gpu)]

    for aug_name, aug_audio in variants:
        mel = firmware_mel_spectrogram(aug_audio, cfg, mel_fb, window)
        n_frames = mel.shape[0]
        targets = make_beat_targets(beat_times, n_frames, frame_rate, sigma)

        result = {
            "mel": mel,
            "target": targets,
            "aug": aug_name,
            "source": audio_path.stem,
        }

        if use_downbeat:
            result["downbeat_target"] = make_downbeat_targets(
                downbeat_times, n_frames, frame_rate, sigma)

        results.append(result)

        if augment and aug_name == "clean":
            conditioned_mel = apply_spectral_conditioning(mel)
            cond_result = {
                "mel": conditioned_mel,
                "target": targets,
                "aug": "conditioned",
                "source": audio_path.stem,
            }
            if use_downbeat:
                cond_result["downbeat_target"] = result["downbeat_target"]
            results.append(cond_result)

    return results


def chunk_data(mel: np.ndarray, target: np.ndarray,
               chunk_frames: int, chunk_stride: int,
               downbeat_target: np.ndarray | None = None) -> tuple:
    """Split mel/target arrays into overlapping fixed-length chunks."""
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
    parser.add_argument("--device", default=None, help="Device: cuda, cpu, or auto (default: auto)")
    args = parser.parse_args()

    cfg = load_config(args.config)

    audio_dir = Path(args.audio_dir or cfg["data"]["audio_dir"])
    labels_dir = Path(args.labels_dir or cfg["data"]["labels_dir"])
    output_dir = Path(args.output_dir or cfg["data"]["processed_dir"])
    rir_dir = Path(args.rir_dir) if args.rir_dir else Path(cfg["data"].get("rir_dir", "data/rir"))
    seed = args.seed if args.seed is not None else cfg["training"]["seed"]

    if args.device is None or args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    output_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)

    # Precompute mel filterbank and window on device (reused for every file)
    mel_fb = _build_mel_filterbank(cfg, device)
    n_fft = cfg["audio"]["n_fft"]
    window = torch.hamming_window(n_fft, periodic=False).to(device)

    # Find paired audio + label files
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_files = sorted(f for f in audio_dir.rglob("*") if f.suffix.lower() in audio_extensions)

    pairs = []
    for af in audio_files:
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

    # File-level train/val split (prevents data leakage between splits)
    pairs_shuffled = list(pairs)
    rng.shuffle(pairs_shuffled)
    val_split = cfg["training"]["val_split"]
    n_val_files = max(1, int(len(pairs_shuffled) * val_split))
    val_pairs = pairs_shuffled[:n_val_files]
    train_pairs = pairs_shuffled[n_val_files:]

    print(f"Found {len(pairs)} paired files. Augmentation: {'ON' if args.augment else 'OFF'}")
    print(f"File-level split: {len(train_pairs)} train, {len(val_pairs)} val")

    chunk_frames = cfg["training"]["chunk_frames"]
    chunk_stride = cfg["training"]["chunk_stride"]
    n_mels = cfg["audio"]["n_mels"]
    SHARD_BATCH = 500  # files per shard (limits RAM to ~3 GB)

    for split_name, split_pairs in [("train", train_pairs), ("val", val_pairs)]:
        shard_dir = Path(tempfile.mkdtemp(prefix=f"blinky_{split_name}_"))
        shard_idx = 0
        shard_counts = []
        batch_X, batch_Y, batch_D = [], [], []
        total_variants = 0
        errors = 0

        for i, (audio_path, label_path) in enumerate(tqdm(split_pairs, desc=split_name)):
            try:
                results = process_file(audio_path, label_path, cfg, args.augment,
                                       rir_dir, rng, device, mel_fb, window)
                for r in results:
                    mel_chunks, target_chunks, db_chunks = chunk_data(
                        r["mel"], r["target"], chunk_frames, chunk_stride,
                        downbeat_target=r.get("downbeat_target"),
                    )
                    batch_X.append(mel_chunks)
                    batch_Y.append(target_chunks)
                    if db_chunks is not None:
                        batch_D.append(db_chunks)
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
                    if batch_D:
                        D_s = np.concatenate(batch_D)
                        np.save(shard_dir / f"D_{shard_idx}.npy", D_s)
                        del D_s
                    shard_counts.append(len(X_s))
                    del X_s, Y_s
                    shard_idx += 1
                batch_X, batch_Y, batch_D = [], [], []
                gc.collect()
                torch.cuda.empty_cache()

        # Merge shards into final .npy using memmap (never holds full dataset in RAM)
        total = sum(shard_counts)
        if total == 0:
            print(f"  WARNING: No data for {split_name} split!")
            shutil.rmtree(shard_dir)
            continue

        X_out = np.lib.format.open_memmap(
            str(output_dir / f"X_{split_name}.npy"), mode='w+',
            dtype=np.float32, shape=(total, chunk_frames, n_mels))
        Y_out = np.lib.format.open_memmap(
            str(output_dir / f"Y_{split_name}.npy"), mode='w+',
            dtype=np.float32, shape=(total, chunk_frames))

        has_db = (shard_dir / "D_0.npy").exists()
        D_out = None
        if has_db:
            D_out = np.lib.format.open_memmap(
                str(output_dir / f"Y_db_{split_name}.npy"), mode='w+',
                dtype=np.float32, shape=(total, chunk_frames))

        offset = 0
        for s in range(shard_idx):
            X_s = np.load(shard_dir / f"X_{s}.npy")
            Y_s = np.load(shard_dir / f"Y_{s}.npy")
            n = len(X_s)
            X_out[offset:offset + n] = X_s
            Y_out[offset:offset + n] = Y_s
            if has_db and (shard_dir / f"D_{s}.npy").exists():
                D_s = np.load(shard_dir / f"D_{s}.npy")
                D_out[offset:offset + n] = D_s
                del D_s
            offset += n
            del X_s, Y_s

        X_out.flush()
        Y_out.flush()
        if D_out is not None:
            D_out.flush()

        del X_out, Y_out
        if D_out is not None:
            del D_out

        shutil.rmtree(shard_dir)
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

    # Compute positive ratios from a sample (avoid loading full memmap)
    sample_idx = np.arange(min(10000, len(Y_train)))
    print(f"  Positive ratio (sample): {Y_train[sample_idx].mean():.3f} (train)")
    if (output_dir / "Y_db_train.npy").exists():
        Y_db = np.load(output_dir / "Y_db_train.npy", mmap_mode='r')
        print(f"  Downbeat ratio (sample): {Y_db[sample_idx].mean():.3f} (train)")


if __name__ == "__main__":
    main()
