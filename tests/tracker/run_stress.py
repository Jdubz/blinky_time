#!/usr/bin/env python3
"""Stress the tracker harness with pathological input at extreme loop rates.

Signals are synthesized here (no fixtures): digital silence, a mid-track gap,
full-scale clipping, white noise, a 50 Hz hum, and 128 BPM clicks. Each runs at
15, 60 and 250 loop fps. Build the harness with
`-fsanitize=undefined,float-cast-overflow,float-divide-by-zero
-fno-sanitize-recover=all` so any UB aborts it.

Fails (exit 1) if the harness exits non-zero (UB abort, or exit 3 when a
firmware BLINKY_ASSERT fired after begin()), writes no rows, or emits a
non-finite phase / bpm / period.

Also checks the level normalizer on a smooth, transient-free noise riser
(#176): `level` must not stay pinned at 1.0 for RISER_MAX_PIN_S or longer
during the rise (it used to sit at 1.0 for ~9 s of a 20 dB / 20 s sweep).

    python tests/tracker/run_stress.py --harness build/tracker_harness_ubsan
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

SR = 16000
DUR_S = 40.0
FPS = (15.0, 60.0, 250.0)
RISER_MAX_PIN_S = 2.0
RISER_HIT_MAX_DROP = 0.15
# One loud block: shorter than one 16 ms PDM block, so the instant-adapt path
# (which needs 2 consecutive loud blocks) ignores it by design.
HIT_S = 0.010


def signals() -> dict[str, np.ndarray]:
    n = int(SR * DUR_S)
    t = np.arange(n) / SR
    rng = np.random.default_rng(1)
    clicks = np.zeros(n)
    period = int(SR * 60.0 / 128.0)
    for i in range(0, n, period):
        clicks[i:i + 160] = np.hanning(160) * 0.9
    gap = clicks.copy()
    gap[int(n * 0.4):int(n * 0.6)] = 0.0  # 8 s dropout mid-groove
    return {
        "zeros": np.zeros(n),
        "gap": gap,
        "clip": np.clip(clicks * 8.0 + 0.5 * np.sin(2 * np.pi * 60 * t), -1.0, 1.0),
        "noise": rng.uniform(-1.0, 1.0, n),
        "tone50": 0.8 * np.sin(2 * np.pi * 50 * t),
        "click128": clicks,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True)
    args = ap.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        for name, x in signals().items():
            pcm = tmpd / f"{name}.pcm"
            (np.clip(x, -1.0, 1.0) * 32767).astype("<i2").tofile(pcm)
            for fps in FPS:
                out = tmpd / f"{name}_{fps:.0f}.csv"
                proc = subprocess.run([args.harness, str(pcm), str(out), str(fps)],
                                      capture_output=True, text=True)
                label = f"{name}@{fps:.0f}fps"
                if proc.returncode != 0:
                    # Exit 3 = a firmware BLINKY_ASSERT fired after begin(); its
                    # [ASSERT] text goes to stdout via the Serial stub.
                    asserts = "\n".join(l for l in proc.stdout.splitlines()
                                        if "[ASSERT]" in l or "[FALLBACK]" in l)
                    failures.append(f"{label}: exit {proc.returncode}\n"
                                    f"{proc.stderr[-2000:]}{asserts[-2000:]}")
                    continue
                if "runtime error" in proc.stderr:
                    failures.append(f"{label}: sanitizer report\n{proc.stderr[-2000:]}")
                    continue
                rows = np.genfromtxt(out, delimiter=",", names=True)
                if rows.size == 0:
                    failures.append(f"{label}: no output rows")
                    continue
                bad = [c for c in ("phase", "bpm", "period_f") if not np.all(np.isfinite(rows[c]))]
                if bad:
                    failures.append(f"{label}: non-finite {', '.join(bad)}")
                    continue
                print(f"ok  {label:18s} rows={rows.size:6d} bpm_end={rows['bpm'][-1]:7.2f}")
        for fps in FPS:
            for check in (check_riser, check_riser_hit):
                fail = check(args.harness, tmpd, fps)
                if fail:
                    failures.append(fail)
                else:
                    print(f"ok  {check.__name__}@{fps:.0f}fps")
    total = len(signals()) * len(FPS) + 2 * len(FPS)
    for f in failures:
        print(f"FAIL {f}")
    print(f"{total - len(failures)}/{total} passed")
    return 1 if failures else 0


RISE = (10.0, 30.0)  # rise window in the riser signal (s)


def riser_signal(hit_at: float | None = None) -> np.ndarray:
    """10 s steady noise, a 20 dB / 20 s noise sweep, 10 s at the top.

    hit_at: optionally add one loud burst (HIT_S long, under one PDM block) at that time (s)."""
    rng = np.random.default_rng(5)
    n = int(20 * SR)
    sig = np.concatenate([rng.uniform(-1, 1, 10 * SR) * 0.15,
                          rng.uniform(-1, 1, n) * np.geomspace(0.03, 0.3, n),
                          rng.uniform(-1, 1, 10 * SR) * 0.3])
    if hit_at is not None:
        i = int(hit_at * SR)
        sig[i:i + int(HIT_S * SR)] = rng.uniform(-1, 1, int(HIT_S * SR)) * 0.95
    return sig


def run_level(harness: str, tmpd: Path, sig: np.ndarray, fps: float, tag: str):
    """Returns (t, level) or an error string. Rejects missing / non-finite output."""
    pcm, out = tmpd / f"{tag}.pcm", tmpd / f"{tag}.csv"
    (np.clip(sig, -1, 1) * 32767).astype("<i2").tofile(pcm)
    proc = subprocess.run([harness, str(pcm), str(out), str(fps)], capture_output=True, text=True)
    if proc.returncode != 0:
        return f"{tag}: harness exit {proc.returncode}\n{proc.stderr[-1000:]}"
    rows = np.genfromtxt(out, delimiter=",", names=True)
    if rows.size == 0 or "level" not in rows.dtype.names:
        return f"{tag}: no level output"
    t, lv = rows["t_ms"] / 1000.0, rows["level"]
    sel = (t >= RISE[0]) & (t < RISE[1])
    if sel.sum() < 10:
        return f"{tag}: only {int(sel.sum())} rows in the rise window"
    if not np.all(np.isfinite(lv)):
        return f"{tag}: non-finite level ({int((~np.isfinite(lv)).sum())} rows)"
    return t, lv


def check_riser(harness: str, tmpd: Path, fps: float) -> str | None:
    """level must not stay pinned at 1.0 for RISER_MAX_PIN_S during a smooth rise (#176)."""
    tag = f"riser@{fps:.0f}fps"
    res = run_level(harness, tmpd, riser_signal(), fps, tag)
    if isinstance(res, str):
        return res
    t, lv = res
    sel = (t >= RISE[0]) & (t < RISE[1])
    tt, pinned = t[sel], lv[sel] >= 0.999
    longest = cur = 0.0
    for i in range(1, len(tt)):
        cur = cur + (tt[i] - tt[i - 1]) if (pinned[i] and pinned[i - 1]) else 0.0
        longest = max(longest, cur)
    if longest >= RISER_MAX_PIN_S:
        return (f"{tag}: level pinned at 1.0 for {longest:.1f} s during the rise "
                f"(limit {RISER_MAX_PIN_S:.0f} s, #176)")
    return None


def check_riser_hit(harness: str, tmpd: Path, fps: float) -> str | None:
    """A single loud hit mid-rise must not hijack the riser target (#176 review).

    Compares level 0.5-3 s after the hit with the same riser without it."""
    tag = f"riser_hit@{fps:.0f}fps"
    hit = 20.0
    base = run_level(harness, tmpd, riser_signal(), fps, tag + "_base")
    with_hit = run_level(harness, tmpd, riser_signal(hit_at=hit), fps, tag)
    for res in (base, with_hit):
        if isinstance(res, str):
            return res
    (tb, lb), (th, lh) = base, with_hit
    win = np.linspace(hit + 0.5, hit + 3.0, 50)
    drop = float(np.mean(np.interp(win, tb, lb) - np.interp(win, th, lh)))
    if drop > RISER_HIT_MAX_DROP:
        return (f"{tag}: level after a single mid-rise hit is {drop:.2f} below the no-hit run "
                f"(limit {RISER_HIT_MAX_DROP:.2f}); the hit hijacked the riser peak")
    return None

if __name__ == "__main__":
    sys.exit(main())
