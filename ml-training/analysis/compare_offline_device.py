"""Compare offline Python |d| vs on-device |d| per signal per track.

Takes a validation job result JSON (from the server's `/api/test/jobs/{id}`)
and the offline cross-corpus ranking JSON produced by `run_catalog.py`, and
prints a side-by-side table showing where the on-device discrimination
matches the offline prediction and where it falls apart (sim-to-real gap).

Usage:
    ./venv/bin/python -m analysis.compare_offline_device \
        --job-result outputs/validation_bXXX.json \
        --offline outputs/feature_catalog_clean/ranking.json
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-result", type=Path, required=True)
    parser.add_argument("--offline", type=Path, required=True,
                        help="Path to offline ranking.json (cross_corpus section read)")
    args = parser.parse_args()

    job = json.loads(args.job_result.read_text())
    offline = json.loads(args.offline.read_text())

    # Offline cross-corpus d by signal. Map from our Python names to the
    # server's wire/signal names (both sides currently use the same set,
    # but keep the map explicit for future divergence).
    offline_by_name = {r["feature"]: r for r in offline["cross_corpus"]}

    # Collect per-device-per-track cohens_d for each signal from the job.
    results = job.get("result", {}).get("results", [])
    per_signal: dict[str, list[float]] = {}
    n_tracks = len(results)
    for row in results:
        for dev_id, score in row.get("scores", {}).items():
            for g in score.get("signals", {}).get("gaps", []):
                per_signal.setdefault(g["signal"], []).append(g["cohensD"])

    print(f"{'signal':10s} {'offline d':>10s} {'dev d μ':>10s} {'dev d σ':>10s} {'n':>5s}  verdict")
    print("-" * 70)
    for name in sorted(offline_by_name, key=lambda n: -abs(offline_by_name[n]["cohens_d"])):
        off_d = offline_by_name[name]["cohens_d"]
        samples = per_signal.get(name, [])
        if len(samples) < 2:
            print(f"{name:10s} {off_d:>+10.3f}  (no on-device data for signal)")
            continue
        mean_d = statistics.mean(samples)
        std_d = statistics.stdev(samples)
        # Verdict: sign match + magnitude comparison
        same_sign = (off_d > 0) == (mean_d > 0)
        ratio = abs(mean_d) / abs(off_d) if abs(off_d) > 1e-6 else 0.0
        if not same_sign:
            v = "sign FLIP"
        elif ratio < 0.3:
            v = f"survived weakly ({ratio:.0%})"
        elif ratio < 0.7:
            v = f"moderate survival ({ratio:.0%})"
        else:
            v = f"preserved ({ratio:.0%})"
        print(
            f"{name:10s} {off_d:>+10.3f} {mean_d:>+10.3f} {std_d:>10.3f} {len(samples):>5d}  {v}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
