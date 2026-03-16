#!/usr/bin/env python3
"""Evaluate beat activation model offline (PyTorch, GPU-accelerated).

Computes per-track and aggregate metrics:
  - Frame-level: precision, recall, F1 (at threshold)
  - Beat-level: precision, recall, F1 (using mir_eval with ±70ms tolerance)
  - Activation plots for visual inspection

Usage:
    # Evaluate on validation set
    python evaluate.py --config configs/default.yaml

    # Evaluate on specific tracks
    python evaluate.py --config configs/default.yaml --audio-dir ../blinky-test-player/music/edm
"""

import argparse
import json
import sys
from pathlib import Path

import librosa
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import mir_eval
import numpy as np
import torch

from models.beat_cnn import build_beat_cnn
from scripts.audio import (
    build_mel_filterbank_torch as _build_mel_filterbank,
    firmware_mel_spectrogram_torch as firmware_mel_spectrogram,
    load_config,
)


def compute_acf_tempo_quality(activations: np.ndarray, ref_beats: np.ndarray,
                              frame_rate: float) -> dict:
    """Compute ACF-based ODF quality metrics against ground truth tempo.

    Measures how well the activation signal's autocorrelation peak matches
    the expected tempo period -- a proxy for how useful the ODF is for a
    CBSS beat tracker (which relies on clear periodicity, not just isolated
    peak accuracy).

    Returns:
        acf_peak_ratio:      ACF value at the ground-truth lag relative to
                             lag-0 (periodicity strength, 0-1).
        acf_peak_prominence: Ratio of the peak value to the mean ACF in its
                             surrounding region (how sharp/clear the peak is).
        acf_lag_error:       Absolute error in frames between the nearest ACF
                             peak and the expected lag.
    """
    result = {
        "acf_peak_ratio": 0.0,
        "acf_peak_prominence": 0.0,
        "acf_lag_error": float("inf"),
    }

    # Need at least 3 reference beats to compute a meaningful IBI
    if len(ref_beats) < 3 or len(activations) < 4:
        return result

    # Ground truth tempo from median inter-beat interval
    ibi = float(np.median(np.diff(ref_beats)))
    if ibi <= 0:
        return result

    expected_lag = ibi * frame_rate  # in frames

    # ACF needs enough signal to cover at least one full period
    if expected_lag < 2 or expected_lag >= len(activations) // 2:
        return result

    # FFT-based autocorrelation (O(N log N) vs O(N^2) for np.correlate)
    x = activations - np.mean(activations)
    n = len(x)
    fft_size = 1
    while fft_size < 2 * n:
        fft_size *= 2
    X_fft = np.fft.rfft(x, n=fft_size)
    acf_full = np.fft.irfft(X_fft * np.conj(X_fft), n=fft_size)
    # Take the positive-lag half (including lag 0)
    acf = acf_full[:n]

    # Normalize by lag-0 (energy)
    if acf[0] <= 0:
        return result
    acf = acf / acf[0]

    # Search for the ACF peak within ±10% of expected lag
    search_lo = max(1, int(expected_lag * 0.9))
    search_hi = min(len(acf) - 1, int(expected_lag * 1.1) + 1)
    if search_lo >= search_hi:
        return result

    search_region = acf[search_lo:search_hi]
    peak_idx_local = int(np.argmax(search_region))
    peak_idx = search_lo + peak_idx_local
    peak_value = float(acf[peak_idx])

    # acf_peak_ratio: periodicity strength (ACF at best lag vs lag 0)
    result["acf_peak_ratio"] = peak_value

    # acf_lag_error: distance from the peak to the expected lag
    result["acf_lag_error"] = abs(peak_idx - expected_lag)

    # acf_peak_prominence: peak value relative to surrounding ACF mean
    # Use ±20% of expected lag around the peak as the "surrounding region"
    margin = max(1, int(expected_lag * 0.2))
    surr_lo = max(1, peak_idx - margin)
    surr_hi = min(len(acf), peak_idx + margin + 1)
    surrounding = np.concatenate([acf[surr_lo:peak_idx], acf[peak_idx + 1:surr_hi]])
    if len(surrounding) > 0:
        surr_mean = float(np.mean(surrounding))
        # Avoid division by zero/negative; use small floor
        result["acf_peak_prominence"] = peak_value / max(surr_mean, 1e-6)
    else:
        result["acf_peak_prominence"] = 0.0

    return result


