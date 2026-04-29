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
    """Find audio files with matching onset ground truth.

    Task is ONSET DETECTION (every percussive event). Onset GT lives in
    `<base>.onsets_consensus.json`. Older `.beats.json` files were beat
    consensus and are not appropriate ground truth for onset evaluation —
    they were archived 2026-04-29 to remove the long-running confusion
    where firmware F1 was scored against the wrong target.

    Returns list of {name, audio_file, ground_truth, onset_ground_truth}
    sorted by name. `ground_truth` and `onset_ground_truth` both point at
    the .onsets_consensus.json file (kept as two keys for back-compat with
    callers that still pass both into load_ground_truth).
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
        onset_file = f"{base}.onsets_consensus.json"
        if onset_file not in files:
            # No onset GT for this track — skip silently (matches old behaviour
            # of skipping tracks without GT).
            continue
        gt_path = str(d / onset_file)
        track: dict[str, str] = {
            "name": base,
            "audio_file": str(d / fname),
            "ground_truth": gt_path,
            "onset_ground_truth": gt_path,
        }
        tracks.append(track)

    return tracks


def load_ground_truth(gt_path: str, onset_path: str | None = None) -> GroundTruth:
    """Load onset ground truth from a `.onsets_consensus.json` file.

    Schema: ``{"onsets": [{"time": float, "strength": float, "systems": int}]}``
    Both `gt_path` and `onset_path` are expected to be the same file (the
    onsets_consensus json) — the dual-arg signature is preserved for back-
    compat but the legacy beat-consensus path no longer exists.
    """
    onset_data_path = onset_path or gt_path
    with open(onset_data_path) as f:
        data = json.load(f)

    raw_onsets = data.get("onsets", [])
    onsets = [GroundTruthOnset(time=o["time"], strength=o.get("strength", 1.0)) for o in raw_onsets]

    # Build hits list from onsets too — some scoring code paths still iterate
    # `gt.hits` for things like timing-offset measurement. Each onset becomes
    # a hit with type="onset" and expect_trigger=True (full-mix consensus
    # implies the onset is real and should trigger).
    hits = [
        GroundTruthHit(
            time=o["time"],
            type="onset",
            strength=o.get("strength", 1.0),
            expect_trigger=True,
        )
        for o in raw_onsets
    ]

    return GroundTruth(
        pattern=data.get("pattern", Path(onset_data_path).stem),
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
