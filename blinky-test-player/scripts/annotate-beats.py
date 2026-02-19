#!/usr/bin/env python3
"""
annotate-beats.py - Generate beat annotations from audio files using librosa.

Uses librosa's beat tracking (dynamic programming + onset strength) to produce
ground truth beat annotations for testing the blinky audio system.

Output format matches the blinky test player's PatternOutput ground truth:
{
  "pattern": "<filename>",
  "durationMs": <duration>,
  "bpm": <estimated_bpm>,
  "hits": [
    {"time": 0.468, "type": "low", "instrument": "beat", "strength": 1.0, "expectTrigger": true},
    ...
  ]
}

Usage:
  python annotate-beats.py input.mp3 --output beats.json
  python annotate-beats.py input.wav --bpm 128  # override BPM estimate
  python annotate-beats.py music/*.wav --output-dir annotations/

Requirements:
  pip install librosa soundfile numpy
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np

try:
    import librosa
except ImportError:
    print("Error: librosa is required. Install with: pip install librosa soundfile", file=sys.stderr)
    sys.exit(1)


def annotate_beats(filepath: str, override_bpm: float | None = None,
                   use_downbeats: bool = True) -> dict:
    """
    Annotate beats in an audio file using librosa.

    Args:
        filepath: Path to audio file (WAV, MP3, FLAC, etc.)
        override_bpm: If set, use this BPM instead of estimating
        use_downbeats: If True, try to detect downbeats (beat 1) for stronger annotation

    Returns:
        Ground truth dict matching blinky test player format
    """
    filename = Path(filepath).stem

    # Load audio
    print(f"Processing: {filepath}", file=sys.stderr)
    y, sr = librosa.load(filepath, sr=22050, mono=True)
    duration_sec = librosa.get_duration(y=y, sr=sr)
    duration_ms = int(duration_sec * 1000)
    print(f"  Duration: {duration_sec:.1f}s, Sample rate: {sr}", file=sys.stderr)

    # Compute onset strength envelope for beat tracking
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)

    # Beat tracking
    if override_bpm:
        tempo, beat_frames = librosa.beat.beat_track(
            onset_envelope=onset_env, sr=sr, bpm=override_bpm, units='frames'
        )
    else:
        tempo, beat_frames = librosa.beat.beat_track(
            onset_envelope=onset_env, sr=sr, units='frames'
        )

    # Convert frames to times
    beat_times = librosa.frames_to_time(beat_frames, sr=sr)

    if len(beat_times) == 0:
        print(f"  Warning: No beats detected in {filepath}", file=sys.stderr)
        return {
            "pattern": filename,
            "durationMs": duration_ms,
            "bpm": override_bpm or 0,
            "hits": [],
        }

    # Estimate BPM from inter-beat intervals
    if len(beat_times) >= 2:
        ibis = np.diff(beat_times)
        median_ibi = np.median(ibis)
        estimated_bpm = 60.0 / median_ibi
    else:
        estimated_bpm = override_bpm or 120.0

    bpm = override_bpm or round(float(estimated_bpm), 1)

    # Extract tempo value (librosa may return array)
    tempo_val = float(np.atleast_1d(tempo)[0]) if hasattr(tempo, '__len__') else float(tempo)
    print(f"  Detected {len(beat_times)} beats, librosa tempo: {tempo_val:.1f}, IBI-estimated BPM: {estimated_bpm:.1f}", file=sys.stderr)
    if override_bpm:
        print(f"  Using override BPM: {override_bpm}", file=sys.stderr)

    # Downbeat detection for beat strength annotation
    downbeat_indices = set()
    if use_downbeats and len(beat_times) >= 4:
        try:
            # Use librosa's beat/bar estimation
            # Group beats into bars (assume 4/4 time by default)
            # The first beat of each group of 4 gets downbeat strength
            beats_per_bar = 4
            for i in range(0, len(beat_times), beats_per_bar):
                downbeat_indices.add(i)
            print(f"  Marked {len(downbeat_indices)} downbeats (every {beats_per_bar} beats)", file=sys.stderr)
        except Exception as e:
            print(f"  Downbeat detection failed: {e}", file=sys.stderr)

    # Get onset strengths at beat positions for relative strength annotation
    beat_strengths = onset_env[beat_frames] if len(beat_frames) > 0 else np.ones(len(beat_times))
    if beat_strengths.max() > 0:
        beat_strengths = beat_strengths / beat_strengths.max()  # Normalize to 0-1

    # Build ground truth hits
    hits = []
    for i, beat_time in enumerate(beat_times):
        beat_time = float(beat_time)

        # Combine downbeat info with onset strength
        is_downbeat = i in downbeat_indices
        base_strength = float(beat_strengths[i]) if i < len(beat_strengths) else 0.85
        # Downbeats get full strength, others scaled by onset strength
        strength = max(base_strength, 0.9) if is_downbeat else max(base_strength * 0.85, 0.5)

        hits.append({
            "time": round(beat_time, 4),
            "type": "low",  # Beats are generally low-band (kick/bass drum)
            "instrument": "beat",
            "strength": round(strength, 3),
            "expectTrigger": True,
        })

    result = {
        "pattern": filename,
        "durationMs": duration_ms,
        "bpm": bpm,
        "hits": hits,
    }

    print(f"  Generated {len(hits)} ground truth hits", file=sys.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Generate beat annotations from audio files using librosa"
    )
    parser.add_argument(
        "input",
        nargs="+",
        help="Audio file(s) to annotate (WAV, MP3, FLAC, etc.)",
    )
    parser.add_argument(
        "--output", "-o",
        help="Output file path (for single input). Default: stdout",
    )
    parser.add_argument(
        "--output-dir",
        help="Output directory (for multiple inputs). Creates <name>.beats.json files",
    )
    parser.add_argument(
        "--bpm",
        type=float,
        help="Override BPM estimate (use known tempo)",
    )
    parser.add_argument(
        "--no-downbeats",
        action="store_true",
        help="Skip downbeat detection (faster, all beats get equal strength)",
    )

    args = parser.parse_args()

    results = []
    for filepath in args.input:
        if not os.path.exists(filepath):
            print(f"Error: File not found: {filepath}", file=sys.stderr)
            sys.exit(1)

        result = annotate_beats(
            filepath,
            override_bpm=args.bpm,
            use_downbeats=not args.no_downbeats,
        )
        results.append((filepath, result))

    # Output results
    if args.output_dir:
        os.makedirs(args.output_dir, exist_ok=True)
        for filepath, result in results:
            name = Path(filepath).stem
            out_path = os.path.join(args.output_dir, f"{name}.beats.json")
            with open(out_path, "w") as f:
                json.dump(result, f, indent=2)
            print(f"Written: {out_path}", file=sys.stderr)
    elif args.output and len(results) == 1:
        with open(args.output, "w") as f:
            json.dump(results[0][1], f, indent=2)
        print(f"Written: {args.output}", file=sys.stderr)
    elif len(results) == 1:
        print(json.dumps(results[0][1], indent=2))
    else:
        # Multiple files, no output dir - print all as JSON array
        print(json.dumps([r for _, r in results], indent=2))


if __name__ == "__main__":
    main()
