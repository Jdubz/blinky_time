#!/usr/bin/env python3
"""Merge per-system beat labels into consensus labels.

Reads per-system label files ({stem}.{system}.beats.json) produced by
label_beats_multi.py and creates consensus labels where beats are only
retained if multiple systems agree on their position within a tolerance.

The output format is compatible with blinky-test-player .beats.json:
    {
        "hits": [
            {
                "time": 0.5,
                "expectTrigger": true,
                "strength": 0.75,
                "systems": ["beat_this", "essentia", "librosa"]
            },
            ...
        ],
        "consensus_stats": {
            "systems_used": ["beat_this", "essentia", "librosa", "madmom"],
            "total_candidates": 500,
            "consensus_beats": 120,
            "agreement_histogram": {"2": 40, "3": 55, "4": 25}
        }
    }

Usage:
    # Default: 50ms tolerance, 2+ systems must agree
    python scripts/merge_consensus_labels.py \\
        --labels-dir data/labels/multi \\
        --output-dir data/labels/consensus

    # Stricter: 30ms tolerance, 3+ systems
    python scripts/merge_consensus_labels.py \\
        --labels-dir data/labels/multi \\
        --output-dir data/labels/consensus \\
        --tolerance 0.03 \\
        --min-agreement 3
"""

import argparse
import json
import sys
import textwrap
from collections import defaultdict
from pathlib import Path

import numpy as np


def discover_stems(labels_dir: Path) -> dict[str, list[Path]]:
    """Find all label files and group them by audio stem.

    Returns a dict mapping stem -> list of per-system JSON paths.
    Files are expected to be named {stem}.{system}.beats.json.
    """
    stems = defaultdict(list)
    for f in sorted(labels_dir.glob("*.beats.json")):
        # Parse: stem.system.beats.json -> stem = everything before the
        # second-to-last dot-separated segment
        parts = f.name.split(".")
        # Expected: [stem_parts..., system, "beats", "json"]
        if len(parts) < 4 or parts[-2] != "beats" or parts[-1] != "json":
            continue
        system = parts[-3]
        stem = ".".join(parts[:-3])
        if stem and system:
            stems[stem].append(f)
    return dict(stems)


def load_system_beats(label_files: list[Path]) -> dict[str, dict]:
    """Load beat data from each per-system label file.

    Returns dict mapping system_name -> {beats: np.array, downbeats: np.array, tempo: float}.
    """
    systems = {}
    for f in label_files:
        parts = f.name.split(".")
        system = parts[-3]
        with open(f) as fp:
            data = json.load(fp)
        systems[system] = {
            "beats": np.array(data.get("beats", []), dtype=float),
            "downbeats": np.array(data.get("downbeats", []), dtype=float),
            "tempo": data.get("tempo", 0.0),
        }
    return systems


def align_beats(
    system_beats: dict[str, dict],
    tolerance: float,
    min_agreement: int,
) -> list[dict]:
    """Align beats across systems and return consensus beats.

    Algorithm:
    1. Pool all beats from all systems into a single sorted list, tagged
       with their source system.
    2. Greedily cluster consecutive beats that are within ±tolerance of
       each other (but no two beats from the same system in one cluster).
    3. Keep clusters with >= min_agreement distinct systems.
    4. For each kept cluster, the consensus time is the median of the
       cluster's beat times.

    Returns a sorted list of consensus beat dicts.
    """
    total_systems = len(system_beats)
    if total_systems == 0:
        return []

    # Pool all beats with system tags: (time, system_name)
    tagged = []
    for sys_name, data in system_beats.items():
        for t in data["beats"]:
            tagged.append((float(t), sys_name))

    if not tagged:
        return []

    # Sort by time
    tagged.sort(key=lambda x: x[0])

    # Greedy clustering: walk through sorted beats, group those within
    # tolerance of the cluster anchor. If a beat from the same system is
    # already in the cluster, start a new cluster.
    clusters = []  # list of lists of (time, system)
    current_cluster = [tagged[0]]

    for i in range(1, len(tagged)):
        t, sys_name = tagged[i]
        cluster_systems = {s for _, s in current_cluster}
        cluster_start = current_cluster[0][0]

        # Belongs to current cluster if within tolerance of the anchor
        # (first beat in cluster) AND system not already present
        if (t - cluster_start) <= tolerance and sys_name not in cluster_systems:
            current_cluster.append((t, sys_name))
        else:
            # Finalize current cluster and start new one
            clusters.append(current_cluster)
            current_cluster = [(t, sys_name)]

    clusters.append(current_cluster)

    # Filter by min_agreement and build consensus beats
    consensus = []
    for cluster in clusters:
        systems_in_cluster = list({s for _, s in cluster})
        if len(systems_in_cluster) < min_agreement:
            continue

        times = np.array([t for t, _ in cluster])
        median_time = round(float(np.median(times)), 4)
        strength = round(len(systems_in_cluster) / total_systems, 4)

        # Check if this is a downbeat in any contributing system
        is_downbeat = False
        for sys_name in systems_in_cluster:
            db_arr = system_beats[sys_name]["downbeats"]
            if len(db_arr) > 0:
                # Check if any downbeat is close to this consensus time
                if np.min(np.abs(db_arr - median_time)) < tolerance:
                    is_downbeat = True
                    break

        consensus.append({
            "time": median_time,
            "expectTrigger": True,
            "strength": strength,
            "systems": sorted(systems_in_cluster),
            "isDownbeat": is_downbeat,
        })

    # Sort by time and remove duplicates (shouldn't happen, but be safe)
    consensus.sort(key=lambda x: x["time"])
    return consensus


