#!/usr/bin/python3
"""Test box camera calibration with the firmware's ``testpattern`` command
(blinky-firmware render/TestPattern.h; docs/TEST_BOX_CAMERA_HARNESS_PLAN.md, "Calibration").

Drives one chip through blinky-server's command API (per-chip commands are allowed on the
box) and the camera in-process. Always ends with ``testpattern off``. A secondary check like
the rest of the camera tooling: it reports, it never gates.

  walk    light each logical pixel alone; which camera region lights: the LED map,
          dead / stuck / shared LEDs, and which way the panel faces
  timing  an irregular flash train while the camera records; the chip's edge times (on
          the host clock through a best-of-N sync) against the camera's onsets: the
          camera's offset and floor, and the rolling-shutter line time
  pwm     solid colour across an exposure sweep: brightness ripple per exposure (minima
          at whole multiples of the LEDs' PWM period)

Runs on the box's system Python (numpy, picamera2 from apt). Reports go to
~/.local/share/blinky-camera/calibration/<stamp>/.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import camera_analysis as ca
import camera_capture as cc
from camera_review import api

OUT_ROOT = Path.home() / ".local" / "share" / "blinky-camera" / "calibration"
SYNC_SAMPLES = 9


def command(device: str, text: str) -> str:
    """One console command on one chip; an ``ERROR`` reply raises with it."""
    resp = str(api("POST", f"/devices/{device}/command", {"command": text}).get("response", ""))
    if "ERROR" in resp or "Unknown command" in resp:
        raise SystemExit(f"{device}: {text!r} -> {resp.strip()}")
    return resp


def info(device: str) -> dict[str, Any]:
    resp = command(device, "json info")
    return dict(json.loads(resp[resp.index("{") :]))


def clock_sync(device: str, samples: int = SYNC_SAMPLES) -> dict[str, float]:
    """Chip millis() to host wall clock (ms): the shortest of ``samples`` json info round
    trips through the server's API, offset = midpoint - millis, error at most RTT/2."""
    best: tuple[float, float] | None = None
    for _ in range(samples):
        t0 = time.time() * 1000
        millis = float(info(device)["millis"])
        t1 = time.time() * 1000
        rtt = t1 - t0
        if best is None or rtt < best[1]:
            best = ((t0 + t1) / 2 - millis, rtt)
    assert best is not None
    return {"offset_ms": best[0], "rtt_ms": best[1]}


class Camera:
    """The locked camera, reduced to per-region sums, with the frame clock on wall time."""

    def __init__(self, regions_path: Path, exposure_us: int, gain: float) -> None:
        m = np.load(regions_path)
        self.regions = json.loads(str(m["regions"]))
        self.settings = json.loads(str(m["settings"]))
        if self.settings["exposure_us"] != exposure_us or self.settings["analog_gain"] != gain:
            raise SystemExit(
                f"region map was made at {self.settings['exposure_us']} us / gain "
                f"{self.settings['analog_gain']}; pass those or re-run the survey"
            )
        self.order, self.starts, self.ids = cc.region_index(m["labels"])
        self.exposure_us, self.gain = exposure_us, gain
        self.cam = cc.open_camera(exposure_us, gain)
        cc.settle(self.cam, exposure_us, gain)

    def set_exposure(self, exposure_us: int) -> None:
        self.cam.set_controls({"ExposureTime": exposure_us})
        self.exposure_us = exposure_us
        time.sleep(0.25)
        cc.settle(self.cam, exposure_us, self.gain)

    def frames(self, n: int) -> tuple[np.ndarray, np.ndarray, int, int]:
        """n frames: (sensor ts ns, brightness per region, frames with control drift,
        frames with saturated pixels)."""
        ts, lum, bad, sat = [], [], 0, 0
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
            ts.append(int(md["SensorTimestamp"]))
            lum.append(rgb.sum(axis=1))
        return np.asarray(ts, np.int64), np.asarray(lum, np.float64), bad, sat

    @staticmethod
    def wall_offset_s() -> float:
        """Sensor timestamps (CLOCK_MONOTONIC) to wall clock, read now."""
        p = cc.clock_pair()
        return (p["realtime_ns"] - p["monotonic_ns"]) / 1e9

    def close(self) -> None:
        self.cam.stop()
        self.cam.close()


