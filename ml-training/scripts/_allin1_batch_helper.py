#!/usr/bin/env python3
"""Batch All-In-One music structure analyzer helper script.

Runs in venv311 (Python 3.11) because allin1 depends on madmom.
Called from label_beats.py via a SINGLE long-lived subprocess.

Key optimization: loads Demucs + allin1 models ONCE, then processes
all tracks sequentially. This eliminates the ~4s per-track overhead
of subprocess-per-track mode (Python startup + torch import + model loading).

Communication protocol:
  - Reads newline-delimited JSON commands from stdin:
    {"audio_path": "/path/to/audio.mp3", "output_path": "/path/to/output.json"}
  - Writes newline-delimited JSON results to stdout:
    {"audio_path": "...", "success": true, "error": ""}
    {"audio_path": "...", "success": false, "error": "reason"}
  - EOF on stdin signals shutdown.
  - All diagnostic output goes to stderr.

Usage (called by label_beats.py, not directly):
    venv311/bin/python scripts/_allin1_batch_helper.py [--device cpu|cuda] [--demix-dir ./demix]
"""

import json
import os
import sys
import time
from pathlib import Path

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

    metrical.postprocess_metrical_structure = _patched
    helpers.postprocess_metrical_structure = _patched


def analyze_single(audio_path: Path, model, device, demix_dir, spec_dir):
    """Analyze a single track using pre-loaded models.

    Reimplements allin1.analyze() pipeline but with persistent model.
    """
    import torch
    from allin1.demix import demix
    from allin1.spectrogram import extract_spectrograms
    from allin1.helpers import run_inference
    from allin1.postprocessing.functional import postprocess_functional_structure

    # Step 1: Demucs separation (checks cache automatically)
    demix_paths = demix([audio_path], demix_dir, device)

    # Step 2: Spectrogram extraction (checks cache automatically)
    spec_paths = extract_spectrograms(demix_paths, spec_dir, multiprocess=False)

    # Step 3: NN inference with pre-loaded model
    with torch.no_grad():
        result = run_inference(
            path=audio_path,
            spec_path=spec_paths[0],
            model=model,
            device=device,
            include_activations=False,
            include_embeddings=False,
        )

    # Step 4: Format output
    beats = [round(float(t), 4) for t in result.beats]
    downbeats = [round(float(t), 4) for t in result.downbeats]

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

    # Keep stems — they're reused for training data augmentation (drum-only
    # mel spectrograms). Only clean up spectrograms (allin1-specific format,
    # not needed by other tools).
    for spec_path in spec_paths:
        spec_path.unlink(missing_ok=True)

    # Free GPU memory between tracks to prevent fragmentation
    if torch.cuda.is_available() and device != "cpu":
        torch.cuda.empty_cache()

    return {
        "system": "allin1",
        "beats": beats,
        "downbeats": downbeats,
        "tempo": bpm,
        "segments": segments,
    }


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default=None,
                        help="Device: cuda or cpu (default: auto-detect)")
    parser.add_argument("--demix-dir", default="./demix",
                        help="Directory for Demucs stem cache (default: ./demix)")
    parser.add_argument("--spec-dir", default="./spec",
                        help="Directory for spectrogram cache (default: ./spec)")
    args = parser.parse_args()

    # Redirect stdout to stderr during initialization (Demucs writes to C stdout)
    stdout_fd = sys.stdout.fileno()
    stderr_fd = sys.stderr.fileno()
    saved_stdout_fd = os.dup(stdout_fd)

    os.dup2(stderr_fd, stdout_fd)

    t0 = time.perf_counter()

    # Apply patches
    _patch_dbn_threshold()

    # Import and load models ONCE
    import torch
    from allin1.models import load_pretrained_model

    if args.device is None:
        device = "cuda" if torch.cuda.is_available() else "cpu"
    else:
        device = args.device

    print(f"[allin1-batch] Loading model on {device}...", file=sys.stderr)
    model = load_pretrained_model("harmonix-all", device=device)
    print(f"[allin1-batch] Model loaded ({len(model.models)}-fold ensemble) "
          f"in {time.perf_counter() - t0:.1f}s", file=sys.stderr)

    demix_dir = Path(args.demix_dir)
    spec_dir = Path(args.spec_dir)
    demix_dir.mkdir(parents=True, exist_ok=True)
    spec_dir.mkdir(parents=True, exist_ok=True)

    # Restore stdout for JSON output
    os.dup2(saved_stdout_fd, stdout_fd)

    # Process tracks from stdin
    processed = 0
    errors = 0
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            cmd = json.loads(line)
        except json.JSONDecodeError as e:
            print(json.dumps({"audio_path": "", "success": False,
                              "error": f"invalid JSON: {e}"}),
                  flush=True)
            continue

        audio_path = cmd.get("audio_path", "")
        output_path = cmd.get("output_path", "")

        # Redirect stdout during analysis (Demucs C-level output),
        # restore in finally block to guarantee JSON response goes to stdout.
        os.dup2(stderr_fd, stdout_fd)
        try:
            t_track = time.perf_counter()
            result = analyze_single(
                Path(audio_path), model, device, demix_dir, spec_dir)

            # Save result
            if output_path:
                with open(output_path, "w") as f:
                    json.dump(result, f, indent=2)

            elapsed = time.perf_counter() - t_track
            processed += 1
            print(f"[allin1-batch] {processed}: {Path(audio_path).name} "
                  f"({elapsed:.1f}s, {len(result['beats'])} beats)",
                  file=sys.stderr)
            response = {"audio_path": audio_path, "success": True, "error": ""}

        except Exception as e:
            errors += 1
            response = {"audio_path": audio_path, "success": False,
                        "error": str(e)[:200]}
        finally:
            # Always restore stdout before writing JSON response
            os.dup2(saved_stdout_fd, stdout_fd)

        print(json.dumps(response), flush=True)

    # Restore stdout
    os.dup2(saved_stdout_fd, stdout_fd)
    os.close(saved_stdout_fd)

    print(f"[allin1-batch] Done: {processed} processed, {errors} errors",
          file=sys.stderr)


if __name__ == "__main__":
    main()
