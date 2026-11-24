"""Test box camera analysis: pure functions on arrays (docs/TEST_BOX_CAMERA_HARNESS_PLAN.md).

No camera and no picamera2 here, only numpy, so CI tests it with synthetic data. The capture
side (camera_capture.py) reduces frames to per-region numbers; this module turns a survey
into a region map and a record into visual onsets.

Design-agnostic: nothing here knows the LED count or layout. Regions are found from what
varies over time, onsets the way the firmware finds audio onsets: a rectified rise, an
adaptive threshold (median + k * MAD), peak picking with a minimum gap.

Measurement problems raise ``Invalid`` with the reason (CLAUDE.md, "No Silent Fallbacks"):
a run with bad data is INVALID, never quietly scored.
"""

from __future__ import annotations

import json
from collections import deque
from dataclasses import dataclass
from typing import Any

import numpy as np

# Measured on the box (OV5647, 640x480 binned, 2026-11-24): exposure moves in steps of about
# 15.87 us, the sensor's line time. Rows are read top to bottom one line time apart, so a row's
# light is sampled (row * LINE_NS) after row 0's. Re-measure if the mode or frame period changes.
LINE_NS = 15_870


class Invalid(Exception):
    """The measurement can't be trusted (not a device failure): the run is INVALID."""


@dataclass(frozen=True)
class Region:
    id: int
    area: int
    row: float  # centroid
    col: float
    bbox: tuple[int, int, int, int]  # top, left, bottom, right (inclusive)


def label_components(mask: np.ndarray) -> np.ndarray:
    """4-connected components of a boolean mask: labels 1..K, 0 elsewhere. Pure numpy and
    Python (no scipy in the box's runtime), linear in the lit pixels."""
    h, w = mask.shape
    labels = np.zeros((h, w), np.int32)
    lit = np.argwhere(mask)
    n = 0
    for r0, c0 in lit:
        if labels[r0, c0]:
            continue
        n += 1
        labels[r0, c0] = n
        todo = deque([(int(r0), int(c0))])
        while todo:
            r, c = todo.popleft()
            for rr, cc in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1)):
                if 0 <= rr < h and 0 <= cc < w and mask[rr, cc] and not labels[rr, cc]:
                    labels[rr, cc] = n
                    todo.append((rr, cc))
    return labels


def box_mean(a: np.ndarray, r: int) -> np.ndarray:
    """Mean over a (2r+1)^2 window (zero-padded), via a summed-area table."""
    c = np.cumsum(np.cumsum(np.pad(a.astype(np.float64), ((r + 1, r), (r + 1, r))), 0), 1)
    k = 2 * r + 1
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def max_filter(a: np.ndarray, r: int) -> np.ndarray:
    """Max over a (2r+1)^2 window."""
    h, w = a.shape
    p = np.pad(a, r, constant_values=-np.inf)
    out = np.full(a.shape, -np.inf)
    for dy in range(2 * r + 1):
        for dx in range(2 * r + 1):
            np.maximum(out, p[dy : dy + h, dx : dx + w], out=out)
    return out


