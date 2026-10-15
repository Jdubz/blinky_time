#!/usr/bin/env python3
"""Fail if the embedded FrameOnsetNN model can't accept the firmware's features.

`frame_onset_model_data.h` shipped the 80-mel v36 export against 50-mel
firmware from 2026-05-13 until it was caught: FrameOnsetNN::begin() rejects an
unknown input width (initError 5) and the device silently runs without its
onset NN. This check turns that into a build-time failure.

Accepted per-frame widths mirror FrameOnsetNN.h's input-mode detection:
N (mel only), N+2 (hybrid), N+3 (band flux, only valid at N=26), 2N (delta).

Usage:  python tools/check_model_input.py   (needs: pip install tflite)
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODEL_H = REPO / "blinky-things" / "audio" / "frame_onset_model_data.h"
SPECTRAL_H = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.h"


def main() -> int:
    try:
        import tflite  # type: ignore
    except ImportError:
        print("ERROR: pip install tflite", file=sys.stderr)
        return 2

    m = re.search(r"constexpr int NUM_MEL_BANDS = (\d+);", SPECTRAL_H.read_text())
    if not m:
        print(f"ERROR: NUM_MEL_BANDS not found in {SPECTRAL_H}", file=sys.stderr)
        return 2
    n_mel = int(m.group(1))

    src = MODEL_H.read_text()
    start = src.index("{", src.index("frame_onset_model_data[]"))
    body = src[start:src.index("};", start)]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))

    model = tflite.Model.GetRootAsModel(data, 0)
    graph = model.Subgraphs(0)
    shape = graph.Tensors(graph.Inputs(0)).ShapeAsNumpy()
    width = int(shape[-1])

    accepted = {n_mel: "mel only", n_mel + 2: "hybrid", n_mel * 2: "delta"}
    if n_mel == 26:
        accepted[n_mel + 3] = "band flux"
    if width not in accepted:
        print(f"FAIL: model input {list(shape)}: per-frame width {width} does not match "
              f"NUM_MEL_BANDS={n_mel} (accepted: {sorted(accepted)}). FrameOnsetNN would "
              f"refuse it (initError 5) and the device would run without the onset NN.",
              file=sys.stderr)
        return 1
    print(f"OK: model input {list(shape)} = {accepted[width]} for NUM_MEL_BANDS={n_mel} "
          f"({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
