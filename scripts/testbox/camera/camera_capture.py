#!/usr/bin/python3
"""Test box camera capture (docs/TEST_BOX_CAMERA_HARNESS_PLAN.md, "Capture").

Runs on the box's system Python, which has ``python3-picamera2`` and ``python3-numpy`` from
apt (scripts/testbox/setup.sh). blinky-server's venv has neither, and the process boundary
keeps camera work out of the server's event loop. Capture only: it reduces frames to a few
numbers per region and never stores video. Analysis is in ``camera_analysis.py``.

Modes:

  still   one frame as a PNG, with the locked controls (framing and levels checks)
  survey  per-pixel mean, variance and max of brightness over N seconds: the input to
          region discovery (camera_analysis.find_regions). Run it while the LEDs animate.
  record  per frame: sequence number, sensor timestamp (CLOCK_MONOTONIC ns), exposure, gain,
          and per region the summed R, G, B, the saturated-pixel count and the peak value.

Every frame's metadata is checked against the locked controls; drifted controls and dropped
frames are counted and reported, never hidden. The summary goes to stdout as one JSON line
and into the output file, so the caller decides whether the run is valid.

The camera is an OV5647 (rolling shutter). Measured on the box, 2026-11-24, 640x480 binned:
frame period 14.816 ms (67.5 fps, sd 1.5 us); exposure steps and the rolling-shutter line time of
about 31.75 us (camera_analysis.LINE_NS).
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

SIZE = (640, 480)
FRAME_US = 14815  # the sensor's ceiling at 640x480 (67.5 fps)
SATURATED = 250  # a channel at or above this is clipped (8-bit)
EXPOSURE_TOL_US = 40  # about 2.5 line times: the sensor rounds to whole lines
GAIN_TOL = 0.02

_stop = False


def _on_signal(signum: int, frame: Any) -> None:
    global _stop
    _stop = True


def clock_pair() -> dict[str, int]:
    """The host clocks read together: the sensor timestamp is CLOCK_MONOTONIC; boottime is
    recorded too, so a suspend (never expected on the box) would show as a difference."""
    return {
        "monotonic_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC),
        "boottime_ns": time.clock_gettime_ns(time.CLOCK_BOOTTIME),
        "realtime_ns": time.time_ns(),
    }


def open_camera(exposure_us: int, gain: float) -> Any:
    """Camera started with every automatic control off: fixed frame period, exposure, analog
    gain and white balance, and no denoise (it blends frames, which smears onsets)."""
    from libcamera import controls as lc  # type: ignore[import-not-found]
    from picamera2 import Picamera2  # type: ignore[import-not-found]

    cam = Picamera2()
    cfg = cam.create_video_configuration(
        main={"size": SIZE, "format": "RGB888"},
        buffer_count=6,
        controls={
            "FrameDurationLimits": (FRAME_US, FRAME_US),
            "AeEnable": False,
            "AwbEnable": False,
            "ExposureTime": exposure_us,
            "AnalogueGain": gain,
            "ColourGains": (1.0, 1.0),
            "NoiseReductionMode": lc.draft.NoiseReductionModeEnum.Off,
        },
    )
    cam.configure(cfg)
    cam.start()
    return cam


def settle(
    cam: Any, exposure_us: int, gain: float, timeout_s: float = 3.0, digital_tol: float = 0.02
) -> dict[str, Any]:
    """Wait until frames carry the locked controls (they apply a few frames after start)."""
    deadline = time.monotonic() + timeout_s
    md: dict[str, Any] = {}
    while time.monotonic() < deadline:
        md = cam.capture_metadata()
        if not control_errors(md, exposure_us, gain, digital_tol):
            return md
    raise SystemExit(
        f"camera controls never locked: {control_errors(md, exposure_us, gain, digital_tol)}"
    )


def control_errors(
    md: dict[str, Any], exposure_us: int, gain: float, digital_tol: float = GAIN_TOL
) -> list[str]:
    """What in a frame's metadata differs from the locked controls (a missing field counts)."""
    out = []
    # FrameDuration is not checked: on this OV5647 it reports 16001 us (the driver's listed
    # 62.5 fps) while the sensor timestamps run at 14816 us (measured 2026-11-24). The frame
    # period is checked from the timestamps instead (_timing; camera_analysis.frame_timing).
    cg = md.get("ColourGains")
    if cg is None or any(abs(g - 1.0) > GAIN_TOL for g in cg):
        out.append(f"colour gains {cg}, want (1.0, 1.0)")
    exp = md.get("ExposureTime")
    if exp is None or abs(exp - exposure_us) > EXPOSURE_TOL_US:
        out.append(f"exposure {exp} us, want {exposure_us}")
    ag = md.get("AnalogueGain")
    if ag is None or abs(ag - gain) > GAIN_TOL * gain:
        out.append(f"analog gain {ag}, want {gain}")
    dg = md.get("DigitalGain")
    # The ISP adds digital gain when a requested exposure falls between the sensor's steps
    # (~31.75 us here); timing captures keep it at 1.0, a ripple sweep may tolerate more.
    if dg is not None and abs(dg - 1.0) > digital_tol:
        out.append(f"digital gain {dg}, want 1.0")
    return out


