#!/usr/bin/env python3
"""Merge per-system beat labels into consensus labels (v2 — quality-aware).

Improvements over v1:
  - System failure detection (too few beats, implausible tempo)
  - Octave normalization: detects and resolves half/double-time disagreements
  - Systematic timing offset correction (librosa +30ms late bias)
  - Per-track quality score stored in output
  - Configurable system weights (beat_this/madmom weighted higher)

Usage:
    python scripts/merge_consensus_labels_v2.py \
        --labels-dir /mnt/storage/blinky-ml-data/labels/multi \
        --output-dir /mnt/storage/blinky-ml-data/labels/consensus_v2

    # Stricter: only keep tracks with quality >= 0.5
    python scripts/merge_consensus_labels_v2.py \
        --labels-dir /mnt/storage/blinky-ml-data/labels/multi \
        --output-dir /mnt/storage/blinky-ml-data/labels/consensus_v2 \
        --min-quality 0.5
"""

from __future__ import annotations

import argparse
import json
import sys
import textwrap
from collections import defaultdict
from itertools import combinations
from pathlib import Path

import numpy as np
from tqdm import tqdm


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
MIN_BEATS_FOR_VALID = 8
MIN_PLAUSIBLE_BPM = 30.0
MAX_PLAUSIBLE_BPM = 250.0
OCTAVE_RATIO_TOLERANCE = 0.15  # |log2(ratio) - 1.0| < this = octave relation

# Systematic timing offsets to correct (seconds, positive = system is late).
# Measured from validate_labels.py pairwise analysis across 6993 tracks.
SYSTEM_OFFSETS = {
    "librosa": 0.031,   # 31ms late relative to beat_this/essentia/madmom
}

# System reliability weights for consensus position calculation.
# Higher = more trusted. Based on pairwise F1/CMLt from validation.
SYSTEM_WEIGHTS = {
    "beat_this": 1.0,
    "madmom": 0.95,
    "demucs_beats": 0.85,  # Drum-separated Beat This! — independent error profile
    "beatnet": 0.8,        # CRNN + particle filter — architecturally distinct
    "essentia": 0.7,
    "allin1": 0.7,         # Structure-aware NN — weaker on short clips
    "librosa": 0.5,
}


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
# System failure detection
# ---------------------------------------------------------------------------
def is_system_failed(data: dict) -> bool:
    """Check if a system produced garbage output."""
    if len(data["beats"]) < MIN_BEATS_FOR_VALID:
        return True
    if data["tempo"] > 0 and data["tempo"] < MIN_PLAUSIBLE_BPM:
        return True
    if data["tempo"] > MAX_PLAUSIBLE_BPM:
        return True
    return False


# ---------------------------------------------------------------------------
# Timing offset correction
# ---------------------------------------------------------------------------
def correct_timing_offset(beats: np.ndarray, system: str) -> np.ndarray:
    """Apply systematic timing offset correction."""
    offset = SYSTEM_OFFSETS.get(system, 0.0)
    if offset != 0.0 and len(beats) > 0:
        return beats - offset  # Subtract positive offset to shift earlier
    return beats


# ---------------------------------------------------------------------------
# Octave normalization
# ---------------------------------------------------------------------------
def estimate_tempo_from_beats(beats: np.ndarray) -> float:
    """Get tempo from median IBI."""
    if len(beats) < 2:
        return 0.0
    ibis = np.diff(beats)
    return 60.0 / float(np.median(ibis))


def is_octave_related(tempo_a: float, tempo_b: float) -> str | None:
    """Check if tempos are related by factor of 2."""
    if tempo_a <= 0 or tempo_b <= 0:
        return None
    ratio = tempo_a / tempo_b
    log_ratio = abs(np.log2(ratio))
    if abs(log_ratio - 1.0) < OCTAVE_RATIO_TOLERANCE:
        return "double" if ratio > 1 else "half"
    return None


