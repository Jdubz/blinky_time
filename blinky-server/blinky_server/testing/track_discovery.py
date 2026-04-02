"""Track discovery for music test validation suites.

Scans a directory for audio files with matching ground truth annotations.
Ported from blinky-serial-mcp/src/lib/track-discovery.ts.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

from .types import GroundTruth, GroundTruthHit, GroundTruthOnset

log = logging.getLogger(__name__)

AUDIO_EXTENSIONS = {".mp3", ".wav", ".flac"}


def discover_tracks(
    directory: str | Path,
) -> list[dict[str, str]]:
    """Find audio files with matching .beats.json ground truth.

    Returns list of {name, audio_file, ground_truth, onset_ground_truth?}
    sorted by name.
    """
    d = Path(directory)
    if not d.is_dir():
        raise FileNotFoundError(f"Track directory does not exist: {d}")

    files = {f.name for f in d.iterdir() if f.is_file()}
    tracks = []

    for fname in sorted(files):
        suffix = Path(fname).suffix.lower()
        if suffix not in AUDIO_EXTENSIONS:
            continue
        base = Path(fname).stem
        gt_file = f"{base}.beats.json"
        if gt_file not in files:
            continue
        track: dict[str, str] = {
            "name": base,
            "audio_file": str(d / fname),
            "ground_truth": str(d / gt_file),
        }
        onset_file = f"{base}.onsets_consensus.json"
        if onset_file in files:
            track["onset_ground_truth"] = str(d / onset_file)
        tracks.append(track)

    return tracks


def load_ground_truth(gt_path: str, onset_path: str | None = None) -> GroundTruth:
    """Load ground truth from .beats.json and optional .onsets_consensus.json."""
    with open(gt_path) as f:
        data = json.load(f)

    hits = [
        GroundTruthHit(
            time=h["time"],
            type=h.get("type", "beat"),
            strength=h.get("strength", 1.0),
            expect_trigger=h.get("expectTrigger", True),
        )
        for h in data.get("hits", [])
    ]

    onsets = None
    if onset_path:
        try:
            with open(onset_path) as f:
                onset_data = json.load(f)
            onsets = [
                GroundTruthOnset(time=o["time"], strength=o.get("strength", 1.0))
                for o in onset_data.get("onsets", [])
            ]
        except (FileNotFoundError, json.JSONDecodeError) as e:
            log.warning("Failed to load onset ground truth %s: %s", onset_path, e)

    return GroundTruth(
        pattern=data.get("pattern", Path(gt_path).stem),
        duration_ms=data.get("durationMs", 0),
        hits=hits,
        onsets=onsets,
    )


def load_track_manifest(directory: str | Path) -> dict[str, Any]:
    """Load track_manifest.json for seek offsets.

    Returns {track_name: {seekOffset, ...}} or empty dict if not found.
    """
    manifest_path = Path(directory) / "track_manifest.json"
    if not manifest_path.exists():
        return {}
    try:
        with open(manifest_path) as f:
            result: dict[str, Any] = json.load(f)
            return result
    except (json.JSONDecodeError, OSError) as e:
        log.warning("Failed to load track manifest: %s", e)
        return {}
