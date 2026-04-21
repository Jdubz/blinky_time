"""Materialize the held-out EDM corpus as a sibling directory of symlinks.

Selects GiantSteps tracks that are *not* in the v27-hybrid training
corpus (per `processed_v27/.prep_splits.json`) and symlinks their
audio + `.beats.json` labels into a single flat directory so that
`run_catalog.py` can load them with no other plumbing.

Usage:
    cd ml-training && ./venv/bin/python -m analysis.setup_holdout_corpus \
        --out ../blinky-test-player/music/edm_holdout
"""

from __future__ import annotations

import argparse
import json
import logging
import os
from pathlib import Path

log = logging.getLogger("holdout_corpus")

SPLITS_PATH = Path("/mnt/storage/blinky-ml-data/processed_v27/.prep_splits.json")
GS_AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio/giantsteps/giantsteps-tempo-dataset/audio")
GS_LABEL_DIR = Path("/mnt/storage/blinky-ml-data/labels/giantsteps")


def find_heldout_stems() -> list[str]:
    splits = json.loads(SPLITS_PATH.read_text())
    in_training = set(splits.get("train", [])) | set(splits.get("val", []))
    audio_stems = {
        p.stem for p in GS_AUDIO_DIR.glob("*.mp3")
    }
    label_stems = {
        p.name.removesuffix(".beats.json")
        for p in GS_LABEL_DIR.glob("*.beats.json")
    }
    # Held out of training AND has both audio + label
    return sorted((audio_stems & label_stems) - in_training)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="Output directory for symlinks")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    stems = find_heldout_stems()
    log.info("Found %d held-out GiantSteps stems", len(stems))

    args.out.mkdir(parents=True, exist_ok=True)
    for stem in stems:
        audio_src = GS_AUDIO_DIR / f"{stem}.mp3"
        label_src = GS_LABEL_DIR / f"{stem}.beats.json"
        audio_dst = args.out / f"{stem}.mp3"
        label_dst = args.out / f"{stem}.beats.json"
        for src, dst in [(audio_src, audio_dst), (label_src, label_dst)]:
            if dst.exists() or dst.is_symlink():
                dst.unlink()
            os.symlink(src, dst)
    log.info("Symlinked %d audio+label pairs into %s", len(stems), args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
