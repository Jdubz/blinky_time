#!/usr/bin/env python3
"""Generate kick-weighted onset targets for training.

Creates per-track target arrays where:
  - Kick onsets (<200 Hz) get target = 1.0
  - Snare onsets (200-4000 Hz) get target = 1.0 (equally important as kicks)
  - Hi-hat/cymbal onsets (>4000 Hz) get target = 0.0 (suppress)

This teaches the NN to fire strongly on kicks and snares (equally important),
and not at all on hi-hats — producing the 2-4 major events per bar
density target needed for smooth LED animations.

Usage:
    python scripts/generate_kick_weighted_targets.py \\
        --audio-dir /mnt/storage/blinky-ml-data/audio \\
        --output-dir /mnt/storage/blinky-ml-data/labels/kick_weighted \\
        --workers 4

Output format (per track):
    {
        "onsets": [{"time": 0.5, "weight": 1.0, "type": "kick"}, ...],
        "kick_count": 42,
        "snare_count": 38,
        "hihat_count": 85,
        "source": "bandpass_onset_detect"
    }
"""

import argparse
import json
from multiprocessing import Pool
from pathlib import Path

import librosa
import numpy as np
from scipy.signal import butter, sosfilt


# Frequency bands for onset classification
KICK_LOW = 30
KICK_HIGH = 200
SNARE_LOW = 200
SNARE_HIGH = 4000
HIHAT_LOW = 4000
HIHAT_HIGH = 8000

# Target weights per onset type
# Kicks and snares are equally important — both are primary visual events.
# Snares cut through the mix ("the talking drum") and drive accents just
# as strongly as kicks drive the downbeat.
KICK_WEIGHT = 1.0
SNARE_WEIGHT = 1.0
HIHAT_WEIGHT = 0.0

SR = 16000
HOP = 256


def bandpass_onsets(y: np.ndarray, sr: int, low_hz: float, high_hz: float) -> np.ndarray:
    """Detect onsets in a frequency band."""
    nyq = sr / 2
    low = max(low_hz / nyq, 0.001)
    high = min(high_hz / nyq, 0.999)
    sos = butter(4, [low, high], btype='band', output='sos')
    filtered = sosfilt(sos, y)
    times = librosa.onset.onset_detect(
        y=filtered, sr=sr, hop_length=HOP,
        backtrack=False, units='time'
    )
    return times


def merge_weighted_onsets(kick_times, snare_times, hihat_times, merge_tol=0.03):
    """Merge onset times from different bands with priority weighting.

    When onsets from different bands overlap (within merge_tol seconds),
    the highest-priority band wins:  kick > snare > hihat.

    This prevents double-counting when a kick triggers onsets in both
    the kick and snare bands (which is common due to spectral leakage).
    """
    events = []
    for t in kick_times:
        events.append({"time": float(t), "weight": KICK_WEIGHT, "type": "kick"})
    for t in snare_times:
        events.append({"time": float(t), "weight": SNARE_WEIGHT, "type": "snare"})
    for t in hihat_times:
        events.append({"time": float(t), "weight": HIHAT_WEIGHT, "type": "hihat"})

    # Sort by time
    events.sort(key=lambda e: e["time"])

    # Merge overlapping events: highest weight wins
    merged = []
    for evt in events:
        if merged and abs(evt["time"] - merged[-1]["time"]) < merge_tol:
            # Overlapping — keep the higher weight
            if evt["weight"] > merged[-1]["weight"]:
                merged[-1] = evt
        else:
            merged.append(evt)

    return merged


