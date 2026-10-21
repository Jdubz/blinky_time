#!/usr/bin/env python3
"""
release_gate.py - bench-gate a firmware release candidate (docs/RELEASE_PROCESS.md).

Flashes release b<N> to ONE listed test device with tools/deploy_local.py's
gated pipeline, then checks it on the bench and records the result on the
GitHub release as gate-<device_id>.json. tools/release_promote.py refuses to
promote a candidate without a passing gate record.

Checks (each one must pass):
  version      `json info` reports exactly the release's manifest version
  configured   device.configured is true and device.id is unchanged
  fps          loop fps >= --min-fps (default 60: a disaster floor, not a
               quality bar)
  frametime    every 5 s loop-metrics window's longest frame (json info
               maxFrameMs) <= --max-frame-ms (default 100; hat_v1 with music
               peaks at 24-31 ms). A long frame freezes the visuals: settings
               saves, blocking output, stalls (#258, #275)
  stability    no reboot for --stable-minutes (uptime keeps rising) and
               audioOverruns grows by at most --max-overrun-growth
  asserts      no [ASSERT] / [FALLBACK] line seen on the console
  music        with music playing near the device (operator starts it when
               prompted): normalized mic level mean >= --min-level and
               >= --min-onsets-per-s pulse onsets
  operator     the operator confirms the visuals look right (prompt, or
               --operator-confirmed "<note>" when a human already confirmed)

Usage:
  python tools/release_gate.py b214 --port COM3 --device-id hat_v1
  python tools/release_gate.py b214 --port COM3 --device-id hat_v1 --skip-flash  # already on it
"""

import argparse
import datetime
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import serial

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import firmware_release  # noqa: E402
import deploy_local  # noqa: E402  (json_info / DeployError serial helpers)


def log(msg=""):
    print(msg, flush=True)


def watch_console(port, seconds, on_line):
    """Read console lines for `seconds` while `stream on` runs; on_line(line)."""
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(b"stream on\n")
        deadline = time.monotonic() + seconds
        buf = b""
        while time.monotonic() < deadline:
            buf += s.read(4096)
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                on_line(raw.decode("utf-8", "replace").strip())
        s.write(b"stream off\n")
        time.sleep(0.3)
    finally:
        s.close()