def _load_model(model_path: str, cfg: dict, device: torch.device):
    """Load a trained beat activation model (CNN, DS-TCN, or frame FC)."""
    checkpoint = torch.load(model_path, map_location=device, weights_only=True)

    # Handle both bare state_dict and full checkpoint
    if isinstance(checkpoint, dict) and "state_dict" in checkpoint:
        state_dict = checkpoint["state_dict"]
        use_downbeat = checkpoint.get("use_downbeat", cfg["model"].get("downbeat", False))
    else:
        state_dict = checkpoint
        use_downbeat = cfg["model"].get("downbeat", False)

    model_type = cfg["model"].get("type", "causal_cnn")
    if model_type == "frame_fc":
        from models.beat_fc import build_beat_fc
        model = build_beat_fc(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"].get("dropout", 0.1),
            downbeat=use_downbeat,
        ).to(device)
    elif model_type == "frame_conv1d":
        from models.beat_conv1d import build_beat_conv1d
        model = build_beat_conv1d(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            dropout=cfg["model"].get("dropout", 0.1),
            downbeat=use_downbeat,
            sum_head=cfg["model"].get("sum_head", False),
        ).to(device)
    elif model_type == "frame_conv1d_pool":
        from models.beat_conv1d_pool import build_beat_conv1d_pool
        model = build_beat_conv1d_pool(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            pool_sizes=cfg["model"]["pool_sizes"],
            dropout=cfg["model"].get("dropout", 0.1),
            downbeat=use_downbeat,
            use_stride=cfg["model"].get("use_stride", False),
        ).to(device)
    else:
        model = build_beat_cnn(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_size=cfg["model"]["kernel_size"],
            dilations=cfg["model"]["dilations"],
            dropout=cfg["model"].get("dropout", 0.1),
            downbeat=use_downbeat,
            model_type=model_type,
            residual=cfg["model"].get("residual", False),
        ).to(device)
    model.load_state_dict(state_dict)
    model.eval()
    pool_factor = getattr(model, 'pool_factor', 1)
    return model, use_downbeat, pool_factor


