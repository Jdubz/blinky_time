#!/usr/bin/env python3
"""
Derive pattern ground truth from beat/onset labels for pattern memory testing.

Per track, produces a .pattern_gt.json file containing:
- Bar boundaries (from downbeat timestamps)
- Per-bar onset counts and density ratios
- Fill detection (onset density > 2× rolling average)
- Silence gaps (>2s between consecutive onsets)
- Section boundaries (>50% density change across 4-bar phrases)
- Phrase repeats (cosine similarity of 4-bar density profiles >0.80)

Usage:
    python3 derive_pattern_ground_truth.py --music-dir blinky-test-player/music/edm
"""

import argparse
import json
import math
import os
import sys
from pathlib import Path


def load_beats(beats_path):
    """Load beat timestamps with downbeat flags."""
    with open(beats_path) as f:
        data = json.load(f)
    return data.get("hits", [])


def load_onsets(onsets_path):
    """Load onset timestamps."""
    with open(onsets_path) as f:
        data = json.load(f)
    return data.get("onsets", [])


def load_manifest(music_dir):
    """Load track manifest."""
    manifest_path = os.path.join(music_dir, "track_manifest.json")
    with open(manifest_path) as f:
        data = json.load(f)
    # Filter out non-track keys
    return {k: v for k, v in data.items() if isinstance(v, dict) and "duration" in v}


def extract_bar_boundaries(hits):
    """Extract bar start times from downbeat flags."""
    downbeats = [h["time"] for h in hits if h.get("isDownbeat", False)]
    return sorted(downbeats)


def count_onsets_in_range(onsets, start, end):
    """Count onsets within [start, end)."""
    count = 0
    for t in onsets:
        if t >= start and t < end:
            count += 1
    return count


def detect_fills(bars, rolling_window=4):
    """Detect fill bars: onset count > 2× rolling 4-bar average."""
    fills = []
    for i, bar in enumerate(bars):
        # Compute rolling average of preceding bars
        start_idx = max(0, i - rolling_window)
        window = bars[start_idx:i]
        if len(window) == 0:
            continue
        avg_count = sum(b["onsetCount"] for b in window) / len(window)
        if avg_count > 0:
            ratio = bar["onsetCount"] / avg_count
        else:
            ratio = 0.0
        bar["densityRatio"] = round(ratio, 2)
        if ratio > 2.0:
            fills.append({
                "barIndex": bar["index"],
                "startTime": round(bar["startTime"], 3),
                "endTime": round(bar["endTime"], 3),
                "densityRatio": round(ratio, 2),
            })
    # Backfill densityRatio for bars that didn't get one
    for bar in bars:
        if "densityRatio" not in bar:
            bar["densityRatio"] = 1.0
    return fills


def detect_silence_gaps(onsets, min_gap=2.0):
    """Detect silence gaps: consecutive onsets > min_gap seconds apart."""
    gaps = []
    for i in range(1, len(onsets)):
        dt = onsets[i] - onsets[i - 1]
        if dt > min_gap:
            gaps.append({
                "startTime": round(onsets[i - 1], 3),
                "endTime": round(onsets[i], 3),
                "durationSec": round(dt, 2),
            })
    return gaps


