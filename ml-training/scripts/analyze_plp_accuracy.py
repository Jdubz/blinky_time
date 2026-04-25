"""Analyze on-device PLP signal quality vs ground-truth beats.

Reads a validation job result with persist_raw=true and per-frame signal
captures, correlates the PLP pulse value (`plpPulse`) and confidence
(`plpConfidence`) with the .beats.json ground truth, and reports whether
the PLP-based AND-gate (b142+, default beatGridPatternMin=0.4) is
trustworthy.

Quantitative outputs:
  - PLP pulse @ GT-beat frames vs random-frame baseline (signal ratio)
  - plpConfidence histogram + mean over the 35 s capture
  - Fraction of GT beats where plpPulseValue >= 0.4 (= AND-gate would pass)
  - Fraction where plpPulseValue >= 0.4 BUT not at a GT beat (= gate FP suppression)

Usage:
    python scripts/analyze_plp_accuracy.py <validation_result.json> [<beats_dir>]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: analyze_plp_accuracy.py <validation_result.json> [<beats_dir>]")
        return 1

    result_path = Path(sys.argv[1])
    beats_dir = (
        Path(sys.argv[2]) if len(sys.argv) >= 3
        else Path(__file__).resolve().parents[2] / "blinky-test-player/music/edm_holdout"
    )

    job = json.loads(result_path.read_text())
    results = job.get("result", {}).get("results", [])
    if not results:
        print("No results in job — was persist_raw=true?", file=sys.stderr)
        return 1

    print(f"Analyzing {len(results)} (track, run) pairs from {result_path}")
    print(f"GT labels from {beats_dir}\n")

    per_track_stats = []
    rng = np.random.default_rng(42)

    for r in results:
        track = r["track"]
        beats_json = beats_dir / f"{track}.beats.json"
        if not beats_json.exists():
            print(f"  {track}: no .beats.json, skipping")
            continue
        gt = json.loads(beats_json.read_text())
        gt_times = sorted(
            h["time"] for h in gt.get("hits", []) if h.get("expectTrigger", True)
        )
        if not gt_times:
            print(f"  {track}: empty GT, skipping")
            continue

        # All devices in this run for this track. We expect 1 in our usage.
        for dev_id, sc in r["scores"].items():
            sf = sc.get("signals", {}).get("frames", [])
            if not sf:
                print(f"  {track}/{dev_id}: no signal_frames captured (was persist_raw=true?)")
                continue

            # Build per-frame arrays. Frame timestamps are in ms.
            times_ms = np.array([f["timestamp_ms"] for f in sf], dtype=np.float64)
            # Many keys live under values; PLP fields may be top-level or nested.
            # Try a few common locations.
            def get_val(frame, *names):
                for n in names:
                    v = frame.get(n)
                    if v is not None and isinstance(v, int | float):
                        return float(v)
                    vv = frame.get("values", {}).get(n)
                    if vv is not None:
                        return float(vv)
                return float("nan")

            plp_pulse = np.array(
                [get_val(f, "plpPulse", "plp_pulse", "pp") for f in sf]
            )
            plp_conf = np.array(
                [get_val(f, "plpConfidence", "plp_confidence", "conf") for f in sf]
            )
            activations = np.array([f.get("activation", float("nan")) for f in sf])

            # Map GT times to frame indices (nearest).
            track_dur_s = times_ms[-1] / 1000.0 if len(times_ms) else 35
            gt_in_window = [t for t in gt_times if t <= track_dur_s]
            if not gt_in_window:
                continue

            gt_frame_idx = np.searchsorted(times_ms / 1000.0, gt_in_window)
            gt_frame_idx = gt_frame_idx[gt_frame_idx < len(plp_pulse)]

            # Random non-GT baseline: sample 200 random frame indices not in
            # ±2-frame neighborhood of any GT.
            gt_neighborhood = set()
            for f in gt_frame_idx:
                for d in range(-2, 3):
                    if 0 <= f + d < len(plp_pulse):
                        gt_neighborhood.add(f + d)
            non_gt = [i for i in range(len(plp_pulse)) if i not in gt_neighborhood]
            if len(non_gt) < 50:
                continue
            random_idx = rng.choice(non_gt, size=min(200, len(non_gt)), replace=False)

            valid_pulse = ~np.isnan(plp_pulse)
            if valid_pulse.sum() == 0:
                print(f"  {track}/{dev_id}: no valid plpPulse signal (firmware not emitting?)")
                continue

            pulse_at_gt = plp_pulse[gt_frame_idx]
            pulse_at_random = plp_pulse[random_idx]
            ratio = float(np.nanmean(pulse_at_gt) / max(np.nanmean(pulse_at_random), 1e-9))

            stats = {
                "track": track,
                "dev_id": dev_id,
                "n_frames": len(sf),
                "n_gt_in_window": int(len(gt_frame_idx)),
                "plp_pulse_mean_at_gt": float(np.nanmean(pulse_at_gt)),
                "plp_pulse_mean_random": float(np.nanmean(pulse_at_random)),
                "plp_pulse_signal_ratio": ratio,
                "frac_gt_above_0_4": float(np.nanmean(pulse_at_gt >= 0.4)),
                "frac_random_above_0_4": float(np.nanmean(pulse_at_random >= 0.4)),
                "plp_conf_mean": float(np.nanmean(plp_conf)) if not np.isnan(plp_conf).all() else None,
                "plp_conf_above_0_2_frac": (
                    float(np.nanmean(plp_conf >= 0.2)) if not np.isnan(plp_conf).all() else None
                ),
                "activation_mean": float(np.nanmean(activations)),
            }
            per_track_stats.append(stats)

            print(f"  {track}/{dev_id[:8]}: "
                  f"frames={stats['n_frames']:3d} "
                  f"gt={stats['n_gt_in_window']:3d} "
                  f"plp@gt={stats['plp_pulse_mean_at_gt']:.3f} "
                  f"plp@rand={stats['plp_pulse_mean_random']:.3f} "
                  f"ratio={ratio:.2f}x "
                  f"frac_gt>=0.4={stats['frac_gt_above_0_4']:.2f} "
                  f"frac_rand>=0.4={stats['frac_random_above_0_4']:.2f} "
                  f"conf_mean={stats['plp_conf_mean'] or '-'}")

    if not per_track_stats:
        print("No tracks analyzed.", file=sys.stderr)
        return 1

    print()
    print("=== Aggregate ===")
    sr = [s["plp_pulse_signal_ratio"] for s in per_track_stats]
    f_gt = [s["frac_gt_above_0_4"] for s in per_track_stats]
    f_rand = [s["frac_random_above_0_4"] for s in per_track_stats]
    confs = [s["plp_conf_mean"] for s in per_track_stats if s["plp_conf_mean"] is not None]

    print(f"PLP pulse @ GT vs random:    mean ratio = {np.mean(sr):.2f}x  "
          f"(min {min(sr):.2f}, max {max(sr):.2f}, n={len(sr)})")
    print(f"  fraction of GT beats with plpPulse >= 0.4 (gate would PASS): {np.mean(f_gt):.3f}")
    print(f"  fraction of random frames with plpPulse >= 0.4 (gate would PASS where no GT): {np.mean(f_rand):.3f}")
    if confs:
        print(f"  plpConfidence mean across tracks: {np.mean(confs):.3f}")

    print()
    print("Interpretation:")
    print(f"  - If GT-vs-random ratio >> 1.0 → PLP correlates with beats (gate is principled)")
    print(f"  - If GT pass-rate at threshold 0.4 is low (< 0.6) → gate suppresses many TRUE onsets")
    print(f"  - If random pass-rate is low (< 0.2) → gate is selective when fired (precision-positive)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
