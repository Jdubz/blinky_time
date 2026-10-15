#!/usr/bin/env python3
"""Fail if the embedded FrameOnsetNN model would be refused by the firmware.

`frame_onset_model_data.h` shipped the 80-mel v36 export against 50-mel
firmware from 2026-05-13 until it was caught: FrameOnsetNN::begin() rejected
it (initError 5) and every device silently ran without its onset NN. This
mirrors begin()'s static checks so any model it would refuse fails the build:

  4  placeholder model (< 100 bytes)
  2  schema version != TFLITE_SCHEMA_VERSION (3)
  5  per-frame width not N (mel), N+2 (hybrid), N+BAND_FLUX_CHANNELS
     (band flux) or 2N (delta)
  9  band-flux width at N != 26 (computeBandFlux hard-codes the 26-band split)
  8  per-frame width > MAX_INPUT_FEATURES
  6  window frames > MAX_WINDOW_FRAMES
  7  output channels outside 1..4 (or fewer outputs than channels), or an
     int8 input with a non-positive scale

Not checked (needs the TFLite Micro runtime): AllocateTensors failing for
arena size or an unsupported op (initErrors 1 and 3).

Constants are read from the firmware headers, so they can't drift.

Usage:  python tools/check_model_input.py   (needs: pip install tflite)
"""

import math
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODEL_H = REPO / "blinky-things" / "audio" / "frame_onset_model_data.h"
SPECTRAL_H = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.h"
NN_H = REPO / "blinky-things" / "audio" / "FrameOnsetNN.h"
TFLITE_SCHEMA_VERSION = 3
INT8 = 9  # tflite.TensorType.INT8


def const(path: Path, pattern: str) -> int:
    m = re.search(pattern, path.read_text(encoding="utf-8"))
    if not m:
        print(f"ERROR: {pattern!r} not found in {path}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1))


def main() -> int:
    try:
        import tflite  # type: ignore
    except ImportError:
        print("ERROR: pip install tflite", file=sys.stderr)
        return 2

    n_mel = const(SPECTRAL_H, r"constexpr int NUM_MEL_BANDS = (\d+);")
    flux_ch = const(NN_H, r"BAND_FLUX_CHANNELS\s*=\s*(\d+)")
    max_window = const(NN_H, r"MAX_WINDOW_FRAMES\s*=\s*(\d+)")
    max_features = 2 * n_mel  # MAX_INPUT_FEATURES = INPUT_MEL_BANDS * 2
    if not re.search(r"MAX_INPUT_FEATURES\s*=\s*INPUT_MEL_BANDS\s*\*\s*2",
                     NN_H.read_text(encoding="utf-8")):
        print("ERROR: MAX_INPUT_FEATURES definition changed; update this check", file=sys.stderr)
        return 2

    src = MODEL_H.read_text(encoding="utf-8")
    start = src.index("{", src.index("frame_onset_model_data[]"))
    body = src[start:src.index("};", start)]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))

    errors = []
    if len(data) < 100:
        print(f"FAIL: model is {len(data)} bytes (placeholder, initError 4)", file=sys.stderr)
        return 1
    model = tflite.Model.GetRootAsModel(data, 0)
    if model.Version() != TFLITE_SCHEMA_VERSION:
        errors.append(f"schema version {model.Version()} != {TFLITE_SCHEMA_VERSION} (initError 2)")

    graph = model.Subgraphs(0)
    inp = graph.Tensors(graph.Inputs(0))
    out = graph.Tensors(graph.Outputs(0))
    in_shape = [int(d) for d in inp.ShapeAsNumpy()]
    out_shape = [int(d) for d in out.ShapeAsNumpy()]
    width = in_shape[-1]

    modes = {n_mel: "mel only", n_mel + 2: "hybrid", n_mel * 2: "delta"}
    modes.setdefault(n_mel + flux_ch, "band flux")
    mode = modes.get(width)
    if mode is None:
        errors.append(f"per-frame width {width} does not match NUM_MEL_BANDS={n_mel} "
                      f"(accepted: {sorted(modes)}) (initError 5)")
    elif mode == "band flux" and n_mel != 26:
        errors.append(f"band-flux model needs 26 mel bands, firmware has {n_mel} (initError 9)")
    if width > max_features:
        errors.append(f"per-frame width {width} > MAX_INPUT_FEATURES={max_features} (initError 8)")
    window = math.prod(in_shape) // width if width else 0
    if window > max_window:
        errors.append(f"window {window} frames > MAX_WINDOW_FRAMES={max_window} (initError 6)")

    channels = out_shape[-1] if out_shape else 0
    if not 1 <= channels <= 4 or math.prod(out_shape) < channels:
        errors.append(f"output shape {out_shape}: channels {channels} not in 1..4 (initError 7)")
    if inp.Type() == INT8:
        q = inp.Quantization()
        scale = q.Scale(0) if q is not None and q.ScaleLength() > 0 else 0.0
        if not scale > 0.0:
            errors.append(f"int8 input with scale {scale} (initError 7)")

    if errors:
        for e in errors:
            print(f"FAIL: model input {in_shape}, output {out_shape}: {e}. "
                  "FrameOnsetNN would refuse it and the device would run without the onset NN.",
                  file=sys.stderr)
        return 1
    print(f"OK: input {in_shape} = {mode}, window {window} frames, output {out_shape}, "
          f"for NUM_MEL_BANDS={n_mel} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