def detect_sections(bars, phrase_len=4):
    """Detect sections: 4-bar phrase density changes > 50% → new section."""
    if len(bars) == 0:
        return []

    # Group bars into phrases
    phrases = []
    for i in range(0, len(bars), phrase_len):
        phrase_bars = bars[i : i + phrase_len]
        avg_density = sum(b["onsetCount"] for b in phrase_bars) / len(phrase_bars)
        phrases.append({
            "startBar": phrase_bars[0]["index"],
            "endBar": phrase_bars[-1]["index"],
            "avgDensity": avg_density,
        })

    if len(phrases) == 0:
        return []

    # Assign section labels based on density
    sections = [phrases[0].copy()]
    for i in range(1, len(phrases)):
        prev_density = phrases[i - 1]["avgDensity"]
        curr_density = phrases[i]["avgDensity"]
        # Check for >50% change
        if prev_density > 0:
            change = abs(curr_density - prev_density) / prev_density
        else:
            change = 1.0 if curr_density > 0 else 0.0

        if change > 0.5:
            # New section
            sections.append(phrases[i].copy())
        else:
            # Extend current section
            sections[-1]["endBar"] = phrases[i]["endBar"]
            # Update average density
            start_bar = sections[-1]["startBar"]
            end_bar = sections[-1]["endBar"]
            section_bars = [b for b in bars if start_bar <= b["index"] <= end_bar]
            sections[-1]["avgDensity"] = (
                sum(b["onsetCount"] for b in section_bars) / len(section_bars)
                if section_bars
                else 0
            )

    # Label sections by density
    for s in sections:
        d = s["avgDensity"]
        if d < 2:
            s["label"] = "sparse_intro" if s["startBar"] < 4 else "sparse"
        elif d < 5:
            s["label"] = "moderate"
        else:
            s["label"] = "dense_A"
        s["avgDensity"] = round(s["avgDensity"], 2)

    return sections


def cosine_similarity(a, b):
    """Cosine similarity between two vectors."""
    dot = sum(x * y for x, y in zip(a, b))
    norm_a = math.sqrt(sum(x * x for x in a))
    norm_b = math.sqrt(sum(x * x for x in b))
    if norm_a < 1e-8 or norm_b < 1e-8:
        return 0.0
    return dot / (norm_a * norm_b)


def detect_phrase_repeats(bars, phrase_len=4, min_similarity=0.80, min_gap=2):
    """Detect phrase repeats: cosine similarity of 4-bar density profiles > threshold."""
    repeats = []
    # Build density profiles for each 4-bar phrase
    profiles = []
    for i in range(0, len(bars) - phrase_len + 1, phrase_len):
        phrase_bars = bars[i : i + phrase_len]
        profile = [b["onsetCount"] for b in phrase_bars]
        profiles.append((i, phrase_bars, profile))

    for i, (idx_a, bars_a, prof_a) in enumerate(profiles):
        for j, (idx_b, bars_b, prof_b) in enumerate(profiles):
            if j <= i + min_gap // phrase_len:
                continue  # Skip adjacent and self
            sim = cosine_similarity(prof_a, prof_b)
            if sim >= min_similarity:
                repeats.append({
                    "firstBars": [b["index"] for b in bars_a],
                    "repeatBars": [b["index"] for b in bars_b],
                    "similarity": round(sim, 3),
                })

    return repeats


