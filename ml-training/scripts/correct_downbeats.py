#!/usr/bin/env python3
"""Correct and complete downbeat annotations using BPM-aware grid alignment.

Problem: Only 2/4 labeling systems (beat_this, madmom) provide downbeat
annotations, and they disagree on ~33% of tracks. AND-merge (consensus_v3)
leaves many tracks with sparse or missing downbeats. Even OR-merge is noisy.

Solution: Use the reliable consensus beat grid + per-system downbeat
annotations to vote on the correct downbeat phase (mod 4 in 4/4 time),
then fill in ALL downbeats at the winning phase. This gives complete,
regular downbeat labels for every track with a stable beat grid.

Algorithm:
  1. Load consensus beats (already reliable from 4-system agreement)
  2. Load per-system downbeats from beat_this and madmom
  3. Map each downbeat to its nearest consensus beat index
  4. For each phase p in {0,1,2,3}: count how many system downbeats land
     on beats where (index % 4 == p), weighted by system reliability
  5. Pick the phase with the highest score; require margin over runner-up
  6. Mark ALL beats at that phase as downbeats

Outputs consensus_v4 labels: same format as v3, with corrected downbeats
and added metadata (downbeat_correction field).

Usage:
    python correct_downbeats.py [--analyze-only] [--min-margin 1.5]
    python correct_downbeats.py --output-dir /path/to/consensus_v4
"""

import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# System weights for downbeat phase voting.
# madmom gets higher weight because it's far more regular (84% spacing=4)
# beat_this is noisy (16.5% spacing=1, many spurious downbeat marks).
DOWNBEAT_SYSTEM_WEIGHTS = {
    "beat_this": 0.7,
    "madmom": 1.0,
}

# Tolerance for matching a downbeat time to a consensus beat (seconds)
MATCH_TOLERANCE = 0.1

# Minimum beats to attempt correction
MIN_BEATS = 12

# Default meter (beats per measure)
DEFAULT_METER = 4

# Minimum margin: winning phase score must be this many times the runner-up
DEFAULT_MIN_MARGIN = 1.5

# Minimum votes to accept a phase (across all systems, weighted)
MIN_TOTAL_VOTES = 3.0

# Gap filling: IBI ratio threshold to detect a missing beat
GAP_THRESHOLD = 1.5

# Beat grid regularity: CV above this is too irregular for phase voting
CONSENSUS_GRID_MAX_CV = 0.35

# Madmom fallback grid: must be this regular to substitute for consensus
MADMOM_GRID_MAX_CV = 0.20


# ---------------------------------------------------------------------------
# Core algorithm
# ---------------------------------------------------------------------------

def map_downbeats_to_beat_indices(
    db_times: np.ndarray,
    beat_times: np.ndarray,
    tolerance: float | None = None,
) -> list[int]:
    """Map downbeat times to their nearest consensus beat indices.

    Tolerance defaults to min(IBI/3, 0.25s) — adaptive so it works at any
    tempo while staying below half a beat period. Some systems (e.g. madmom)
    report downbeat times offset from their own beat times by ~0.2s.
    """
    if tolerance is None:
        if len(beat_times) >= 2:
            median_ibi = float(np.median(np.diff(beat_times)))
            tolerance = min(median_ibi / 3.0, 0.25)
        else:
            tolerance = MATCH_TOLERANCE

    indices = []
    for dt in db_times:
        dists = np.abs(beat_times - dt)
        best = np.argmin(dists)
        if dists[best] < tolerance:
            indices.append(int(best))
    return indices


def detect_meter(db_indices: list[int]) -> int:
    """Detect likely meter from downbeat spacings. Returns 3, 4, or 6."""
    if len(db_indices) < 3:
        return DEFAULT_METER
    spacings = np.diff(db_indices)
    if len(spacings) == 0:
        return DEFAULT_METER
    counts = Counter(int(s) for s in spacings)
    mode_spacing, mode_count = counts.most_common(1)[0]
    # If the dominant spacing is 3 (and accounts for >50% of spacings), use 3/4
    if mode_spacing == 3 and mode_count > len(spacings) * 0.5:
        return 3
    # If dominant is 6, use 6/8
    if mode_spacing == 6 and mode_count > len(spacings) * 0.5:
        return 6
    return DEFAULT_METER