def merge_stem(
    label_files: list[Path],
    tolerance: float,
    min_agreement: int,
) -> dict:
    """Merge per-system labels for one audio stem into a consensus dict."""
    system_beats = load_system_beats(label_files)
    systems_used = sorted(system_beats.keys())
    total_systems = len(systems_used)

    # Count total candidate beats across all systems
    total_candidates = sum(len(d["beats"]) for d in system_beats.values())

    # Align and filter
    consensus_beats = align_beats(system_beats, tolerance, min_agreement)

    # Agreement histogram: how many beats have N-system agreement
    agreement_hist = defaultdict(int)
    for b in consensus_beats:
        n = len(b["systems"])
        agreement_hist[str(n)] += 1

    # Estimate consensus tempo from median inter-beat interval
    if len(consensus_beats) > 1:
        times = np.array([b["time"] for b in consensus_beats])
        ibis = np.diff(times)
        consensus_tempo = round(60.0 / float(np.median(ibis)), 1)
    else:
        # Fall back to median of per-system tempos
        tempos = [d["tempo"] for d in system_beats.values() if d["tempo"] > 0]
        consensus_tempo = round(float(np.median(tempos)), 1) if tempos else 0.0

    return {
        "hits": consensus_beats,
        "tempo": consensus_tempo,
        "consensus_stats": {
            "systems_used": systems_used,
            "total_systems": total_systems,
            "total_candidates": total_candidates,
            "consensus_beats": len(consensus_beats),
            "agreement_histogram": dict(agreement_hist),
            "tolerance_s": tolerance,
            "min_agreement": min_agreement,
        },
    }


def main():
    parser = argparse.ArgumentParser(
        description="Merge per-system beat labels into consensus labels.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Examples:
              python scripts/merge_consensus_labels.py \\
                  --labels-dir data/labels/multi \\
                  --output-dir data/labels/consensus

              python scripts/merge_consensus_labels.py \\
                  --labels-dir data/labels/multi \\
                  --output-dir data/labels/consensus \\
                  --tolerance 0.03 --min-agreement 3
        """),
    )
    parser.add_argument(
        "--labels-dir", required=True,
        help="Directory containing {stem}.{system}.beats.json files",
    )
    parser.add_argument(
        "--output-dir", required=True,
        help="Output directory for consensus .beats.json files",
    )
    parser.add_argument(
        "--tolerance", type=float, default=0.05,
        help="Max time difference (seconds) to consider beats as matching (default: 0.05)",
    )
    parser.add_argument(
        "--min-agreement", type=int, default=2,
        help="Minimum number of systems that must agree for a consensus beat (default: 2)",
    )
    args = parser.parse_args()

    labels_dir = Path(args.labels_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not labels_dir.is_dir():
        print(f"Error: {labels_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Discover stems
    stems = discover_stems(labels_dir)
    if not stems:
        print(f"No per-system label files found in {labels_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(stems)} audio stems with per-system labels")
    print(f"Tolerance: {args.tolerance}s, Min agreement: {args.min_agreement} systems")
    print(f"Output: {output_dir}\n")

    # Aggregate statistics
    total_consensus = 0
    total_candidates = 0
    agreement_totals = defaultdict(int)
    errors = []

    for stem in sorted(stems.keys()):
        try:
            result = merge_stem(stems[stem], args.tolerance, args.min_agreement)

            out_path = output_dir / f"{stem}.beats.json"
            with open(out_path, "w") as fp:
                json.dump(result, fp, indent=2)

            stats = result["consensus_stats"]
            total_consensus += stats["consensus_beats"]
            total_candidates += stats["total_candidates"]
            for n_str, count in stats["agreement_histogram"].items():
                agreement_totals[int(n_str)] += count

        except Exception as e:
            errors.append((stem, str(e)))
            print(f"  ERROR: {stem}: {e}")

    # Print summary
    processed = len(stems) - len(errors)
    print(f"\n{'=' * 60}")
    print(f"Summary: {processed}/{len(stems)} stems processed")
    print(f"Total consensus beats: {total_consensus} (from {total_candidates} candidates)")

    if agreement_totals:
        print(f"\nAgreement distribution:")
        for n in sorted(agreement_totals.keys()):
            count = agreement_totals[n]
            pct = 100.0 * count / total_consensus if total_consensus > 0 else 0
            print(f"  {n} systems agree: {count:6d} beats ({pct:5.1f}%)")

    if errors:
        print(f"\n{len(errors)} errors:")
        for stem, err in errors:
            print(f"  {stem}: {err}")


if __name__ == "__main__":
    main()
