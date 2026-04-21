"""Run the five-gate check on held-out validation data.

Gate definitions are documented in docs/HYBRID_FEATURE_ANALYSIS_PLAN.md
"Working principles". This script executes the cheap four (a already
passed by definition for features in the shortlist):

  (b) TP-vs-FP |d| on NN-firing moments — uses on-device transients +
      signal frames from a validation job. TP = transient within ±50 ms
      of a GT onset; FP = transient far from any GT onset. For each
      feature, take the value at the transient timestamp, compute |d|
      between the TP and FP populations.

  (c) R² regressing each feature against the 30 mel bands on the same
      audio. >= 0.95 ⇒ the feature carries no new information relative
      to mel. Requires the parity harness output for mel + feature
      values on a representative audio file.

  (d) pairwise |r| between features. >= 0.9 ⇒ the two features carry
      the same information; keep the cheaper one.

Inputs:
  --job-result    validation job JSON (held-out corpus preferred)
  --harness-csv   parity-harness output for feature/mel correlation
                  analysis (tracks need not match the job exactly —
                  this is about intrinsic feature properties, and any
                  EDM track gives the same correlation structure)

Outputs: a ranked table per gate, printed to stdout.
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import math
from collections import defaultdict
from pathlib import Path

import numpy as np

log = logging.getLogger("gate_check")

WINDOW_SEC = 0.050
SIGNAL_FIELDS = ["flatness", "raw_flux", "centroid", "crest", "rolloff", "hfc"]


def load_job_transients_and_signals(job_path: Path):
    """Extract per-(track, device) transients + signal frames + GT from job JSON.

    Returns a list of dicts with keys: track, device, transients, signal_frames,
    gt_onsets (seconds), audio_start (epoch ms).
    """
    d = json.loads(job_path.read_text())
    # blinky-server stores transients and signal_frames under each track run's
    # raw capture. The validation result summarises but doesn't expose the raw
    # per-frame values. Fall back to fetching if only summary is present.
    raw_runs = d["result"].get("raw_captures")
    if raw_runs is None:
        # Newer API may not keep raw captures in the final result. Read what's
        # there and stitch what we can.
        raw_runs = []
    return d, raw_runs


def tp_fp_from_summary(job_data: dict) -> dict[str, dict[str, list[float]]]:
    """Reconstruct TP/FP feature values per signal from the job summary.

    Each per-device score has a `signals.gaps` list in "frame" mode giving
    onset_mean / non_mean / gap / cohens_d. That's what we reported before,
    but we want the per-*firing*-moment split (TP vs FP) not the
    per-*frame* split (onset-frame vs non-onset-frame).

    The signal_frames in the TestData aren't preserved in the final job
    JSON, so this is a limitation — we can't reconstruct the exact
    transient→signal-value alignment from the summary alone.

    Instead, return the per-signal onset/non-onset population means +
    stds from the "frame" mode as a diagnostic. A true gate-(b) check
    needs raw signal_frames; mark this as APPROX and recommend the
    capture-with-raw-signal-frames pathway.
    """
    per_device_per_signal: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for row in job_data["result"]["results"]:
        for dev, score in row["scores"].items():
            for g in score.get("signals", {}).get("gaps", []):
                # "peak" mode: one sample per GT onset (peak in ±50 ms of
                #  each GT onset) vs non-onset frames. Approximates TP-like
                #  values.
                # "frame" mode: all frames in ±50 ms (onset) vs far frames.
                if g.get("mode") != "peak":
                    continue
                per_device_per_signal[g["signal"]]["onset_mean"].append(g["onsetMean"])
                per_device_per_signal[g["signal"]]["onset_std"].append(g["onsetStd"])
                per_device_per_signal[g["signal"]]["non_mean"].append(g["nonMean"])
                per_device_per_signal[g["signal"]]["non_std"].append(g["nonStd"])
                per_device_per_signal[g["signal"]]["d"].append(g["cohensD"])
    return per_device_per_signal


def _pooled_cohens_d(mean_a, std_a, mean_b, std_b):
    pooled = math.sqrt((std_a**2 + std_b**2) / 2.0) if (std_a or std_b) else 0.0
    if pooled < 1e-12:
        return 0.0
    return (mean_a - mean_b) / pooled


def gate_b_from_summary(job_data: dict) -> list[dict]:
    """Per-signal per-track-device |d| between GT-onset-peak and non-onset-frame.

    This is the BEST-CASE approximation of gate (b) reconstructible from
    job summary alone. It measures "at a drum-peak moment vs at a
    far-from-drum moment" — which conflates TP / FP because the sample
    size is per-onset (one peak) vs per-non-onset-frame (many frames).
    Direction and magnitude still inform gate readiness. For the strict
    TP-vs-FP measurement, raw signal_frames + transients are required.
    """
    out = []
    per = tp_fp_from_summary(job_data)
    for sig in SIGNAL_FIELDS:
        entries = per.get(sig, {})
        if not entries:
            continue
        ds = entries["d"]
        out.append(
            {
                "signal": sig,
                "mean_abs_d": float(np.mean(np.abs(ds))),
                "mean_d": float(np.mean(ds)),
                "n": len(ds),
                "sign_positive": int(sum(1 for x in ds if x > 0)),
            }
        )
    return sorted(out, key=lambda x: -x["mean_abs_d"])


def load_harness_csv(csv_path: Path) -> tuple[dict[str, np.ndarray], np.ndarray]:
    """Return (signal_values, mel_bands) from a parity-harness CSV."""
    with csv_path.open() as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [[float(x) for x in r] for r in reader]
    data = np.asarray(rows, dtype=np.float32)
    name_to_col = {n: i for i, n in enumerate(header)}
    signals = {s: data[:, name_to_col[s]] for s in SIGNAL_FIELDS if s in name_to_col}
    mel_cols = sorted(
        [c for c in header if c.startswith("mel")], key=lambda x: int(x[3:])
    )
    mel = data[:, [name_to_col[c] for c in mel_cols]]
    return signals, mel


def gate_c_r2(signals: dict[str, np.ndarray], mel: np.ndarray) -> list[dict]:
    """Linear regression per-feature against mel bands. Return R² per feature."""
    results = []
    mel_aug = np.hstack([mel, np.ones((mel.shape[0], 1), dtype=mel.dtype)])  # bias
    for name, y in signals.items():
        # lstsq for numerical stability on potentially-rank-deficient mel.
        coef, *_ = np.linalg.lstsq(mel_aug, y, rcond=None)
        y_hat = mel_aug @ coef
        ss_res = float(np.sum((y - y_hat) ** 2))
        ss_tot = float(np.sum((y - y.mean()) ** 2))
        r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-12 else 0.0
        results.append({"signal": name, "r2_vs_mel": r2})
    return sorted(results, key=lambda x: x["r2_vs_mel"])


def gate_d_pairwise(signals: dict[str, np.ndarray]) -> list[dict]:
    names = list(signals.keys())
    n = len(names)
    mat = np.zeros((n, n), dtype=np.float64)
    for i in range(n):
        for j in range(n):
            x, y = signals[names[i]], signals[names[j]]
            if x.std() < 1e-12 or y.std() < 1e-12:
                mat[i, j] = 0.0
            else:
                mat[i, j] = float(np.corrcoef(x, y)[0, 1])
    return {"names": names, "matrix": mat.tolist()}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-result", type=Path, required=True)
    parser.add_argument("--harness-csv", type=Path, required=True,
                        help="parity harness output (e.g. outputs/parity/<track>.harness.csv)")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)
    logging.basicConfig(level=getattr(logging, args.log_level.upper()),
                        format="%(asctime)s %(levelname)s %(message)s")

    # ---- Gate (b): TP-vs-FP approximation from job summary ----
    job = json.loads(args.job_result.read_text())
    b_rows = gate_b_from_summary(job)
    print("\n=== Gate (b) — peak-mode |d| per signal (proxy for TP-vs-FP) ===")
    print(f"{'signal':12s} {'|d| μ':>7s} {'d μ':>7s} {'sign+':>8s} {'n':>4s}")
    print("-" * 45)
    for r in b_rows:
        print(f"{r['signal']:12s} {r['mean_abs_d']:>7.3f} {r['mean_d']:>+7.3f} "
              f"{r['sign_positive']:>4d}/{r['n']:<3d} {r['n']:>4d}")
    print("\nnote: summary JSON doesn't preserve raw signal_frames, so this is\n"
          "the peak-mode onset-vs-non-onset proxy, not true TP-vs-FP. The real\n"
          "gate (b) needs raw signal_frames + transient timestamps. Acceptable\n"
          "as a first filter.")

    # ---- Gates (c) and (d) from parity-harness CSV ----
    signals, mel = load_harness_csv(args.harness_csv)
    n_frames, n_mel = mel.shape
    log.info("Loaded %d frames, %d mel bands, %d signals from %s",
             n_frames, n_mel, len(signals), args.harness_csv)

    c_rows = gate_c_r2(signals, mel)
    print("\n=== Gate (c) — R² predicting each feature from mel bands ===")
    print(f"{'signal':12s} {'R²':>7s}  verdict")
    print("-" * 40)
    for r in c_rows:
        verdict = "passes (non-redundant)" if r["r2_vs_mel"] < 0.95 else "REDUNDANT with mel"
        print(f"{r['signal']:12s} {r['r2_vs_mel']:>7.3f}  {verdict}")

    d_res = gate_d_pairwise(signals)
    print("\n=== Gate (d) — pairwise |r| between features ===")
    names = d_res["names"]
    mat = np.asarray(d_res["matrix"])
    print(f"{'':12s}" + "".join(f"{n[:8]:>9s}" for n in names))
    for i, n in enumerate(names):
        row = [f"{mat[i, j]:>+9.3f}" for j in range(len(names))]
        print(f"{n:12s}" + "".join(row))
    print("\n|r| ≥ 0.9 marks a redundant pair — drop the more expensive one.\n"
          "(Cost estimates in plan: centroid ~0.2 ms, crest ~0.3 ms, "
          "flatness ~0.3 ms, hfc ~0.2 ms, raw_flux ~0.2 ms, rolloff ~0.3 ms).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