def music_check(port, seconds, asserts):
    """Stream with music playing: level mean and pulse-onset rate."""
    levels, onsets, prev_p = [], 0, 0.0

    def on_line(line):
        nonlocal onsets, prev_p
        if "[ASSERT]" in line or "[FALLBACK]" in line:
            asserts.append(line)
        if not line.startswith('{"a":'):
            return
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            return
        levels.append(float(d["a"]["l"]))
        p = float(d.get("m", {}).get("p", 0.0))
        if p > 0.4 and prev_p <= 0.4:   # rising edge of the pulse envelope
            onsets += 1
        prev_p = p

    watch_console(port, seconds, on_line)
    if not levels:
        return {"pass": False, "detail": "no audio stream lines"}
    mean = sum(levels) / len(levels)
    return {"level_mean": round(mean, 3), "onsets_per_s": round(onsets / seconds, 2),
            "samples": len(levels)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", help="release tag, e.g. b214")
    ap.add_argument("--port", required=True)
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--skip-flash", action="store_true", help="device already runs this release")
    ap.add_argument("--min-fps", type=float, default=60.0)
    ap.add_argument("--stable-minutes", type=float, default=5.0)
    ap.add_argument("--max-overrun-growth", type=int, default=5)
    ap.add_argument("--max-frame-ms", type=int, default=100,
                    help="longest single loop frame allowed in any 5 s window (json info maxFrameMs)")
    ap.add_argument("--music-seconds", type=float, default=15.0)
    ap.add_argument("--min-level", type=float, default=0.05)
    ap.add_argument("--min-onsets-per-s", type=float, default=1.0)
    ap.add_argument("--music-ready", action="store_true",
                    help="music is already playing; don't wait for Enter")
    ap.add_argument("--operator-confirmed", metavar="NOTE",
                    help="a human has watched the visuals and approves (recorded verbatim)")
    ap.add_argument("--no-upload", action="store_true", help="write the record locally only")
    args = ap.parse_args()

    checks, asserts = {}, []
    record = {"tag": args.tag, "device_id": args.device_id,
              "started": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")}
    try:
        firmware_release.require_test_device(args.device_id, f"gate of {args.tag}")
        rel = firmware_release.download(args.tag, Path(tempfile.gettempdir()) / "blinky-gate")
        expected = rel["manifest"]["version"]
        record["version"] = expected

        if not args.skip_flash:
            log(f"=== Flashing {args.tag} ({expected}) to {args.device_id} ===")
            r = subprocess.run([sys.executable, str(REPO / "tools/deploy_local.py"), "--port", args.port,
                                "--device-id", args.device_id, "--release", args.tag], cwd=REPO)
            checks["flash"] = {"pass": r.returncode == 0, "exit": r.returncode}
            if r.returncode != 0:
                raise firmware_release.ReleaseError(f"deploy_local exited {r.returncode}")

        log("=== Post-flash checks ===")
        info = deploy_local.json_info(args.port)
        dev = info.get("device") or {}
        checks["version"] = {"pass": info.get("version") == expected, "got": info.get("version")}
        checks["configured"] = {"pass": bool(dev.get("configured")) and dev.get("id") == args.device_id,
                                "id": dev.get("id")}

        log(f"=== Stability: {args.stable_minutes} min (do not reset the device) ===")
        first = info
        fps_seen, last_ms, rebooted = [], int(first.get("millis", 0)), False
        frame_ms_seen = []
        end = time.monotonic() + args.stable_minutes * 60
        while time.monotonic() < end:
            # 2 s of console + ~2.5 s for json info: one read per < 5 s, so
            # every 5 s loop-metrics window's maxFrameMs is read at least once.
            watch_console(args.port, 2, lambda l: asserts.append(l)
                          if ("[ASSERT]" in l or "[FALLBACK]" in l) else None)
            cur = deploy_local.json_info(args.port)
            ms = int(cur.get("millis", 0))
            if ms <= last_ms:
                rebooted = True
            last_ms = ms
            fps_seen.append(float(cur.get("fps") or 0.0))
            if cur.get("maxFrameMs") is not None:
                frame_ms_seen.append(int(cur["maxFrameMs"]))
            log(f"  uptime {ms // 1000}s fps {cur.get('fps')} maxFrameMs {cur.get('maxFrameMs')} overruns {cur.get('audioOverruns')}")
            info = cur
        growth = int(info.get("audioOverruns") or 0) - int(first.get("audioOverruns") or 0)
        fps_min = min(fps_seen) if fps_seen else 0.0
        checks["fps"] = {"pass": fps_min >= args.min_fps, "min": fps_min, "floor": args.min_fps}
        # Missing maxFrameMs is an instrumentation regression, not a pass.
        frame_max = max(frame_ms_seen) if frame_ms_seen else None
        checks["frametime"] = {"pass": frame_max is not None and frame_max <= args.max_frame_ms,
                               "max_ms": frame_max, "limit_ms": args.max_frame_ms,
                               "windows": len(frame_ms_seen)}
        checks["stability"] = {"pass": not rebooted and growth <= args.max_overrun_growth,
                               "rebooted": rebooted, "overrun_growth": growth}

        log("=== Music check ===")
        if not args.music_ready:
            input("  Start music near the device, then press Enter... ")
        m = music_check(args.port, args.music_seconds, asserts)
        m["pass"] = (m.get("level_mean", 0) >= args.min_level
                     and m.get("onsets_per_s", 0) >= args.min_onsets_per_s)
        checks["music"] = m
        checks["asserts"] = {"pass": not asserts, "lines": asserts[:20]}

        log("=== Operator ===")
        note = args.operator_confirmed
        if note is None:
            ans = input("  Do the visuals look right with music (smooth, reactive)? [y/N] ").strip().lower()
            note = "confirmed at prompt" if ans == "y" else None
        checks["operator"] = {"pass": note is not None, "note": note}
    except (firmware_release.ReleaseError, deploy_local.DeployError,
            serial.SerialException, OSError) as e:
        # A reset or USB drop mid-gate is a gate FAILURE: record and upload it
        # so an earlier passing record can't be the one promote finds alone.
        checks["error"] = {"pass": False, "detail": f"{type(e).__name__}: {e}"}
    except KeyboardInterrupt:
        checks["error"] = {"pass": False, "detail": "interrupted"}

    record["checks"] = checks
    record["passed"] = bool(checks) and all(c.get("pass") for c in checks.values())
    record["finished"] = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    out = Path(tempfile.gettempdir()) / f"gate-{args.device_id}.json"
    out.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    log("")
    for name, c in checks.items():
        log(f"  {'PASS' if c.get('pass') else 'FAIL'}  {name}: "
            + ", ".join(f"{k}={v}" for k, v in c.items() if k != "pass"))
    log(f"\nGATE {'PASSED' if record['passed'] else 'FAILED'}: {args.tag} on {args.device_id} ({out})")
    if not args.no_upload:
        try:
            firmware_release.gh("release", "upload", args.tag, str(out), "--clobber")
            log(f"  recorded on release {args.tag}")
        except firmware_release.ReleaseError as e:
            log(f"  ERROR: could not attach the gate record: {e}")
            return 1
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