def evaluate_on_tracks(model_path: str, audio_dir: Path, cfg: dict,
                       output_dir: Path, threshold: float = 0.5,
                       device: torch.device = None):
    """Run model on full tracks and evaluate beat detection accuracy."""
    if device is None:
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]
    chunk_frames = cfg["training"]["chunk_frames"]

    model, has_downbeat, pool_factor = _load_model(model_path, cfg, device)
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    audio_files = sorted(
        f for f in audio_dir.rglob("*")
        if f.suffix.lower() in {".mp3", ".wav", ".flac"}
    )

    all_results = []

    for audio_path in audio_files:
        label_path = audio_path.parent / f"{audio_path.stem}.beats.json"
        if not label_path.exists():
            continue

        # Load and process (normalize RMS to match firmware AGC level)
        audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)
        target_rms_db = cfg["audio"].get("target_rms_db", -35)
        rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)

        # Run model on overlapping chunks, average predictions
        n_frames = mel.shape[0]
        activations = np.zeros(n_frames, dtype=np.float32)
        db_activations = np.zeros(n_frames, dtype=np.float32) if has_downbeat else None
        counts = np.zeros(n_frames, dtype=np.float32)

        stride = chunk_frames // 2
        mel_tensor = torch.from_numpy(mel).float().to(device)

        with torch.no_grad():
            for start in range(0, max(1, n_frames - chunk_frames + 1), stride):
                end = start + chunk_frames
                if end > n_frames:
                    chunk = torch.zeros(chunk_frames, mel.shape[1],
                                        device=device, dtype=torch.float32)
                    chunk[:n_frames - start] = mel_tensor[start:n_frames]
                else:
                    chunk = mel_tensor[start:end]

                pred = model(chunk.unsqueeze(0))[0]  # (time or time//pf, channels)

                if pool_factor > 1:
                    # Upsample pooled output back to input resolution
                    pred_np = pred.cpu().numpy()
                    pred_np = np.repeat(pred_np, pool_factor, axis=0)
                    actual_len = min(chunk_frames, n_frames - start)
                    pred_np = pred_np[:actual_len]
                else:
                    actual_len = min(chunk_frames, n_frames - start)
                    pred_np = pred[:actual_len].cpu().numpy()

                activations[start:start + actual_len] += pred_np[:, 0]
                if has_downbeat:
                    db_activations[start:start + actual_len] += pred_np[:, 1]
                counts[start:start + actual_len] += 1

        activations /= np.maximum(counts, 1)
        if has_downbeat:
            db_activations /= np.maximum(counts, 1)

        # Load ground truth beats
        with open(label_path) as f:
            labels = json.load(f)
        ref_beats = np.array([h["time"] for h in labels["hits"] if h.get("expectTrigger", True)])

        # Peak-pick activations to get estimated beat times
        est_beats = _peak_pick(activations, threshold, frame_rate)

        # Beat-level F1 using mir_eval (±70ms tolerance)
        if len(ref_beats) > 0 and len(est_beats) > 0:
            scores = mir_eval.beat.f_measure(ref_beats, est_beats, f_measure_threshold=0.07)
        else:
            scores = 0.0

        result = {
            "track": audio_path.stem,
            "ref_beats": len(ref_beats),
            "est_beats": len(est_beats),
            "f1": float(scores),
        }

        # Downbeat evaluation
        if has_downbeat:
            ref_downbeats = np.array([
                h["time"] for h in labels["hits"]
                if h.get("expectTrigger", True) and h.get("isDownbeat", False)
            ])
            est_downbeats = _peak_pick(db_activations, threshold, frame_rate)
            if len(ref_downbeats) > 0 and len(est_downbeats) > 0:
                db_scores = mir_eval.beat.f_measure(
                    ref_downbeats, est_downbeats, f_measure_threshold=0.07)
            else:
                db_scores = 0.0
            result["db_f1"] = float(db_scores)
            result["ref_downbeats"] = len(ref_downbeats)
            result["est_downbeats"] = len(est_downbeats)

        # ACF-based ODF quality metrics
        acf_metrics = compute_acf_tempo_quality(activations, ref_beats, frame_rate)
        result["acf_peak_ratio"] = acf_metrics["acf_peak_ratio"]
        result["acf_peak_prominence"] = acf_metrics["acf_peak_prominence"]
        lag_err = acf_metrics["acf_lag_error"]
        result["acf_lag_error"] = lag_err if np.isfinite(lag_err) else None

        all_results.append(result)
        db_str = f", DB F1={result['db_f1']:.3f}" if has_downbeat else ""
        acf_err_str = f"{lag_err:.1f}f" if np.isfinite(lag_err) else "n/a"
        acf_str = f", ACF ratio={acf_metrics['acf_peak_ratio']:.3f} prom={acf_metrics['acf_peak_prominence']:.2f} err={acf_err_str}"
        print(f"  {audio_path.stem}: F1={scores:.3f} (ref={len(ref_beats)}, est={len(est_beats)}){db_str}{acf_str}")

        # Save activation plot
        _plot_activation(activations, ref_beats, est_beats, frame_rate,
                         audio_path.stem, output_dir / "plots",
                         db_activations=db_activations)

    # Aggregate
    if all_results:
        f1s = [r["f1"] for r in all_results]
        print(f"\nAggregate Beat: mean F1={np.mean(f1s):.3f}, median={np.median(f1s):.3f}, "
              f"min={np.min(f1s):.3f}, max={np.max(f1s):.3f}")

        db_f1s = [r["db_f1"] for r in all_results if "db_f1" in r]
        if db_f1s:
            print(f"Aggregate Downbeat: mean F1={np.mean(db_f1s):.3f}, "
                  f"median={np.median(db_f1s):.3f}, "
                  f"min={np.min(db_f1s):.3f}, max={np.max(db_f1s):.3f}")

        # ACF tempo quality aggregates
        acf_ratios = [r["acf_peak_ratio"] for r in all_results]
        acf_proms = [r["acf_peak_prominence"] for r in all_results]
        acf_errs = [r["acf_lag_error"] for r in all_results
                    if r["acf_lag_error"] is not None]
        if acf_errs:
            print(f"Aggregate ACF Tempo Quality: "
                  f"mean peak_ratio={np.mean(acf_ratios):.3f}, "
                  f"mean prominence={np.mean(acf_proms):.2f}, "
                  f"mean lag_error={np.mean(acf_errs):.1f}f")
        else:
            print("Aggregate ACF Tempo Quality: no valid ACF metrics")

        # Save results
        with open(output_dir / "eval_results.json", "w") as f:
            json.dump(all_results, f, indent=2)


def _peak_pick(activations: np.ndarray, threshold: float,
               frame_rate: float, min_interval_s: float = 0.2) -> np.ndarray:
    """Simple peak-picking on activation signal."""
    min_frames = int(min_interval_s * frame_rate)
    peaks = []
    last_peak = -min_frames

    for i in range(1, len(activations) - 1):
        if (activations[i] > threshold and
                activations[i] >= activations[i - 1] and
                activations[i] >= activations[i + 1] and
                i - last_peak >= min_frames):
            peaks.append(i / frame_rate)
            last_peak = i

    return np.array(peaks)