def process_track(track_name, music_dir, manifest_entry):
    """Process a single track and return ground truth dict."""
    beats_path = os.path.join(music_dir, f"{track_name}.beats.json")
    onsets_path = os.path.join(music_dir, f"{track_name}.onsets.json")

    if not os.path.exists(beats_path) or not os.path.exists(onsets_path):
        print(f"  SKIP {track_name}: missing beats/onsets file")
        return None

    hits = load_beats(beats_path)
    onsets = load_onsets(onsets_path)
    gt_bpm = manifest_entry.get("groundTruthBpm", 0)
    duration = manifest_entry.get("duration", 0)

    # Extract bar boundaries from downbeats
    bar_starts = extract_bar_boundaries(hits)
    if len(bar_starts) < 2:
        print(f"  SKIP {track_name}: insufficient downbeats ({len(bar_starts)})")
        return None

    # Filter out short bars from downbeat label jitter (< 50% of median IBI)
    bar_ibis_raw = [bar_starts[i + 1] - bar_starts[i] for i in range(len(bar_starts) - 1)]
    median_ibi = sorted(bar_ibis_raw)[len(bar_ibis_raw) // 2]
    min_bar_dur = median_ibi * 0.5
    bar_starts = [bar_starts[0]] + [
        bar_starts[i + 1] for i in range(len(bar_starts) - 1)
        if bar_ibis_raw[i] >= min_bar_dur
    ]

    # Compute bar period (barPeriodMs = mean inter-downbeat interval, not necessarily 4 beats)
    bar_ibis = [bar_starts[i + 1] - bar_starts[i] for i in range(len(bar_starts) - 1)]
    bar_period_ms = (sum(bar_ibis) / len(bar_ibis)) * 1000.0

    # Build per-bar data
    bars = []
    for i in range(len(bar_starts) - 1):
        start = bar_starts[i]
        end = bar_starts[i + 1]
        onset_count = count_onsets_in_range(onsets, start, end)
        bars.append({
            "index": i,
            "startTime": round(start, 3),
            "endTime": round(end, 3),
            "onsetCount": onset_count,
            "isFill": False,
        })
    # Last bar: extend to duration or last onset
    if bar_starts:
        last_start = bar_starts[-1]
        last_end = last_start + (bar_period_ms / 1000.0)
        if last_end > duration:
            last_end = duration
        onset_count = count_onsets_in_range(onsets, last_start, last_end)
        bars.append({
            "index": len(bar_starts) - 1,
            "startTime": round(last_start, 3),
            "endTime": round(last_end, 3),
            "onsetCount": onset_count,
            "isFill": False,
        })

    # Detect fills
    fills = detect_fills(bars)
    for f in fills:
        for b in bars:
            if b["index"] == f["barIndex"]:
                b["isFill"] = True

    # Detect silence gaps
    silence_gaps = detect_silence_gaps(onsets)

    # Detect sections
    sections = detect_sections(bars)

    # Detect phrase repeats
    phrase_repeats = detect_phrase_repeats(bars)

    result = {
        "track": track_name,
        "groundTruthBpm": gt_bpm,
        "duration": round(duration, 2),
        "barCount": len(bars),
        "barPeriodMs": round(bar_period_ms, 1),
        "bars": [
            {
                "index": b["index"],
                "startTime": b["startTime"],
                "endTime": b["endTime"],
                "onsetCount": b["onsetCount"],
                "isFill": b["isFill"],
                "densityRatio": b.get("densityRatio", 1.0),
            }
            for b in bars
        ],
        "fills": fills,
        "silenceGaps": silence_gaps,
        "sections": sections,
        "phraseRepeats": phrase_repeats,
    }

    return result


def main():
    parser = argparse.ArgumentParser(
        description="Derive pattern ground truth from beat/onset labels"
    )
    parser.add_argument(
        "--music-dir",
        default="blinky-test-player/music/edm",
        help="Directory containing .beats.json, .onsets.json, and track_manifest.json",
    )
    parser.add_argument(
        "--tracks",
        default=None,
        help="Comma-separated list of track names (default: all)",
    )
    args = parser.parse_args()

    music_dir = args.music_dir
    if not os.path.isdir(music_dir):
        print(f"ERROR: Music directory not found: {music_dir}")
        sys.exit(1)

    manifest = load_manifest(music_dir)
    if not manifest:
        print(f"ERROR: No tracks in manifest at {music_dir}/track_manifest.json")
        sys.exit(1)

    if args.tracks:
        track_names = [t.strip() for t in args.tracks.split(",")]
    else:
        track_names = sorted(manifest.keys())

    print(f"Processing {len(track_names)} tracks from {music_dir}")

    for track_name in track_names:
        if track_name not in manifest:
            print(f"  SKIP {track_name}: not in manifest")
            continue

        result = process_track(track_name, music_dir, manifest[track_name])
        if result is None:
            continue

        output_path = os.path.join(music_dir, f"{track_name}.pattern_gt.json")
        with open(output_path, "w") as f:
            json.dump(result, f, indent=2)

        fill_count = len(result["fills"])
        gap_count = len(result["silenceGaps"])
        section_count = len(result["sections"])
        repeat_count = len(result["phraseRepeats"])
        print(
            f"  {track_name}: {result['barCount']} bars, "
            f"{fill_count} fills, {gap_count} gaps, "
            f"{section_count} sections, {repeat_count} repeats"
        )

    print("Done.")


if __name__ == "__main__":
    main()
