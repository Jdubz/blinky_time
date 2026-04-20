"""Run the deployed v27-hybrid NN offline on parity-harness features.

The parity harness dumps per-frame feature values that match firmware
bit-for-bit (up to float rounding). Feeding those features into the same
`.tflite` model the device ships gives an offline NN activation that
matches what the device would compute on the same pre-compressor inputs
— closing Phase 3 gap 5 (tflite-runtime vs tflite-micro equivalence).

TensorFlow's interpreter and TFLite Micro share the same Flatbuffer
spec and INT8 quantization semantics. For Conv1D INT8 models the output
matches bit-for-bit; differences, if any, would be library rounding
quirks at the float↔INT8 boundary, bounded by INT8 quantisation
(≤ 1 quantum difference).

Usage (called from run_parity_test.py --with-nn, or directly):

    ./venv/bin/python -m analysis.run_nn_offline \\
        --features outputs/parity/breakbeat-drive.harness.csv \\
        --model outputs/v27-hybrid-real/export/frame_onset_model_data_int8.tflite \\
        --out outputs/parity/breakbeat-drive.nn.csv

Emits one row per input frame. First `window_frames - 1` frames of the
output are zero-padded (the NN sees a filled window from frame
`window_frames - 1` onward).
"""

from __future__ import annotations

import argparse
import csv
import logging
import sys
from pathlib import Path

import numpy as np

log = logging.getLogger("nn_offline")


def load_features_csv(path: Path) -> tuple[list[str], np.ndarray]:
    """Read the parity harness CSV. Returns (column_names, (n_frames, n_cols))."""
    with path.open() as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [[float(x) for x in row] for row in reader]
    return header, np.asarray(rows, dtype=np.float32)


def assemble_windows(
    mel: np.ndarray,
    flatness: np.ndarray,
    raw_flux: np.ndarray,
    window_frames: int,
) -> np.ndarray:
    """Build sliding (n_frames, window_frames, n_features) input tensor.

    v27-hybrid input layout is [mel0..mel29, flatness, raw_flux] per frame,
    stacked over `window_frames` consecutive frames. First frames before a
    full window is available are zero-padded on the history axis so the
    first `window_frames - 1` predictions are anchored at zero-history.
    """
    n_frames, n_mel = mel.shape
    n_feat = n_mel + 2
    features = np.zeros((n_frames, n_feat), dtype=np.float32)
    features[:, :n_mel] = mel
    features[:, n_mel] = flatness
    features[:, n_mel + 1] = raw_flux
    padded = np.zeros((n_frames + window_frames - 1, n_feat), dtype=np.float32)
    padded[window_frames - 1 :] = features
    windows = np.stack(
        [padded[i : i + window_frames] for i in range(n_frames)],
        axis=0,
    )
    return windows


def run_nn(
    features_csv: Path,
    model_path: Path,
    window_frames: int,
) -> np.ndarray:
    """Invoke the tflite interpreter on each frame's window. Returns
    (n_frames,) float32 activation values in [0, 1]."""
    import tensorflow as tf  # local import — keeps module import cheap

    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    in_detail = interpreter.get_input_details()[0]
    out_detail = interpreter.get_output_details()[0]
    log.info(
        "TFLite input shape=%s dtype=%s, output shape=%s dtype=%s",
        in_detail["shape"],
        in_detail["dtype"],
        out_detail["shape"],
        out_detail["dtype"],
    )

    header, data = load_features_csv(features_csv)
    col_index = {name: i for i, name in enumerate(header)}
    mel_cols = [c for c in header if c.startswith("mel")]
    if not mel_cols:
        raise SystemExit("features CSV has no mel columns — harness needs to dump mel bands")
    mel = data[:, [col_index[c] for c in sorted(mel_cols, key=lambda x: int(x[3:]))]]
    flatness = data[:, col_index["flatness"]]
    raw_flux = data[:, col_index["raw_flux"]]
    n_frames = mel.shape[0]

    windows = assemble_windows(mel, flatness, raw_flux, window_frames)
    # Input-quantization step: match TFLite Micro's per-frame pipeline.
    in_scale, in_zero = in_detail["quantization"]
    out_scale, out_zero = out_detail["quantization"]
    in_dtype = in_detail["dtype"]

    activations = np.zeros(n_frames, dtype=np.float32)
    for i in range(n_frames):
        inp = windows[i : i + 1]  # (1, window_frames, n_feat)
        if in_dtype == np.int8:
            inp_q = np.round(inp / in_scale + in_zero).clip(-128, 127).astype(np.int8)
        else:
            inp_q = inp.astype(in_dtype)
        interpreter.set_tensor(in_detail["index"], inp_q)
        interpreter.invoke()
        out = interpreter.get_tensor(out_detail["index"])
        if out_detail["dtype"] == np.int8:
            act = (out.astype(np.float32) - out_zero) * out_scale
        else:
            act = out.astype(np.float32)
        # Model output layout per FrameOnsetNN: (1, time, channels). We take
        # the last time-step's channel-0 activation — that's the "current
        # frame" prediction, matching on-device per-frame invocation.
        activations[i] = float(act.flatten()[-1])

    return activations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--features", type=Path, required=True,
                        help="Parity harness CSV (must include mel0..melN-1, flatness, raw_flux)")
    parser.add_argument("--model", type=Path, required=True,
                        help=".tflite model path (typically outputs/vXX/export/*.tflite)")
    parser.add_argument("--out", type=Path, required=True,
                        help="Output CSV: frame,activation")
    parser.add_argument("--window-frames", type=int, default=16,
                        help="NN context window (v27-hybrid uses 16)")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    acts = run_nn(args.features, args.model, args.window_frames)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as f:
        f.write("frame,activation\n")
        for i, a in enumerate(acts):
            f.write(f"{i},{a:.6f}\n")
    log.info("Wrote %d activations to %s", len(acts), args.out)
    log.info(
        "Activation stats: mean=%.4f std=%.4f min=%.4f max=%.4f",
        float(acts.mean()), float(acts.std()), float(acts.min()), float(acts.max()),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
