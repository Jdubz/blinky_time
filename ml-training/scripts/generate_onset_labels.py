#!/usr/bin/env python3
"""Generate librosa onset detection labels for the full training dataset.

For each audio file that has a consensus_v5 label, runs librosa.onset.onset_detect
at 16 kHz / hop_length=256 and saves onset times as JSON.

Output: /mnt/storage/blinky-ml-data/labels/onsets_librosa/{stem}.onsets.json
Format: {"onsets": [0.1, 0.35, ...], "count": 42,
         "source": "librosa.onset.onset_detect", "sr": 16000, "hop": 256}

Usage:
    python scripts/generate_onset_labels.py
    python scripts/generate_onset_labels.py --workers 8
    python scripts/generate_onset_labels.py --audio-dir /path/to/audio --labels-dir /path/to/labels
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import librosa
import numpy as np

# Defaults
AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio")
LABELS_DIR = Path("/mnt/storage/blinky-ml-data/labels/consensus_v5")
OUTPUT_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_librosa")
SR = 16000
HOP = 256


def process_track(args: tuple[Path, Path]) -> str | None:
    """Process a single track: load audio, detect onsets, save JSON.

    Returns the stem on success, or None on failure.
    """
    audio_path, output_path = args

    # Skip if already done (resumability)
    if output_path.exists():
        return None

    try:
        y, _ = librosa.load(str(audio_path), sr=SR, mono=True)
        onsets = librosa.onset.onset_detect(
            y=y, sr=SR, hop_length=HOP, backtrack=False, units="time"
        )
        result = {
            "onsets": [round(float(t), 4) for t in onsets],
            "count": len(onsets),
            "source": "librosa.onset.onset_detect",
            "sr": SR,
            "hop": HOP,
        }
        with open(output_path, "w") as f:
            json.dump(result, f)
        return audio_path.stem
    except Exception as e:
        print(f"ERROR processing {audio_path.stem}: {e}", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(
        description="Generate librosa onset labels for training dataset"
    )
    parser.add_argument(
        "--audio-dir", type=Path, default=AUDIO_DIR,
        help=f"Root audio directory (default: {AUDIO_DIR})"
    )
    parser.add_argument(
        "--labels-dir", type=Path, default=LABELS_DIR,
        help=f"Consensus labels directory (default: {LABELS_DIR})"
    )
    parser.add_argument(
        "--output-dir", type=Path, default=OUTPUT_DIR,
        help=f"Output directory for onset labels (default: {OUTPUT_DIR})"
    )
    parser.add_argument(
        "--workers", type=int, default=4,
        help="Number of parallel workers (default: 4)"
    )
    args = parser.parse_args()

    audio_dir = args.audio_dir
    labels_dir = args.labels_dir
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect stems from consensus labels
    label_stems = {f.stem.replace(".beats", "") for f in labels_dir.glob("*.beats.json")}
    print(f"Found {len(label_stems)} consensus labels in {labels_dir}")

    # Build audio file index: stem -> path (rglob, same strategy as prepare_dataset.py)
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_index: dict[str, Path] = {}
    for f in audio_dir.rglob("*"):
        if f.suffix.lower() in audio_extensions:
            audio_index[f.stem] = f
    print(f"Found {len(audio_index)} audio files in {audio_dir}")

    # Match: only process tracks that have both audio and consensus label
    work_items: list[tuple[Path, Path]] = []
    missing_audio = 0
    already_done = 0
    for stem in sorted(label_stems):
        if stem not in audio_index:
            missing_audio += 1
            continue
        out_path = output_dir / f"{stem}.onsets.json"
        if out_path.exists():
            already_done += 1
            continue
        work_items.append((audio_index[stem], out_path))

    total_matched = len(work_items) + already_done
    print(f"Matched {total_matched} tracks (audio + label)")
    if missing_audio > 0:
        print(f"  {missing_audio} labels have no matching audio (skipped)")
    if already_done > 0:
        print(f"  {already_done} already processed (skipped)")
    print(f"  {len(work_items)} tracks to process")

    if not work_items:
        print("Nothing to do.")
        return

    print(f"\nProcessing with {args.workers} workers...")
    t0 = time.time()
    done = 0
    errors = 0

    with Pool(processes=args.workers) as pool:
        for result in pool.imap_unordered(process_track, work_items, chunksize=4):
            done += 1
            if result is None:
                errors += 1
            if done % 100 == 0:
                elapsed = time.time() - t0
                rate = done / elapsed
                eta = (len(work_items) - done) / rate if rate > 0 else 0
                print(
                    f"  Progress: {done}/{len(work_items)} "
                    f"({done * 100 / len(work_items):.1f}%) "
                    f"- {rate:.1f} tracks/s - ETA {eta:.0f}s"
                )

    elapsed = time.time() - t0
    successful = done - errors
    print(f"\nDone! Processed {successful} tracks in {elapsed:.1f}s "
          f"({successful / elapsed:.1f} tracks/s)")
    if errors > 0:
        print(f"  {errors} tracks failed (see ERROR messages above)")

    # Final count
    total_outputs = len(list(output_dir.glob("*.onsets.json")))
    print(f"  Total onset label files: {total_outputs}")


if __name__ == "__main__":
    main()