def process_track(args):
    """Process a single track."""
    audio_path, output_path = args

    if output_path.exists():
        return None  # Skip existing (resumable)

    try:
        y, sr = librosa.load(str(audio_path), sr=SR)
    except Exception as e:
        return f"FAILED {audio_path.name}: {e}"

    kick_times = bandpass_onsets(y, sr, KICK_LOW, KICK_HIGH)
    snare_times = bandpass_onsets(y, sr, SNARE_LOW, SNARE_HIGH)
    hihat_times = bandpass_onsets(y, sr, HIHAT_LOW, HIHAT_HIGH)

    merged = merge_weighted_onsets(kick_times, snare_times, hihat_times)

    kick_count = sum(1 for e in merged if e["type"] == "kick")
    snare_count = sum(1 for e in merged if e["type"] == "snare")
    hihat_count = sum(1 for e in merged if e["type"] == "hihat")

    result = {
        "onsets": merged,
        "kick_count": kick_count,
        "snare_count": snare_count,
        "hihat_count": hihat_count,
        "total": len(merged),
        "source": "bandpass_onset_detect",
        "weights": {
            "kick": KICK_WEIGHT,
            "snare": SNARE_WEIGHT,
            "hihat": HIHAT_WEIGHT
        },
        "bands_hz": {
            "kick": [KICK_LOW, KICK_HIGH],
            "snare": [SNARE_LOW, SNARE_HIGH],
            "hihat": [HIHAT_LOW, HIHAT_HIGH]
        }
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w') as f:
        json.dump(result, f, indent=2)

    return None


def main():
    parser = argparse.ArgumentParser(description="Generate kick-weighted onset targets")
    parser.add_argument("--audio-dir", type=str,
                        default="/mnt/storage/blinky-ml-data/audio",
                        help="Directory containing audio files")
    parser.add_argument("--output-dir", type=str,
                        default="/mnt/storage/blinky-ml-data/labels/kick_weighted",
                        help="Output directory for weighted onset labels")
    parser.add_argument("--labels-dir", type=str,
                        default="/mnt/storage/blinky-ml-data/labels/consensus_v5",
                        help="Consensus labels dir (only process tracks with labels)")
    parser.add_argument("--workers", type=int, default=4,
                        help="Number of parallel workers")
    args = parser.parse_args()

    audio_dir = Path(args.audio_dir)
    output_dir = Path(args.output_dir)
    labels_dir = Path(args.labels_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Find all audio files (recursive — audio may be in subdirectories)
    audio_files = sorted(
        f for f in audio_dir.rglob("*")
        if f.suffix.lower() in (".mp3", ".wav", ".flac", ".ogg")
    )
    print(f"Found {len(audio_files)} audio files in {audio_dir}")

    # Build stem→path map, only keep tracks that have consensus labels
    label_stems = set(f.stem.replace(".beats", "") for f in labels_dir.glob("*.beats.json"))
    audio_by_stem = {}
    for af in audio_files:
        # Handle stems like "000397.LOFI" → "000397"
        stem = af.stem.split(".")[0]
        if stem in label_stems and stem not in audio_by_stem:
            audio_by_stem[stem] = af
    print(f"Matched {len(audio_by_stem)} audio files to consensus labels")

    # Prepare work items
    work = []
    skipped = 0
    for stem, af in sorted(audio_by_stem.items()):
        out = output_dir / f"{stem}.kick_weighted.json"
        if out.exists():
            skipped += 1
        else:
            work.append((af, out))

    print(f"Processing {len(work)} tracks ({skipped} already done)")

    if not work:
        print("Nothing to do!")
        # Count totals from existing files
        existing = list(output_dir.glob("*.kick_weighted.json"))
        if existing:
            kicks = snares = hihats = 0
            for f in existing:
                with open(f) as fh:
                    data = json.load(fh)
                kicks += data["kick_count"]
                snares += data["snare_count"]
                hihats += data["hihat_count"]
            print(f"Existing labels: {len(existing)} tracks, "
                  f"{kicks} kicks, {snares} snares, {hihats} hi-hats")
        return

    # Process with multiprocessing
    errors = 0
    done = 0
    with Pool(args.workers) as pool:
        for result in pool.imap_unordered(process_track, work):
            done += 1
            if result:
                print(f"  {result}")
                errors += 1
            if done % 100 == 0:
                print(f"  Progress: {done}/{len(work)} "
                      f"({100*done/len(work):.1f}%)")

    print(f"\nDone! Processed {done} tracks ({errors} errors)")

    # Summary stats
    all_files = list(output_dir.glob("*.kick_weighted.json"))
    kicks = snares = hihats = 0
    for f in all_files:
        with open(f) as fh:
            data = json.load(fh)
        kicks += data["kick_count"]
        snares += data["snare_count"]
        hihats += data["hihat_count"]
    total = kicks + snares + hihats
    print(f"Total labels: {len(all_files)} tracks")
    if total > 0:
        print(f"  Kicks:  {kicks} ({100*kicks/total:.0f}%)")
        print(f"  Snares: {snares} ({100*snares/total:.0f}%)")
        print(f"  HiHats: {hihats} ({100*hihats/total:.0f}%)")
    else:
        print("  No onsets found in any track")


if __name__ == "__main__":
    main()
