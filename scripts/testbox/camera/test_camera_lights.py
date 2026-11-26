"""camera_lights.py, the lights check's camera worker, without a camera: its protocol, the
walk's bookkeeping, and a missing region map reported as a rig error."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import camera_lights as cl

HERE = Path(__file__).parent


def test_a_missing_region_map_is_a_rig_error(tmp_path: Path) -> None:
    r = subprocess.run(
        [sys.executable, str(HERE / "camera_lights.py"), "--regions", str(tmp_path / "x.npz")],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert r.returncode == 3
    assert "no region map" in json.loads(r.stdout.splitlines()[0])["rig_error"]


class FakeLights(cl.Lights):
    """Lights with the camera replaced: region k is bright when LED k is on."""

    def __init__(self, n: int, stuck_dark: set[int] = frozenset()) -> None:
        self.regions = [{"id": i + 1} for i in range(n)]
        self.dark = None
        self.dark_frame = 0.0
        self.lit = {}
        self.on: set[int] = set()
        self.stuck_dark = set(stuck_dark)
        self.rng = np.random.default_rng(1)

    def frames(self, n: int) -> tuple[np.ndarray, float, int, int]:
        lum = 1000 + self.rng.normal(0, 2, (n, len(self.regions)))
        for k in self.on - self.stuck_dark:
            lum[:, k] += 400
        return lum, 27.8 + 0.01 * len(self.on), 0, 0


def test_the_walk_finds_every_led_and_names_a_dead_one() -> None:
    lights = FakeLights(4)
    assert "error" in lights.handle({"op": "lit"})  # no baseline yet
    lights.handle({"op": "dark", "n": 30})
    lights.on = {0, 1, 2, 3}
    assert lights.handle({"op": "lit"})["lit"] == [0, 1, 2, 3]
    for k in range(4):
        lights.on = {k}
        lights.handle({"op": "lit", "key": f"{k % 2},{k // 2}"})
    assert lights.handle({"op": "report", "width": 2, "height": 2})["ok"] is True

    dead = FakeLights(4, stuck_dark={2})
    dead.handle({"op": "dark", "n": 30})
    for k in range(4):
        dead.on = {k}
        dead.handle({"op": "lit", "key": f"{k % 2},{k // 2}"})
    rep = dead.handle({"op": "report", "width": 2, "height": 2})
    assert rep["ok"] is False and rep["dead"] == ["0,1"] and rep["unseen"] == [2]