def find_regions(
    var: np.ndarray,
    *,
    radius: int = 5,
    rel: float = 0.3,
    k: float = 8.0,
    min_sd: float = 3.0,
    min_area: int = 12,
) -> tuple[np.ndarray, list[Region]]:
    """The LEDs, from a survey's per-pixel brightness variance.

    Each LED is a local peak of the (lightly blurred) standard-deviation map. Peaks are judged
    against their own neighbourhood, not one global threshold, because generators light some
    LEDs far more often than others (fire rarely reaches the top rows). Then:

    - a peak must clear the noise floor: median + k * MAD of the map, and ``min_sd`` counts;
    - peaks closer than ``radius`` are one LED (a flat top has several equal maxima);
    - an LED's region is the pixels within ``radius`` of its peak, nearest to it of all peaks,
      whose level above the image's median is at least ``rel`` of the peak's (on the
      unblurred map, so a hot pixel stays one pixel): no overlap, and dim glow between LEDs
      (or reflections) is left out;
    - regions under ``min_area`` pixels are dropped.

    ``radius`` should be under half the LED spacing in pixels (the box's grid: about 16 px).
    Returns a label image (uint16, regions numbered 1..K top to bottom, then left to right)
    and the regions. Raises Invalid when nothing varies: the LEDs didn't animate during the
    survey, or the camera doesn't see them."""
    raw = np.sqrt(np.maximum(var, 0.0).astype(np.float64))
    sd = box_mean(raw, 2)  # peaks on the blurred map, membership on the raw one
    med = float(np.median(sd))
    base = float(np.median(raw))
    floor = max(med + k * 1.4826 * float(np.median(np.abs(sd - med))), min_sd)
    ys, xs = np.nonzero((sd == max_filter(sd, radius)) & (sd > floor))
    order = np.argsort(-sd[ys, xs])
    peaks: list[tuple[int, int]] = []
    for i in order:
        y, x = int(ys[i]), int(xs[i])
        if all((y - py) ** 2 + (x - px) ** 2 > radius * radius for py, px in peaks):
            peaks.append((y, x))
    if not peaks:
        raise Invalid("no region varies: did the LEDs animate during the survey?")
    h, w = var.shape
    owner = np.zeros((h, w), np.int32)  # index into peaks, +1
    dist = np.full((h, w), np.inf)
    for n, (py, px) in enumerate(peaks, start=1):
        y0, y1 = max(0, py - radius), min(h, py + radius + 1)
        x0, x1 = max(0, px - radius), min(w, px + radius + 1)
        gy, gx = np.mgrid[y0:y1, x0:x1]
        d2 = (gy - py) ** 2 + (gx - px) ** 2
        level = raw[y0:y1, x0:x1] - base
        ref = float(level[d2 <= radius * radius].max())  # the LED's brightest raw pixel
        ok = (d2 <= radius * radius) & (level >= rel * ref)
        closer = ok & (d2 < dist[y0:y1, x0:x1])
        dist[y0:y1, x0:x1][closer] = d2[closer]
        owner[y0:y1, x0:x1][closer] = n
    found = []
    for n in range(1, len(peaks) + 1):
        rr, cc = np.nonzero(owner == n)
        if len(rr) >= min_area:
            found.append((float(rr.mean()), float(cc.mean()), rr, cc))
    if not found:
        raise Invalid(f"{len(peaks)} peaks, none with {min_area} pixels: lower min_area?")
    # Stable numbering: by row band (a region's height), then column.
    band = max(1.0, float(np.median([np.ptp(f[2]) + 1 for f in found])))
    found.sort(key=lambda f: (round(f[0] / band), f[1]))
    labels = np.zeros(var.shape, np.uint16)
    regions = []
    for i, (row, col, rr, cc) in enumerate(found, start=1):
        labels[rr, cc] = i
        regions.append(
            Region(
                i,
                len(rr),
                round(row, 2),
                round(col, 2),
                (int(rr.min()), int(cc.min()), int(rr.max()), int(cc.max())),
            )
        )
    return labels, regions


def region_map(
    labels: np.ndarray, regions: list[Region], settings: dict[str, Any]
) -> dict[str, Any]:
    """What ``np.savez`` stores as the region map: camera_capture.py's ``record`` refuses a
    map made with other camera settings."""
    return {
        "labels": labels,
        "settings": json.dumps(settings),
        "regions": json.dumps([r.__dict__ for r in regions]),
    }


def row_offset_ns(row: float, line_ns: int = LINE_NS) -> int:
    """How long after the frame's first row a row is sampled (rolling shutter)."""
    return round(row * line_ns)


def frame_timing(ts_ns: np.ndarray, *, max_dropped: int = 0) -> dict[str, Any]:
    """Frame spacing; raises Invalid on more than ``max_dropped`` dropped frames (a gap over
    1.5 periods), since a dropped frame can hide an onset or a freeze."""
    if len(ts_ns) < 3:
        raise Invalid(f"only {len(ts_ns)} frames")
    d = np.diff(ts_ns).astype(np.float64)
    if (d <= 0).any():
        raise Invalid("frame timestamps not increasing")
    period = float(np.median(d))
    gaps = d[d > 1.5 * period]
    dropped = int(np.round(gaps / period).sum() - len(gaps))
    if dropped > max_dropped:
        raise Invalid(f"{dropped} dropped frames (longest gap {d.max() / 1e6:.1f} ms)")
    return {"period_ns": period, "dropped": dropped}


