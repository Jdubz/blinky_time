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


def order_rows(found: list[Any]) -> list[Any]:
    """Regions (row, col, rows, cols) in reading order: top row first, each row left to right.
    Rows are grouped by gaps between sorted row centroids (a new row where the gap exceeds half
    a region's height), not fixed bands, so a slightly tilted camera can't split a row."""
    if not found:
        return found
    half = max(1.0, float(np.median([np.ptp(f[2]) + 1 for f in found])) / 2)
    by_row = sorted(found, key=lambda f: f[0])
    rows: list[list[Any]] = [[by_row[0]]]
    for f in by_row[1:]:
        if f[0] - rows[-1][-1][0] > half:
            rows.append([f])
        else:
            rows[-1].append(f)
    return [f for row in rows for f in sorted(row, key=lambda f: f[1])]


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
    found = order_rows(found)
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


def frame_timing(
    ts_ns: np.ndarray, expected_period_ns: float, *, max_dropped: int = 0
) -> dict[str, Any]:
    """Frame spacing against the locked frame period (the observed median is reported, never
    used as the reference: losing every other frame would double it and hide the loss).
    Raises Invalid on more than ``max_dropped`` dropped frames (a gap over 1.5 periods), since
    a dropped frame can hide an onset or a freeze, or when the observed period is off."""
    if len(ts_ns) < 3:
        raise Invalid(f"only {len(ts_ns)} frames")
    d = np.diff(ts_ns).astype(np.float64)
    if (d <= 0).any():
        raise Invalid("frame timestamps not increasing")
    period = float(np.median(d))
    gaps = d[d > 1.5 * expected_period_ns]
    dropped = int(np.round(gaps / expected_period_ns).sum() - len(gaps))
    if dropped > max_dropped:
        raise Invalid(f"{dropped} dropped frames (longest gap {d.max() / 1e6:.1f} ms)")
    if abs(period - expected_period_ns) > 0.01 * expected_period_ns:
        raise Invalid(
            f"frame period {period / 1e6:.3f} ms, locked {expected_period_ns / 1e6:.3f} ms"
        )
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
    - flux: the half-wave rectified frame-to-frame rise, normalized by the series' range (max -
      p1), so thresholds don't depend on LED brightness or exposure;
    - threshold: median + k * MAD of the flux over a centered window of ``window_s``, and at
      least ``min_rise`` of the range (silence would otherwise make noise an onset);
    - peaks: a local maximum of flux over threshold, at least ``min_gap_ms`` after the last.

    An onset's time is the first frame of its rise (the frame where the light first went up),
    plus the region's rolling-shutter row offset. Times are CLOCK_MONOTONIC ns, like the
    sensor timestamps."""
    x = np.asarray(signal, np.float64)
    if len(x) != len(ts_ns):
        raise ValueError("signal and timestamps differ in length")
    # Scale: from a low percentile (the dark level) to the maximum. A high percentile would
    # miss a lone flash in a long recording; noise-only series are still rejected below.
    lo, hi = float(np.percentile(x, 1)), float(x.max())
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


# -- matching serial detections to light ------------------------------------------------------


def match_events(
    det_s: np.ndarray, onset_s: np.ndarray, lo_ms: float = -30.0, hi_ms: float = 80.0
) -> tuple[list[tuple[int, int]], list[int], list[int]]:
    """One-to-one pairs (detection index, onset index) with onset - detection inside
    [lo_ms, hi_ms], closest first; then the detections that never lit and the onsets with no
    detection. The window is wide on purpose: frame quantization, the chip's frame loop and
    the clock sync all spread the lag, and the lag's distribution is what's reported."""
    cand = []
    for i, d in enumerate(det_s):
        for j, o in enumerate(onset_s):
            lag = (o - d) * 1000.0
            if lo_ms <= lag <= hi_ms:
                cand.append((abs(lag), i, j))
    cand.sort()
    used_d: set[int] = set()
    used_o: set[int] = set()
    pairs = []
    for _, i, j in cand:
        if i not in used_d and j not in used_o:
            pairs.append((i, j))
            used_d.add(i)
            used_o.add(j)
    pairs.sort()
    unlit = [i for i in range(len(det_s)) if i not in used_d]
    orphans = [j for j in range(len(onset_s)) if j not in used_o]
    return pairs, unlit, orphans


def distribution(values_ms: np.ndarray | list[float]) -> dict[str, Any]:
    """Median, IQR and p10/p90 (ms): timing is only ever reported as a distribution."""
    v = np.asarray(values_ms, np.float64)
    if len(v) == 0:
        return {"n": 0}
    p10, p25, p50, p75, p90 = np.percentile(v, [10, 25, 50, 75, 90])
    return {
        "n": len(v),
        "median": round(float(p50), 2),
        "iqr": [round(float(p25), 2), round(float(p75), 2)],
        "p10": round(float(p10), 2),
        "p90": round(float(p90), 2),
    }


def best_constant_offset(
    det_s: np.ndarray, onset_s: np.ndarray, search_s: tuple[float, float], tol_ms: float = 20.0
) -> float:
    """The constant shift (s) that lines most detections up with an onset within ``tol_ms``.
    For runs without absolute timing: it fixes the lag's center, so only the lag's spread is
    meaningful, never its median."""
    best, best_n = search_s[0], -1
    for shift in np.arange(search_s[0], search_s[1], 0.001):
        d = onset_s[None, :] - (det_s[:, None] + shift)
        n = int((np.abs(d) < tol_ms / 1000.0).any(axis=1).sum())
        if n > best_n:
            best, best_n = float(shift), n
    # Many shifts tie (anything within the tolerance matches); center on the matched pairs.
    d = onset_s[None, :] - (det_s[:, None] + best)
    nearest = d[np.arange(len(det_s)), np.abs(d).argmin(axis=1)]
    ok = np.abs(nearest) < tol_ms / 1000.0
    return best + (float(np.median(nearest[ok])) if ok.any() else 0.0)


def timing_budget(
    *, frame_ns: float, exposure_us: float, chip_fps: float | None, clock: dict[str, Any] | None
) -> dict[str, Any]:
    """What spreads or shifts a detection-to-light lag, in ms, so a report says how far its
    numbers can be trusted. The camera and the chip run unsynced, so the frame terms are
    uniform spreads, not errors that average out of a single event.

    - camera_frame: the light came on somewhere in the frame before the one that shows it;
      onsets are placed mid-interval, so +-period/2 (sd period/sqrt 12);
    - exposure: the timestamp's position in the exposure (start or readout) is unknown, a
      shift of up to the exposure time;
    - chip_frame: a detection waits for the chip's next render frame, 0 to 1/fps;
    - clock_sync: firmware times are placed on the host clock to within RTT/2 (a constant
      shift for the run); drift is corrected linearly and listed for the record."""
    period_ms = frame_ns / 1e6
    out: dict[str, Any] = {
        "camera_frame_ms": {"spread": round(period_ms, 2), "sd": round(period_ms / 12**0.5, 2)},
        "exposure_ms": {"shift_up_to": round(exposure_us / 1000.0, 3)},
    }
    if chip_fps:
        f = 1000.0 / chip_fps
        out["chip_frame_ms"] = {"spread": round(f, 2), "sd": round(f / 12**0.5, 2)}
    if clock:
        out["clock_sync_ms"] = {"shift_up_to": round(clock["rtt_ms"] / 2, 2)}
        if "drift_ms" in clock:
            out["clock_drift_ms"] = round(clock["drift_ms"], 2)
    return out


def responsiveness(
    signals: np.ndarray, ts_ns: np.ndarray, rows: list[float], det_s: np.ndarray, t_s: np.ndarray
) -> tuple[np.ndarray, np.ndarray]:
    """Per region (columns of ``signals``): recall, the share of detections followed by an
    onset in that region within -30..+80 ms, and precision, the share of the region's onsets
    that follow a detection. A region showing a device's hits has both high; one that only
    moves with the music (an energy bar) lights on loud hits too but flashes without them.
    With two devices, each region belongs to the device it follows best."""
    recall = np.zeros(signals.shape[1])
    precision = np.zeros(signals.shape[1])
    if len(det_s) == 0:
        return recall, precision
    for k in range(signals.shape[1]):
        on = visual_onsets(signals[:, k], ts_ns, row=rows[k])
        if not on:
            continue
        on_s = t_s[[o["frame"] for o in on]] + row_offset_ns(rows[k]) / 1e9
        pairs, _, _ = match_events(det_s, on_s)
        recall[k] = len(pairs) / len(det_s)
        precision[k] = len(pairs) / len(on)
    return recall, precision


def select_regions(
    recall: np.ndarray,
    precision: np.ndarray,
    elsewhere: np.ndarray | None = None,
    *,
    floor: float = 0.5,
    rel: float = 0.8,
) -> list[int]:
    """The regions showing a device's hits: recall and precision both at least ``floor``, an
    F-score (2PR / (P + R)) at least ``rel`` of the best region's, and more recall than any
    other device gets from it (``elsewhere``). Relative, because on a dense track a region that
    moves with the music (an energy bar) clears any fixed bar: on the box's grid the transient
    rows score F ~0.85 and the energy row ~0.55."""
    f = np.where(
        recall + precision > 0, 2 * recall * precision / np.maximum(recall + precision, 1e-9), 0
    )
    if f.max() <= 0:
        return []
    other = np.zeros_like(recall) if elsewhere is None else elsewhere
    return [
        k
        for k in range(len(recall))
        if recall[k] >= floor
        and precision[k] >= floor
        and f[k] >= rel * f.max()
        and recall[k] > other[k]
    ]


def split_unlit(det_s: np.ndarray, unlit: list[int], masked_ms: float = 160.0) -> dict[str, int]:
    """Unlit detections that came while the previous detection's flash was still fading (within
    ``masked_ms``: the second rise barely shows, a generator trait, not a dropped render) and
    the isolated ones (worth a look)."""
    masked = sum(1 for i in unlit if i > 0 and (det_s[i] - det_s[i - 1]) * 1000 < masked_ms)
    return {"during_previous_flash": masked, "isolated": len(unlit) - masked}


def sensor_noise(signal: np.ndarray, near_dark: float = 0.1) -> float:
    """The camera's frame-to-frame noise on a region (sd), from frames near its dark level
    (within ``near_dark`` of the way to its peak), where the LEDs contribute nothing. The
    animation's own spread must not set a freeze threshold. The cut sits well above the dark
    level so it doesn't clip the noise itself."""
    x = np.asarray(signal, np.float64)
    d = np.diff(x)
    lo, hi = np.percentile(x, [5, 99.5])
    cut = lo + near_dark * (hi - lo)
    dark = (x[1:] <= cut) & (x[:-1] <= cut)  # both frames dark: no flash edge in the difference
    d = d[dark] if dark.sum() >= 10 else d
    return 1.4826 * float(np.median(np.abs(d - np.median(d))))


def freezes(
    signal: np.ndarray, ts_ns: np.ndarray, noise: float, min_ms: float = 100.0
) -> list[dict[str, Any]]:
    """Stretches of at least ``min_ms`` where the region's brightness changed by no more than
    the camera's noise (``sensor_noise``) frame to frame: a stuck frame on an animating
    generator. Only meaningful where the generator never holds still (the caller decides)."""
    x = np.asarray(signal, np.float64)
    still = np.abs(np.diff(x)) <= 3 * max(noise, 1e-9)
    out = []
    start = None
    for i, s in enumerate(np.append(still, False)):
        if s and start is None:
            start = i
        elif not s and start is not None:
            ms = (ts_ns[i] - ts_ns[start]) / 1e6
            if ms >= min_ms:
                out.append({"start_ns": int(ts_ns[start]), "ms": round(float(ms), 1)})
            start = None
    return out


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
