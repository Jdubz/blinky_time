#!/usr/bin/env python3
"""BeatNet beat/downbeat detection helper script.

Runs in venv311 (Python 3.11) because BeatNet depends on madmom.
Called from label_beats.py via subprocess.

Usage (called by label_beats.py, not directly):
    venv311/bin/python scripts/_beatnet_helper.py /path/to/audio.mp3

Prints a JSON object to stdout:
    {
        "system": "beatnet",
        "beats": [0.5, 1.0, 1.5, ...],
        "downbeats": [0.5, 2.5, ...],
        "tempo": 120.0
    }
"""

import json
import sys

import numpy as np


def detect_beats(audio_path: str) -> dict:
    """Run BeatNet offline beat/downbeat tracking.

    BeatNet uses a CRNN + particle filter, architecturally distinct from
    Beat This! (TCN+Transformer) and madmom (RNN+DBN). This provides
    independent error characteristics for consensus labeling.

    Mode 1 = offline (best accuracy, processes full file).
    """
    # BeatNet imports pyaudio at top level (for streaming modes).
    # Mock it out since we only use offline mode.
    import types
    sys.modules.setdefault("pyaudio", types.ModuleType("pyaudio"))

    from BeatNet.BeatNet import BeatNet

    # mode=1: offline, inference_model=PF (particle filter for meter)
    estimator = BeatNet(
        1,  # mode: 1=offline
        inference_model="PF",  # particle filter (handles meter changes)
        plot=[],  # no plotting
        thread=False,  # no threading needed for offline
    )

    result = estimator.process(audio_path)
    # result is Nx2 array: [time, beat_position]
    # beat_position == 1 indicates a downbeat
    if result is None or len(result) == 0:
        return {
            "system": "beatnet",
            "beats": [],
            "downbeats": [],
            "tempo": 0.0,
        }

    beats = result[:, 0]
    downbeats = result[result[:, 1] == 1, 0]

    if len(beats) > 1:
        ibis = np.diff(beats)
        bpm = round(60.0 / float(np.median(ibis)), 1)
    else:
        bpm = 0.0

    return {
        "system": "beatnet",
        "beats": [round(float(t), 4) for t in beats],
        "downbeats": [round(float(t), 4) for t in downbeats],
        "tempo": bpm,
    }


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <audio_path>", file=sys.stderr)
        sys.exit(1)

    try:
        result = detect_beats(sys.argv[1])
        print(json.dumps(result))
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