def _settings(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "size": list(SIZE),
        "frame_us": FRAME_US,
        "exposure_us": args.exposure_us,
        "analog_gain": args.gain,
        "colour_gains": [1.0, 1.0],
        "format": "RGB888",
    }


def cmd_still(args: argparse.Namespace) -> dict[str, Any]:
    from PIL import Image

    cam = open_camera(args.exposure_us, args.gain)
    try:
        settle(cam, args.exposure_us, args.gain)
        req = cam.capture_request()  # image and metadata from the same frame
        try:
            md = req.get_metadata()
            frame = req.make_array("main")
        finally:
            req.release()
    finally:
        cam.stop()
        cam.close()
    errors = control_errors(md, args.exposure_us, args.gain)
    if errors:
        raise SystemExit(f"still frame's controls drifted: {errors}")
    Image.fromarray(frame[:, :, ::-1]).save(args.out)  # RGB888 is B, G, R in memory
    return {
        "mode": "still",
        "out": str(args.out),
        "settings": _settings(args),
        "max": int(frame.max()),
        "saturated_px": int((frame >= SATURATED).any(axis=2).sum()),
        "exposure_us": md.get("ExposureTime"),
        "analog_gain": md.get("AnalogueGain"),
    }


def _frames(cam: Any, seconds: float) -> Any:
    """(metadata, BGR frame) until the time is up or a signal arrives."""
    end = time.monotonic() + seconds
    while not _stop and time.monotonic() < end:
        req = cam.capture_request()
        try:
            md = req.get_metadata()
            frame = req.make_array("main")
        finally:
            req.release()
        yield md, frame


def _timing(ts: np.ndarray) -> dict[str, Any]:
    """Frame spacing, and dropped frames counted against the locked period (a gap over 1.5 of
    it): the observed median would hide sustained loss."""
    if len(ts) < 2:
        return {"frames": len(ts), "dropped": 0}
    d = np.diff(ts) / 1e3  # us
    period = float(np.median(d))
    gaps = d[d > 1.5 * FRAME_US]
    return {
        "frames": len(ts),
        "period_us": round(period, 3),
        "period_sd_us": round(float(d.std()), 3),
        "dropped": int(np.round(gaps / FRAME_US).sum() - len(gaps)),
        "longest_gap_us": round(float(d.max()), 1),
    }


def cmd_survey(args: argparse.Namespace) -> dict[str, Any]:
    """Per-pixel mean and variance of brightness (R+G+B) from integer sums, plus the per-pixel
    max. A survey isn't timed, so a frame dropped while summing costs nothing but a sample."""
    cam = open_camera(args.exposure_us, args.gain)
    n = 0
    total = total_sq = peak = None
    ts: list[int] = []
    bad = 0
    start = clock_pair()
    try:
        settle(cam, args.exposure_us, args.gain)
        for md, frame in _frames(cam, args.seconds):
            if control_errors(md, args.exposure_us, args.gain):
                bad += 1
                continue
            y = frame.sum(axis=2, dtype=np.uint32)  # <= 765, so y*y fits in uint32
            ts.append(int(md["SensorTimestamp"]))
            n += 1
            if total is None:
                total = np.zeros(y.shape, np.uint64)
                total_sq = np.zeros(y.shape, np.uint64)
                peak = np.zeros(y.shape, np.uint8)
            total += y
            total_sq += y * y
            np.maximum(peak, frame.max(axis=2), out=peak)
    finally:
        cam.stop()
        cam.close()
    if n < 2 or total is None or total_sq is None or peak is None:
        raise SystemExit("survey captured fewer than 2 good frames")
    mean = total / n
    var = np.maximum(total_sq / n - mean * mean, 0.0) * n / (n - 1)
    summary = {
        "mode": "survey",
        "settings": _settings(args),
        "clock": {"start": start, "end": clock_pair()},
        "timing": _timing(np.asarray(ts, np.int64)),
        "control_errors": bad,
        "saturated_px": int((peak >= SATURATED).sum()),
    }
    np.savez_compressed(
        args.out,
        mean=mean.astype(np.float32),
        var=var.astype(np.float32),
        peak=peak,
        summary=json.dumps(summary),
    )
    summary["out"] = str(args.out)
    return summary


