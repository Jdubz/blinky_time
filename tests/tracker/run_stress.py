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
        riser_fail = check_riser(args.harness, tmpd)
        if riser_fail:
            failures.append(riser_fail)
        else:
            print("ok  noise riser: level never pinned >= "
                  f"{RISER_MAX_PIN_S:.0f} s during the rise")
    total = len(signals()) * len(FPS) + 1
    for f in failures:
        print(f"FAIL {f}")
    print(f"{total - len(failures)}/{total} passed")
    return 1 if failures else 0


def check_riser(harness: str, tmpd: Path) -> str | None:
    """10 s steady noise, then a 20 dB / 20 s noise sweep, then 10 s at the top."""
    rng = np.random.default_rng(5)
    n = int(20 * SR)
    sig = np.concatenate([rng.uniform(-1, 1, 10 * SR) * 0.15,
                          rng.uniform(-1, 1, n) * np.geomspace(0.03, 0.3, n),
                          rng.uniform(-1, 1, 10 * SR) * 0.3])
    pcm, out = tmpd / "riser.pcm", tmpd / "riser.csv"
    (np.clip(sig, -1, 1) * 32767).astype("<i2").tofile(pcm)
    proc = subprocess.run([harness, str(pcm), str(out), "60"], capture_output=True, text=True)
    if proc.returncode != 0:
        return f"noise riser: harness exit {proc.returncode}\n{proc.stderr[-1000:]}"
    rows = np.genfromtxt(out, delimiter=",", names=True)
    t = rows["t_ms"] / 1000.0
    sel = (t >= 10.0) & (t < 30.0)
    tt, pinned = t[sel], rows["level"][sel] >= 0.999
    longest = cur = 0.0
    for i in range(1, len(tt)):
        cur = cur + (tt[i] - tt[i - 1]) if (pinned[i] and pinned[i - 1]) else 0.0
        longest = max(longest, cur)
    if longest >= RISER_MAX_PIN_S:
        return (f"noise riser: level pinned at 1.0 for {longest:.1f} s during the rise "
                f"(limit {RISER_MAX_PIN_S:.0f} s, #176)")
    return None


if __name__ == "__main__":
    sys.exit(main())
