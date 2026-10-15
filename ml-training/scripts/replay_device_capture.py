#!/usr/bin/env python3
"""Replay captured device mel data through offline model for parity analysis.

Loads JSONL from POST /api/test/capture-nn/{device_id} (blinky-server),
extracts the 26-band mel arrays the device NN actually saw, runs the same
PyTorch model offline, and compares onset predictions.

This isolates the sim-to-real gap:
  - If offline model on device mel also scores ~0.47 → gap is mel domain shift
  - If offline model on device mel scores ~0.75 → gap is quantization/inference

Usage:
    python scripts/replay_device_capture.py \\
        --capture capture.jsonl \\
        --model outputs/v16-no-delta/best_model.pt \\
        --config configs/conv1d_w16_onset_v16.yaml

    # With ground truth onset labels for F1 scoring:
    python scripts/replay_device_capture.py \\
        --capture capture.jsonl \\
        --model outputs/v16-no-delta/best_model.pt \\
        --config configs/conv1d_w16_onset_v16.yaml \\
        --labels ../blinky-test-player/music/edm/techno-minimal-01.onsets_consensus.json
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from scripts.audio import load_config
from evaluate import load_model


SPECTRAL_FPS = 62.5  # firmware spectral frame rate (16 kHz / 256-sample hop)
# Architectures whose output at frame t uses only frames <= t (frame_fc pads
# window-1 zero frames on the left; frame_conv1d uses causal convolutions), so
# overlapping-window averaging adds no look-ahead. Anything else is masked
# conservatively on both sides.
CAUSAL_MODEL_TYPES = {"frame_fc", "frame_conv1d"}


def load_capture(jsonl_path: str) -> dict:
    """Parse JSONL capture file from blinky-server capture-nn endpoint."""
    frames = []
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            frame = json.loads(line)
            if frame.get("type") == "NN":
                frames.append(frame)

    if not frames:
        print("ERROR: No NN frames found in capture file", file=sys.stderr)
        sys.exit(1)

    mel = np.array([f["mel"] for f in frames], dtype=np.float32)
    # Prefer raw NN activation (nna) over gated pulse (onset) when available.
    # Post-b107 firmware: onset=gated pulse (mostly zero), nna=raw continuous activation.
    # Pre-b107 captures: onset=raw continuous activation (old meaning), nna absent.
    has_nna = "nna" in frames[0]
    if not has_nna:
        print("  WARNING: No 'nna' field — using 'onset'.", file=sys.stderr)
        print("  In post-b107 captures 'onset' may be gated; in pre-b107 captures it may be raw.",
              file=sys.stderr)
        print("  Post-b107 captures should have 'nna', and parity metrics may be misleading when",
              file=sys.stderr)
        print("  falling back to gated 'onset'.", file=sys.stderr)
    if has_nna:
        device_onset = np.array([f["nna"] for f in frames], dtype=np.float32)
    else:
        device_onset = np.array([f["onset"] for f in frames], dtype=np.float32)
    timestamps = np.array([f["ts"] for f in frames], dtype=np.float64)
    nn_active = all(f.get("nn", 0) == 1 for f in frames)

    # Frame counter (b2xx+, #170): the device emits one NN line per loop update,
    # so frames processed in a catch-up update are missing. "fc" exposes them.
    has_fc = any("fc" in f for f in frames)
    if has_fc:
        if not all("fc" in f for f in frames):
            print("ERROR: some NN lines lack 'fc' (mixed firmware in one capture?)", file=sys.stderr)
            sys.exit(1)
        fc = np.array([f["fc"] for f in frames], dtype=np.int64)
        steps = np.diff(fc)
        if np.any(steps <= 0):
            print(f"ERROR: frame counter not increasing at {int(np.sum(steps <= 0))} line(s) "
                  "(device reboot or merged captures?)", file=sys.stderr)
            sys.exit(1)
        missing = int(np.sum(steps - 1))
        n_missing = missing
        total = int(fc[-1] - fc[0] + 1)
        if missing:
            print(f"  WARNING: {missing}/{total} frames ({100.0 * missing / total:.1f}%) missing "
                  f"across {int(np.sum(steps > 1))} gap(s); parity uses gap-free windows only.",
                  file=sys.stderr)
        # Mic-ring overruns (b2xx+): frames still count up by one, but audio was
        # lost, so the timeline drifts from wall clock. Needed for --labels.
        if "ovr" in frames[0]:
            ovr = np.array([f["ovr"] for f in frames], dtype=np.int64)
            n_overruns = int(ovr[-1] - ovr[0])
            if n_overruns:
                print(f"  WARNING: {n_overruns} mic-ring overrun(s) during the capture; "
                      "audio timeline is not continuous.", file=sys.stderr)
        else:
            n_overruns = None
    else:
        fc = None
        n_missing = None
        n_overruns = None
        print("  WARNING: no 'fc' field (pre-#170 capture): dropped frames cannot be detected; "
              "windows spanning a gap will corrupt parity silently.", file=sys.stderr)

    # Compute actual frame rate from timestamps
    if len(timestamps) > 1:
        dt_ms = np.median(np.diff(timestamps))
        actual_fps = 1000.0 / dt_ms
    else:
        actual_fps = 62.5

    return {
        "mel": mel,
        "device_onset": device_onset,
        "timestamps": timestamps,
        "nn_active": nn_active,
        "actual_fps": actual_fps,
        "n_frames": len(frames),
        "onset_field": "nna" if has_nna else "onset",
        "fc": fc,
        "n_missing": n_missing,  # None = unknown (pre-#170 capture)
        "n_overruns": n_overruns,  # None = unknown (capture predates the ovr field)
    }


def gap_free_mask(fc: np.ndarray | None, before: int, after: int) -> np.ndarray | None:
    """True where lines [i-before, i+after] all exist and are consecutive frames.

    Offline sliding_window_inference averages 50%-overlapping chunks. For a
    causal model (CAUSAL_MODEL_TYPES) output i still depends only on the
    window-1 lines before it, so after=0. For any other architecture output i
    can depend on lines up to window-1 AFTER i, so after=window-1.
    Delta/band-flux features need one more line before.
    """
    if fc is None:
        return None
    n = len(fc)
    # gap[j] = True when line j does not follow line j-1 by exactly one frame.
    gap = np.concatenate([[False], np.diff(fc) != 1]).astype(np.int64)
    csum = np.concatenate([[0], np.cumsum(gap)])  # csum[k] = gaps among lines < k
    idx = np.arange(n)
    lo, hi = idx - before, idx + after
    inside = (lo >= 0) & (hi <= n - 1)
    lo_c, hi_c = np.clip(lo, 0, n - 1), np.clip(hi, 0, n - 1)
    # Gaps strictly inside (lo, hi]: a gap at line j breaks the pair (j-1, j).
    gaps_in = csum[hi_c + 1] - csum[lo_c + 1]
    return inside & (gaps_in == 0)


def append_features_from_cfg(mel: np.ndarray, cfg: dict) -> np.ndarray:
    """Append delta or band-flux features based on config."""
    from scripts.features import append_features
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    return append_features(mel, use_delta=use_delta, use_band_flux=use_band_flux)


def run_inference(mel_features: np.ndarray, model, cfg: dict,
                  device: torch.device) -> np.ndarray:
    """Run offline model inference on captured mel data with sliding window."""
    from scripts.features import sliding_window_inference
    window_frames = cfg["model"]["window_frames"]
    return sliding_window_inference(mel_features, model, window_frames, device)


def peak_pick(activations: np.ndarray, threshold: float,
              frame_rate: float, min_interval_s: float = 0.05,
              times: np.ndarray | None = None,
              fc: np.ndarray | None = None,
              valid: np.ndarray | None = None) -> np.ndarray:
    """Simple peak-picking (same as evaluate.py).

    times: per-line time in seconds (from the frame counter) when the capture
    has gaps; otherwise line index / frame_rate.
    fc: frame counter; a line whose neighbour is across a gap can't be a peak
    (its neighbours aren't adjacent frames).
    valid: optional mask; a peak needs the line and both neighbours valid
    (the neighbours feed the local-max test).
    """
    if times is None:
        times = np.arange(len(activations)) / frame_rate
    # Same quantization as evaluate.py's frame-count check (int(0.05*62.5) = 3
    # frames = 48 ms), expressed in seconds.
    min_gap_s = int(min_interval_s * frame_rate) / frame_rate
    peaks = []
    last_t = -np.inf
    for i in range(1, len(activations) - 1):
        # Spacing is measured in time, so a dropped frame between two peaks
        # can't make them look closer than they are.
        if fc is not None and (fc[i] - fc[i - 1] != 1 or fc[i + 1] - fc[i] != 1):
            continue
        if valid is not None and not (valid[i - 1] and valid[i] and valid[i + 1]):
            continue
        if (activations[i] > threshold and
                activations[i] >= activations[i - 1] and
                activations[i] >= activations[i + 1] and
                times[i] - last_t >= min_gap_s - 1e-9):
            peaks.append(times[i])
            last_t = times[i]
    return np.array(peaks)


def main():
    parser = argparse.ArgumentParser(description="Replay device mel capture through offline model")
    parser.add_argument("--capture", required=True, help="JSONL capture file from capture-nn endpoint")
    parser.add_argument("--model", required=True, help="Trained model checkpoint (.pt)")
    parser.add_argument("--config", required=True, help="Model config YAML")
    parser.add_argument("--labels", default=None,
                        help="Ground-truth onsets (.onsets_consensus.json). Beat files are "
                             "refused: the task is onset detection (CLAUDE.md).")
    parser.add_argument("--threshold", type=float, default=0.5, help="Onset detection threshold")
    parser.add_argument("--device", default="cpu", help="Torch device (cpu/cuda)")
    args = parser.parse_args()

    cfg = load_config(args.config)
    device = torch.device(args.device)

    # Load capture
    print(f"Loading capture: {args.capture}")
    capture = load_capture(args.capture)
    print(f"  Frames: {capture['n_frames']}, FPS: {capture['actual_fps']:.1f}, "
          f"Duration: {capture['n_frames'] / capture['actual_fps']:.1f}s, "
          f"NN active: {capture['nn_active']}, "
          f"onset field: {capture['onset_field']}")

    # Mel distribution summary
    mel = capture["mel"]
    print(f"\n--- Device Mel Distribution ---")
    print(f"  Mean: {mel.mean():.4f}  Std: {mel.std():.4f}")
    print(f"  Min:  {mel.min():.4f}  Max: {mel.max():.4f}")
    print(f"  Per-band mean range: [{mel.mean(axis=0).min():.4f}, {mel.mean(axis=0).max():.4f}]")

    # Append features (delta / band-flux)
    use_delta = cfg.get("features", {}).get("use_delta", False)
    use_band_flux = cfg.get("features", {}).get("use_band_flux", False)
    mel_features = append_features_from_cfg(mel, cfg)
    print(f"  Features per frame: {mel_features.shape[1]} "
          f"({'mel+delta' if use_delta else 'mel+flux' if use_band_flux else 'mel only'})")

    # Load model
    print(f"\nLoading model: {args.model}")
    model, pool_factor = load_model(args.model, cfg, device)
    if pool_factor != 1:
        print(f"  WARNING: pool_factor={pool_factor}, replay may not match firmware")

    # Run inference
    print("Running offline inference on captured mel data...")
    offline_onset = run_inference(mel_features, model, cfg, device)

    # Compare device vs offline activations, only where the model window is
    # gap-free (offline windows spanning a dropped frame see different input).
    device_onset = capture["device_onset"]
    window = cfg["model"]["window_frames"]
    extra = 1 if (use_delta or use_band_flux) else 0
    causal = cfg["model"].get("type", "causal_cnn") in CAUSAL_MODEL_TYPES
    mask = gap_free_mask(capture["fc"], before=window - 1 + extra,
                         after=0 if causal else window - 1)
    if mask is not None:
        if not mask.any():
            print("ERROR: no gap-free model windows in this capture", file=sys.stderr)
            sys.exit(1)
        print(f"  Parity frames (gap-free windows): {int(mask.sum())}/{len(mask)}")
        dev_cmp, off_cmp = device_onset[mask], offline_onset[mask]
    else:
        dev_cmp, off_cmp = device_onset, offline_onset
    correlation = np.corrcoef(dev_cmp, off_cmp)[0, 1]
    mae = np.mean(np.abs(dev_cmp - off_cmp))
    print(f"\n--- Parity Analysis ---")
    print(f"  Activation correlation: {correlation:.4f}")
    print(f"  Activation MAE:         {mae:.4f}")
    print(f"  Device mean activation: {dev_cmp.mean():.4f}")
    print(f"  Offline mean activation:{off_cmp.mean():.4f}")

    # Peak-pick both
    # With fc, both peak times and the minimum spacing use the 62.5 Hz spectral
    # clock; the measured line rate (actual_fps) is only right for legacy
    # captures, where line index is the only clock.
    frame_rate = SPECTRAL_FPS if capture["fc"] is not None else capture["actual_fps"]
    # With a frame counter, time each line by its frame index at the nominal
    # 62.5 Hz frame rate, so dropped frames don't shift later peaks.
    times = (capture["fc"] - capture["fc"][0]) / SPECTRAL_FPS if capture["fc"] is not None else None
    device_peaks = peak_pick(device_onset, args.threshold, frame_rate, times=times,
                             fc=capture["fc"], valid=mask)
    offline_peaks = peak_pick(offline_onset, args.threshold, frame_rate, times=times,
                              fc=capture["fc"], valid=mask)
    print(f"  Device onset count:     {len(device_peaks)}")
    print(f"  Offline onset count:    {len(offline_peaks)}")

    # Score against ground truth if provided. Not with dropped frames: offline
    # predictions near a gap saw different input than the device did, so F1
    # differences would mix capture loss with model differences.
    if args.labels and capture["n_missing"]:
        print(f"ERROR: --labels scoring refused: capture is missing {capture['n_missing']} "
              "frame(s). Re-capture without catch-up frames (or score a gap-free span).",
              file=sys.stderr)
        sys.exit(1)
    if args.labels and capture["n_overruns"]:
        print(f"ERROR: --labels scoring refused: {capture['n_overruns']} mic-ring overrun(s) "
              "during the capture, so frame times no longer match the labels' timeline.",
              file=sys.stderr)
        sys.exit(1)
    if args.labels:
        print(f"\n--- Ground Truth Scoring ---")
        with open(args.labels, encoding="utf-8") as f:
            label_data = json.load(f)
        # Onset detection, not beat detection (CLAUDE.md): only onset ground
        # truth is valid here. onsets_consensus stores {"time", "strength"}.
        if "onsets" not in label_data:
            print(f"ERROR: {args.labels} has no 'onsets' list (keys: {sorted(label_data)}). "
                  "Use the track's .onsets_consensus.json; beat files are not onset ground truth.",
                  file=sys.stderr)
            sys.exit(1)
        ref_times = np.array([o["time"] if isinstance(o, dict) else o
                              for o in label_data["onsets"]], dtype=np.float64)
        # Score only where a peak could have been detected: the mask excludes
        # lines without full model context (capture edges), and an onset there
        # would count as a miss for both arms.
        if mask is not None:
            idx = np.arange(len(mask))
            eligible = idx[1:-1][mask[:-2] & mask[1:-1] & mask[2:]]
            if len(eligible) == 0:
                print("ERROR: no lines eligible for peak detection", file=sys.stderr)
                sys.exit(1)
            t_lo, t_hi = times[eligible[0]], times[eligible[-1]]
            n_all = len(ref_times)
            ref_times = ref_times[(ref_times >= t_lo) & (ref_times <= t_hi)]
            print(f"  Reference onsets in the scorable span "
                  f"[{t_lo:.2f}, {t_hi:.2f}] s: {len(ref_times)}/{n_all}")
        if len(ref_times) == 0:
            print("  No reference onsets found in labels file")
        else:
            import mir_eval
            # Score device detections
            if len(device_peaks) > 0:
                d_f1, d_p, d_r = mir_eval.onset.f_measure(ref_times, device_peaks, window=0.05)
            else:
                d_f1, d_p, d_r = 0.0, 0.0, 0.0
            # Score offline detections
            if len(offline_peaks) > 0:
                o_f1, o_p, o_r = mir_eval.onset.f_measure(ref_times, offline_peaks, window=0.05)
            else:
                o_f1, o_p, o_r = 0.0, 0.0, 0.0
            print(f"  {'':20s} {'F1':>8s} {'Prec':>8s} {'Recall':>8s}")
            print(f"  {'Device (firmware)':20s} {d_f1:>8.3f} {d_p:>8.3f} {d_r:>8.3f}")
            print(f"  {'Offline (PyTorch)':20s} {o_f1:>8.3f} {o_p:>8.3f} {o_r:>8.3f}")
            gap = o_f1 - d_f1
            print(f"  Gap (offline - device): {gap:+.3f}")
            if abs(gap) < 0.03:
                print("  → Gap is small — mel domain shift is NOT the primary cause")
                print("    Investigate: quantization, threshold, cooldown differences")
            elif gap > 0.1:
                print("  → Large gap — mel domain shift IS the primary cause")
                print("    PCEN normalization should help")

    print("\nDone.")


if __name__ == "__main__":
    main()
