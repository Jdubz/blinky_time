#!/usr/bin/env python3
"""Evaluate onset activation model offline (PyTorch, GPU-accelerated).

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

from models.onset_cnn import build_onset_cnn
from scripts.audio import (
    append_delta_features,
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
    """Load a trained onset activation model (CNN, DS-TCN, or frame FC)."""
    checkpoint = torch.load(model_path, map_location=device, weights_only=True)

    # Handle both bare state_dict and full checkpoint
    if isinstance(checkpoint, dict) and "state_dict" in checkpoint:
        state_dict = checkpoint["state_dict"]
    else:
        state_dict = checkpoint

    model_type = cfg["model"].get("type", "causal_cnn")
    if model_type == "frame_fc":
        from models.onset_fc import build_onset_fc
        model = build_onset_fc(
            n_mels=cfg["audio"]["n_mels"],
            window_frames=cfg["model"]["window_frames"],
            hidden_dims=cfg["model"]["hidden_dims"],
            dropout=cfg["model"].get("dropout", 0.1),
        ).to(device)
    elif model_type == "frame_conv1d":
        from models.onset_conv1d import build_onset_conv1d
        use_delta = cfg.get("features", {}).get("use_delta", False)
        input_features = cfg["audio"]["n_mels"] * (2 if use_delta else 1)
        model = build_onset_conv1d(
            n_mels=input_features,
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            dropout=cfg["model"].get("dropout", 0.1),
            num_tempo_bins=cfg["model"].get("num_tempo_bins", 0),
            freq_pos_encoding=cfg["model"].get("freq_pos_encoding", False),
            num_output_channels=cfg["model"].get("num_output_channels", 0),
        ).to(device)
    elif model_type == "frame_conv1d_pool":
        from models.onset_conv1d_pool import build_onset_conv1d_pool
        model = build_onset_conv1d_pool(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_sizes=cfg["model"]["kernel_sizes"],
            pool_sizes=cfg["model"]["pool_sizes"],
            dropout=cfg["model"].get("dropout", 0.1),
            use_stride=cfg["model"].get("use_stride", False),
        ).to(device)
    else:
        model = build_onset_cnn(
            n_mels=cfg["audio"]["n_mels"],
            channels=cfg["model"]["channels"],
            kernel_size=cfg["model"]["kernel_size"],
            dilations=cfg["model"]["dilations"],
            dropout=cfg["model"].get("dropout", 0.1),
            model_type=model_type,
            residual=cfg["model"].get("residual", False),
        ).to(device)
    model.load_state_dict(state_dict)
    model.eval()
    pool_factor = getattr(model, 'pool_factor', 1)
    return model, pool_factor


def evaluate_on_tracks(model_path: str, audio_dir: Path, cfg: dict,
                       output_dir: Path, threshold: float = 0.5,
                       device: torch.device = None):
    """Run model on full tracks and evaluate beat detection accuracy."""
    if device is None:
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]
    chunk_frames = cfg["training"]["chunk_frames"]

    model, pool_factor = _load_model(model_path, cfg, device)
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

        # Append delta features if configured
        if cfg.get("features", {}).get("use_delta", False):
            mel = append_delta_features(mel)

        # Run model on overlapping chunks, average predictions
        n_frames = mel.shape[0]
        n_out_ch = model.out_channels
        # Accumulate all output channels
        all_activations = np.zeros((n_frames, n_out_ch), dtype=np.float32)
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
                    pred_np = pred.cpu().numpy()
                    pred_np = np.repeat(pred_np, pool_factor, axis=0)
                    actual_len = min(chunk_frames, n_frames - start)
                    pred_np = pred_np[:actual_len]
                else:
                    actual_len = min(chunk_frames, n_frames - start)
                    pred_np = pred[:actual_len].cpu().numpy()

                all_activations[start:start + actual_len] += pred_np
                counts[start:start + actual_len] += 1

        all_activations /= np.maximum(counts, 1)[:, np.newaxis]

        # Extract channel activations
        activations = all_activations[:, 0]  # ch0: onset (or kick for instrument models)
        is_instrument_model = n_out_ch >= 3

        # Load ground truth beats
        with open(label_path) as f:
            labels = json.load(f)
        ref_beats = np.array([h["time"] for h in labels["hits"] if h.get("expectTrigger", True)])

        # Peak-pick activations to get estimated onset times.
        # Named est_beats for legacy JSON compatibility, but these are onset
        # detections (the model outputs onset activation, not a beat grid).
        est_beats = _peak_pick(activations, threshold, frame_rate)

        # Onset F1 using mir_eval.onset (±50ms MIREX standard)
        if len(ref_beats) > 0 and len(est_beats) > 0:
            scores = mir_eval.onset.f_measure(ref_beats, est_beats, window=0.05)[0]
        else:
            scores = 0.0

        result = {
            "track": audio_path.stem,
            "ref_beats": len(ref_beats),
            "est_beats": len(est_beats),
            "f1": float(scores),
        }

        # Onset-level evaluation (if .onsets.json exists alongside .beats.json)
        onset_path = label_path.parent / f"{audio_path.stem}.onsets.json"
        if onset_path.exists():
            with open(onset_path) as f:
                onset_data = json.load(f)
            ref_onsets = np.array(onset_data["onsets"])
            if len(ref_onsets) > 0 and len(est_beats) > 0:
                onset_f1 = mir_eval.onset.f_measure(ref_onsets, est_beats, window=0.05)[0]
            else:
                onset_f1 = 0.0
            result["onset_f1"] = float(onset_f1)
            result["ref_onsets"] = len(ref_onsets)

        # Kick-weighted onset evaluation: overall F1 + per-instrument recall
        # Overall F1 against all annotated onsets captures both false positives
        # (detections where no onset exists) and missed onsets correctly.
        # Per-instrument recall shows selectivity (v5 should suppress hihats).
        kw_path = audio_path.parent / "kick_weighted" / f"{audio_path.stem}.kick_weighted.json"
        if kw_path.exists():
            with open(kw_path) as f:
                kw_data = json.load(f)
            # Overall F1: all annotated onsets vs model detections
            all_ref_onsets = np.array([o["time"] for o in kw_data["onsets"]])
            if len(all_ref_onsets) > 0 and len(est_beats) > 0:
                kw_f1 = mir_eval.onset.f_measure(
                    all_ref_onsets, est_beats, window=0.05)[0]
            else:
                kw_f1 = 0.0
            result["kw_onset_f1"] = float(kw_f1)
            result["kw_ref_onsets"] = len(all_ref_onsets)
            # Per-instrument recall breakdown (vectorized via searchsorted)
            est_sorted = np.sort(est_beats)
            for onset_type in ("kick", "snare", "hihat"):
                ref_typed = np.array([o["time"] for o in kw_data["onsets"]
                                      if o["type"] == onset_type])
                if len(ref_typed) > 0 and len(est_sorted) > 0:
                    # For each ref onset, find nearest detection via binary search
                    idx = np.searchsorted(est_sorted, ref_typed)
                    # Check distance to nearest neighbor on both sides
                    dists = np.full(len(ref_typed), np.inf)
                    valid_right = idx < len(est_sorted)
                    dists[valid_right] = np.abs(est_sorted[idx[valid_right]] - ref_typed[valid_right])
                    valid_left = idx > 0
                    left_dist = np.abs(est_sorted[idx[valid_left] - 1] - ref_typed[valid_left])
                    dists[valid_left] = np.minimum(dists[valid_left], left_dist)
                    typed_recall = float(np.mean(dists <= 0.05))
                else:
                    typed_recall = 0.0
                result[f"{onset_type}_recall"] = float(typed_recall)

        # Per-channel instrument evaluation (3-channel models: kick/snare/hihat)
        if is_instrument_model and kw_path.exists():
            channel_names = cfg["model"].get("output_channel_names", ["kick", "snare", "hihat"])
            assert len(channel_names) >= n_out_ch, (
                f"output_channel_names has {len(channel_names)} entries but model has {n_out_ch} outputs")
            for ch_idx, ch_name in enumerate(channel_names[:n_out_ch]):
                ch_act = all_activations[:, ch_idx]
                ch_est = _peak_pick(ch_act, threshold, frame_rate)
                # Evaluate this channel against its corresponding instrument type
                ref_typed = np.array([o["time"] for o in kw_data["onsets"]
                                      if o["type"] == ch_name])
                if len(ref_typed) > 0 and len(ch_est) > 0:
                    ch_f1 = mir_eval.onset.f_measure(
                        ref_typed, ch_est, window=0.05)[0]
                else:
                    ch_f1 = 0.0
                result[f"{ch_name}_ch_f1"] = float(ch_f1)
                result[f"{ch_name}_ch_est"] = len(ch_est)
                result[f"{ch_name}_ch_ref"] = len(ref_typed)

            # Combined onset: max(kick, snare) — what firmware would use
            combined_act = np.maximum(all_activations[:, 0], all_activations[:, 1])
            combined_est = _peak_pick(combined_act, threshold, frame_rate)
            # F1 against kick+snare onsets only (no hihats)
            ref_kick_snare = np.array([o["time"] for o in kw_data["onsets"]
                                       if o["type"] in ("kick", "snare")])
            if len(ref_kick_snare) > 0 and len(combined_est) > 0:
                combined_f1 = mir_eval.onset.f_measure(
                    ref_kick_snare, combined_est, window=0.05)[0]
            else:
                combined_f1 = 0.0
            result["combined_kick_snare_f1"] = float(combined_f1)
            result["combined_est"] = len(combined_est)

        # ACF-based ODF quality metrics
        acf_metrics = compute_acf_tempo_quality(activations, ref_beats, frame_rate)
        result["acf_peak_ratio"] = acf_metrics["acf_peak_ratio"]
        result["acf_peak_prominence"] = acf_metrics["acf_peak_prominence"]
        lag_err = acf_metrics["acf_lag_error"]
        result["acf_lag_error"] = lag_err if np.isfinite(lag_err) else None

        all_results.append(result)
        acf_err_str = f"{lag_err:.1f}f" if np.isfinite(lag_err) else "n/a"
        acf_str = f", ACF ratio={acf_metrics['acf_peak_ratio']:.3f} prom={acf_metrics['acf_peak_prominence']:.2f} err={acf_err_str}"
        onset_str = f", onsetF1={result['onset_f1']:.3f}" if "onset_f1" in result else ""
        kw_str = ""
        if "kw_onset_f1" in result:
            kw_str = (f", kwF1={result['kw_onset_f1']:.3f} "
                      f"kick={result['kick_recall']:.3f} snare={result['snare_recall']:.3f} "
                      f"hihat={result['hihat_recall']:.3f}")
        inst_str = ""
        if is_instrument_model and "kick_ch_f1" in result:
            inst_str = (f", ch: kick={result['kick_ch_f1']:.3f} snare={result['snare_ch_f1']:.3f} "
                        f"hihat={result['hihat_ch_f1']:.3f} combined={result['combined_kick_snare_f1']:.3f}")
        print(f"  {audio_path.stem}: F1={scores:.3f} (ref={len(ref_beats)}, est={len(est_beats)}){onset_str}{kw_str}{inst_str}{acf_str}")

        # Save activation plot
        _plot_activation(activations, ref_beats, est_beats, frame_rate,
                         audio_path.stem, output_dir / "plots")

    # Aggregate
    if all_results:
        onset_f1s = [r["onset_f1"] for r in all_results if "onset_f1" in r]
        if onset_f1s:
            print(f"\nAggregate Onset: mean F1={np.mean(onset_f1s):.3f}, median={np.median(onset_f1s):.3f}, "
                  f"min={np.min(onset_f1s):.3f}, max={np.max(onset_f1s):.3f}")
        else:
            # Fallback: no onset labels available, show beat-position F1 with caveat
            f1s = [r["f1"] for r in all_results]
            print(f"\nAggregate (vs beat labels, not onset): mean F1={np.mean(f1s):.3f}, median={np.median(f1s):.3f}, "
                  f"min={np.min(f1s):.3f}, max={np.max(f1s):.3f}")

        # Kick-weighted onset metrics
        kw_f1s = [r["kw_onset_f1"] for r in all_results if "kw_onset_f1" in r]
        if kw_f1s:
            print(f"Aggregate KW Onset F1: mean={np.mean(kw_f1s):.3f}, "
                  f"median={np.median(kw_f1s):.3f}, "
                  f"min={np.min(kw_f1s):.3f}, max={np.max(kw_f1s):.3f}")
        for onset_type in ("kick", "snare", "hihat"):
            key = f"{onset_type}_recall"
            typed_vals = [r[key] for r in all_results if key in r]
            if typed_vals:
                print(f"  {onset_type.capitalize():>6} recall: mean={np.mean(typed_vals):.3f}, "
                      f"median={np.median(typed_vals):.3f}, "
                      f"min={np.min(typed_vals):.3f}, max={np.max(typed_vals):.3f}")

        # Per-channel instrument F1 (3-channel models)
        for ch_name in ("kick", "snare", "hihat"):
            key = f"{ch_name}_ch_f1"
            ch_vals = [r[key] for r in all_results if key in r]
            if ch_vals:
                print(f"  {ch_name.capitalize():>6} channel F1: mean={np.mean(ch_vals):.3f}, "
                      f"median={np.median(ch_vals):.3f}, "
                      f"min={np.min(ch_vals):.3f}, max={np.max(ch_vals):.3f}")
        combined_vals = [r["combined_kick_snare_f1"] for r in all_results
                         if "combined_kick_snare_f1" in r]
        if combined_vals:
            print(f"Aggregate Combined (kick+snare) F1: mean={np.mean(combined_vals):.3f}, "
                  f"median={np.median(combined_vals):.3f}")

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
               frame_rate: float, min_interval_s: float = 0.05) -> np.ndarray:
    """Simple peak-picking on activation signal.

    Default min_interval_s=0.05 (50ms) is appropriate for onset detection.
    Firmware uses 40-150ms tempo-adaptive cooldown.
    """
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
                     title: str, plot_dir: Path):
    """Save activation plot with reference and estimated beats."""
    plot_dir.mkdir(parents=True, exist_ok=True)

    times = np.arange(len(activations)) / frame_rate
    fig, ax = plt.subplots(figsize=(14, 3))
    ax.plot(times, activations, "b-", linewidth=0.5, alpha=0.8, label="Onset")

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

    model, pool_factor = _load_model(model_path, cfg, device)
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    thresholds = np.arange(0.1, 0.95, 0.05)

    # Collect activations + ref onsets for all tracks
    # Use kick-weighted onset labels (preferred) or librosa onsets as sweep target.
    # Previous versions used .beats.json (beat F1) which created threshold artifacts
    # when comparing models with different activation distributions.
    tracks = []
    for audio_path in sorted(f for f in audio_dir.rglob("*")
                              if f.suffix.lower() in {".mp3", ".wav", ".flac"}):
        # Sweep only needs onset labels — .beats.json is NOT required here.
        # (The full eval at the end will skip tracks without .beats.json.)
        # Load onset reference: prefer kick-weighted, fall back to .onsets.json
        kw_path = audio_path.parent / "kick_weighted" / f"{audio_path.stem}.kick_weighted.json"
        onset_path = audio_path.parent / f"{audio_path.stem}.onsets.json"
        if kw_path.exists():
            with open(kw_path) as f:
                kw_data = json.load(f)
            ref_onsets = np.array([o["time"] for o in kw_data["onsets"]])
        elif onset_path.exists():
            with open(onset_path) as f:
                onset_data = json.load(f)
            ref_onsets = np.array(onset_data["onsets"])
        else:
            continue  # skip tracks without onset labels

        audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)
        target_rms_db = cfg["audio"].get("target_rms_db", -35)
        rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)

        if cfg.get("features", {}).get("use_delta", False):
            mel = append_delta_features(mel)

        n_frames = mel.shape[0]
        n_out_ch = model.out_channels
        is_inst = n_out_ch >= 3
        all_act = np.zeros((n_frames, n_out_ch), dtype=np.float32)
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
                    all_act[start:start + actual_len] += pred_np[:actual_len]
                else:
                    all_act[start:start + actual_len] += pred[:actual_len].cpu().numpy()
                counts[start:start + actual_len] += 1

        all_act /= np.maximum(counts, 1)[:, np.newaxis]
        # For instrument models, sweep on combined kick+snare; else use ch0
        if is_inst:
            activations = np.maximum(all_act[:, 0], all_act[:, 1])
        else:
            activations = all_act[:, 0]

        tracks.append((audio_path.stem, activations, ref_onsets))

    # Sweep thresholds against onset F1 using mir_eval.onset (MIREX standard).
    # Uses 50ms window (standard onset detection eval), NOT mir_eval.beat which
    # has different matching semantics and created threshold artifacts in v12/v13.
    print(f"\n{'Thresh':>8} {'Mean F1':>8} {'Median':>8} {'Min':>8} {'Max':>8} {'Est/Ref':>8}")
    best_t, best_f1 = 0.5, 0.0
    for thresh in thresholds:
        f1s = []
        ratios = []
        for name, act, ref in tracks:
            est = _peak_pick(act, thresh, frame_rate)
            if len(ref) > 0 and len(est) > 0:
                f1 = mir_eval.onset.f_measure(ref, est, window=0.05)[0]
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

    print(f"\nBest threshold: {best_t:.2f} (onset F1={best_f1:.3f})")

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

    model, pool_factor = _load_model(model_path, cfg, device)

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

    num_output_channels = cfg["model"].get("num_output_channels", 0)
    multichannel_targets = Y_val.ndim == 3 and Y_val.shape[2] > 1

    if multichannel_targets:
        # Per-channel frame metrics for instrument models
        channel_names = cfg["model"].get("output_channel_names",
                                         [f"ch{i}" for i in range(Y_val.shape[2])])
        for ch_idx in range(min(Y_pred_all.shape[2], Y_val.shape[2])):
            ch_name = channel_names[ch_idx] if ch_idx < len(channel_names) else f"ch{ch_idx}"
            _print_frame_metrics(ch_name.capitalize(), Y_pred_all[:, :, ch_idx], Y_val[:, :, ch_idx])
    else:
        Y_pred_beat = Y_pred_all[:, :, 0]
        _print_frame_metrics("Onset", Y_pred_beat, Y_val)


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
    parser = argparse.ArgumentParser(description="Evaluate onset activation model")
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
