"""Track discovery for music test validation suites.

Scans a directory for audio files with matching ground truth annotations.
Ported from blinky-serial-mcp/src/lib/track-discovery.ts.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

from .onset_label_merge import apply_human_edits, load_human_edits
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

    Returns list of {name, audio_file, ground_truth} sorted by name.
    `ground_truth` points at the .onsets_consensus.json file.
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
        track: dict[str, str] = {
            "name": base,
            "audio_file": str(d / fname),
            "ground_truth": str(d / onset_file),
        }
        # Optional human-edit overlay sits next to the audio. Riding with git
        # is the sync mechanism between curation (devtop) and validation
        # (blinkyhost) — the file is checked in.
        human_file = f"{base}.onsets_human.json"
        if human_file in files:
            track["human_edits"] = str(d / human_file)
        tracks.append(track)

    return tracks


def load_ground_truth(gt_path: str, human_edits_path: str | None = None) -> GroundTruth:
    """Load onset ground truth from a `.onsets_consensus.json` file.

    Schema: ``{"onsets": [{"time": float, "strength": float, "systems": int}]}``

    If ``human_edits_path`` is provided and the file exists, the human-curated
    overlay is merged in (see :mod:`onset_label_merge`). The merged onsets
    carry a ``source`` field (``'auto' | 'auto_edited' | 'human'``) so scoring
    can later split metrics by provenance.
    """
    with open(gt_path) as f:
        data = json.load(f)

    raw_onsets = data.get("onsets", [])

    edits_doc = load_human_edits(human_edits_path) if human_edits_path else None
    merged, stats = apply_human_edits(raw_onsets, edits_doc)
    if edits_doc is not None and (stats["edited"] or stats["removed"] or stats["created"]):
        log.info(
            "Loaded human edits for %s: %d edited, %d removed, %d created (vs %d auto)",
            Path(gt_path).stem, stats["edited"], stats["removed"], stats["created"],
            len(raw_onsets),
        )

    onsets = [
        GroundTruthOnset(
            time=o["time"],
            strength=o.get("strength", 1.0),
            source=o.get("source", "auto"),
        )
        for o in merged
    ]

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
            source=o.get("source", "auto"),
        )
        for o in merged
    ]

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