def region_index(labels: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """For a label image (0 = none, 1..K = regions): the flat pixel indices sorted by region,
    the start offset of each region in them, and the region ids. Lets each frame reduce with
    one gather and one ``np.add.reduceat``."""
    flat = labels.ravel()
    order = np.flatnonzero(flat)
    order = order[np.argsort(flat[order], kind="stable")]
    ids, starts = np.unique(flat[order], return_index=True)
    return order, starts, ids


def reduce_frame(
    frame: np.ndarray, order: np.ndarray, starts: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Per region: summed (R, G, B), saturated-pixel count, peak value. ``frame`` is BGR."""
    px = frame.reshape(-1, 3)[order]
    rgb = np.add.reduceat(px[:, ::-1].astype(np.uint32), starts, axis=0)
    sat = np.add.reduceat((px >= SATURATED).any(axis=1).astype(np.uint32), starts)
    peak = np.maximum.reduceat(px.max(axis=1), starts)
    return rgb, sat, peak


def cmd_record(args: argparse.Namespace) -> dict[str, Any]:
    regions = np.load(args.regions)
    labels = regions["labels"]
    if tuple(labels.shape) != (SIZE[1], SIZE[0]):
        raise SystemExit(f"region map is {labels.shape}, camera is {SIZE[1]}x{SIZE[0]}")
    made_with = json.loads(str(regions["settings"]))
    if made_with != _settings(args):
        raise SystemExit(
            f"region map was made with {made_with}, recording with {_settings(args)}: "
            "re-run the region survey"
        )
    order, starts, ids = region_index(labels)
    preview_every = args.preview_every_s
    next_preview = 0.0
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)

    seq: list[int] = []
    ts: list[int] = []
    exposure: list[int] = []
    gain: list[float] = []
    rgb_rows: list[np.ndarray] = []
    sat_rows: list[np.ndarray] = []
    peak_rows: list[np.ndarray] = []
    bad = 0
    cam = open_camera(args.exposure_us, args.gain)
    start = clock_pair()
    try:
        settle(cam, args.exposure_us, args.gain)
        print(json.dumps({"ready": True, **clock_pair()}), flush=True)
        for md, frame in _frames(cam, args.seconds):
            if control_errors(md, args.exposure_us, args.gain):
                bad += 1
            rgb, sat, peak = reduce_frame(frame, order, starts)
            seq.append(int(md.get("FrameSequence", -1)) if "FrameSequence" in md else len(seq))
            ts.append(int(md["SensorTimestamp"]))
            exposure.append(int(md.get("ExposureTime", -1)))
            gain.append(float(md.get("AnalogueGain", -1.0)))
            rgb_rows.append(rgb)
            sat_rows.append(sat)
            peak_rows.append(peak)
            if args.preview_dir and preview_every and time.monotonic() >= next_preview:
                from PIL import Image

                Image.fromarray(frame[:, :, ::-1]).save(
                    args.preview_dir / f"{ts[-1]}.jpg", quality=80
                )
                next_preview = time.monotonic() + preview_every
    finally:
        cam.stop()
        cam.close()
    ts_a = np.asarray(ts, np.int64)
    sat_a = np.asarray(sat_rows, np.uint32).reshape(len(ts), len(ids))
    summary = {
        "mode": "record",
        "settings": _settings(args),
        "clock": {"start": start, "end": clock_pair()},
        "timing": _timing(ts_a),
        "regions": len(ids),
        "control_errors": bad,
        "frames_with_saturation": int((sat_a > 0).any(axis=1).sum()),
    }
    np.savez_compressed(
        args.out,
        seq=np.asarray(seq, np.int64),
        ts_ns=ts_a,
        exposure_us=np.asarray(exposure, np.int32),
        analog_gain=np.asarray(gain, np.float32),
        region_ids=ids.astype(np.int32),
        rgb=np.asarray(rgb_rows, np.uint32).reshape(len(ts), len(ids), 3),
        saturated=sat_a,
        peak=np.asarray(peak_rows, np.uint8).reshape(len(ts), len(ids)),
        summary=json.dumps(summary),
    )
    summary["out"] = str(args.out)
    return summary


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--exposure-us", type=int, default=1000)
    p.add_argument("--gain", type=float, default=1.0)
    p.add_argument("--cpu", type=int, help="pin to this core (keep off ffplay's and the server's)")
    sub = p.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("still")
    s.add_argument("--out", type=Path, required=True)
    s = sub.add_parser("survey")
    s.add_argument("--seconds", type=float, default=20.0)
    s.add_argument("--out", type=Path, required=True)
    s = sub.add_parser("record")
    s.add_argument("--regions", type=Path, required=True)
    s.add_argument("--seconds", type=float, default=600.0, help="upper bound; SIGTERM ends it")
    s.add_argument("--out", type=Path, required=True)
    s.add_argument("--preview-dir", type=Path)
    s.add_argument("--preview-every-s", type=float, default=0.0)
    args = p.parse_args(argv)
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)
    out = {"still": cmd_still, "survey": cmd_survey, "record": cmd_record}[args.mode](args)
    print(json.dumps(out), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
