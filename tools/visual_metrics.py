#!/usr/bin/env python3
"""
visual_metrics.py — agent-friendly audio-visual responsiveness measurement.

Captures the firmware's combined audio + visual JSON stream from one or more
devices in parallel, then computes summary statistics and audio→visual
coupling metrics. Designed for iterative parameter tuning loops where a
human cannot say "how's it look?" — emits machine-readable JSON by default.

Usage
-----
    # Capture 15s from a single port, human-readable table:
    python tools/visual_metrics.py --port COM12

    # Capture from multiple ports in parallel, emit JSON:
    python tools/visual_metrics.py --port COM12 --port COM15 --port COM21 --json

    # Auto-detect all XIAO ports (Windows only):
    python tools/visual_metrics.py --all

    # Pass/fail gate for a CI/agent loop (exits non-zero on regression):
    python tools/visual_metrics.py --port COM12 --json \
        --require-pulse-corr 0.30 --require-modulation 0.50

Metrics
-------
Per device:
  mic_raw_*       Raw PDM amplitude (firmware-version-agnostic). Diagnoses
                  physical mic / acoustic-path health.
  mic_pk_*        Peak tracker (envelope of raw).
  energy_*        AudioControl.energy (post smolder floor).
  pulse_*         AudioControl.pulse (transient envelope).
  plp_pulse_*     PLP-derived pulse (music-mode response).
  rhythm_str_*    rhythmStrength (0–1 music-mode engagement).
  vis_L_*         Per-frame avg luminance (post-generator+effect, 0–255).
  vis_mx_*        Per-frame max luminance.
  vis_ct_*        RMS contrast within frame.
  vis_act_*       Frame-to-frame pixel activity (0–1).

Audio→visual coupling:
  pulse_corr      Peak cross-correlation R between `pulse` and `vis_L`
                  (best Pearson over lag 0–250 ms).
  pulse_lag_ms    Time-lag at peak correlation (positive = visual follows audio).
  modulation_depth  (vis_L_max − vis_L_min) / vis_L_mean — how dramatically
                  brightness swings over the capture window.
  pulse_response_ratio  mean(vis_L | pulse > 0.5) / mean(vis_L | pulse < 0.1)
                  — how much brighter does fire get during loud beats vs quiet?

Exit codes (for agent loops):
  0   capture OK, all gates passed (or no gates set)
  1   capture failed (port error, no data, parse error)
  2   one or more gates failed
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import serial  # pyserial


JSON_LINE_RE = re.compile(r'^\{"a":\{')


# -------------------------- Capture --------------------------------------


@dataclass
class SamplePoint:
    """One serial stream sample (one printed JSON line)."""
    t_ms: float                  # host monotonic timestamp in ms
    mic_l: float = 0.0
    mic_pk: float = 0.0
    mic_raw: float = 0.0
    energy: float = 0.0
    pulse: float = 0.0
    plp_pulse: float = 0.0
    rhythm_str: float = 0.0
    vis_l: float = 0.0
    vis_mx: float = 0.0
    vis_ct: float = 0.0
    vis_act: float = 0.0
    vis_cx: float = 0.0
    vis_cy: float = 0.0
    vis_sat: float = 0.0
    has_v: bool = False         # true if "v" block was present (firmware has FrameMetrics)


def _parse_line(line: str, t_ms: float) -> Optional[SamplePoint]:
    """Parse one stream JSON line. Returns None if not a stream line."""
    if not JSON_LINE_RE.search(line):
        return None
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None
    a = obj.get('a', {})
    m = obj.get('m', {})
    v = obj.get('v')
    sp = SamplePoint(
        t_ms=t_ms,
        mic_l=float(a.get('l', 0)),
        mic_pk=float(a.get('pk', 0)),
        mic_raw=float(a.get('raw', 0)),
        energy=float(m.get('e', 0)),
        pulse=float(m.get('p', 0)),
        plp_pulse=float(m.get('pp', 0)),
        rhythm_str=float(m.get('str', 0)),
    )
    if v is not None:
        sp.has_v = True
        sp.vis_l = float(v.get('l', 0))
        sp.vis_mx = float(v.get('mx', 0))
        sp.vis_ct = float(v.get('ct', 0))
        sp.vis_act = float(v.get('act', 0))
        sp.vis_cx = float(v.get('cx', 0))
        sp.vis_cy = float(v.get('cy', 0))
        sp.vis_sat = float(v.get('sat', 0))
    return sp


def _capture_one(port: str, duration_s: float, baud: int = 115200) -> List[SamplePoint]:
    """Open `port`, send `stream on`, read for `duration_s`, send `stream off`."""
    samples: List[SamplePoint] = []
    try:
        ser = serial.Serial(port, baud, timeout=0.1, dsrdtr=False)
    except serial.SerialException as e:
        print(f"[{port}] open failed: {e}", file=sys.stderr)
        return samples

    try:
        ser.dtr = True
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b'stream on\n')
        ser.flush()
        t0 = time.monotonic()
        deadline = t0 + duration_s
        buf = b''
        while time.monotonic() < deadline:
            chunk = ser.read(2048)
            if chunk:
                buf += chunk
                # Split on newline; keep tail for next iter.
                *complete, buf = buf.split(b'\n')
                now_ms = (time.monotonic() - t0) * 1000.0
                for raw in complete:
                    line = raw.decode('utf-8', errors='ignore').strip()
                    sp = _parse_line(line, now_ms)
                    if sp:
                        samples.append(sp)
            else:
                time.sleep(0.005)
        ser.write(b'stream off\n')
        ser.flush()
        time.sleep(0.2)
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return samples


def capture(ports: List[str], duration_s: float) -> Dict[str, List[SamplePoint]]:
    """Capture from all ports in parallel via threads (shares the same wall clock)."""
    results: Dict[str, List[SamplePoint]] = {p: [] for p in ports}
    lock = threading.Lock()

    def runner(p: str) -> None:
        s = _capture_one(p, duration_s)
        with lock:
            results[p] = s

    threads = [threading.Thread(target=runner, args=(p,), daemon=True) for p in ports]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


# -------------------------- Analysis -------------------------------------


def _stats(values: List[float]) -> Dict[str, float]:
    if not values:
        return {"n": 0, "min": 0.0, "avg": 0.0, "max": 0.0, "nonzero_pct": 0.0}
    nz = sum(1 for v in values if v > 0)
    return {
        "n": len(values),
        "min": min(values),
        "avg": statistics.fmean(values),
        "max": max(values),
        "nonzero_pct": 100.0 * nz / len(values),
    }


def _xcorr_peak(x: List[float], y: List[float], max_lag: int) -> Tuple[float, int]:
    """
    Cross-correlation Pearson R over lag in [0, max_lag]. Positive lag means y
    is delayed relative to x (so x leads y). Returns (peak_R, lag_at_peak).
    Streaming samples are ~50 ms apart (20 Hz), so max_lag=5 covers 250 ms.
    """
    n = min(len(x), len(y))
    if n < 4 or max_lag < 0:
        return 0.0, 0
    max_lag = min(max_lag, n // 2)
    best_r, best_lag = 0.0, 0
    for lag in range(0, max_lag + 1):
        xs = x[: n - lag]
        ys = y[lag: n]
        if len(xs) < 4:
            break
        # Pearson R
        mx = statistics.fmean(xs)
        my = statistics.fmean(ys)
        num = sum((xs[i] - mx) * (ys[i] - my) for i in range(len(xs)))
        dx = sum((v - mx) ** 2 for v in xs)
        dy = sum((v - my) ** 2 for v in ys)
        if dx <= 0 or dy <= 0:
            continue
        r = num / (dx ** 0.5 * dy ** 0.5)
        if r > best_r:
            best_r, best_lag = r, lag
    return best_r, best_lag


def analyze(samples: List[SamplePoint]) -> Dict:
    """Compute summary stats + audio-visual coupling metrics for one device."""
    if not samples:
        return {"sample_count": 0, "duration_s": 0.0, "error": "no_samples"}

    duration_s = (samples[-1].t_ms - samples[0].t_ms) / 1000.0 if len(samples) > 1 else 0.0
    has_v = any(s.has_v for s in samples)

    result = {
        "sample_count": len(samples),
        "duration_s": round(duration_s, 2),
        "sample_rate_hz": round(len(samples) / duration_s, 1) if duration_s > 0 else 0.0,
        "has_visual_metrics": has_v,
        "stats": {
            "mic_raw": _stats([s.mic_raw for s in samples]),
            "mic_pk":  _stats([s.mic_pk  for s in samples]),
            "mic_l":   _stats([s.mic_l   for s in samples]),
            "energy":  _stats([s.energy  for s in samples]),
            "pulse":   _stats([s.pulse   for s in samples]),
            "plp_pulse":  _stats([s.plp_pulse  for s in samples]),
            "rhythm_str": _stats([s.rhythm_str for s in samples]),
        },
    }

    if has_v:
        vL  = [s.vis_l   for s in samples]
        vmx = [s.vis_mx  for s in samples]
        vct = [s.vis_ct  for s in samples]
        vac = [s.vis_act for s in samples]
        pulse = [s.pulse for s in samples]
        result["stats"]["vis_l"]   = _stats(vL)
        result["stats"]["vis_mx"]  = _stats(vmx)
        result["stats"]["vis_ct"]  = _stats(vct)
        result["stats"]["vis_act"] = _stats(vac)

        # Cross-correlation pulse → vis_L. Stream period ~50 ms so lag in
        # samples * 50 ≈ ms.
        sample_period_ms = duration_s * 1000.0 / max(1, len(samples) - 1)
        max_lag_samples = int(round(250.0 / sample_period_ms))  # search up to 250 ms
        r, lag = _xcorr_peak(pulse, vL, max_lag_samples)
        result["coupling"] = {
            "pulse_corr_R": round(r, 3),
            "pulse_lag_ms": round(lag * sample_period_ms, 1),
        }

        # Modulation depth: (max - min) / mean
        vL_mean = result["stats"]["vis_l"]["avg"]
        vL_min  = result["stats"]["vis_l"]["min"]
        vL_max  = result["stats"]["vis_l"]["max"]
        result["coupling"]["modulation_depth"] = (
            round((vL_max - vL_min) / vL_mean, 3) if vL_mean > 0 else 0.0
        )

        # Pulse response ratio: how much brighter when pulse is loud vs quiet
        loud  = [vL[i] for i, p in enumerate(pulse) if p > 0.5]
        quiet = [vL[i] for i, p in enumerate(pulse) if p < 0.1]
        if loud and quiet and statistics.fmean(quiet) > 0:
            result["coupling"]["pulse_response_ratio"] = round(
                statistics.fmean(loud) / statistics.fmean(quiet), 2
            )
        else:
            result["coupling"]["pulse_response_ratio"] = None

    return result


# -------------------------- Reporting ------------------------------------


def _format_stat_row(label: str, st: Dict[str, float], width: int = 12) -> str:
    if st.get("n", 0) == 0:
        return f"  {label:<{width}} (no data)"
    return (f"  {label:<{width}} n={st['n']:>3}  "
            f"min={st['min']:>8.4f}  avg={st['avg']:>8.4f}  "
            f"max={st['max']:>8.4f}  nz={st['nonzero_pct']:>5.1f}%")


def render_table(port: str, result: Dict) -> str:
    lines = [f"=== {port} ===",
             f"  samples={result['sample_count']}  duration={result.get('duration_s', 0)}s  "
             f"rate={result.get('sample_rate_hz', 0)}Hz  has_v={result.get('has_visual_metrics', False)}"]
    if result.get('error'):
        lines.append(f"  ERROR: {result['error']}")
        return "\n".join(lines)
    s = result["stats"]
    for k in ("mic_raw", "mic_pk", "mic_l", "energy", "pulse", "plp_pulse",
              "rhythm_str", "vis_l", "vis_mx", "vis_ct", "vis_act"):
        if k in s:
            lines.append(_format_stat_row(k, s[k]))
    cp = result.get("coupling")
    if cp:
        lines.append("  ---- coupling ----")
        lines.append(f"  pulse_corr_R         = {cp.get('pulse_corr_R')}")
        lines.append(f"  pulse_lag_ms         = {cp.get('pulse_lag_ms')}")
        lines.append(f"  modulation_depth     = {cp.get('modulation_depth')}")
        lines.append(f"  pulse_response_ratio = {cp.get('pulse_response_ratio')}")
    return "\n".join(lines)


# -------------------------- Port discovery -------------------------------


def list_xiao_ports() -> List[str]:
    """Windows: enumerate XIAO (VID 2886) COM ports via PowerShell."""
    if sys.platform != "win32":
        return []
    ps = (r"Get-CimInstance Win32_PnPEntity | "
          r"Where-Object { $_.Caption -match 'COM\d+' -and $_.DeviceID -match 'VID_2886' } | "
          r"ForEach-Object { ($_.Caption -replace '.*\((COM\d+)\).*','$1') }")
    try:
        out = subprocess.check_output(["powershell", "-NoProfile", "-Command", ps],
                                       timeout=10).decode(errors="ignore").strip()
        return [line.strip() for line in out.splitlines() if line.strip()]
    except subprocess.SubprocessError:
        return []


# -------------------------- Main -----------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Capture audio+visual stream from device(s) and report metrics.")
    ap.add_argument("--port", action="append", default=[],
                    help="Serial port (e.g. COM12). Repeat for multiple devices.")
    ap.add_argument("--all", action="store_true",
                    help="Auto-detect all XIAO ports (Windows only).")
    ap.add_argument("--duration", type=float, default=15.0,
                    help="Capture duration in seconds (default: 15).")
    ap.add_argument("--json", action="store_true",
                    help="Emit machine-readable JSON instead of a table.")
    ap.add_argument("--require-pulse-corr", type=float, default=None,
                    help="Fail if any port's pulse_corr_R is below this threshold.")
    ap.add_argument("--require-modulation", type=float, default=None,
                    help="Fail if any port's modulation_depth is below this threshold.")
    ap.add_argument("--require-pulse-ratio", type=float, default=None,
                    help="Fail if any port's pulse_response_ratio is below this threshold.")
    args = ap.parse_args()

    ports = list(args.port)
    if args.all:
        ports.extend(p for p in list_xiao_ports() if p not in ports)
    if not ports:
        print("ERROR: specify --port COMxx (one or more) or --all", file=sys.stderr)
        return 1

    captures = capture(ports, args.duration)
    analyses = {p: analyze(captures[p]) for p in ports}

    if args.json:
        print(json.dumps(analyses, indent=2))
    else:
        for p in ports:
            print(render_table(p, analyses[p]))
            print()

    # Gate checks
    failed = []
    for p, r in analyses.items():
        cp = r.get("coupling", {})
        if args.require_pulse_corr is not None:
            v = cp.get("pulse_corr_R")
            if v is None or v < args.require_pulse_corr:
                failed.append(f"{p}: pulse_corr_R={v} < {args.require_pulse_corr}")
        if args.require_modulation is not None:
            v = cp.get("modulation_depth")
            if v is None or v < args.require_modulation:
                failed.append(f"{p}: modulation_depth={v} < {args.require_modulation}")
        if args.require_pulse_ratio is not None:
            v = cp.get("pulse_response_ratio")
            if v is None or v < args.require_pulse_ratio:
                failed.append(f"{p}: pulse_response_ratio={v} < {args.require_pulse_ratio}")
    if failed:
        print("\nGATE FAILURES:", file=sys.stderr)
        for f in failed:
            print(f"  {f}", file=sys.stderr)
        return 2

    # Bail if any port returned no samples (port issue / device dead)
    if any(r.get("sample_count", 0) == 0 for r in analyses.values()):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