def halve_beats(beats: np.ndarray) -> np.ndarray:
    """Take every other beat (reduce double-time to single-time)."""
    if len(beats) < 4:
        return beats
    # Try both even and odd subsets, pick the one with more regular IBIs
    even = beats[::2]
    odd = beats[1::2]
    cv_even = np.std(np.diff(even)) / np.mean(np.diff(even)) if len(even) > 1 else 999
    cv_odd = np.std(np.diff(odd)) / np.mean(np.diff(odd)) if len(odd) > 1 else 999
    return even if cv_even <= cv_odd else odd


def double_beats(beats: np.ndarray) -> np.ndarray:
    """Interpolate beats between each pair (double the rate)."""
    if len(beats) < 2:
        return beats
    doubled = []
    for i in range(len(beats) - 1):
        doubled.append(beats[i])
        doubled.append((beats[i] + beats[i + 1]) / 2.0)
    doubled.append(beats[-1])
    return np.array(doubled)


def normalize_octaves(system_beats: dict[str, dict]) -> dict[str, dict]:
    """Resolve octave disagreements by normalizing to majority metrical level.

    Strategy:
    1. Compute tempo from IBIs for each valid system
    2. Group systems by tempo octave (within ±15% of 2:1 ratio)
    3. Find the majority metrical level (weighted by system reliability)
    4. Halve or double outlier systems to match majority

    Note: Only handles 2:1 (single octave) ratios. 4:1 (double-octave) is not
    covered, which is fine for EDM where double-octave disagreements are rare.
    """
    # Get tempos from actual beat IBIs (more reliable than reported tempo)
    tempos = {}
    for sys_name, data in system_beats.items():
        t = estimate_tempo_from_beats(data["beats"])
        if t > 0:
            tempos[sys_name] = t

    if len(tempos) < 2:
        return system_beats

    # Check all pairs for octave relations
    sys_names = sorted(tempos.keys())
    octave_groups: dict[str, list[str]] = {}  # base_tempo -> [systems]

    # Build groups: systems with tempos within 15% of each other
    assigned = set()
    for sa in sys_names:
        if sa in assigned:
            continue
        group = [sa]
        assigned.add(sa)
        for sb in sys_names:
            if sb in assigned:
                continue
            ratio = tempos[sa] / tempos[sb]
            if abs(ratio - 1.0) < 0.15:
                group.append(sb)
                assigned.add(sb)
        octave_groups[sa] = group

    if len(octave_groups) <= 1:
        # All systems agree on metrical level
        return system_beats

    # Multiple tempo groups exist — find the majority (by weighted count)
    group_weights = {}
    for leader, members in octave_groups.items():
        weight = sum(SYSTEM_WEIGHTS.get(m, 0.5) for m in members)
        group_weights[leader] = (weight, tempos[leader], members)

    # Sort by weight descending
    sorted_groups = sorted(group_weights.items(), key=lambda x: -x[1][0])
    majority_leader = sorted_groups[0][0]
    majority_tempo = sorted_groups[0][1][1]

    # Normalize other groups to match majority
    result = dict(system_beats)
    for leader, (weight, tempo, members) in sorted_groups[1:]:
        relation = is_octave_related(tempo, majority_tempo)
        if relation == "double":
            # This group is at double the majority tempo — halve their beats
            for m in members:
                new_data = dict(result[m])
                halved = halve_beats(new_data["beats"])
                new_data["beats"] = halved
                # Filter downbeats to only those surviving the halving
                if len(new_data.get("downbeats", [])) > 0:
                    halved_set = set(halved.tolist())
                    new_data["downbeats"] = np.array(
                        [d for d in new_data["downbeats"] if d in halved_set],
                        dtype=float)
                new_data["_octave_corrected"] = "halved"
                result[m] = new_data
        elif relation == "half":
            # This group is at half the majority tempo — double their beats
            # Downbeats stay at original positions (interpolated beats aren't downbeats)
            for m in members:
                new_data = dict(result[m])
                new_data["beats"] = double_beats(new_data["beats"])
                new_data["_octave_corrected"] = "doubled"
                result[m] = new_data
        # If not octave-related, leave as-is (genuine disagreement)

    return result


