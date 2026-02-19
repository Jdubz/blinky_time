#!/usr/bin/env python3
"""
convert-ballroom-beats.py - Convert Ballroom .beats annotations to blinky ground truth format.

Reads .beats files from the BallroomAnnotations repo and produces .beats.json files
compatible with the blinky test player's ground truth format.

The Ballroom .beats format is:
  timestamp beat_position
  9.430022675 3

Where beat_position 1 = downbeat, 2+ = other beats in the bar.

Usage:
  python convert-ballroom-beats.py BallroomAnnotations/ --output-dir ballroom-gt/
  python convert-ballroom-beats.py BallroomAnnotations/Albums-Ballroom_Classics4-01.beats
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np


def convert_beats_file(beats_path: str, audio_duration_ms: int | None = None) -> dict:
    """Convert a Ballroom .beats file to blinky ground truth format."""
    filename = Path(beats_path).stem

    beats = []
    with open(beats_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) >= 2:
                time = float(parts[0])
                beat_pos = int(parts[1])
                beats.append((time, beat_pos))

    if not beats:
        return {
            "pattern": filename,
            "durationMs": audio_duration_ms or 0,
            "bpm": 0,
            "hits": [],
        }

    # Estimate BPM from inter-beat intervals
    times = [b[0] for b in beats]
    if len(times) >= 2:
        ibis = np.diff(times)
        median_ibi = np.median(ibis)
        estimated_bpm = round(60.0 / median_ibi, 1)
    else:
        estimated_bpm = 0

    # Estimate duration from last beat + one beat interval
    if audio_duration_ms is None:
        last_beat = times[-1]
        audio_duration_ms = int((last_beat + (median_ibi if len(times) >= 2 else 1.0)) * 1000)

    # Build ground truth hits
    hits = []
    for time, beat_pos in beats:
        is_downbeat = beat_pos == 1
        strength = 1.0 if is_downbeat else 0.85

        hits.append({
            "time": round(time, 4),
            "type": "low",
            "instrument": "beat",
            "strength": strength,
            "expectTrigger": True,
        })

    return {
        "pattern": filename,
        "durationMs": audio_duration_ms,
        "bpm": estimated_bpm,
        "hits": hits,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Convert Ballroom .beats annotations to blinky ground truth format"
    )
    parser.add_argument(
        "input",
        nargs="+",
        help=".beats file(s) or directory containing .beats files",
    )
    parser.add_argument(
        "--output-dir", "-o",
        help="Output directory for .beats.json files (default: same directory as input)",
    )

    args = parser.parse_args()

    # Collect all .beats files
    beats_files = []
    for path in args.input:
        if os.path.isdir(path):
            for f in sorted(os.listdir(path)):
                if f.endswith('.beats'):
                    beats_files.append(os.path.join(path, f))
        elif path.endswith('.beats'):
            beats_files.append(path)

    if not beats_files:
        print("No .beats files found", file=sys.stderr)
        sys.exit(1)

    output_dir = args.output_dir
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    print(f"Converting {len(beats_files)} .beats files...", file=sys.stderr)

    for beats_path in beats_files:
        result = convert_beats_file(beats_path)
        name = Path(beats_path).stem

        if output_dir:
            out_path = os.path.join(output_dir, f"{name}.beats.json")
        else:
            out_path = beats_path.replace('.beats', '.beats.json')

        with open(out_path, 'w') as f:
            json.dump(result, f, indent=2)

    print(f"Converted {len(beats_files)} files", file=sys.stderr)

    # Print summary stats
    bpms = []
    for beats_path in beats_files:
        result = convert_beats_file(beats_path)
        if result['bpm'] > 0:
            bpms.append(result['bpm'])

    if bpms:
        print(f"BPM range: {min(bpms):.0f} - {max(bpms):.0f}", file=sys.stderr)
        print(f"Median BPM: {np.median(bpms):.0f}", file=sys.stderr)


if __name__ == "__main__":
    main()
