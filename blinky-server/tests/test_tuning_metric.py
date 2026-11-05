"""The server's tuning metric reads drum metrics (test_runner._extract_metric, #294). The drum
scoring itself is tested where it lives, in blinky-corpus (blinky_scoring, #357)."""

from __future__ import annotations

from blinky_scoring.drum_scoring import score_drums
from blinky_scoring.types import DrumLabels

import blinky_server  # noqa: F401  (puts the corpus submodule's blinky_scoring on the path)
from blinky_server.testing.test_runner import _extract_metric

LABELS = DrumLabels([1.0, 2.0, 3.0], [1.5, 2.5, 3.5], [1.25, 1.75])


def test_tuning_targets_drum_metrics_and_skips_missing_values() -> None:
    assert _extract_metric({"drums": {"kickR50": 0.8}}, "kickR50") == 0.8
    assert _extract_metric({"drums": None}, "kickR50") is None  # track without drum labels
    assert _extract_metric({"drums": {"kickR50": None}}, "kickR50") is None  # no lag estimate
    assert _extract_metric({"onsetTracking": {"f1": 0.6}}, "onsetF1") == 0.6
    silent = score_drums([], LABELS, 4.0).as_dict()
    assert _extract_metric({"drums": silent}, "drumF") == 0.0  # counted, not skipped