# ---------------------------------------------------------------------------
# Consensus alignment
# ---------------------------------------------------------------------------
def align_beats(
    system_beats: dict[str, dict],
    tolerance: float,
    min_agreement: int,
    total_original_systems: int | None = None,
    downbeat_min_agreement: int = 2,
) -> list[dict]:
    """Align beats across systems and return consensus beats.

    Uses weighted median for consensus position (system weights).
    total_original_systems: the number of systems before failure filtering.
    Used for strength calculation so failed systems don't inflate strength
    (e.g., 2/2 valid = 1.0 is wrong when 2 of 4 original systems failed).
    """
    total_systems = total_original_systems if total_original_systems is not None else len(system_beats)
    if total_systems == 0:
        return []

    # Pool all beats with system tags
    tagged = []
    for sys_name, data in system_beats.items():
        for t in data["beats"]:
            tagged.append((float(t), sys_name))

    if not tagged:
        return []

    tagged.sort(key=lambda x: x[0])

    # Greedy clustering
    clusters = []
    current_cluster = [tagged[0]]

    for i in range(1, len(tagged)):
        t, sys_name = tagged[i]
        cluster_systems = {s for _, s in current_cluster}
        cluster_start = current_cluster[0][0]

        if (t - cluster_start) <= tolerance and sys_name not in cluster_systems:
            current_cluster.append((t, sys_name))
        else:
            clusters.append(current_cluster)
            current_cluster = [(t, sys_name)]

    clusters.append(current_cluster)

    # Filter by min_agreement and build consensus
    consensus = []
    for cluster in clusters:
        systems_in_cluster = list({s for _, s in cluster})
        if len(systems_in_cluster) < min_agreement:
            continue

        # Weighted median: use system weights for position
        times = np.array([t for t, _ in cluster])
        weights = np.array([SYSTEM_WEIGHTS.get(s, 0.5) for _, s in cluster])

        # Weighted median via sorted interpolation
        sorted_idx = np.argsort(times)
        sorted_times = times[sorted_idx]
        sorted_weights = weights[sorted_idx]
        cumw = np.cumsum(sorted_weights)
        half = cumw[-1] / 2.0
        median_idx = np.searchsorted(cumw, half)
        median_time = round(float(sorted_times[min(median_idx, len(sorted_times) - 1)]), 4)

        strength = round(len(systems_in_cluster) / total_systems, 4)

        # Downbeat check — count how many systems agree this is a downbeat.
        # Only beat_this and madmom provide downbeats (essentia/librosa don't).
        # Require 2+ systems (AND merge) for high-confidence downbeats.
        db_system_count = 0
        db_systems = []
        for sys_name in systems_in_cluster:
            db_arr = system_beats[sys_name]["downbeats"]
            if len(db_arr) > 0:
                if np.min(np.abs(db_arr - median_time)) < tolerance:
                    db_system_count += 1
                    db_systems.append(sys_name)
        is_downbeat = db_system_count >= downbeat_min_agreement

        consensus.append({
            "time": median_time,
            "expectTrigger": True,
            "strength": strength,
            "systems": sorted(systems_in_cluster),
            "isDownbeat": is_downbeat,
            "downbeatSystemCount": db_system_count,
            "downbeatSystems": sorted(db_systems),
        })

    consensus.sort(key=lambda x: x["time"])
    return consensus


# ---------------------------------------------------------------------------
# Per-track quality score
# ---------------------------------------------------------------------------
def compute_quality_score(
    system_beats: dict[str, dict],
    consensus: list[dict],
    failed_systems: set[str],
    octave_corrected: set[str],
) -> float:
    """Compute a 0-1 quality score for this track's consensus labels."""
    score = 1.0
    total_systems = len(system_beats) + len(failed_systems)

    # Penalize failed systems
    score -= 0.15 * len(failed_systems)

    # Penalize octave corrections (uncertainty about metrical level)
    score -= 0.05 * len(octave_corrected)

    # Penalize low agreement
    if consensus:
        mean_strength = np.mean([b["strength"] for b in consensus])
        if mean_strength < 0.5:
            score -= 0.15
    else:
        score -= 0.3

    # Penalize irregular IBIs in consensus
    if len(consensus) > 2:
        times = np.array([b["time"] for b in consensus])
        ibis = np.diff(times)
        if len(ibis) > 1:
            cv = np.std(ibis) / np.mean(ibis) if np.mean(ibis) > 0 else 1.0
            if cv > 0.3:
                score -= 0.1

    return round(max(0.0, min(1.0, score)), 3)


