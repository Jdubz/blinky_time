#!/usr/bin/env python3
"""Madmom beat/downbeat detection helper script.

This script is designed to be called from label_beats_multi.py via subprocess,
using a separate Python 3.11 venv where madmom is installed (madmom does not
support Python 3.12+).

Usage (called by label_beats_multi.py, not directly):
    venv311/bin/python scripts/_madmom_helper.py /path/to/audio.mp3

Prints a JSON object to stdout:
    {
        "system": "madmom",
        "beats": [0.5, 1.0, 1.5, ...],
        "downbeats": [0.5, 2.5, ...],
        "tempo": 120.0
    }

Errors are printed to stderr. Non-zero exit code on failure.
"""

import json
import sys

import numpy as np


def detect_beats_and_downbeats(audio_path: str) -> dict:
    """Run madmom RNN+DBN beat and downbeat tracking on an audio file.

    Uses:
      - RNNBeatProcessor + DBNBeatTrackingProcessor for beat positions
      - RNNDownBeatProcessor + DBNDownBeatTrackingProcessor for downbeats

    Returns dict with system, beats, downbeats, tempo.
    """
    import madmom

    # --- Beat detection ---
    beat_proc = madmom.features.beats.RNNBeatProcessor()(audio_path)
    beat_tracker = madmom.features.beats.DBNBeatTrackingProcessor(
        min_bpm=55, max_bpm=215, fps=100
    )
    beats = beat_tracker(beat_proc)  # 1D array of beat times in seconds

    # --- Downbeat detection ---
    try:
        downbeat_proc = madmom.features.downbeats.RNNDownBeatProcessor()(audio_path)
        downbeat_tracker = madmom.features.downbeats.DBNDownBeatTrackingProcessor(
            beats_per_bar=[3, 4], fps=100
        )
        downbeat_result = downbeat_tracker(downbeat_proc)
        # downbeat_result is Nx2 array: [time, beat_position]
        # beat_position == 1 indicates a downbeat
        downbeats = downbeat_result[downbeat_result[:, 1] == 1, 0]
    except Exception as e:
        # Downbeat detection can fail on some files; fall back to empty
        print(f"Warning: downbeat detection failed: {e}", file=sys.stderr)
        downbeats = np.array([])

    # Estimate BPM from median inter-beat interval
    if len(beats) > 1:
        ibis = np.diff(beats)
        bpm = round(60.0 / float(np.median(ibis)), 1)
    else:
        bpm = 0.0

    return {
        "system": "madmom",
        "beats": [round(float(t), 4) for t in beats],
        "downbeats": [round(float(t), 4) for t in downbeats],
        "tempo": bpm,
    }


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <audio_path>", file=sys.stderr)
        sys.exit(1)

    audio_path = sys.argv[1]

    try:
        result = detect_beats_and_downbeats(audio_path)
        print(json.dumps(result))
    except Exception as e:
        print(f"Error processing {audio_path}: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