def do_walk(args: argparse.Namespace, cam: Camera, dev: dict[str, Any]) -> dict[str, Any]:
    w, h = dev["device"]["width"], dev["device"]["height"]
    lvl = args.level
    command(args.device, "testpattern solid 0 0 0")
    time.sleep(0.2)
    _, dark, bad, _ = cam.frames(30)
    lit: dict[tuple[int, int], list[int]] = {}
    sat_frames = 0
    for y in range(h):
        for x in range(w):
            command(args.device, f"testpattern pixel {x} {y} {lvl} {lvl} {lvl}")
            cam.frames(4)  # let the command reach the LEDs, past any frame in flight
            _, on, b, s = cam.frames(3)
            bad += b
            sat_frames += s
            lit[(x, y)] = ca.lit_regions(on, dark)
    report = ca.walk_report(lit, w, h, len(cam.regions))
    report.update(
        width=w, height=h, level=lvl, control_errors=bad, frames_with_saturation=sat_frames
    )
    return report


def do_timing(args: argparse.Namespace, cam: Camera, dev: dict[str, Any]) -> dict[str, Any]:
    lvl = args.level
    before = clock_sync(args.device)
    reply = command(
        args.device,
        f"testpattern flash {lvl} {lvl} {lvl} {args.on_ms} {args.period_ms} {args.count}",
    )
    expected_s = args.count * args.period_ms * 1.5 / 1000 + args.period_ms / 1000 + 1.0
    n_frames = int(expected_s / (cc.FRAME_US / 1e6)) + 30
    wall = cam.wall_offset_s()
    ts, lum, bad, sat = cam.frames(n_frames)
    wall = (wall + cam.wall_offset_s()) / 2
    deadline = time.monotonic() + 30
    while True:
        status_resp = command(args.device, "testpattern status")
        status = json.loads(status_resp[status_resp.index("{") :])
        if status["done"] or time.monotonic() > deadline:
            break
        time.sleep(1)
    after = clock_sync(args.device)
    if not status["done"]:
        raise SystemExit(f"flash train never finished: {status_resp[:200]}")
    frame = ca.frame_timing(ts, cc.FRAME_US * 1e3)
    t_s = ts / 1e9 + wall
    edges = np.asarray(status["edges"], np.float64)
    latch_s = status["pixels"] * status["usPerPixel"] / 1e6
    # The chip's millis() on the host clock, its drift spread linearly across the train.
    t0 = edges[0, 0]
    span = max(edges[-1, 0] - t0, 1.0)
    drift = after["offset_ms"] - before["offset_ms"]
    on_s = (edges[:, 0] + before["offset_ms"] + drift * (edges[:, 0] - t0) / span) / 1000 + latch_s
    rows = [r["row"] for r in cam.regions]
    sig = lum.sum(axis=1)
    row_all = float(np.mean(rows))
    period_ns = float(np.median(np.diff(ts)))
    mid = (cam.exposure_us * 1e3 - period_ns / 2) / 1e9
    onsets = ca.visual_onsets(sig, ts, row=row_all, min_gap_ms=args.on_ms + 10)
    onset_s = np.array([t_s[o["frame"]] + ca.row_offset_ns(row_all) / 1e9 + mid for o in onsets])
    out: dict[str, Any] = {
        "reply": reply.strip(),
        "on_ms": args.on_ms,
        "period_ms": args.period_ms,
        "level": lvl,
        "frames": frame,
        "control_errors": bad,
        "frames_with_saturation": sat,
        "clock": {"before": before, "after": after, "drift_ms": round(drift, 3)},
        "latch_ms": round(latch_s * 1000, 3),
        "flash": ca.flash_lags(on_s, onset_s),
        "note": (
            "lag = camera onset (mid-interval estimate) - chip on-edge (+ latch) on the host "
            "clock; its median is the camera's offset, its spread the camera's floor"
        ),
    }
    # Line time: the top and bottom LED rows' onset frames for the same flashes.
    top = [k for k, r in enumerate(rows) if abs(r - min(rows)) < 4]
    bottom = [k for k, r in enumerate(rows) if abs(r - max(rows)) < 4]
    if top and bottom:
        tf = [
            o["frame"] for o in ca.visual_onsets(lum[:, top].sum(1), ts, min_gap_ms=args.on_ms + 10)
        ]
        bf = [
            o["frame"]
            for o in ca.visual_onsets(lum[:, bottom].sum(1), ts, min_gap_ms=args.on_ms + 10)
        ]
        tf_s = np.array(tf, np.float64)
        pairs_t, pairs_b = [], []
        for f in bf:  # pair each bottom onset with the top onset within a frame of it
            j = int(np.argmin(np.abs(tf_s - f))) if len(tf_s) else -1
            if j >= 0 and abs(tf_s[j] - f) <= 1:
                pairs_t.append(tf_s[j])
                pairs_b.append(f)
        gap = float(np.mean([rows[k] for k in bottom]) - np.mean([rows[k] for k in top]))
        out["line_time"] = ca.line_time_from_rows(
            np.array(pairs_t), np.array(pairs_b), gap, period_ns
        )
        out["line_time"]["row_gap_px"] = round(gap, 1)
    return out


