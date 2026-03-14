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

import contextlib
import io
import json
import os
import sys

import numpy as np


def _patch_dbn_threshold():
    """Disable DBN threshold clipping for short clips.

    allin1's postprocessing passes best_threshold_downbeat (0.24) to madmom's
    DBNDownBeatTrackingProcessor, which crops the activation sequence to only
    frames exceeding the threshold before Viterbi decoding. After the 3-way
    normalization (beat/downbeat/no-beat), peak activations on 30s clips are
    compressed below this threshold, causing near-total beat loss. Setting
    threshold=None lets the HMM decode the full sequence — its tempo model
    and transition probabilities already handle noise.

    Must patch allin1.helpers (where the bound name lives), not just
    allin1.postprocessing.metrical (the defining module).
    """
    import allin1.helpers as helpers
    import allin1.postprocessing.metrical as metrical
    from allin1.typings import AllInOneOutput
    from allin1.config import Config

    _original = metrical.postprocess_metrical_structure

    def _patched(logits: AllInOneOutput, cfg: Config):
        orig_threshold = cfg.best_threshold_downbeat
        cfg.best_threshold_downbeat = None
        try:
            return _original(logits, cfg)
        finally:
            cfg.best_threshold_downbeat = orig_threshold

    # Patch both the defining module and the caller module (which captured
    # the name at import time via `from .postprocessing import ...`)
    metrical.postprocess_metrical_structure = _patched
    helpers.postprocess_metrical_structure = _patched


def analyze(audio_path: str) -> dict:
    """Run All-In-One analysis for beats, downbeats, and segments."""
    _patch_dbn_threshold()
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
