#!/usr/bin/env python3
"""Download GiantSteps Tempo + Key datasets for training data.

GiantSteps Tempo: 664 EDM tracks with beat annotations (research use).
GiantSteps Key:   604 EDM tracks (useful for data diversity).

Both datasets are available from the MTG GitHub repositories.
Audio is on Beatport previews (~2 min clips).

Usage:
    python scripts/download_giantsteps.py --output-dir /mnt/storage/blinky-ml-data/audio/giantsteps
"""

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

# GiantSteps Tempo dataset
GIANTSTEPS_TEMPO_REPO = "https://github.com/GiantSteps/giantsteps-tempo-dataset.git"
# GiantSteps Key dataset
GIANTSTEPS_KEY_REPO = "https://github.com/GiantSteps/giantsteps-key-dataset.git"


def download_giantsteps_tempo(output_dir: Path):
    """Clone GiantSteps Tempo dataset and download audio."""
    repo_dir = output_dir / "giantsteps-tempo-dataset"

    if repo_dir.exists():
        print(f"  Repo already cloned: {repo_dir}")
    else:
        print("  Cloning GiantSteps Tempo dataset...")
        subprocess.run(["git", "clone", "--depth", "1", GIANTSTEPS_TEMPO_REPO, str(repo_dir)],
                       check=True, capture_output=True)

    # Check for annotations
    annotations_dir = repo_dir / "annotations" / "tempo"
    if annotations_dir.exists():
        n = len(list(annotations_dir.glob("*.bpm")))
        print(f"  Found {n} tempo annotations")
    else:
        # Try alternative structure
        for d in repo_dir.rglob("*.bpm"):
            annotations_dir = d.parent
            break
        n = len(list(annotations_dir.glob("*.bpm"))) if annotations_dir.exists() else 0
        print(f"  Found {n} tempo annotations in {annotations_dir}")

    # Check for audio download script
    audio_script = repo_dir / "audio_dl.sh"
    if not audio_script.exists():
        # Look for any download script
        for script in repo_dir.rglob("*.sh"):
            if "audio" in script.name.lower() or "download" in script.name.lower():
                audio_script = script
                break

    audio_dir = output_dir / "audio"
    audio_dir.mkdir(exist_ok=True)

    # Check if audio already downloaded
    existing = list(audio_dir.glob("*.mp3"))
    if len(existing) > 100:
        print(f"  Audio already downloaded: {len(existing)} files")
        return

    print(f"  Note: GiantSteps audio must be downloaded from Beatport.")
    print(f"  Check {repo_dir}/README.md for download instructions.")
    print(f"  Audio files should be placed in: {audio_dir}/")

    return repo_dir


def convert_giantsteps_annotations(repo_dir: Path, output_dir: Path):
    """Convert GiantSteps .bpm annotations to .beats.json format.

    GiantSteps Tempo only has BPM annotations, not beat positions.
    We'll need to run Beat This! on the audio to get beat positions.
    This just creates a mapping of track IDs to tempos for validation.
    """
    tempo_map = {}

    # Find all .bpm files
    for bpm_file in sorted(repo_dir.rglob("*.bpm")):
        track_id = bpm_file.stem
        try:
            with open(bpm_file) as f:
                bpm = float(f.read().strip())
            tempo_map[track_id] = bpm
        except (ValueError, IOError):
            continue

    if tempo_map:
        output_path = output_dir / "giantsteps_tempos.json"
        with open(output_path, "w") as f:
            json.dump(tempo_map, f, indent=2)
        print(f"  Saved {len(tempo_map)} tempo annotations to {output_path}")

    return tempo_map


def main():
    parser = argparse.ArgumentParser(description="Download GiantSteps datasets")
    parser.add_argument("--output-dir", default="/mnt/storage/blinky-ml-data/audio/giantsteps")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print("GiantSteps Tempo Dataset:")
    repo_dir = download_giantsteps_tempo(output_dir)

    if repo_dir:
        print("\nConverting annotations...")
        tempo_map = convert_giantsteps_annotations(repo_dir, output_dir)
        if tempo_map:
            bpms = list(tempo_map.values())
            print(f"  BPM range: {min(bpms):.0f} - {max(bpms):.0f}")
            print(f"  Mean BPM: {np.mean(bpms):.0f}")

    print("\nNote: GiantSteps audio requires manual download from Beatport.")
    print("For now, the FMA electronic subset is the primary training source.")


if __name__ == "__main__":
    main()
