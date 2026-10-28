"""blinky-server's drum-label cleaning (#294) must stay the rule clean_kick_weighted.py applies.

The server can't import ml-training, so drum_scoring carries its own copy of the rule; this
checks the two agree on every edm/ track, the way check_label_merge_sync.py guards the label
merge.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ML = Path(__file__).resolve().parents[1]
REPO = ML.parent
sys.path.insert(0, str(ML))
sys.path.insert(0, str(REPO / "blinky-server"))

from blinky_server.testing.drum_scoring import (  # noqa: E402
    CLEAN_MIN_SYSTEMS,
    CLEAN_TOLERANCE_SEC,
    clean_kick_weighted,
)
from scripts.clean_kick_weighted import filter_track  # noqa: E402

EDM = REPO / "blinky-test-player" / "music" / "edm"


def test_server_cleaning_matches_clean_kick_weighted() -> None:
    kws = sorted((EDM / "kick_weighted").glob("*.kick_weighted.json"))
    assert len(kws) == 18
    for kw in kws:
        stem = kw.name[: -len(".kick_weighted.json")]
        cons = EDM / f"{stem}.onsets_consensus.json"
        ref = filter_track(kw, cons, CLEAN_TOLERANCE_SEC, CLEAN_MIN_SYSTEMS)
        assert ref is not None
        got = clean_kick_weighted(json.loads(kw.read_text()), json.loads(cons.read_text()))
        assert [(o["time"], o["type"]) for o in got] == \
            [(o["time"], o["type"]) for o in ref["onsets"]], stem
