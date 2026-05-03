"""Apply human-curated edits over auto onset-consensus labels.

The label reviewer (`tools/label-reviewer/`) writes one overlay file per track
with this schema:

    {
      "stem": str,
      "source": "onsets_consensus",
      "source_count": int,           # len(auto_onsets) when edits were saved
      "edits": {                     # keyed by stringified index into auto list
        "<i>": {
          "time": float?,            # absent → keep auto.time
          "strength": float?,        # absent → keep auto.strength
          "edited": bool,            # canonical: this entry was changed by a human
          "removed": bool            # absent/false → keep, true → drop entirely
        }
      },
      "created": [
        {"time": float, "strength": float}    # auto-less labels added by hand
      ]
    }

The reviewer is the only writer. Training (prepare_dataset.py) and validation
(blinky-server's track_discovery) are the readers.

A near-identical copy lives at
``blinky-server/blinky_server/testing/onset_label_merge.py`` so the validation
harness can apply edits without depending on the ml-training package.
**If you change this file, change the other one.**
"""

from __future__ import annotations

import json
import logging
from collections.abc import Sequence
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)


class HumanEditDriftError(Exception):
    """Edits doc indices no longer line up with the auto onset list.

    Loud-fail by design: if the auto onsets file was regenerated, the integer
    indices in `edits` no longer point to the same acoustic events. Re-curate
    the affected tracks, or roll back the regeneration. The reviewer's
    ``source_count`` field is the canary.
    """


def _validate_doc_shape(doc: dict[str, Any]) -> None:
    if not isinstance(doc, dict):
        raise ValueError(f"edits doc must be a dict, got {type(doc).__name__}")
    edits = doc.get("edits", {})
    created = doc.get("created", [])
    if not isinstance(edits, dict):
        raise ValueError(f"edits doc 'edits' must be a dict, got {type(edits).__name__}")
    if not isinstance(created, list):
        raise ValueError(f"edits doc 'created' must be a list, got {type(created).__name__}")


def load_human_edits(path: Path | str) -> dict[str, Any] | None:
    """Load an edits doc from disk. Returns ``None`` if the file does not exist.

    Callers decide whether absent-file means "no edits" (the default behaviour
    for both training and validation) or is an error. We never substitute
    defaults silently when the file *does* exist but is malformed.
    """
    p = Path(path)
    if not p.exists():
        return None
    with open(p) as f:
        doc: dict[str, Any] = json.load(f)
    _validate_doc_shape(doc)
    return doc


def find_human_edits_for_stem(
    stem: str,
    candidate_dirs: Sequence[Path | str],
) -> tuple[Path, dict[str, Any]] | None:
    """Search for ``<stem>.onsets_human.json`` across the given dirs in order.

    Returns ``(path, doc)`` for the first hit, or ``None`` if no overlay exists.
    Use this to support multiple overlay locations (e.g. training falls back
    to the in-repo validation overlay so EDM corrections flow into training).
    """
    for d in candidate_dirs:
        p = Path(d) / f"{stem}.onsets_human.json"
        doc = load_human_edits(p)
        if doc is not None:
            return p, doc
    return None


def apply_human_edits(
    auto_onsets: Sequence[dict[str, Any]],
    edits_doc: dict[str, Any] | None,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    """Merge a human edits doc over the auto onset list.

    Returns ``(merged_onsets, stats)``. Each merged entry has at least::

        time:     float
        strength: float
        systems:  int      (carried from auto, or 0 for human-created)
        source:   str      'auto' | 'auto_edited' | 'human'

    plus any other fields carried from the auto entry. Output is sorted by
    time. ``stats`` reports ``{'total', 'edited', 'removed', 'created'}``.

    Raises :class:`HumanEditDriftError` if ``edits_doc['source_count']`` is
    set and differs from ``len(auto_onsets)``.
    """
    if edits_doc is None:
        passthrough: list[dict[str, Any]] = [{**o, "source": "auto"} for o in auto_onsets]
        return passthrough, {
            "total": len(passthrough),
            "edited": 0,
            "removed": 0,
            "created": 0,
        }

    _validate_doc_shape(edits_doc)
    source_count = edits_doc.get("source_count")
    if source_count is not None and source_count != len(auto_onsets):
        raise HumanEditDriftError(
            f"edits doc references {source_count} auto onsets but the auto "
            f"list now has {len(auto_onsets)}. Indices in 'edits' are stale; "
            f"re-curate the track or roll back the auto regeneration."
        )

    edits = edits_doc.get("edits", {})
    created = edits_doc.get("created", [])

    merged: list[dict[str, Any]] = []
    edited_count = 0
    removed_count = 0

    for i, o in enumerate(auto_onsets):
        patch = edits.get(str(i), {})
        if patch.get("removed"):
            removed_count += 1
            continue
        entry = dict(o)
        is_edited = bool(patch.get("edited"))
        if "time" in patch:
            entry["time"] = float(patch["time"])
        if "strength" in patch:
            entry["strength"] = float(patch["strength"])
        entry["source"] = "auto_edited" if is_edited else "auto"
        if is_edited:
            edited_count += 1
        merged.append(entry)

    for c in created:
        merged.append(
            {
                "time": float(c["time"]),
                "strength": float(c.get("strength", 1.0)),
                "systems": 0,  # no auto support — mark explicitly
                "source": "human",
            }
        )

    merged.sort(key=lambda x: x["time"])

    return merged, {
        "total": len(merged),
        "edited": edited_count,
        "removed": removed_count,
        "created": len(created),
    }
