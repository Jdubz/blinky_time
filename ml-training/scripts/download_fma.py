#!/usr/bin/env python3
"""Download FMA (Free Music Archive) electronic subset for training data.

FMA-medium contains 25,000 30-second clips across 16 genres.
We filter to electronic/dance genres (~5,000 tracks).

The FMA dataset is hosted on GitHub/Zenodo:
  - Audio: https://os.unil.cloud.switch.ch/fma/fma_medium.zip (22 GB)
  - Metadata: https://os.unil.cloud.switch.ch/fma/fma_metadata.zip (342 MB)

Usage:
    # Download and filter electronic tracks
    python scripts/download_fma.py --output-dir /mnt/storage/blinky-ml-data/audio/fma

    # Just show what would be downloaded (dry run)
    python scripts/download_fma.py --output-dir /mnt/storage/blinky-ml-data/audio/fma --dry-run
"""

import argparse
import csv
import io
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

METADATA_URL = "https://os.unil.cloud.switch.ch/fma/fma_metadata.zip"
AUDIO_URL = "https://os.unil.cloud.switch.ch/fma/fma_medium.zip"

# FMA genre IDs for electronic/dance music
ELECTRONIC_GENRES = {
    "Electronic", "Techno", "House", "Ambient Electronic",
    "Drum & Bass", "Trip-Hop", "Dubstep", "Breakbeat",
    "Trance", "Downtempo", "Glitch", "IDM", "Jungle",
}


def download_file(url: str, output_path: Path, desc: str = ""):
    """Download a file with progress using curl."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        print(f"  Already exists: {output_path}")
        return

    print(f"  Downloading {desc or url}...")
    result = subprocess.run(
        ["curl", "-L", "-o", str(output_path), "--progress-bar", url],
        check=True,
    )


def get_electronic_track_ids(metadata_dir: Path) -> set[int]:
    """Parse FMA metadata to find electronic genre track IDs."""
    tracks_csv = metadata_dir / "fma_metadata" / "tracks.csv"
    if not tracks_csv.exists():
        print(f"Error: {tracks_csv} not found", file=sys.stderr)
        sys.exit(1)

    electronic_ids = set()

    with open(tracks_csv, encoding="utf-8") as f:
        # FMA tracks.csv has a multi-row header; skip first 2 rows
        reader = csv.reader(f)
        header1 = next(reader)
        header2 = next(reader)
        # Find genre column indices
        # The "track" section has "genre_top" column
        for row in reader:
            try:
                track_id = int(row[0])
                genre_top = row[40] if len(row) > 40 else ""
                if genre_top in ELECTRONIC_GENRES:
                    electronic_ids.add(track_id)
            except (ValueError, IndexError):
                continue

    return electronic_ids


def main():
    parser = argparse.ArgumentParser(description="Download FMA electronic subset")
    parser.add_argument("--output-dir", required=True, help="Where to store audio files")
    parser.add_argument("--metadata-dir", default=None, help="Where to store/find metadata")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be downloaded")
    parser.add_argument("--skip-audio", action="store_true", help="Only download metadata")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    metadata_dir = Path(args.metadata_dir) if args.metadata_dir else output_dir.parent / "metadata"

    output_dir.mkdir(parents=True, exist_ok=True)
    metadata_dir.mkdir(parents=True, exist_ok=True)

    # Step 1: Download metadata
    metadata_zip = metadata_dir / "fma_metadata.zip"
    print("Step 1: Metadata")
    if not (metadata_dir / "fma_metadata" / "tracks.csv").exists():
        download_file(METADATA_URL, metadata_zip, "FMA metadata (342 MB)")
        print("  Extracting metadata...")
        with zipfile.ZipFile(metadata_zip) as zf:
            zf.extractall(metadata_dir)
    else:
        print("  Metadata already extracted")

    # Step 2: Find electronic tracks
    print("\nStep 2: Finding electronic tracks...")
    electronic_ids = get_electronic_track_ids(metadata_dir)
    print(f"  Found {len(electronic_ids)} electronic tracks")

    if args.dry_run:
        print(f"\nDry run: would download {len(electronic_ids)} tracks from FMA-medium (~22 GB total archive)")
        print(f"Estimated electronic subset: ~{len(electronic_ids) * 5 // 1024} GB")
        return

    if args.skip_audio:
        print("Skipping audio download (--skip-audio)")
        # Save track IDs for later
        ids_file = output_dir / "electronic_track_ids.txt"
        with open(ids_file, "w") as f:
            for tid in sorted(electronic_ids):
                f.write(f"{tid}\n")
        print(f"  Saved {len(electronic_ids)} track IDs to {ids_file}")
        return

    # Step 3: Download audio
    audio_zip = output_dir / "fma_medium.zip"
    print(f"\nStep 3: Download FMA-medium audio (22 GB)")
    download_file(AUDIO_URL, audio_zip, "FMA-medium audio")

    # Step 4: Extract only electronic tracks
    print("\nStep 4: Extracting electronic tracks...")
    extracted = 0
    with zipfile.ZipFile(audio_zip) as zf:
        for entry in zf.namelist():
            # FMA files are stored as fma_medium/XXXXXX.mp3
            if not entry.endswith(".mp3"):
                continue
            basename = Path(entry).stem
            try:
                track_id = int(basename)
            except ValueError:
                continue

            if track_id in electronic_ids:
                # Extract to output dir
                target = output_dir / f"{basename}.mp3"
                if not target.exists():
                    with zf.open(entry) as src, open(target, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted += 1

    print(f"\nDone: extracted {extracted} electronic tracks to {output_dir}")
    print(f"Total electronic tracks available: {len(electronic_ids)}")

    # Save track ID list
    ids_file = output_dir / "electronic_track_ids.txt"
    with open(ids_file, "w") as f:
        for tid in sorted(electronic_ids):
            f.write(f"{tid}\n")


if __name__ == "__main__":
    main()