def _plot_activation(activations: np.ndarray, ref_beats: np.ndarray,
                     est_beats: np.ndarray, frame_rate: float,
                     title: str, plot_dir: Path,
                     db_activations: np.ndarray = None):
    """Save activation plot with reference and estimated beats."""
    plot_dir.mkdir(parents=True, exist_ok=True)

    times = np.arange(len(activations)) / frame_rate
    fig, ax = plt.subplots(figsize=(14, 3))
    ax.plot(times, activations, "b-", linewidth=0.5, alpha=0.8, label="Beat")

    if db_activations is not None:
        ax.plot(times, db_activations, "m-", linewidth=0.5, alpha=0.6, label="Downbeat")

    for bt in ref_beats:
        ax.axvline(bt, color="green", alpha=0.3, linewidth=0.5)
    for bt in est_beats:
        ax.axvline(bt, color="red", alpha=0.3, linewidth=0.5, linestyle="--")

    ax.set_xlim(0, times[-1] if len(times) > 0 else 1)
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Activation")
    ax.set_title(title)
    ax.legend(loc="upper right", fontsize=8)

    fig.tight_layout()
    fig.savefig(plot_dir / f"{title}.png", dpi=100)
    plt.close(fig)


def sweep_thresholds(model_path: str, audio_dir: Path, cfg: dict,
                     output_dir: Path, device: torch.device = None):
    """Sweep detection thresholds and report best F1."""
    if device is None:
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]
    chunk_frames = cfg["training"]["chunk_frames"]

    model, has_downbeat, pool_factor = _load_model(model_path, cfg, device)
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    thresholds = np.arange(0.1, 0.95, 0.05)

    # Collect activations + ref beats for all tracks
    tracks = []
    for audio_path in sorted(f for f in audio_dir.rglob("*")
                              if f.suffix.lower() in {".mp3", ".wav", ".flac"}):
        label_path = audio_path.parent / f"{audio_path.stem}.beats.json"
        if not label_path.exists():
            continue

        audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)
        target_rms_db = cfg["audio"].get("target_rms_db", -35)
        rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)

        n_frames = mel.shape[0]
        activations = np.zeros(n_frames, dtype=np.float32)
        counts = np.zeros(n_frames, dtype=np.float32)
        stride = chunk_frames // 2
        mel_tensor = torch.from_numpy(mel).float().to(device)

        with torch.no_grad():
            for start in range(0, max(1, n_frames - chunk_frames + 1), stride):
                end = start + chunk_frames
                if end > n_frames:
                    chunk = torch.zeros(chunk_frames, mel.shape[1],
                                        device=device, dtype=torch.float32)
                    chunk[:n_frames - start] = mel_tensor[start:n_frames]
                else:
                    chunk = mel_tensor[start:end]
                pred = model(chunk.unsqueeze(0))[0]
                actual_len = min(chunk_frames, n_frames - start)

                if pool_factor > 1:
                    pred_np = np.repeat(pred.cpu().numpy(), pool_factor, axis=0)
                    activations[start:start + actual_len] += pred_np[:actual_len, 0]
                else:
                    activations[start:start + actual_len] += pred[:actual_len, 0].cpu().numpy()
                counts[start:start + actual_len] += 1

        activations /= np.maximum(counts, 1)

        with open(label_path) as f:
            labels = json.load(f)
        ref_beats = np.array([h["time"] for h in labels["hits"]
                              if h.get("expectTrigger", True)])
        tracks.append((audio_path.stem, activations, ref_beats))

    # Sweep thresholds
    print(f"\n{'Thresh':>8} {'Mean F1':>8} {'Median':>8} {'Min':>8} {'Max':>8} {'Est/Ref':>8}")
    best_t, best_f1 = 0.5, 0.0
    for thresh in thresholds:
        f1s = []
        ratios = []
        for name, act, ref in tracks:
            est = _peak_pick(act, thresh, frame_rate)
            if len(ref) > 0 and len(est) > 0:
                f1 = mir_eval.beat.f_measure(ref, est, f_measure_threshold=0.07)
            else:
                f1 = 0.0
            f1s.append(f1)
            ratios.append(len(est) / max(len(ref), 1))
        mean_f1 = np.mean(f1s)
        print(f"{thresh:>8.2f} {mean_f1:>8.3f} {np.median(f1s):>8.3f} "
              f"{np.min(f1s):>8.3f} {np.max(f1s):>8.3f} {np.mean(ratios):>7.2f}x")
        if mean_f1 > best_f1:
            best_f1 = mean_f1
            best_t = thresh

    print(f"\nBest threshold: {best_t:.2f} (F1={best_f1:.3f})")

    # Run full eval at best threshold
    print(f"\n--- Full evaluation at threshold={best_t:.2f} ---")
    evaluate_on_tracks(model_path, audio_dir, cfg, output_dir, best_t, device)


