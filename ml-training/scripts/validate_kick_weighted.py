#!/usr/bin/env python3
"""Validate kick-weighted onset labels before training.

Checks for quality issues that would degrade model training:
  - Empty/near-empty labels (Demucs separation failures)
  - Over-detection (ghost notes, noise)
  - Missing provenance metadata
  - Corrupt/malformed JSON

Usage:
    python scripts/validate_kick_weighted.py \
        --label-dir /mnt/storage/blinky-ml-data/labels/kick_weighted_drums \
        --consensus-dir /mnt/storage/blinky-ml-data/labels/consensus_v5

    # Quarantine bad tracks (move to .quarantined/ subdir):
    python scripts/validate_kick_weighted.py \
        --label-dir /mnt/storage/blinky-ml-data/labels/kick_weighted_drums \
        --quarantine

Output: summary stats + list of flagged tracks with reasons.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np

# Thresholds for quality gating
MIN_KICK_SNARE = 10        # Fewer than this = likely Demucs failure
MAX_DENSITY_PER_MIN = 600  # More than this = likely over-detection
MAX_CONSENSUS_RATIO = 8.0  # More than 8x consensus = suspicious
MIN_CONSENSUS_RATIO = 0.15 # Less than 15% of consensus = likely failed separation


def load_label(path: Path) -> dict:
    """Load and validate JSON structure."""
    with open(path) as f:
        data = json.load(f)
    if "onsets" not in data:
        raise ValueError(f"Missing 'onsets' key in {path.name}")
    if not isinstance(data["onsets"], list):
        raise ValueError(f"'onsets' is not a list in {path.name}")
    return data


def validate_track(stem: str, label_data: dict, consensus_data: dict | None) -> list[str]:
    """Return list of quality issues for a track. Empty = OK."""
    issues = []

    # Check if explicitly skipped during generation (silent drum stem)
    if label_data.get("skipped"):
        issues.append(f"skipped: {label_data.get('skip_reason', 'unknown')}")
        return issues

    n_kick = label_data.get("kick_count", 0)
    n_snare = label_data.get("snare_count", 0)
    n_ks = n_kick + n_snare

    # Check minimum events
    if n_ks < MIN_KICK_SNARE:
        issues.append(f"too_few_events: {n_ks} kick+snare (min {MIN_KICK_SNARE})")

    # Check density
    if label_data["onsets"]:
        duration_s = max(o["time"] for o in label_data["onsets"])
        if duration_s > 0:
            density = n_ks / (duration_s / 60)
            if density > MAX_DENSITY_PER_MIN:
                issues.append(f"high_density: {density:.0f}/min (max {MAX_DENSITY_PER_MIN})")

    # Check ratio vs consensus
    if consensus_data is not None:
        n_consensus = len(consensus_data.get("hits", []))
        if n_consensus > 10:
            ratio = n_ks / n_consensus
            if ratio > MAX_CONSENSUS_RATIO:
                issues.append(f"high_ratio: {ratio:.1f}x consensus ({n_ks}/{n_consensus})")
            elif ratio < MIN_CONSENSUS_RATIO:
                issues.append(f"low_ratio: {ratio:.2f}x consensus ({n_ks}/{n_consensus})")

    return issues


def main():
    parser = argparse.ArgumentParser(description="Validate kick-weighted onset labels")
    parser.add_argument("--label-dir", type=str, required=True,
                        help="Directory containing kick_weighted JSON labels")
    parser.add_argument("--consensus-dir", type=str, default="",
                        help="Consensus labels dir for cross-validation")
    parser.add_argument("--quarantine", action="store_true",
                        help="Move flagged labels to .quarantined/ subdir")
    parser.add_argument("--quiet", action="store_true",
                        help="Only print summary, not per-track issues")
    args = parser.parse_args()

    label_dir = Path(args.label_dir)
    consensus_dir = Path(args.consensus_dir) if args.consensus_dir else None
    quarantine_dir = label_dir / ".quarantined"

    files = sorted(label_dir.glob("*.kick_weighted.json"))
    print(f"Validating {len(files)} labels in {label_dir}")

    # Stats
    total = len(files)
    ok = 0
    flagged = 0
    corrupt = 0
    all_issues: dict[str, list[str]] = {}
    issue_counts: dict[str, int] = {}

    kick_counts = []
    snare_counts = []
    densities = []

    for lf in files:
        stem = lf.stem.replace(".kick_weighted", "")

        # Load label
        try:
            label_data = load_label(lf)
        except (json.JSONDecodeError, ValueError) as e:
            corrupt += 1
            all_issues[stem] = [f"corrupt: {e}"]
            continue

        # Load consensus if available
        consensus_data = None
        if consensus_dir:
            cf = consensus_dir / f"{stem}.beats.json"
            if cf.exists():
                try:
                    with open(cf) as f:
                        consensus_data = json.load(f)
                except json.JSONDecodeError:
                    pass

        # Validate
        issues = validate_track(stem, label_data, consensus_data)
        n_ks = label_data.get("kick_count", 0) + label_data.get("snare_count", 0)
        kick_counts.append(label_data.get("kick_count", 0))
        snare_counts.append(label_data.get("snare_count", 0))

        if label_data["onsets"]:
            dur = max(o["time"] for o in label_data["onsets"])
            if dur > 0:
                densities.append(n_ks / (dur / 60))

        if issues:
            flagged += 1
            all_issues[stem] = issues
            for issue in issues:
                tag = issue.split(":")[0]
                issue_counts[tag] = issue_counts.get(tag, 0) + 1
        else:
            ok += 1

    # Print results
    print(f"\n{'='*60}")
    print(f"Results: {ok} OK, {flagged} flagged, {corrupt} corrupt / {total} total")
    print(f"{'='*60}")

    if issue_counts:
        print("\nIssue breakdown:")
        for tag, count in sorted(issue_counts.items(), key=lambda x: -x[1]):
            print(f"  {tag}: {count} tracks")

    if not args.quiet and all_issues:
        print(f"\nFlagged tracks ({len(all_issues)}):")
        for stem, issues in sorted(all_issues.items()):
            print(f"  {stem}: {'; '.join(issues)}")

    # Distribution stats
    if kick_counts:
        kc = np.array(kick_counts)
        sc = np.array(snare_counts)
        d = np.array(densities) if densities else np.array([0])
        print(f"\nDistribution (kick+snare per track):")
        print(f"  Kicks:  min={kc.min()}, p5={np.percentile(kc,5):.0f}, "
              f"median={np.median(kc):.0f}, p95={np.percentile(kc,95):.0f}, max={kc.max()}")
        print(f"  Snares: min={sc.min()}, p5={np.percentile(sc,5):.0f}, "
              f"median={np.median(sc):.0f}, p95={np.percentile(sc,95):.0f}, max={sc.max()}")
        print(f"  Density: min={d.min():.0f}, p5={np.percentile(d,5):.0f}, "
              f"median={np.median(d):.0f}, p95={np.percentile(d,95):.0f}, max={d.max():.0f}/min")

    # Quarantine if requested
    if args.quarantine and all_issues:
        quarantine_dir.mkdir(exist_ok=True)
        moved = 0
        for stem in all_issues:
            src = label_dir / f"{stem}.kick_weighted.json"
            dst = quarantine_dir / f"{stem}.kick_weighted.json"
            if src.exists():
                src.rename(dst)
                moved += 1
        print(f"\nQuarantined {moved} files to {quarantine_dir}")

    # Exit code: non-zero if any issues found
    if flagged > 0 or corrupt > 0:
        pct = 100 * flagged / total if total > 0 else 0
        print(f"\n⚠ {flagged} tracks ({pct:.1f}%) have quality issues")
        return 1 if pct > 5 else 0  # Fail if >5% flagged
    else:
        print("\n✓ All labels pass validation")
        return 0


if __name__ == "__main__":
    sys.exit(main())