def do_pwm(args: argparse.Namespace, cam: Camera, dev: dict[str, Any]) -> dict[str, Any]:
    lvl = args.level
    command(args.device, "testpattern solid 0 0 0")
    time.sleep(0.2)
    _, dark, _, _ = cam.frames(30)
    pedestal = dark.mean(axis=0)
    command(args.device, f"testpattern solid {lvl} {lvl} {lvl}")
    time.sleep(0.2)
    sweep = []
    try:
        for exp in range(args.exp_from, args.exp_to + 1, args.exp_step):
            cam.set_exposure(exp)
            _, lum, bad, sat = cam.frames(40)
            sig = lum - pedestal
            mean = sig.mean(axis=0)
            ok = mean > 0
            cv = float(np.median(sig.std(axis=0)[ok] / mean[ok])) if ok.any() else None
            sweep.append(
                {
                    "exposure_us": exp,
                    "ripple_cv": cv,
                    "mean": float(mean.mean()),
                    "saturated": sat,
                    "control_errors": bad,
                }
            )
    finally:
        cam.set_exposure(args.exposure_us)
    valid = [s for s in sweep if s["ripple_cv"] is not None and not s["saturated"]]
    best = sorted(valid, key=lambda s: s["ripple_cv"])[:5]
    return {"level": lvl, "sweep": sweep, "lowest_ripple": best}


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("mode", choices=["walk", "timing", "pwm"])
    p.add_argument("--device", help="device id (default: the only connected one)")
    p.add_argument("--regions", type=Path, required=True)
    p.add_argument("--exposure-us", type=int, default=1000)
    p.add_argument("--gain", type=float, default=1.0)
    p.add_argument("--level", type=int, default=80, help="pattern colour 0-255 (white)")
    p.add_argument("--on-ms", type=int, default=40)
    p.add_argument("--period-ms", type=int, default=250)
    p.add_argument("--count", type=int, default=200)
    p.add_argument("--exp-from", type=int, default=200)
    p.add_argument("--exp-to", type=int, default=4000)
    p.add_argument("--exp-step", type=int, default=25)
    p.add_argument("--out", type=Path)
    args = p.parse_args(argv)
    if not args.device:
        connected = [d["id"] for d in api("GET", "/devices") if d["state"] == "connected"]
        if len(connected) != 1:
            raise SystemExit(f"pass --device: connected devices are {connected}")
        args.device = connected[0]
    out_dir = args.out or OUT_ROOT / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.mode}"
    out_dir.mkdir(parents=True, exist_ok=True)
    dev = info(args.device)
    cam = Camera(args.regions, args.exposure_us, args.gain)
    try:
        fn = {"walk": do_walk, "timing": do_timing, "pwm": do_pwm}[args.mode]
        report = fn(args, cam, dev)
    finally:
        cam.close()
        try:
            command(args.device, "testpattern off")
        except (OSError, SystemExit) as e:
            print(f"WARNING: testpattern off failed on {args.device}: {e}", file=sys.stderr)
    report.update(mode=args.mode, device=args.device, version=dev.get("version"))
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, default=float))
    print(
        json.dumps(
            {k: v for k, v in report.items() if k not in ("mapping", "sweep")},
            indent=2,
            default=float,
        )
    )
    print(f"report in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