def evaluate_validation_set(model_path: str, cfg: dict, output_dir: Path,
                            device: torch.device = None):
    """Evaluate on the processed validation set (frame-level metrics)."""
    if device is None:
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    data_dir = Path(cfg["data"]["processed_dir"])
    X_val = np.load(data_dir / "X_val.npy")
    Y_val = np.load(data_dir / "Y_val.npy")

    model, has_downbeat, pool_factor = _load_model(model_path, cfg, device)

    # Batch predict (chunked to avoid GPU OOM on large val sets)
    batch_size = 4096
    Y_pred_parts = []
    with torch.no_grad():
        for start in range(0, len(X_val), batch_size):
            X_batch = torch.from_numpy(X_val[start:start + batch_size]).float().to(device)
            Y_pred_parts.append(model(X_batch).cpu().numpy())
    Y_pred_all = np.concatenate(Y_pred_parts, axis=0)

    # For pooled models, downsample labels to match output time dimension
    if pool_factor > 1:
        Y_val = Y_val[:, pool_factor - 1::pool_factor]
        Y_val = Y_val[:, :Y_pred_all.shape[1]]

    Y_pred_beat = Y_pred_all[:, :, 0]
    _print_frame_metrics("Beat", Y_pred_beat, Y_val)

    if has_downbeat:
        db_val_path = data_dir / "Y_db_val.npy"
        if db_val_path.exists():
            Y_db_val = np.load(db_val_path)
            if pool_factor > 1:
                Y_db_val = Y_db_val[:, pool_factor - 1::pool_factor]
                Y_db_val = Y_db_val[:, :Y_pred_all.shape[1]]
            Y_pred_db = Y_pred_all[:, :, 1]
            _print_frame_metrics("Downbeat", Y_pred_db, Y_db_val)
        else:
            print("\nNo Y_db_val.npy found — skipping downbeat frame metrics")


def _print_frame_metrics(label: str, Y_pred: np.ndarray, Y_ref: np.ndarray):
    """Print frame-level precision/recall/F1 at various thresholds."""
    print(f"\n{label} frame-level metrics:")
    print(f"{'Threshold':>10} {'Precision':>10} {'Recall':>10} {'F1':>10}")
    for thresh in [0.3, 0.4, 0.5, 0.6, 0.7]:
        pred_binary = (Y_pred > thresh).astype(float)
        ref_binary = (Y_ref > 0.5).astype(float)

        tp = np.sum(pred_binary * ref_binary)
        fp = np.sum(pred_binary * (1 - ref_binary))
        fn = np.sum((1 - pred_binary) * ref_binary)

        precision = tp / (tp + fp + 1e-10)
        recall = tp / (tp + fn + 1e-10)
        f1 = 2 * precision * recall / (precision + recall + 1e-10)
        print(f"{thresh:>10.1f} {precision:>10.3f} {recall:>10.3f} {f1:>10.3f}")


def main():
    parser = argparse.ArgumentParser(description="Evaluate beat activation model")
    parser.add_argument("--config", default="configs/default.yaml")
    parser.add_argument("--model", default="outputs/best_model.pt")
    parser.add_argument("--audio-dir", default=None, help="Evaluate on full tracks")
    parser.add_argument("--output-dir", default="outputs/eval")
    parser.add_argument("--threshold", type=float, default=0.5,
                        help="Peak-pick threshold (or 0 to sweep 0.1-0.9)")
    parser.add_argument("--sweep-thresholds", action="store_true",
                        help="Sweep thresholds 0.1-0.9 and report best")
    parser.add_argument("--device", default=None, help="Device: cuda, cpu, or auto")
    args = parser.parse_args()

    cfg = load_config(args.config)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.device is None or args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    if args.audio_dir:
        if args.sweep_thresholds:
            sweep_thresholds(args.model, Path(args.audio_dir), cfg, output_dir, device)
        else:
            evaluate_on_tracks(args.model, Path(args.audio_dir), cfg, output_dir,
                               args.threshold, device)
    else:
        evaluate_validation_set(args.model, cfg, output_dir, device)


if __name__ == "__main__":
    main()
