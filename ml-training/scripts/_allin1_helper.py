#!/usr/bin/env python3
"""All-In-One music structure analyzer helper script.

Runs in venv311 (Python 3.11) because allin1 depends on madmom.
Called from label_beats.py via subprocess.

All-In-One provides beats, downbeats, beat positions (1/2/3/4), and
structural segments (intro/verse/chorus/bridge/outro). The structure
awareness improves downbeat placement by understanding musical form.

Usage (called by label_beats.py, not directly):
    venv311/bin/python scripts/_allin1_helper.py /path/to/audio.mp3

Prints a JSON object to stdout:
    {
        "system": "allin1",
        "beats": [0.5, 1.0, 1.5, ...],
        "downbeats": [0.5, 2.5, ...],
        "tempo": 120.0,
        "segments": [{"start": 0.0, "end": 30.0, "label": "verse"}, ...]
    }
"""

import json
import logging
import os
import sys
import warnings

# Suppress natten deprecation warnings (thousands of lines per track)
warnings.filterwarnings("ignore", module="natten")
logging.getLogger("natten").setLevel(logging.ERROR)

import numpy as np

from _allin1_common import patch_dbn_threshold


def analyze(audio_path: str) -> dict:
    """Run All-In-One analysis for beats, downbeats, and segments."""
    patch_dbn_threshold()
    import allin1

    # Redirect stdout → stderr at the file descriptor level during analyze()
    # because Demucs writes to C-level stdout (not Python sys.stdout),
    # which contaminates the JSON output that label_beats.py parses.
    stdout_fd = sys.stdout.fileno()
    stderr_fd = sys.stderr.fileno()
    saved_stdout_fd = os.dup(stdout_fd)
    os.dup2(stderr_fd, stdout_fd)
    try:
        result = allin1.analyze(audio_path)
    finally:
        os.dup2(saved_stdout_fd, stdout_fd)
        os.close(saved_stdout_fd)

    beats = [round(float(t), 4) for t in result.beats]
    downbeats = [round(float(t), 4) for t in result.downbeats]

    # Extract structural segments if available
    segments = []
    if hasattr(result, "segments") and result.segments:
        for seg in result.segments:
            segments.append({
                "start": round(float(seg.start), 4),
                "end": round(float(seg.end), 4),
                "label": str(seg.label),
            })

    if len(beats) > 1:
        ibis = np.diff(beats)
        bpm = round(60.0 / float(np.median(ibis)), 1)
    else:
        bpm = 0.0

    return {
        "system": "allin1",
        "beats": beats,
        "downbeats": downbeats,
        "tempo": bpm,
        "segments": segments,
    }


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <audio_path>", file=sys.stderr)
        sys.exit(1)

    try:
        result = analyze(sys.argv[1])
        print(json.dumps(result))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
