#!/usr/bin/env python3
"""Validate beat label quality across multi-system annotations.

Analyzes per-system label files for:
  - System failures (too few beats, implausible tempo)
  - Octave errors (half-time / double-time disagreement between systems)
  - Systematic timing offsets between systems
  - Inter-beat interval regularity
  - Pairwise agreement metrics (CMLt vs AMLt from mir_eval)
  - Overall consensus quality scores

Generates a per-track report (JSON) and prints aggregate statistics.

Usage:
    python scripts/validate_labels.py \
        --labels-dir /mnt/storage/blinky-ml-data/labels/multi \
        --output report.json
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from itertools import combinations
from pathlib import Path

import mir_eval
import numpy as np
from tqdm import tqdm


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
MIN_PLAUSIBLE_BPM = 30.0
MAX_PLAUSIBLE_BPM = 250.0
MIN_BEATS_FOR_VALID = 8  # Less than this = system failure
OCTAVE_RATIO_TOLERANCE = 0.15  # |log2(ratio) - 1.0| < this = octave error
TIMING_OFFSET_TOLERANCE = 0.07  # 70ms for pairwise beat matching

# EDM tempo priors (for genre-aware flagging)
EDM_TEMPO_RANGE = (85, 185)  # Broad range covering most EDM subgenres


# ---------------------------------------------------------------------------
# File I/O
# ---------------------------------------------------------------------------
def discover_stems(labels_dir: Path) -> dict[str, dict[str, Path]]:
    """Find all label files grouped by stem and system."""
    stems: dict[str, dict[str, Path]] = defaultdict(dict)
    for f in sorted(labels_dir.glob("*.beats.json")):
        parts = f.name.split(".")
        if len(parts) < 4 or parts[-2] != "beats" or parts[-1] != "json":
            continue
        system = parts[-3]
        stem = ".".join(parts[:-3])
        if stem and system:
            stems[stem][system] = f
    return dict(stems)


def load_beats(path: Path) -> dict:
    """Load a per-system beats.json file."""
    with open(path) as f:
        data = json.load(f)
    return {
        "beats": np.array(data.get("beats", []), dtype=float),
        "downbeats": np.array(data.get("downbeats", []), dtype=float),
        "tempo": data.get("tempo", 0.0),
    }


# ---------------------------------------------------------------------------
# Per-system checks
# ---------------------------------------------------------------------------
def check_system_failure(sys_name: str, data: dict) -> list[str]:
    """Check for obvious system failures."""
    issues = []
    n_beats = len(data["beats"])
    tempo = data["tempo"]

    if n_beats < MIN_BEATS_FOR_VALID:
        issues.append(f"{sys_name}: only {n_beats} beats detected (failure)")

    if tempo > 0 and tempo < MIN_PLAUSIBLE_BPM:
        issues.append(f"{sys_name}: tempo {tempo:.1f} BPM < {MIN_PLAUSIBLE_BPM} (failure)")

    if tempo > MAX_PLAUSIBLE_BPM:
        issues.append(f"{sys_name}: tempo {tempo:.1f} BPM > {MAX_PLAUSIBLE_BPM} (failure)")

    return issues


def compute_ibi_stats(beats: np.ndarray) -> dict:
    """Compute inter-beat interval statistics."""
    if len(beats) < 2:
        return {"median_ibi": 0, "cv": 0, "min_max_ratio": 0, "n_beats": len(beats)}

    ibis = np.diff(beats)
    median_ibi = float(np.median(ibis))
    std_ibi = float(np.std(ibis))
    cv = std_ibi / median_ibi if median_ibi > 0 else 0

    # Ratio of min to max IBI (1.0 = perfectly regular)
    min_max_ratio = float(np.min(ibis) / np.max(ibis)) if np.max(ibis) > 0 else 0

    return {
        "median_ibi": round(median_ibi, 4),
        "bpm_from_ibi": round(60.0 / median_ibi, 1) if median_ibi > 0 else 0,
        "cv": round(cv, 4),
        "min_max_ratio": round(min_max_ratio, 4),
        "n_beats": len(beats),
    }


# ---------------------------------------------------------------------------
# Pairwise analysis
# ---------------------------------------------------------------------------
def detect_octave_error(tempo_a: float, tempo_b: float) -> str | None:
    """Check if two tempos are related by an octave (2:1 or 1:2 ratio)."""
    if tempo_a <= 0 or tempo_b <= 0:
        return None
    ratio = tempo_a / tempo_b
    log_ratio = abs(np.log2(ratio))
    if abs(log_ratio - 1.0) < OCTAVE_RATIO_TOLERANCE:
        if ratio > 1:
            return "double_time"  # A is ~2x B
        else:
            return "half_time"  # A is ~0.5x B
    return None


def pairwise_metrics(beats_a: np.ndarray, beats_b: np.ndarray) -> dict:
    """Compute pairwise beat agreement metrics using mir_eval."""
    if len(beats_a) < 2 or len(beats_b) < 2:
        return {"f_measure": 0, "cmlt": 0, "amlt": 0, "octave_gap": 0}

    scores = mir_eval.beat.evaluate(beats_a, beats_b)

    f_measure = scores["F-measure"]
    cmlt = scores["Correct Metric Level Total"]
    amlt = scores["Any Metric Level Total"]

    # The gap between AMLt and CMLt indicates octave disagreement
    octave_gap = amlt - cmlt

    return {
        "f_measure": round(f_measure, 4),
        "cmlt": round(cmlt, 4),
        "amlt": round(amlt, 4),
        "octave_gap": round(octave_gap, 4),
    }


def measure_timing_offset(beats_a: np.ndarray, beats_b: np.ndarray,
                          tolerance: float = TIMING_OFFSET_TOLERANCE) -> float:
    """Measure systematic timing offset of B relative to A.

    Returns median offset in seconds (positive = B is late relative to A).
    """
    if len(beats_a) < 2 or len(beats_b) < 2:
        return 0.0

    offsets = []
    for tb in beats_b:
        # Find closest beat in A
        diffs = beats_a - tb
        idx = np.argmin(np.abs(diffs))
        if abs(diffs[idx]) < tolerance:
            offsets.append(float(diffs[idx]))  # negative = B is late

    if len(offsets) < 5:
        return 0.0

    return round(float(-np.median(offsets)), 4)  # positive = B is late


# ---------------------------------------------------------------------------
# Per-track validation
# ---------------------------------------------------------------------------
def validate_track(stem: str, system_files: dict[str, Path]) -> dict:
    """Run all validation checks on one track."""
    # Load all systems
    systems = {}
    for sys_name, path in system_files.items():
        systems[sys_name] = load_beats(path)

    result = {
        "stem": stem,
        "n_systems": len(systems),
        "issues": [],
        "system_stats": {},
        "pairwise": {},
        "quality_score": 1.0,
    }

    # Per-system checks
    failed_systems = set()
    for sys_name, data in systems.items():
        # System failure checks
        failures = check_system_failure(sys_name, data)
        result["issues"].extend(failures)
        if failures:
            failed_systems.add(sys_name)

        # IBI stats
        result["system_stats"][sys_name] = {
            "tempo": data["tempo"],
            "ibi": compute_ibi_stats(data["beats"]),
            "n_downbeats": len(data["downbeats"]),
            "failed": bool(failures),
        }

    # Pairwise comparisons (skip failed systems)
    valid_systems = {k: v for k, v in systems.items() if k not in failed_systems}
    sys_names = sorted(valid_systems.keys())

    octave_errors = []
    timing_offsets = {}

    for sa, sb in combinations(sys_names, 2):
        pair_key = f"{sa}_vs_{sb}"

        # Tempo octave check
        tempo_a = valid_systems[sa]["tempo"]
        tempo_b = valid_systems[sb]["tempo"]
        octave = detect_octave_error(tempo_a, tempo_b)
        if octave:
            octave_errors.append({
                "pair": pair_key,
                "type": octave,
                "tempo_a": tempo_a,
                "tempo_b": tempo_b,
            })

        # Beat count ratio check
        n_a = len(valid_systems[sa]["beats"])
        n_b = len(valid_systems[sb]["beats"])
        if n_a > 0 and n_b > 0:
            count_ratio = max(n_a, n_b) / min(n_a, n_b)
            if abs(count_ratio - 2.0) < 0.3:
                if not octave:  # Don't double-report
                    octave_errors.append({
                        "pair": pair_key,
                        "type": "count_ratio_2x",
                        "n_a": n_a,
                        "n_b": n_b,
                        "ratio": round(count_ratio, 2),
                    })

        # mir_eval pairwise metrics
        metrics = pairwise_metrics(
            valid_systems[sa]["beats"], valid_systems[sb]["beats"])
        result["pairwise"][pair_key] = metrics

        # Timing offset
        offset = measure_timing_offset(
            valid_systems[sa]["beats"], valid_systems[sb]["beats"])
        if abs(offset) > 0.001:
            timing_offsets[pair_key] = offset

    if octave_errors:
        result["octave_errors"] = octave_errors
        for oe in octave_errors:
            result["issues"].append(
                f"octave error ({oe['type']}): {oe['pair']} "
                f"(tempos: {oe.get('tempo_a', '?')}/{oe.get('tempo_b', '?')})")

    if timing_offsets:
        result["timing_offsets"] = timing_offsets

    # Compute quality score (0-1)
    score = 1.0
    if failed_systems:
        score -= 0.25 * len(failed_systems)
    if octave_errors:
        score -= 0.15 * len(octave_errors)
    # Penalize high IBI variance across valid systems
    cvs = [result["system_stats"][s]["ibi"]["cv"]
           for s in valid_systems if result["system_stats"][s]["ibi"]["cv"] > 0]
    if cvs:
        mean_cv = np.mean(cvs)
        if mean_cv > 0.3:
            score -= 0.1
    # Penalize low pairwise agreement
    fmeasures = [m["f_measure"] for m in result["pairwise"].values()]
    if fmeasures:
        mean_f = np.mean(fmeasures)
        if mean_f < 0.5:
            score -= 0.2
        elif mean_f < 0.7:
            score -= 0.1

    result["quality_score"] = round(max(0.0, min(1.0, score)), 3)
    return result


# ---------------------------------------------------------------------------
# Aggregate statistics
# ---------------------------------------------------------------------------
def compute_aggregate_stats(results: list[dict]) -> dict:
    """Compute dataset-wide statistics from per-track results."""
    n_tracks = len(results)
    n_with_issues = sum(1 for r in results if r["issues"])
    n_with_octave = sum(1 for r in results if r.get("octave_errors"))
    n_with_failures = sum(1 for r in results
                         if any(s["failed"] for s in r["system_stats"].values()))

    # Per-system failure rates
    system_failures = defaultdict(int)
    system_totals = defaultdict(int)
    for r in results:
        for sys_name, stats in r["system_stats"].items():
            system_totals[sys_name] += 1
            if stats["failed"]:
                system_failures[sys_name] += 1

    # Pairwise agreement averages
    pair_fmeasures = defaultdict(list)
    pair_cmlts = defaultdict(list)
    pair_amlts = defaultdict(list)
    pair_octave_gaps = defaultdict(list)
    for r in results:
        for pair, metrics in r["pairwise"].items():
            if metrics["f_measure"] > 0:
                pair_fmeasures[pair].append(metrics["f_measure"])
                pair_cmlts[pair].append(metrics["cmlt"])
                pair_amlts[pair].append(metrics["amlt"])
                pair_octave_gaps[pair].append(metrics["octave_gap"])

    pairwise_summary = {}
    for pair in sorted(pair_fmeasures.keys()):
        pairwise_summary[pair] = {
            "mean_f_measure": round(np.mean(pair_fmeasures[pair]), 4),
            "mean_cmlt": round(np.mean(pair_cmlts[pair]), 4),
            "mean_amlt": round(np.mean(pair_amlts[pair]), 4),
            "mean_octave_gap": round(np.mean(pair_octave_gaps[pair]), 4),
            "n_tracks": len(pair_fmeasures[pair]),
        }

    # Timing offset aggregation
    offset_sums = defaultdict(list)
    for r in results:
        for pair, offset in r.get("timing_offsets", {}).items():
            offset_sums[pair].append(offset)

    timing_summary = {}
    for pair in sorted(offset_sums.keys()):
        offsets = offset_sums[pair]
        timing_summary[pair] = {
            "median_offset_ms": round(np.median(offsets) * 1000, 1),
            "mean_offset_ms": round(np.mean(offsets) * 1000, 1),
            "std_offset_ms": round(np.std(offsets) * 1000, 1),
            "n_tracks": len(offsets),
        }

    # Quality score distribution
    scores = [r["quality_score"] for r in results]
    score_buckets = {
        "excellent_0.9+": sum(1 for s in scores if s >= 0.9),
        "good_0.7-0.9": sum(1 for s in scores if 0.7 <= s < 0.9),
        "fair_0.5-0.7": sum(1 for s in scores if 0.5 <= s < 0.7),
        "poor_<0.5": sum(1 for s in scores if s < 0.5),
    }

    # Octave error details
    octave_type_counts = defaultdict(int)
    octave_pair_counts = defaultdict(int)
    for r in results:
        for oe in r.get("octave_errors", []):
            octave_type_counts[oe["type"]] += 1
            # Extract systems from pair key
            pair_systems = oe["pair"]
            octave_pair_counts[pair_systems] += 1

    return {
        "n_tracks": n_tracks,
        "n_with_issues": n_with_issues,
        "n_with_octave_errors": n_with_octave,
        "n_with_system_failures": n_with_failures,
        "pct_with_issues": round(100 * n_with_issues / n_tracks, 1),
        "pct_with_octave_errors": round(100 * n_with_octave / n_tracks, 1),
        "pct_with_system_failures": round(100 * n_with_failures / n_tracks, 1),
        "system_failure_rates": {
            s: {"failures": system_failures[s],
                "total": system_totals[s],
                "pct": round(100 * system_failures[s] / system_totals[s], 1)}
            for s in sorted(system_totals.keys())
        },
        "pairwise_agreement": pairwise_summary,
        "timing_offsets": timing_summary,
        "quality_score_distribution": score_buckets,
        "quality_score_mean": round(np.mean(scores), 3),
        "octave_error_types": dict(octave_type_counts),
        "octave_errors_by_pair": dict(octave_pair_counts),
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Validate beat label quality across multi-system annotations.")
    parser.add_argument("--labels-dir", required=True,
                        help="Directory with per-system .beats.json files")
    parser.add_argument("--output", default="label_validation_report.json",
                        help="Output report path (default: label_validation_report.json)")
    parser.add_argument("--worst", type=int, default=50,
                        help="Number of worst tracks to print (default: 50)")
    args = parser.parse_args()

    labels_dir = Path(args.labels_dir)
    stems = discover_stems(labels_dir)
    if not stems:
        print(f"No label files found in {labels_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(stems)} stems in {labels_dir}")
    print(f"Running validation...\n")

    # Validate all tracks
    results = []
    for stem in tqdm(sorted(stems.keys()), desc="Validating"):
        result = validate_track(stem, stems[stem])
        results.append(result)

    # Compute aggregate statistics
    stats = compute_aggregate_stats(results)

    # Print summary
    print(f"\n{'=' * 70}")
    print(f"LABEL VALIDATION REPORT")
    print(f"{'=' * 70}")
    print(f"Tracks analyzed:       {stats['n_tracks']}")
    print(f"Tracks with issues:    {stats['n_with_issues']} ({stats['pct_with_issues']}%)")
    print(f"Tracks with octave errors: {stats['n_with_octave_errors']} ({stats['pct_with_octave_errors']}%)")
    print(f"Tracks with sys failures:  {stats['n_with_system_failures']} ({stats['pct_with_system_failures']}%)")
    print(f"Mean quality score:    {stats['quality_score_mean']}")

    print(f"\n--- System Failure Rates ---")
    for sys_name, info in stats["system_failure_rates"].items():
        print(f"  {sys_name:12s}: {info['failures']:4d}/{info['total']} ({info['pct']:.1f}%)")

    print(f"\n--- Pairwise Agreement (mean across tracks) ---")
    print(f"  {'Pair':30s} {'F1':>6s} {'CMLt':>6s} {'AMLt':>6s} {'Gap':>6s}")
    for pair, info in stats["pairwise_agreement"].items():
        print(f"  {pair:30s} {info['mean_f_measure']:6.3f} {info['mean_cmlt']:6.3f} "
              f"{info['mean_amlt']:6.3f} {info['mean_octave_gap']:6.3f}")

    print(f"\n--- Systematic Timing Offsets (B late relative to A) ---")
    print(f"  {'Pair':30s} {'Median':>8s} {'Mean':>8s} {'Std':>8s} {'N':>5s}")
    for pair, info in stats["timing_offsets"].items():
        print(f"  {pair:30s} {info['median_offset_ms']:7.1f}ms {info['mean_offset_ms']:7.1f}ms "
              f"{info['std_offset_ms']:7.1f}ms {info['n_tracks']:5d}")

    print(f"\n--- Octave Error Breakdown ---")
    for err_type, count in sorted(stats["octave_error_types"].items()):
        print(f"  {err_type}: {count}")
    print(f"  By pair:")
    for pair, count in sorted(stats["octave_errors_by_pair"].items(),
                              key=lambda x: -x[1]):
        print(f"    {pair}: {count}")

    print(f"\n--- Quality Score Distribution ---")
    for bucket, count in stats["quality_score_distribution"].items():
        pct = 100 * count / stats["n_tracks"]
        print(f"  {bucket:20s}: {count:5d} ({pct:5.1f}%)")

    # Print worst tracks
    worst = sorted(results, key=lambda r: r["quality_score"])[:args.worst]
    print(f"\n--- {args.worst} Worst Tracks ---")
    for r in worst:
        issues_str = "; ".join(r["issues"][:3])
        print(f"  {r['stem']:20s} score={r['quality_score']:.3f}  {issues_str}")

    # Save full report
    report = {
        "aggregate": stats,
        "tracks": results,
    }

    # Convert numpy types for JSON serialization
    def _convert(obj):
        if isinstance(obj, (np.integer,)):
            return int(obj)
        if isinstance(obj, (np.floating,)):
            return float(obj)
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        return obj

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump(report, f, indent=2, default=_convert)

    print(f"\nFull report saved to {output_path}")
    print(f"Total tracks with data quality concerns: {stats['n_with_issues']}")


if __name__ == "__main__":
    main()
