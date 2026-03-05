#!/usr/bin/env python3
"""Auto-label audio files with beat positions using Beat This! (ISMIR 2024 SOTA).

Outputs .beats.json files compatible with blinky-test-player format.
Runs on GPU for fast inference (~1s per track on RTX 3080).

Usage:
    # Label all MP3s in a directory
    python scripts/label_beats.py --audio-dir /mnt/storage/blinky-ml-data/audio/fma

    # Label a single file
    python scripts/label_beats.py --audio-dir /path/to/file.mp3

    # Label existing test set
    python scripts/label_beats.py --audio-dir ../blinky-test-player/music/edm --output-dir data/labels/edm
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
from tqdm import tqdm


def label_file(model, audio_path: Path) -> dict:
    """Run Beat This! on a single audio file and return beats.json dict."""
    import librosa

    beats, downbeats = model(str(audio_path))

    # Get duration
    duration = librosa.get_duration(path=str(audio_path))

    # Estimate BPM from median inter-beat interval
    if len(beats) > 1:
        ibis = np.diff(beats)
        bpm = round(60.0 / np.median(ibis), 1)
    else:
        bpm = 0.0

    # Build hits array matching blinky-test-player format
    downbeat_set = set(np.round(downbeats, 4))
    hits = []
    for i, t in enumerate(beats):
        t_rounded = round(float(t), 4)
        is_downbeat = any(abs(t_rounded - db) < 0.035 for db in downbeat_set)
        hits.append({
            "time": t_rounded,
            "type": "low",
            "instrument": "beat",
            "strength": 1.0 if is_downbeat else 0.7,
            "expectTrigger": True,
        })

    return {
        "pattern": audio_path.stem,
        "durationMs": int(duration * 1000),
        "bpm": bpm,
        "hits": hits,
    }


def main():
    parser = argparse.ArgumentParser(description="Auto-label beats with Beat This!")
    parser.add_argument("--audio-dir", required=True, help="Directory of audio files (or single file)")
    parser.add_argument("--output-dir", default=None, help="Output directory for .beats.json (default: alongside audio)")
    parser.add_argument("--device", default="cuda", help="Inference device (cuda/cpu)")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing labels")
    parser.add_argument("--extensions", default=".mp3,.wav,.flac,.ogg", help="Audio file extensions")
    args = parser.parse_args()

    audio_path = Path(args.audio_dir)
    extensions = set(args.extensions.split(","))

    # Collect files
    if audio_path.is_file():
        files = [audio_path]
    elif audio_path.is_dir():
        files = sorted(f for f in audio_path.rglob("*") if f.suffix.lower() in extensions)
    else:
        print(f"Error: {audio_path} not found", file=sys.stderr)
        sys.exit(1)

    if not files:
        print(f"No audio files found in {audio_path}", file=sys.stderr)
        sys.exit(1)

    # Determine output directory
    output_dir = Path(args.output_dir) if args.output_dir else None
    if output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)

    # Filter already-labeled files
    if not args.overwrite:
        unlabeled = []
        for f in files:
            out_path = (output_dir or f.parent) / f"{f.stem}.beats.json"
            if not out_path.exists():
                unlabeled.append(f)
        skipped = len(files) - len(unlabeled)
        if skipped:
            print(f"Skipping {skipped} already-labeled files (use --overwrite to redo)")
        files = unlabeled

    if not files:
        print("All files already labeled.")
        return

    # Load model
    print(f"Loading Beat This! model on {args.device}...")
    from beat_this.inference import File2Beats
    model = File2Beats(device=args.device, dbn=False)
    print(f"Model loaded. Labeling {len(files)} files...")

    t0 = time.time()
    errors = []
    for f in tqdm(files, desc="Labeling"):
        try:
            result = label_file(model, f)
            out_path = (output_dir or f.parent) / f"{f.stem}.beats.json"
            with open(out_path, "w") as fp:
                json.dump(result, fp, indent=2)
        except Exception as e:
            errors.append((f.name, str(e)))
            tqdm.write(f"  ERROR: {f.name}: {e}")

    elapsed = time.time() - t0
    print(f"\nDone: {len(files) - len(errors)}/{len(files)} labeled in {elapsed:.1f}s "
          f"({elapsed/max(len(files),1):.1f}s/track)")
    if errors:
        print(f"\n{len(errors)} errors:")
        for name, err in errors:
            print(f"  {name}: {err}")


if __name__ == "__main__":
    main()