def visual_onsets(
    signal: np.ndarray,
    ts_ns: np.ndarray,
    *,
    row: float = 0.0,
    k: float = 4.0,
    window_s: float = 1.0,
    min_gap_ms: float = 50.0,
    min_rise: float = 0.05,
    min_snr: float = 10.0,
) -> list[dict[str, Any]]:
    """Onsets in one region's brightness series (one value per frame).

    - nothing, if the series' range is under ``min_snr`` times its frame-to-frame noise;
    - flux: the half-wave rectified frame-to-frame rise, normalized by the series' robust
      range (p99 - p1), so thresholds don't depend on LED brightness or exposure;
    - threshold: median + k * MAD of the flux over a centered window of ``window_s``, and at
      least ``min_rise`` of the range (silence would otherwise make noise an onset);
    - peaks: a local maximum of flux over threshold, at least ``min_gap_ms`` after the last.

    An onset's time is the first frame of its rise (the frame where the light first went up),
    plus the region's rolling-shutter row offset. Times are CLOCK_MONOTONIC ns, like the
    sensor timestamps."""
    x = np.asarray(signal, np.float64)
    if len(x) != len(ts_ns):
        raise ValueError("signal and timestamps differ in length")
    lo, hi = np.percentile(x, [1, 99])
    span = hi - lo
    # Frame-to-frame noise (sd), from the MAD of the differences: a region whose range isn't
    # well above it never lit (sensor noise alone, normalized, would look like flashes).
    d = np.diff(x)
    noise = 1.4826 * float(np.median(np.abs(d - np.median(d)))) / np.sqrt(2.0)
    if span <= min_snr * noise or span <= 0:
        return []
    flux = np.zeros_like(x)
    flux[1:] = np.maximum(np.diff(x), 0.0) / span
    period_s = float(np.median(np.diff(ts_ns))) / 1e9
    half = max(1, round(window_s / period_s / 2))
    gap_ns = int(min_gap_ms * 1e6)
    off = row_offset_ns(row)
    out: list[dict[str, Any]] = []
    last = -(10**18)
    for i in range(1, len(x) - 1):
        f = flux[i]
        if f < min_rise or f < flux[i - 1] or f < flux[i + 1]:
            continue
        w = flux[max(0, i - half) : i + half + 1]
        med = float(np.median(w))
        mad = float(np.median(np.abs(w - med)))
        if f <= med + k * 1.4826 * mad:
            continue
        start = i
        while start > 1 and flux[start - 1] >= min_rise:  # a rise spread over frames
            start -= 1
        t = int(ts_ns[start]) + off
        if t - last < gap_ns:
            continue
        out.append({"t_ns": t, "frame": start, "peak_frame": i, "strength": round(float(f), 4)})
        last = t
    return out


def background_level(signal: np.ndarray, pct: float = 5.0) -> float:
    """The region's level with its LEDs dark (a low percentile over the run)."""
    return float(np.percentile(np.asarray(signal, np.float64), pct))


def main(argv: list[str] | None = None) -> int:
    """``regions SURVEY --out MAP``: a region map from a survey (camera_capture.py survey)."""
    import argparse

    p = argparse.ArgumentParser(description="Test box camera analysis")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("regions", help="region map from a survey")
    s.add_argument("survey")
    s.add_argument("--out", required=True)
    s.add_argument("--radius", type=int, default=5, help="under half the LED spacing, px")
    s.add_argument("--min-area", type=int, default=12)
    args = p.parse_args(argv)
    survey = np.load(args.survey)
    settings = json.loads(str(survey["summary"]))["settings"]
    labels, regions = find_regions(survey["var"], radius=args.radius, min_area=args.min_area)
    np.savez_compressed(args.out, **region_map(labels, regions, settings))
    print(
        json.dumps({"regions": len(regions), "out": args.out, "areas": [r.area for r in regions]})
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