# ---------------------------------------------------------------------------
# Per-track merge
# ---------------------------------------------------------------------------
def merge_stem(
    label_files: dict[str, Path],
    tolerance: float,
    min_agreement: int,
    downbeat_min_agreement: int = 2,
) -> dict:
    """Merge per-system labels for one audio stem."""
    # Load all systems
    all_systems = {}
    for sys_name, path in label_files.items():
        all_systems[sys_name] = load_beats(path)

    # Step 1: Reject failed systems
    failed_systems = set()
    valid_systems = {}
    for sys_name, data in all_systems.items():
        if is_system_failed(data):
            failed_systems.add(sys_name)
        else:
            valid_systems[sys_name] = data

    if len(valid_systems) < min_agreement:
        # Not enough valid systems
        return _empty_result(all_systems, failed_systems, tolerance, min_agreement)

    # Step 2: Apply timing offset corrections
    for sys_name in valid_systems:
        corrected_beats = correct_timing_offset(
            valid_systems[sys_name]["beats"], sys_name)
        valid_systems[sys_name] = dict(valid_systems[sys_name])
        valid_systems[sys_name]["beats"] = corrected_beats

    # Step 3: Octave normalization
    normalized = normalize_octaves(valid_systems)
    octave_corrected = {s for s in normalized
                        if normalized[s].get("_octave_corrected")}

    # Step 4: Align and filter
    consensus_beats = align_beats(normalized, tolerance, min_agreement,
                                  total_original_systems=len(all_systems),
                                  downbeat_min_agreement=downbeat_min_agreement)

    # Step 5: Compute quality score
    quality = compute_quality_score(
        normalized, consensus_beats, failed_systems, octave_corrected)

    # Stats
    total_candidates = sum(len(d["beats"]) for d in all_systems.values())
    agreement_hist = defaultdict(int)
    for b in consensus_beats:
        agreement_hist[str(len(b["systems"]))] += 1

    # Consensus tempo from IBI
    if len(consensus_beats) > 1:
        times = np.array([b["time"] for b in consensus_beats])
        ibis = np.diff(times)
        consensus_tempo = round(60.0 / float(np.median(ibis)), 1)
    else:
        tempos = [d["tempo"] for d in valid_systems.values() if d["tempo"] > 0]
        consensus_tempo = round(float(np.median(tempos)), 1) if tempos else 0.0

    return {
        "hits": consensus_beats,
        "tempo": consensus_tempo,
        "quality_score": quality,
        "consensus_stats": {
            "systems_used": sorted(valid_systems.keys()),
            "systems_failed": sorted(failed_systems),
            "systems_octave_corrected": sorted(octave_corrected),
            "total_systems": len(all_systems),
            "valid_systems": len(valid_systems),
            "total_candidates": total_candidates,
            "consensus_beats": len(consensus_beats),
            "agreement_histogram": dict(agreement_hist),
            "tolerance_s": tolerance,
            "min_agreement": min_agreement,
        },
    }


