#!/usr/bin/python3
"""The camera half of the hardware checks' lights check (blinky-server
``testing/hardware_checks.py``; docs/TEST_BOX.md, "Hardware checks").

blinky-server drives the chip (``testpattern`` over its own serial connection) and starts this
worker on the box's system Python, which has picamera2 (the server's venv doesn't, and the
process boundary keeps camera work off the server's event loop). They talk in JSON lines: one
request on stdin, one reply on stdout. Logs go to stderr.

On start it loads the region map, opens the camera with the map's exposure and gain locked, and
prints ``{"ready": true, ...}``. If it can't (the camera busy or missing, the map missing or made
for other settings), it prints ``{"rig_error": "..."}`` and exits 3: a rig fault, never the
firmware's.

Requests:

  {"op": "dark", "n": 30}             the LEDs' dark baseline (the chip shows all black): per
                                      region its mean level, and the whole frame's mean
  {"op": "lit", "skip": 4, "n": 3,    regions lit now against the dark baseline (``skip`` frames
   "key": "x,y"}                      first, past any frame in flight); ``key`` records them
                                      for the walk report. Also how much the whole frame rose.
  {"op": "report", "width": 8,        camera_analysis.walk_report over the recorded keys
   "height": 8}
  {"op": "quit"}                      close the camera and exit (so does end of input)

A secondary review tool like the rest of scripts/testbox/camera, with one exception the owner
made (2026-11-25): "do the LEDs work" is a blocking yes/no, and only the camera can see it.
The camera's timing reviews stay secondary and never gate.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import camera_analysis as ca
import camera_capture as cc


def reply(obj: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


class Lights:
    def __init__(self, regions: Path, exposure_us: int | None, gain: float | None) -> None:
        m = np.load(regions)
        self.regions = json.loads(str(m["regions"]))
        settings = json.loads(str(m["settings"]))
        self.exposure_us = int(exposure_us or settings["exposure_us"])
        self.gain = float(gain or settings["analog_gain"])
        if settings["exposure_us"] != self.exposure_us or settings["analog_gain"] != self.gain:
            raise ValueError(
                f"the region map was made at {settings['exposure_us']} us / gain "
                f"{settings['analog_gain']}, not {self.exposure_us} us / gain {self.gain}"
            )
        if tuple(m["labels"].shape) != (cc.SIZE[1], cc.SIZE[0]):
            raise ValueError(f"the region map is {m['labels'].shape}, the camera {cc.SIZE}")
        self.order, self.starts, _ = cc.region_index(m["labels"])
        self.cam = cc.open_camera(self.exposure_us, self.gain)
        cc.settle(self.cam, self.exposure_us, self.gain)
        self.dark: np.ndarray | None = None
        self.dark_frame = 0.0
        self.lit: dict[tuple[int, int], list[int]] = {}

    def frames(self, n: int) -> tuple[np.ndarray, float, int, int]:
        """n frames: brightness per region (frames x regions), the whole frame's mean level,
        frames whose controls drifted, frames with saturated pixels in a region."""
        lum, whole, bad, sat = [], [], 0, 0
        for _ in range(n):
            req = self.cam.capture_request()
            try:
                md = req.get_metadata()
                frame = req.make_array("main")
            finally:
                req.release()
            bad += bool(cc.control_errors(md, self.exposure_us, self.gain))
            rgb, s, _ = cc.reduce_frame(frame, self.order, self.starts)
            sat += bool(s.any())
            lum.append(rgb.sum(axis=1))
            whole.append(float(frame.mean()))
        return np.asarray(lum, np.float64), float(np.mean(whole)), bad, sat

    def handle(self, req: dict[str, Any]) -> dict[str, Any]:
        op = req.get("op")
        if op == "dark":
            lum, whole, bad, sat = self.frames(int(req.get("n", 30)))
            self.dark, self.dark_frame = lum, whole
            means = lum.mean(axis=0)
            return {
                "dark_mean": [round(float(v), 1) for v in means],
                "dark_spread": [round(float(v), 1) for v in lum.std(axis=0)],
                "frame_mean": round(whole, 3),
                "control_errors": bad,
                "saturated_frames": sat,
            }
        if op == "lit":
            if self.dark is None:
                return {"error": "no dark baseline yet (op dark first)"}
            self.frames(int(req.get("skip", 4)))  # past any frame in flight
            lum, whole, bad, sat = self.frames(int(req.get("n", 3)))
            lit = ca.lit_regions(lum, self.dark)
            if req.get("key"):
                x, y = (int(v) for v in str(req["key"]).split(","))
                self.lit[(x, y)] = lit
            return {
                "lit": lit,
                "frame_rise": round(whole - self.dark_frame, 3),
                "control_errors": bad,
                "saturated_frames": sat,
            }
        if op == "report":
            return ca.walk_report(
                self.lit, int(req["width"]), int(req["height"]), len(self.regions)
            )
        return {"error": f"unknown op {op!r}"}

    def close(self) -> None:
        self.cam.stop()
        self.cam.close()


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--regions", type=Path, required=True)
    p.add_argument("--exposure-us", type=int, help="default: the region map's")
    p.add_argument("--gain", type=float, help="default: the region map's")
    args = p.parse_args(argv)
    try:
        if not args.regions.is_file():
            raise ValueError(f"no region map at {args.regions}")
        lights = Lights(args.regions, args.exposure_us, args.gain)
    except (ValueError, OSError, KeyError, RuntimeError, SystemExit, ImportError) as e:
        # SystemExit: camera_capture.settle gives up that way when the controls never lock.
        reply({"rig_error": f"camera: {e}"})
        return 3
    reply(
        {
            "ready": True,
            "regions": len(lights.regions),
            "exposure_us": lights.exposure_us,
            "gain": lights.gain,
        }
    )
    try:
        for line in sys.stdin:
            if not line.strip():
                continue
            try:
                req = json.loads(line)
            except ValueError as e:
                reply({"error": f"not JSON: {e}"})
                continue
            if req.get("op") == "quit":
                break
            try:
                reply(lights.handle(req))
            except Exception as e:  # a reply, not a dead worker: the server decides
                reply({"error": f"{type(e).__name__}: {e}"})
    finally:
        lights.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
