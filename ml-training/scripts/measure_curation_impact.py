#!/usr/bin/env python3
"""Report human-edit curation footprint over an onset-labelled corpus.

Walks a corpus and, for each track with a `<stem>.onsets_human.json` overlay,
applies the merge and tabulates how many onsets were edited/removed/created.
Used as the "did curation actually move anything" sanity check before and
after a label-reviewer session.

Usage:

    # In-repo validation corpus (overlays + auto GT live next to audio)
    python scripts/measure_curation_impact.py \\
        --auto-dir ../blinky-test-player/music/edm \\
        --auto-glob "*.onsets_consensus.json" \\
        --human-dir ../blinky-test-player/music/edm

    # Training corpus (auto in one dir, overlays in another)
    python scripts/measure_curation_impact.py \\
        --auto-dir /mnt/storage/blinky-ml-data/labels/onsets_consensus \\
        --auto-glob "*.onsets.json" \\
        --human-dir /mnt/storage/blinky-ml-data/labels/onsets_human

The script does **not** measure F1 deltas — that requires a model prediction
stream. To measure F1 lift, run the blinky-server validation harness with and
without the overlays in place (toggle them by renaming the human-edits dir).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from onset_label_merge import (  # noqa: E402
    HumanEditDriftError,
    apply_human_edits,
    load_human_edits,
)


def _stem_from_auto(path: Path, glob_pattern: str) -> str:
    """Strip the matched suffix from an auto-onset filename to recover the stem.

    The corpus paths have two conventions: `<stem>.onsets.json` (training) and
    `<stem>.onsets_consensus.json` (validation). We carry the glob the user
    passed in so we can compute the stem deterministically rather than
    guessing suffixes.
    """
    suffix = glob_pattern.lstrip("*")  # e.g. ".onsets.json" or ".onsets_consensus.json"
    name = path.name
    if not name.endswith(suffix):
        raise ValueError(f"file {name} does not match suffix {suffix!r}")
    return name[: -len(suffix)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--auto-dir", type=Path, required=True,
                        help="Directory containing `<stem>.onsets*.json` auto consensus files")
    parser.add_argument("--auto-glob", default="*.onsets_consensus.json",
                        help="Glob for auto onset files (default: %(default)s)")
    parser.add_argument("--human-dir", type=Path, required=True,
                        help="Directory containing `<stem>.onsets_human.json` overlays")
    parser.add_argument("--show-tracks", type=int, default=20,
                        help="Print this many per-track lines (most-edited first). 0 = none.")
    args = parser.parse_args()

    if not args.auto_dir.is_dir():
        print(f"auto-dir does not exist: {args.auto_dir}", file=sys.stderr)
        return 2
    if not args.human_dir.is_dir():
        print(f"human-dir does not exist: {args.human_dir}", file=sys.stderr)
        return 2

    auto_files = sorted(args.auto_dir.glob(args.auto_glob))
    if not auto_files:
        print(f"no files match {args.auto_dir}/{args.auto_glob}", file=sys.stderr)
        return 2

    per_track: list[dict] = []
    drift_stems: list[str] = []
    total = {"edited": 0, "removed": 0, "created": 0, "auto_total": 0, "merged_total": 0}
    tracks_with_edits = 0
    tracks_without_overlay = 0

    for auto_path in auto_files:
        try:
            stem = _stem_from_auto(auto_path, args.auto_glob)
        except ValueError as e:
            print(f"skipping unrecognized filename: {e}", file=sys.stderr)
            continue

        with open(auto_path) as f:
            auto = json.load(f).get("onsets", [])

        overlay_path = args.human_dir / f"{stem}.onsets_human.json"
        edits_doc = load_human_edits(overlay_path)
        if edits_doc is None:
            tracks_without_overlay += 1
            total["auto_total"] += len(auto)
            total["merged_total"] += len(auto)
            continue

        try:
            merged, stats = apply_human_edits(auto, edits_doc)
        except HumanEditDriftError as exc:
            drift_stems.append(stem)
            print(f"DRIFT {stem}: {exc}", file=sys.stderr)
            continue

        any_edits = stats["edited"] or stats["removed"] or stats["created"]
        if any_edits:
            tracks_with_edits += 1
        per_track.append({
            "stem": stem,
            "auto": len(auto),
            "merged": stats["total"],
            "edited": stats["edited"],
            "removed": stats["removed"],
            "created": stats["created"],
        })
        for k in ("edited", "removed", "created"):
            total[k] += stats[k]
        total["auto_total"] += len(auto)
        total["merged_total"] += stats["total"]

    print("=" * 60)
    print(f"Corpus:        {args.auto_dir}")
    print(f"Auto pattern:  {args.auto_glob}")
    print(f"Overlay dir:   {args.human_dir}")
    print(f"Auto tracks scanned:    {len(auto_files)}")
    print(f"  with overlay:         {len(per_track)}")
    print(f"  with non-trivial edits: {tracks_with_edits}")
    print(f"  without overlay:      {tracks_without_overlay}")
    if drift_stems:
        print(f"  DRIFT (skipped):      {len(drift_stems)} -> {drift_stems[:5]}{'...' if len(drift_stems) > 5 else ''}")
    print()
    print(f"Onset totals:           auto={total['auto_total']}  merged={total['merged_total']}")
    print(f"  edited (time/strength): {total['edited']}")
    print(f"  removed:                {total['removed']}")
    print(f"  created:                {total['created']}")
    if total["auto_total"]:
        change = total["edited"] + total["removed"] + total["created"]
        pct = 100.0 * change / total["auto_total"]
        print(f"  total disagreement:     {change} / {total['auto_total']} auto onsets ({pct:.2f}%)")

    if args.show_tracks > 0 and per_track:
        per_track.sort(
            key=lambda t: (t["edited"] + t["removed"] + t["created"]),
            reverse=True,
        )
        print()
        print("Top tracks by edit count:")
        print(f"{'stem':<48} {'auto':>5} {'merged':>6} {'edit':>5} {'rm':>4} {'new':>4}")
        for t in per_track[: args.show_tracks]:
            if (t["edited"] + t["removed"] + t["created"]) == 0:
                break
            print(f"{t['stem']:<48} {t['auto']:>5} {t['merged']:>6} "
                  f"{t['edited']:>5} {t['removed']:>4} {t['created']:>4}")

    return 1 if drift_stems else 0


if __name__ == "__main__":
    raise SystemExit(main())