def _empty_result(all_systems, failed_systems, tolerance, min_agreement):
    """Return an empty result when not enough valid systems."""
    return {
        "hits": [],
        "tempo": 0.0,
        "quality_score": 0.0,
        "consensus_stats": {
            "systems_used": [],
            "systems_failed": sorted(failed_systems),
            "systems_octave_corrected": [],
            "total_systems": len(all_systems),
            "valid_systems": 0,
            "total_candidates": sum(len(d["beats"]) for d in all_systems.values()),
            "consensus_beats": 0,
            "agreement_histogram": {},
            "tolerance_s": tolerance,
            "min_agreement": min_agreement,
        },
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Merge per-system beat labels with quality-aware consensus (v2).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Improvements over v1:
              - Rejects system failures (< 8 beats, BPM < 30 or > 250)
              - Corrects librosa's +31ms systematic late bias
              - Resolves octave errors (half/double-time normalization)
              - Weighted consensus position (beat_this/madmom trusted more)
              - Per-track quality score (0-1) in output
        """),
    )
    parser.add_argument("--labels-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--tolerance", type=float, default=0.05,
                        help="Max time diff for beat matching (default: 0.05)")
    parser.add_argument("--min-agreement", type=int, default=2,
                        help="Min systems to agree (default: 2)")
    parser.add_argument("--downbeat-min-agreement", type=int, default=2,
                        help="Min systems to agree on downbeat (default: 2 = AND merge)")
    parser.add_argument("--min-quality", type=float, default=0.0,
                        help="Skip tracks below this quality score (default: 0.0)")
    args = parser.parse_args()

    labels_dir = Path(args.labels_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    stems = discover_stems(labels_dir)
    if not stems:
        print(f"No label files found in {labels_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(stems)} stems in {labels_dir}")
    print(f"Tolerance: {args.tolerance}s, Min agreement: {args.min_agreement}")
    db_mode = "AND" if args.downbeat_min_agreement >= 2 else "OR"
    print(f"Downbeat min agreement: {args.downbeat_min_agreement} ({db_mode} merge)")
    print(f"Min quality: {args.min_quality}")
    print(f"Timing corrections: {SYSTEM_OFFSETS}")
    print(f"System weights: {SYSTEM_WEIGHTS}")
    print(f"Output: {output_dir}\n")

    # Process
    total_consensus = 0
    total_candidates = 0
    agreement_totals = defaultdict(int)
    quality_scores = []
    n_failed = 0
    n_octave_corrected = 0
    n_skipped_quality = 0
    errors = []

    for stem in tqdm(sorted(stems.keys()), desc="Merging"):
        try:
            result = merge_stem(stems[stem], args.tolerance, args.min_agreement,
                               args.downbeat_min_agreement)

            quality = result["quality_score"]
            quality_scores.append(quality)

            if quality < args.min_quality:
                n_skipped_quality += 1
                continue

            out_path = output_dir / f"{stem}.beats.json"
            with open(out_path, "w") as fp:
                json.dump(result, fp, indent=2)

            stats = result["consensus_stats"]
            total_consensus += stats["consensus_beats"]
            total_candidates += stats["total_candidates"]
            for n_str, count in stats["agreement_histogram"].items():
                agreement_totals[int(n_str)] += count
            if stats["systems_failed"]:
                n_failed += 1
            if stats["systems_octave_corrected"]:
                n_octave_corrected += 1

        except Exception as e:
            errors.append((stem, str(e)))

    # Summary
    processed = len(stems) - len(errors)
    print(f"\n{'=' * 60}")
    print(f"Summary: {processed}/{len(stems)} stems processed")
    print(f"Total consensus beats: {total_consensus} (from {total_candidates} candidates)")
    print(f"Tracks with system failures: {n_failed}")
    print(f"Tracks with octave correction: {n_octave_corrected}")
    if n_skipped_quality > 0:
        print(f"Skipped (quality < {args.min_quality}): {n_skipped_quality}")

    if quality_scores:
        qs = np.array(quality_scores)
        print(f"\nQuality score distribution:")
        print(f"  Mean: {np.mean(qs):.3f}, Median: {np.median(qs):.3f}")
        for threshold in [0.9, 0.7, 0.5, 0.3]:
            n = np.sum(qs >= threshold)
            print(f"  >= {threshold}: {n} ({100*n/len(qs):.1f}%)")

    if agreement_totals:
        print(f"\nAgreement distribution:")
        for n in sorted(agreement_totals.keys()):
            count = agreement_totals[n]
            pct = 100.0 * count / total_consensus if total_consensus > 0 else 0
            print(f"  {n} systems agree: {count:6d} beats ({pct:5.1f}%)")

    if errors:
        print(f"\n{len(errors)} errors:")
        for stem, err in errors[:10]:
            print(f"  {stem}: {err}")


if __name__ == "__main__":
    main()