def vote_on_phase(
    system_db_indices: dict[str, list[int]],
    meter: int,
    weights: dict[str, float] = DOWNBEAT_SYSTEM_WEIGHTS,
) -> tuple[np.ndarray, int, float]:
    """Vote on the best downbeat phase.

    Returns:
        phase_scores: array of scores for each phase (length = meter)
        best_phase: the winning phase index
        margin: ratio of best score to runner-up (inf if only one phase has votes)
    """
    phase_scores = np.zeros(meter, dtype=float)

    for sys_name, indices in system_db_indices.items():
        w = weights.get(sys_name, 0.5)
        for idx in indices:
            phase = idx % meter
            phase_scores[phase] += w

    sorted_scores = np.sort(phase_scores)[::-1]
    best_phase = int(np.argmax(phase_scores))

    if sorted_scores[0] == 0:
        return phase_scores, 0, 0.0

    if len(sorted_scores) < 2 or sorted_scores[1] == 0:
        # Unanimous vote — use None to avoid non-finite JSON values.
        # Callers treat None as "exceeds any threshold" (unanimous).
        margin = None
    else:
        margin = sorted_scores[0] / sorted_scores[1]

    return phase_scores, best_phase, margin


def check_beat_regularity(beat_times: np.ndarray) -> tuple[float, float]:
    """Check how regular the beat grid is.

    Returns:
        median_ibi: median inter-beat interval
        cv: coefficient of variation (std/mean of IBIs)
    """
    if len(beat_times) < 2:
        return 0.0, float("inf")
    ibis = np.diff(beat_times)
    median_ibi = float(np.median(ibis))
    mean_ibi = float(np.mean(ibis))
    if mean_ibi <= 0:
        return median_ibi, float("inf")
    cv = float(np.std(ibis) / mean_ibi)
    return median_ibi, cv


def fill_beat_gaps(hits: list[dict], gap_threshold: float = GAP_THRESHOLD) -> tuple[list[dict], int]:
    """Fill gaps in the beat grid by interpolating missing beats.

    Detects gaps where the inter-beat interval exceeds gap_threshold × median IBI,
    and inserts synthetic beats at evenly-spaced positions within each gap.
    Synthetic beats get strength=0 and a "synthetic" flag.

    Returns:
        filled_hits: new hits list with gaps filled
        n_inserted: number of beats inserted
    """
    if len(hits) < 4:
        return hits, 0

    beat_times = np.array([h["time"] for h in hits])
    ibis = np.diff(beat_times)
    if len(ibis) < 2:
        return hits, 0

    median_ibi = float(np.median(ibis))
    if median_ibi <= 0:
        return hits, 0

    filled = []
    n_inserted = 0

    for i, h in enumerate(hits):
        filled.append(h)

        if i < len(hits) - 1:
            gap = beat_times[i + 1] - beat_times[i]
            ratio = gap / median_ibi
            if ratio > gap_threshold:
                # How many beats are missing?
                n_missing = round(ratio) - 1
                if n_missing < 1:
                    continue
                # Interpolate evenly within the gap
                step = gap / (n_missing + 1)
                for j in range(1, n_missing + 1):
                    t = round(float(beat_times[i] + j * step), 4)
                    filled.append({
                        "time": t,
                        "expectTrigger": True,
                        "strength": 0.0,
                        "systems": [],
                        "isDownbeat": False,  # Will be set by phase correction
                        "synthetic": True,
                    })
                    n_inserted += 1

    # Sort by time (inserted beats are appended after each gap)
    filled.sort(key=lambda x: x["time"])
    return filled, n_inserted


