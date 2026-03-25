#!/usr/bin/env python3
"""Deduplicate onset consensus labels by merging close detections.

The 5 onset detection systems disagree by 40-80ms on the same event.
With 30ms merge tolerance, many events appear 2-3 times. This script
re-merges at a wider tolerance (default 70ms = half a 16th note at
common tempos), keeping the EARLIEST detection time (when the onset
actually started, not when a late system detected it).

Processes labels IN-PLACE — backs up originals to .bak if --backup.

Usage:
    python scripts/deduplicate_onset_labels.py
    python scripts/deduplicate_onset_labels.py --tolerance 70 --backup
    python scripts/deduplicate_onset_labels.py --dir /path/to/labels --dry-run
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


def deduplicate_onsets(onsets: list[dict], tolerance_ms: float) -> list[dict]:
    """Merge onset events within tolerance_ms of each other.

    When merging a group of close detections:
    - Time: use the EARLIEST detection (onset start, not late-system lag)
    - Strength: use the MAXIMUM (most systems agreed on this event cluster)
    - Systems: use the maximum system count from any member

    This preserves the sharp onset timing needed for training snappy
    detection, while combining the consensus information.
    """
    tolerance = tolerance_ms / 1000.0
    if not onsets:
        return []

    sorted_onsets = sorted(onsets, key=lambda o: o['time'])
    deduped = []

    i = 0
    while i < len(sorted_onsets):
        # Start a new group with this onset
        group = [sorted_onsets[i]]
        j = i + 1

        # Add any onsets within tolerance of the PREVIOUS member (sliding window)
        while j < len(sorted_onsets):
            if sorted_onsets[j]['time'] - group[-1]['time'] <= tolerance:
                group.append(sorted_onsets[j])
                j += 1
            else:
                break

        # Merge group: earliest time, max strength, max systems
        deduped.append({
            'time': group[0]['time'],  # EARLIEST detection
            'strength': max(o['strength'] for o in group),
            'systems': max(o['systems'] for o in group),
        })
        i = j

    return deduped


def process_file(path: Path, tolerance_ms: float, dry_run: bool, backup: bool) -> tuple[int, int]:
    """Process one label file. Returns (original_count, deduped_count)."""
    with open(path) as f:
        data = json.load(f)

    onsets = data.get('onsets', [])
    original_count = len(onsets)

    deduped = deduplicate_onsets(onsets, tolerance_ms)
    new_count = len(deduped)

    if not dry_run and new_count != original_count:
        if backup:
            shutil.copy2(path, path.with_suffix('.json.bak'))

        data['onsets'] = deduped
        data['count'] = new_count
        data['dedup_tolerance_ms'] = int(tolerance_ms)

        with open(path, 'w') as f:
            json.dump(data, f)

    return original_count, new_count


def main():
    parser = argparse.ArgumentParser(description="Deduplicate onset consensus labels")
    parser.add_argument('--dir', type=Path,
                        default=Path('/mnt/storage/blinky-ml-data/labels/onsets_consensus'),
                        help='Directory of onset label files')
    parser.add_argument('--test-dir', type=Path,
                        default=Path(__file__).parent.parent.parent / 'blinky-test-player/music/edm',
                        help='Also process test track onset labels')
    parser.add_argument('--tolerance', type=float, default=70,
                        help='Merge tolerance in ms (default: 70)')
    parser.add_argument('--backup', action='store_true',
                        help='Create .bak backup before modifying')
    parser.add_argument('--dry-run', action='store_true',
                        help='Report changes without modifying files')
    args = parser.parse_args()

    dirs = [args.dir]
    if args.test_dir.exists():
        dirs.append(args.test_dir)

    # Only process onset_consensus files (not librosa .onsets.json which are plain float arrays)
    pattern = '*.onsets_consensus.json' if args.test_dir.exists() else '*.onsets.json'

    total_files = 0
    total_original = 0
    total_deduped = 0
    total_changed = 0

    for d in dirs:
        # Only process consensus files (dict format with time/strength/systems),
        # not librosa .onsets.json (plain float arrays)
        files = sorted(d.glob('*.onsets_consensus.json')) if d == args.test_dir else sorted(d.glob('*.onsets.json'))
        if not files:
            continue

        print(f'\n{d} ({len(files)} files):')
        for path in files:
            orig, dedup = process_file(path, args.tolerance, args.dry_run, args.backup)
            removed = orig - dedup
            total_files += 1
            total_original += orig
            total_deduped += dedup
            if removed > 0:
                total_changed += 1
                pct = removed / orig * 100 if orig > 0 else 0
                if removed > orig * 0.2:  # Only print if >20% removed
                    print(f'  {path.name}: {orig} → {dedup} (-{removed}, {pct:.0f}%)')

    removed = total_original - total_deduped
    print(f'\n{"DRY RUN - " if args.dry_run else ""}Summary:')
    print(f'  Files: {total_files} ({total_changed} modified)')
    print(f'  Onsets: {total_original:,} → {total_deduped:,} (-{removed:,}, {removed/total_original*100:.1f}%)')
    print(f'  Tolerance: {args.tolerance}ms')


if __name__ == '__main__':
    main()
