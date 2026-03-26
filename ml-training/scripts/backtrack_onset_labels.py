#!/usr/bin/env python3
"""Backtrack onset labels to the nearest spectral flux rising edge.

Onset detection systems report times near the energy peak or perceptual
attack time, which is 16-64ms AFTER the physical onset start. This script
snaps each onset label to the nearest preceding local minimum in the
spectral flux envelope — the exact moment energy starts rising.

This removes system-specific latency from the training labels, so the
model learns to fire at the true transient start, not the delayed
detection position.

Processes labels IN-PLACE. Audio is loaded per-track to compute the
onset strength envelope.

Usage:
    python scripts/backtrack_onset_labels.py --dry-run
    python scripts/backtrack_onset_labels.py
    python scripts/backtrack_onset_labels.py --max-lookback 4
"""

from __future__ import annotations

import argparse
import json
import time
from multiprocessing import Pool
from pathlib import Path

import librosa
import numpy as np

AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio/combined")
ONSET_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_consensus")
TEST_DIR = Path(__file__).parent.parent.parent / "blinky-test-player/music/edm"
SR = 16000
HOP = 256
FRAME_RATE = SR / HOP  # 62.5 Hz


def backtrack_onsets(onset_times: list[float], audio_path: Path,
                     max_lookback: int = 4) -> list[float]:
    """Snap each onset time to the nearest preceding spectral flux trough.

    For each onset, scan backward up to max_lookback frames in the onset
    strength envelope to find where energy started rising. Returns the
    adjusted times.

    max_lookback: max frames to scan backward (4 frames = 64ms at 62.5Hz)
    """
    y, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    onset_env = librosa.onset.onset_strength(y=y, sr=SR, hop_length=HOP)

    adjusted = []
    for t in onset_times:
        frame = int(t * FRAME_RATE)
        if frame >= len(onset_env) or frame < 1:
            adjusted.append(t)
            continue

        # Scan backward to find the trough (local minimum)
        best_frame = frame
        for lookback in range(1, max_lookback + 1):
            prev = frame - lookback
            if prev < 0:
                break
            if onset_env[prev] <= onset_env[prev + 1]:
                best_frame = prev
                break
            best_frame = prev  # Keep scanning if still descending

        adjusted.append(best_frame / FRAME_RATE)

    return adjusted


def process_track(args: tuple) -> tuple[str, int, float] | None:
    """Process one track. Returns (stem, num_shifted, mean_shift_ms) or None."""
    onset_path, audio_path, max_lookback, dry_run = args
    stem = onset_path.stem.replace(".onsets_consensus", "").replace(".onsets", "")

    try:
        with open(onset_path) as f:
            data = json.load(f)

        onsets = data["onsets"]
        if not onsets:
            return None

        orig_times = [o["time"] for o in onsets]
        new_times = backtrack_onsets(orig_times, audio_path, max_lookback)

        shifts = [(orig - new) * 1000 for orig, new in zip(orig_times, new_times)]
        num_shifted = sum(1 for s in shifts if s > 1)  # >1ms shift
        mean_shift = np.mean(shifts) if shifts else 0

        if not dry_run and num_shifted > 0:
            for i, new_t in enumerate(new_times):
                onsets[i]["time"] = round(new_t, 4)
            data["onsets"] = onsets
            data["backtracked"] = True
            data["backtrack_max_lookback"] = max_lookback
            with open(onset_path, "w") as f:
                json.dump(data, f)

        return (stem, num_shifted, mean_shift)

    except Exception as e:
        print(f"ERROR {stem}: {e}")
        return None


def main():
    parser = argparse.ArgumentParser(description="Backtrack onset labels to spectral flux rising edge")
    parser.add_argument("--onset-dir", type=Path, default=ONSET_DIR)
    parser.add_argument("--audio-dir", type=Path, default=AUDIO_DIR)
    parser.add_argument("--test-dir", type=Path, default=TEST_DIR)
    parser.add_argument("--max-lookback", type=int, default=4,
                        help="Max frames to scan backward (default: 4 = 64ms)")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    # Build audio index
    audio_index = {}
    for f in args.audio_dir.iterdir():
        if f.suffix.lower() in {".mp3", ".wav", ".flac"}:
            audio_index[f.stem] = f

    # Collect work items from both training and test labels
    work_items = []
    for d in [args.onset_dir, args.test_dir]:
        if not d.exists():
            continue
        for f in sorted(d.glob("*.onsets*.json")):
            stem = f.stem.replace(".onsets_consensus", "").replace(".onsets", "")
            # Skip non-consensus formats (librosa plain float arrays)
            try:
                with open(f) as fp:
                    data = json.load(fp)
                if not isinstance(data.get("onsets", [{}])[0], dict):
                    continue
            except (json.JSONDecodeError, IndexError):
                continue

            audio = audio_index.get(stem)
            if audio is None:
                # Check test dir for audio
                for ext in [".mp3", ".wav", ".flac"]:
                    test_audio = args.test_dir / f"{stem}{ext}"
                    if test_audio.exists():
                        audio = test_audio
                        break
            if audio:
                work_items.append((f, audio, args.max_lookback, args.dry_run))

    print(f"{'DRY RUN — ' if args.dry_run else ''}Backtracking {len(work_items)} label files")
    print(f"  Max lookback: {args.max_lookback} frames ({args.max_lookback * 16}ms)")

    t0 = time.time()
    total_shifted = 0
    all_shifts = []

    if args.workers <= 1:
        for item in work_items:
            result = process_track(item)
            if result:
                total_shifted += result[1]
                all_shifts.append(result[2])
    else:
        with Pool(args.workers) as pool:
            for result in pool.imap_unordered(process_track, work_items, chunksize=4):
                if result:
                    total_shifted += result[1]
                    all_shifts.append(result[2])
                    if len(all_shifts) % 100 == 0:
                        elapsed = time.time() - t0
                        print(f"  {len(all_shifts)}/{len(work_items)} ({elapsed:.0f}s)")

    elapsed = time.time() - t0
    print(f"\n{'DRY RUN — ' if args.dry_run else ''}Summary:")
    print(f"  Tracks: {len(all_shifts)}")
    print(f"  Onsets shifted: {total_shifted}")
    if all_shifts:
        print(f"  Mean shift: {np.mean(all_shifts):.1f}ms")
        print(f"  Median shift: {np.median(all_shifts):.1f}ms")
    print(f"  Time: {elapsed:.1f}s")


if __name__ == "__main__":
    main()