def correct_track_downbeats(
    consensus: dict,
    system_beats: dict[str, dict],
    min_margin: float = DEFAULT_MIN_MARGIN,
) -> tuple[dict, dict]:
    """Correct downbeats for a single track.

    Args:
        consensus: the consensus_v3 label dict
        system_beats: {system_name: {"beats": [...], "downbeats": [...]}}
        min_margin: minimum margin for phase selection

    Returns:
        corrected: updated consensus dict with corrected downbeats
        meta: correction metadata
    """
    # Fill gaps in the beat grid before doing anything else.
    # This ensures the beat indices used for phase voting are contiguous.
    raw_hits = consensus["hits"]
    hits, n_beats_inserted = fill_beat_gaps(raw_hits)
    beat_times = np.array([h["time"] for h in hits])
    n_beats = len(beat_times)

    meta = {
        "action": "skip",
        "reason": "",
        "n_beats": n_beats,
        "n_beats_inserted": n_beats_inserted,
        "meter": DEFAULT_METER,
        "phase": -1,
        "margin": 0.0,
        "total_votes": 0.0,
        "phase_scores": [],
        "original_downbeats": sum(1 for h in raw_hits if h.get("isDownbeat", False)),
        "corrected_downbeats": 0,
        "systems_with_db": [],
        "confidence": "high",
    }

    # Need enough beats
    if n_beats < MIN_BEATS:
        meta["reason"] = f"too few beats ({n_beats})"
        return consensus, meta

    # Check beat grid regularity — skip tracks with very irregular beats
    # cv > 0.35 is genuinely irregular (tempo changes, detection failures).
    # cv 0.25-0.35 is borderline but still usable for phase voting.
    _, cv = check_beat_regularity(beat_times)
    use_madmom_grid = False

    if cv > CONSENSUS_GRID_MAX_CV:
        # Fallback: if the consensus grid is noisy but madmom's own grid is
        # regular, use madmom's beats as the reference for phase indexing.
        # This happens when 4 systems merge produces spurious beats but
        # individual systems are fine (common in DnB, trance, ambient).
        mm = system_beats.get("madmom", {})
        mm_beats = np.array(mm.get("beats", []))
        mm_dbs = np.array(mm.get("downbeats", []))
        if len(mm_beats) >= MIN_BEATS and len(mm_dbs) >= 2:
            _, mm_cv = check_beat_regularity(mm_beats)
            if mm_cv < MADMOM_GRID_MAX_CV:
                beat_times = mm_beats
                n_beats = len(beat_times)
                use_madmom_grid = True
                meta["n_beats"] = n_beats

        if not use_madmom_grid:
            meta["reason"] = f"irregular beat grid (cv={cv:.3f})"
            return consensus, meta

    # Collect per-system downbeat indices (mapped to the active beat grid)
    system_db_indices = {}
    for sys_name in ["beat_this", "madmom"]:
        if sys_name not in system_beats:
            continue
        db_times = np.array(system_beats[sys_name].get("downbeats", []))
        if len(db_times) < 2:
            continue
        indices = map_downbeats_to_beat_indices(db_times, beat_times)
        if len(indices) >= 2:
            system_db_indices[sys_name] = indices
            meta["systems_with_db"].append(sys_name)

    if not system_db_indices:
        meta["reason"] = "no systems with usable downbeats"
        return consensus, meta

    # Detect meter from all available downbeat indices
    all_indices = []
    for indices in system_db_indices.values():
        all_indices.extend(indices)
    meter = detect_meter(all_indices)
    meta["meter"] = meter

    # Vote on phase using all systems
    phase_scores, best_phase, margin = vote_on_phase(
        system_db_indices, meter
    )
    total_votes = float(np.sum(phase_scores))
    meta["phase_scores"] = [round(float(s), 2) for s in phase_scores]
    meta["phase"] = best_phase
    meta["margin"] = round(margin, 3) if margin is not None else None
    meta["total_votes"] = round(total_votes, 2)

    # Reject if insufficient evidence
    if total_votes < MIN_TOTAL_VOTES:
        meta["reason"] = f"insufficient votes ({total_votes:.1f} < {MIN_TOTAL_VOTES})"
        return consensus, meta

    # Tiered acceptance:
    # 1. High confidence: margin >= min_margin with both systems
    # 2. Madmom tiebreaker: if combined margin is low, check madmom alone
    #    (madmom is 71% regular vs beat_this 41%, much more reliable for phase)
    # 3. Half-bar fallback: if two phases differ by meter/2, it's a known
    #    ambiguity — apply with lower confidence since grid is still correct
    confidence = "high"

    # margin is None when unanimous (runner-up score is 0) — always passes thresholds
    if margin is not None and margin < min_margin:
        # Try madmom-only phase vote as tiebreaker
        madmom_resolved = False
        if "madmom" in system_db_indices:
            mm_only = {"madmom": system_db_indices["madmom"]}
            mm_scores, mm_phase, mm_margin = vote_on_phase(mm_only, meter)
            mm_votes = float(np.sum(mm_scores))
            if (mm_margin is None or mm_margin >= min_margin) and mm_votes >= 2.0:
                best_phase = mm_phase
                margin = mm_margin
                meta["phase"] = best_phase
                meta["margin"] = round(margin, 3) if margin is not None else None
                confidence = "madmom_tiebreak"
                madmom_resolved = True

        if not madmom_resolved and not use_madmom_grid:
            # Try switching to madmom's own beat grid as a fallback.
            # The consensus grid may be noisy enough to blur the phase vote
            # even when cv < 0.35. Madmom's grid is typically much cleaner.
            mm = system_beats.get("madmom", {})
            mm_beats_fb = np.array(mm.get("beats", []))
            mm_dbs_fb = np.array(mm.get("downbeats", []))
            if len(mm_beats_fb) >= MIN_BEATS and len(mm_dbs_fb) >= 2:
                _, mm_cv_fb = check_beat_regularity(mm_beats_fb)
                if mm_cv_fb < MADMOM_GRID_MAX_CV:
                    # Re-do phase voting with madmom grid
                    fb_indices = {}
                    for sn in ["beat_this", "madmom"]:
                        if sn not in system_beats:
                            continue
                        dbt = np.array(system_beats[sn].get("downbeats", []))
                        if len(dbt) < 2:
                            continue
                        idx = map_downbeats_to_beat_indices(dbt, mm_beats_fb)
                        if len(idx) >= 2:
                            fb_indices[sn] = idx
                    if fb_indices:
                        fb_scores, fb_phase, fb_margin = vote_on_phase(
                            fb_indices, meter
                        )
                        if fb_margin is None or fb_margin >= min_margin:
                            best_phase = fb_phase
                            margin = fb_margin
                            phase_scores = fb_scores
                            total_votes = float(np.sum(fb_scores))
                            meta["phase"] = best_phase
                            meta["margin"] = round(margin, 3) if margin is not None else None
                            meta["total_votes"] = round(total_votes, 2)
                            meta["phase_scores"] = [
                                round(float(s), 2) for s in fb_scores
                            ]
                            beat_times = mm_beats_fb
                            use_madmom_grid = True
                            confidence = "madmom_grid"
                            madmom_resolved = True

        if not madmom_resolved:
            # Check if it's a half-bar ambiguity (diff of meter/2)
            sorted_phases = np.argsort(phase_scores)[::-1]
            phase_diff = abs(int(sorted_phases[0]) - int(sorted_phases[1]))
            is_half_bar = (phase_diff == meter // 2)

            if is_half_bar and margin is not None and margin >= 1.0:
                # Half-bar ambiguity with at least a slight preference.
                # Apply the grid — the meter structure is correct either way.
                confidence = "half_bar_ambiguous"
            else:
                margin_str = f"{margin:.2f}" if margin is not None else "unanimous"
                meta["reason"] = (
                    f"low margin ({margin_str} < {min_margin})"
                )
                return consensus, meta

    if use_madmom_grid:
        confidence = "madmom_grid"

    meta["confidence"] = confidence

    # Apply correction to the gap-filled hits.
    # When using the madmom grid as reference, map each beat to the nearest
    # madmom beat index and check phase against that index.
    meta["action"] = "corrected"
    corrected_hits = []
    n_corrected_db = 0

    for i, h in enumerate(hits):
        new_h = dict(h)
        if use_madmom_grid:
            # Map this beat to nearest madmom beat index
            dists = np.abs(beat_times - h["time"])
            nearest_mm_idx = int(np.argmin(dists))
            if dists[nearest_mm_idx] < MATCH_TOLERANCE * 2:
                is_db = (nearest_mm_idx % meter) == best_phase
            else:
                is_db = False
        else:
            is_db = (i % meter) == best_phase
        new_h["isDownbeat"] = is_db
        if is_db:
            n_corrected_db += 1
        corrected_hits.append(new_h)

    meta["corrected_downbeats"] = n_corrected_db

    corrected = dict(consensus)
    corrected["hits"] = corrected_hits
    corrected["downbeat_correction"] = {
        "version": "v4",
        "method": "madmom_grid_phase_vote" if use_madmom_grid else "grid_phase_vote",
        "meter": meter,
        "phase": best_phase,
        "margin": round(margin, 3),
        "confidence": confidence,
        "total_votes": round(total_votes, 2),
        "phase_scores": meta["phase_scores"],
        "systems_used": meta["systems_with_db"],
        "beats_inserted": n_beats_inserted,
    }

    return corrected, meta


# ---------------------------------------------------------------------------
# Batch processing
# ---------------------------------------------------------------------------

def load_system_beats(multi_dir: str, stem: str) -> dict[str, dict]:
    """Load per-system beat/downbeat annotations for a stem."""
    system_beats = {}
    for sys_name in ["beat_this", "madmom"]:
        path = os.path.join(multi_dir, f"{stem}.{sys_name}.beats.json")
        if os.path.exists(path):
            with open(path) as f:
                system_beats[sys_name] = json.load(f)
    return system_beats


def process_all(
    consensus_dir: str,
    multi_dir: str,
    output_dir: str | None,
    min_margin: float,
    analyze_only: bool,
) -> None:
    """Process all tracks: analyze and optionally write corrected labels."""

    files = sorted(os.listdir(consensus_dir))
    files = [f for f in files if f.endswith(".beats.json")]

    stats = Counter()
    confidence_hist = Counter()
    margin_values = []
    phase_diffs = []  # How many downbeats changed per track
    meter_hist = Counter()
    skip_reasons = Counter()
    total_beats_inserted = 0
    tracks_with_inserts = 0
    quarantine_stems = []  # Tracks to move to audio/quarantined/

    for i, fname in enumerate(files):
        stem = fname.replace(".beats.json", "")

        with open(os.path.join(consensus_dir, fname)) as f:
            consensus = json.load(f)

        system_beats = load_system_beats(multi_dir, stem)
        corrected, meta = correct_track_downbeats(
            consensus, system_beats, min_margin=min_margin
        )

        action = meta["action"]
        stats[action] += 1

        if action == "corrected":
            margin_values.append(meta["margin"])
            meter_hist[meta["meter"]] += 1
            confidence_hist[meta.get("confidence", "high")] += 1
            diff = abs(meta["corrected_downbeats"] - meta["original_downbeats"])
            phase_diffs.append(diff)
            n_ins = meta.get("n_beats_inserted", 0)
            total_beats_inserted += n_ins
            if n_ins > 0:
                tracks_with_inserts += 1

            if output_dir and not analyze_only:
                out_path = os.path.join(output_dir, fname)
                with open(out_path, "w") as f:
                    json.dump(corrected, f, indent=2)
        else:
            quarantine_stems.append(stem)
            reason = meta["reason"]
            # Aggregate skip reasons
            if "too few" in reason:
                skip_reasons["too_few_beats"] += 1
            elif "irregular" in reason:
                skip_reasons["irregular_grid"] += 1
            elif "no systems" in reason:
                skip_reasons["no_downbeats"] += 1
            elif "insufficient" in reason:
                skip_reasons["insufficient_votes"] += 1
            elif "low margin" in reason:
                skip_reasons["low_margin"] += 1
            else:
                skip_reasons[reason] += 1

            if not analyze_only and output_dir:
                # For uncorrectable tracks, zero out downbeats rather than
                # training on unreliable labels. Low-margin tracks keep their
                # AND-merge downbeats as a conservative fallback.
                output = dict(consensus)
                if "low margin" not in reason:
                    # Zero out downbeats for irregular/unusable tracks
                    output["hits"] = [
                        {**h, "isDownbeat": False} for h in consensus["hits"]
                    ]
                output["downbeat_correction"] = {
                    "version": "v4",
                    "method": "skipped",
                    "reason": reason,
                }
                out_path = os.path.join(output_dir, fname)
                with open(out_path, "w") as f:
                    json.dump(output, f, indent=2)

        if (i + 1) % 1000 == 0:
            print(f"  processed {i + 1}/{len(files)}...", file=sys.stderr)

    # Print analysis
    total = sum(stats.values())
    print(f"\n{'='*60}")
    print(f"Downbeat Correction Analysis ({total} tracks)")
    print(f"{'='*60}")
    print(f"\nActions:")
    for action, count in sorted(stats.items()):
        print(f"  {action:20s}: {count:5d} ({100*count/total:.1f}%)")

    corrected_count = stats.get("corrected", 0)
    if corrected_count > 0:
        finite_margins = [m for m in margin_values if np.isfinite(m)]
        print(f"\nCorrected tracks ({corrected_count}):")
        if finite_margins:
            print(f"  Margin (finite): mean={np.mean(finite_margins):.2f}, "
                  f"median={np.median(finite_margins):.2f}, "
                  f"min={np.min(finite_margins):.2f}")
        print(f"  Downbeat count change: "
              f"mean={np.mean(phase_diffs):.1f}, "
              f"max={np.max(phase_diffs)}")
        print(f"  Meter: {dict(meter_hist)}")
        print(f"  Confidence tiers:")
        for tier, count in confidence_hist.most_common():
            print(f"    {tier:25s}: {count:5d} ({100*count/corrected_count:.1f}%)")

    skipped = total - corrected_count
    print(f"\nSkipped ({skipped}):")
    for reason, count in skip_reasons.most_common():
        print(f"  {reason:25s}: {count:5d} ({100*count/total:.1f}%)")

    if corrected_count > 0:
        print(f"\n  Gap filling:")
        print(f"    Tracks with filled gaps: {tracks_with_inserts}")
        print(f"    Total beats inserted: {total_beats_inserted}")

    if output_dir and not analyze_only:
        print(f"\nOutput written to: {output_dir}")
        print(f"  {corrected_count} tracks corrected, "
              f"{skipped} copied as-is")

        # Move quarantined tracks out of audio dirs so they're excluded from
        # all downstream processing (labeling, separation, training)
        audio_root = Path(os.environ.get("BLINKY_DATA_ROOT",
                                          "/mnt/storage/blinky-ml-data")) / "audio"
        quarantine_dir = audio_root / "quarantined"
        quarantine_dir.mkdir(parents=True, exist_ok=True)
        moved = 0
        for stem in sorted(quarantine_stems):
            for subdir in ["combined", "fma"]:
                src = audio_root / subdir / f"{stem}.mp3"
                if src.exists():
                    src.rename(quarantine_dir / src.name)
                    moved += 1
        print(f"  Quarantined: {moved} files moved to {quarantine_dir}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Correct downbeat annotations using BPM-aware grid alignment"
    )
    parser.add_argument(
        "--consensus-dir",
        default="/mnt/storage/blinky-ml-data/labels/consensus_v3",
        help="Input consensus labels directory",
    )
    parser.add_argument(
        "--multi-dir",
        default="/mnt/storage/blinky-ml-data/labels/multi",
        help="Per-system labels directory",
    )
    parser.add_argument(
        "--output-dir",
        default="/mnt/storage/blinky-ml-data/labels/consensus_v4",
        help="Output directory for corrected labels",
    )
    parser.add_argument(
        "--analyze-only",
        action="store_true",
        help="Only print analysis, don't write output",
    )
    parser.add_argument(
        "--min-margin",
        type=float,
        default=DEFAULT_MIN_MARGIN,
        help=f"Minimum margin for phase selection (default: {DEFAULT_MIN_MARGIN})",
    )
    args = parser.parse_args()

    if not os.path.isdir(args.consensus_dir):
        print(f"Error: consensus dir not found: {args.consensus_dir}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isdir(args.multi_dir):
        print(f"Error: multi dir not found: {args.multi_dir}", file=sys.stderr)
        sys.exit(1)

    if not args.analyze_only:
        os.makedirs(args.output_dir, exist_ok=True)

    process_all(
        consensus_dir=args.consensus_dir,
        multi_dir=args.multi_dir,
        output_dir=args.output_dir if not args.analyze_only else None,
        min_margin=args.min_margin,
        analyze_only=args.analyze_only,
    )


if __name__ == "__main__":
    main()
