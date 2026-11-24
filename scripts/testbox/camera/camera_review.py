#!/usr/bin/python3
"""Camera review of a validation run on the test box (docs/TEST_BOX_CAMERA_HARNESS_PLAN.md).

A secondary check, never a gate: the camera and the chips run unsynced (camera 67.5 fps,
each chip its own frame loop and clock), so every timing number comes with its spread and a
budget of what limits it. It answers whether what the chips report over serial shows up on
their LEDs, and how late.

1. Starts ``camera_capture.py record`` (pinned to a core away from ffplay and the server).
2. Submits a validation job to the local blinky-server (``POST /api/test/validate`` with
   ``persist_raw`` and ``gen <generator>``), then puts each chip back on the generator it had
   (``gen`` is runtime state, never saved; without the restore it would stay until a reboot).
3. Stops the capture when the job ends, then for each run and device:
   - puts the chip's detections and the camera frames on the host's wall clock (absolute,
     when the raw capture carries ``audio_start_epoch_ms`` and the clock syncs; relative
     otherwise, where only the lag's spread means anything);
   - finds the LEDs that follow that chip's detections (with two chips, each LED belongs to
     the one it follows);
   - matches detections to visual onsets: the share that lit, flashes with no detection,
     and the detection-to-light lag as a distribution, with its timing budget;
   - for a generator that never holds still, freezes.
4. Writes ``report.json`` and ``report.md`` (with the capture and the job) to the output dir.

Runs on the system Python (numpy, picamera2 from apt), like the capture helper.
"""

from __future__ import annotations

import argparse
import json
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import camera_analysis as ca

HERE = Path(__file__).parent
API = "http://127.0.0.1:8420/api"
CORPUS = Path.home() / "blinky-deploy" / "corpus" / "edm"
OUT_ROOT = Path.home() / ".local" / "share" / "blinky-camera" / "reviews"
RESPONSIVE = (
    0.5  # an LED shows a chip's hits: lights for half its detections, half its flashes on one
)

RUN_SLACK_S = 30.0  # between one run's last detection and the next run's audio start, at most

# What each generator lets the camera check. Fire flickers on its own (per-event matching
# means little, but it never holds still); Audio flashes on each detection and holds still
# between them.
PROFILES: dict[str, dict[str, bool]] = {
    "audio": {"events": True, "freezes": False},
    "fire": {"events": False, "freezes": True},
}


def api(method: str, path: str, body: Any = None) -> Any:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        API + path, method=method, data=data, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def chip_info(device_id: str) -> dict[str, Any]:
    """``json info`` (read-only), for the chip's frame rate."""
    out = api("POST", f"/devices/{device_id}/command", {"command": "json info"})
    resp = out.get("response", "")
    return json.loads(resp[resp.index("{") :]) if "{" in resp else {}


def generator(device_id: str) -> str:
    """The chip's current generator (``json state``, read-only), lower-case as ``gen`` takes it."""
    out = api("POST", f"/devices/{device_id}/command", {"command": "json state"})
    resp = out.get("response", "")
    return str(json.loads(resp[resp.index("{") :])["generator"]).lower()


def restore_generators(before: dict[str, str]) -> None:
    """Put each chip back on the generator it had: the run's ``gen`` is runtime state, never
    saved, but it would otherwise stay until the next reboot (a dark grid under ``audio``)."""
    for dev, gen in before.items():
        try:
            api("POST", f"/devices/{dev}/command", {"command": f"gen {gen}"})
        except (OSError, ValueError) as e:  # urllib errors are OSErrors; report, keep the review
            print(f"WARNING: {dev} not restored to generator {gen}: {e}", file=sys.stderr)


def _log_tail(path: Path, n: int = 5) -> str:
    try:
        return " | ".join(path.read_text().strip().splitlines()[-n:]) or "(empty)"
    except OSError as e:
        return f"(no log: {e})"


def run_capture(args: argparse.Namespace, out_dir: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    """The camera recording around one validation job: (capture summary, job). The helper's
    stderr (where it reports why it stopped) goes to ``camera.log`` and into any error here."""
    log_path = out_dir / "camera.log"
    log_file = log_path.open("w")
    cam = subprocess.Popen(
        [
            "/usr/bin/python3",
            str(HERE / "camera_capture.py"),
            "--exposure-us",
            str(args.exposure_us),
            "--cpu",
            str(args.cpu),
            "record",
            "--regions",
            str(args.regions),
            "--seconds",
            str(args.max_seconds),
            "--out",
            str(out_dir / "capture.npz"),
        ],
        stdout=subprocess.PIPE,
        stderr=log_file,
        text=True,
    )
    job_id = None
    try:
        assert cam.stdout is not None
        ready = json.loads(cam.stdout.readline() or "{}")
        if not ready.get("ready"):
            raise SystemExit(f"camera capture didn't start: {_log_tail(log_path)}")
        job = api(
            "POST",
            "/test/validate",
            {
                "device_ids": args.device,
                "track_dir": str(args.track_dir),
                "track_names": args.track,
                "duration_ms": args.duration_ms,
                "num_runs": args.runs,
                "commands": [f"gen {args.generator}"],
                "persist_raw": True,
            },
        )
        job_id = job["job_id"]
        print(f"job {job_id} running; camera recording", flush=True)
        while True:
            time.sleep(3)
            status = api("GET", f"/test/jobs/{job_id}")
            if status.get("status") not in ("pending", "running", "submitted", "queued"):
                break
        time.sleep(0.5)
    finally:
        cam.send_signal(signal.SIGTERM)
        lines = (cam.communicate(timeout=120)[0] or "").strip().splitlines()
        log_file.close()
    if cam.returncode != 0 or not lines:
        raise SystemExit(
            f"camera capture failed (exit {cam.returncode}) during job {job_id}: "
            f"{_log_tail(log_path)}"
        )
    summary = json.loads(lines[-1])
    (out_dir / "job.json").write_text(json.dumps(status))
    if status.get("status") != "complete":
        raise SystemExit(
            f"validation job {job_id} ended {status.get('status')}: {status.get('error')}"
        )
    return summary, status


def frame_epoch_s(ts_ns: np.ndarray, summary: dict[str, Any]) -> np.ndarray:
    """Camera frames on the host wall clock: the sensor timestamp is CLOCK_MONOTONIC, mapped
    through the capture's (monotonic, realtime) pairs, start and end averaged."""
    c = summary["clock"]
    off = np.mean([c[k]["realtime_ns"] - c[k]["monotonic_ns"] for k in ("start", "end")])
    return (ts_ns + off) / 1e9


def detections_epoch_s(raw: dict[str, Any], duration_s: float) -> np.ndarray | None:
    """The chip's detections on the host wall clock, the clock's drift spread linearly over the
    run (its offset was taken before the music). None without absolute timing."""
    start = raw.get("audio_start_epoch_ms")
    clock = raw.get("clock")
    if start is None or not clock:
        return None
    t = np.array([e["t"] for e in raw["transients"]], np.float64)
    drift_s = clock.get("drift_ms", 0.0) / 1000.0
    return start / 1000.0 + t + drift_s * np.clip(t / duration_s, 0, 1)


def review_run(
    run: dict[str, Any],
    capture: dict[str, np.ndarray],
    summary: dict[str, Any],
    regions: list[dict[str, Any]],
    profile: dict[str, bool],
    fps: dict[str, list[float | None]],
    args: argparse.Namespace,
    job: dict[str, Any],
    search_from: float,
) -> dict[str, Any]:
    """One run's review. ``search_from`` (wall clock, s) is where a relative alignment may
    start looking: past the previous run, so a repeated track can't borrow its frames."""
    ts = capture["ts_ns"]
    t_s = frame_epoch_s(ts, summary)
    lum = capture["rgb"].sum(axis=2).astype(np.float64)
    rows = [r["row"] for r in regions]
    period_ns = float(np.median(np.diff(ts)))
    duration_s = args.duration_ms / 1000.0
    raws = run.get("raw_capture") or {}
    dets: dict[str, np.ndarray] = {}
    absolute = True
    for dev, raw in raws.items():
        d = detections_epoch_s(raw, duration_s)
        if d is None:
            absolute = False
            d = np.array([e["t"] for e in raw["transients"]], np.float64)
        dets[dev] = d
    out: dict[str, Any] = {"track": run["track"], "run": run["run"], "absolute_timing": absolute}
    if not absolute:
        # No audio start on the wall clock: line the devices up by the shift that matches most
        # detections, between ``search_from`` and the job's end (runs are sequential, so this
        # run can't land on an earlier one's frames), in two passes: onsets of every region
        # first, then of the regions that pass selection. Only the lag's spread is meaningful.
        first = next(iter(dets))
        cols = list(range(lum.shape[1]))
        # A run's audio starts within one run (plus syncs and the gap between runs) of the
        # previous run's last detection: bounding the search there keeps a repeated track from
        # locking onto a later run's frames.
        latest_start = min(
            float(job["completed_at"]), search_from + args.duration_ms / 1000.0 + RUN_SLACK_S
        )
        slot = (t_s >= search_from) & (t_s <= latest_start + args.duration_ms / 1000.0 + 2.0)
        for _ in range(2):
            on_t: list[float] = []
            for k in cols:
                on = ca.visual_onsets(lum[slot, k], ts[slot], row=rows[k])
                on_t += [t_s[slot][o["frame"]] for o in on]
            span = float(dets[first].max()) if len(dets[first]) else 0.0
            shift = ca.best_constant_offset(
                dets[first],
                np.unique(np.round(on_t, 4)),
                (
                    search_from,
                    max(search_from + 0.001, min(latest_start, job["completed_at"] - span)),
                ),
            )
            d0 = dets[first] + shift
            w0 = (t_s >= d0.min() - 2) & (t_s <= d0.max() + 2)
            rec, prec = ca.responsiveness(lum[w0], ts[w0], rows, d0, t_s[w0])
            cols = ca.select_regions(rec, prec) or cols
        dets = {dev: d + shift for dev, d in dets.items()}
        out["note"] = "relative timing: lag medians are fixed by alignment, only spreads count"
    # The run's frames: from its first detection's audio start to the end of the music.
    lo = min(float(d.min()) for d in dets.values() if len(d)) - 2.0
    hi = max(float(d.max()) for d in dets.values() if len(d)) + 2.0
    w = (t_s >= lo) & (t_s <= hi)
    ts_w, t_w, lum_w = ts[w], t_s[w], lum[w]
    try:
        out["frames"] = ca.frame_timing(ts_w, summary["settings"]["frame_us"] * 1e3)
    except ca.Invalid as e:
        out["invalid"] = str(e)
        out["ends_at"] = hi - 2.0  # the last detection: the next run starts after it
        return out
    resp = {dev: ca.responsiveness(lum_w, ts_w, rows, d, t_w) for dev, d in dets.items()}
    devices: dict[str, Any] = {}
    for dev, d in dets.items():
        recall, precision = resp[dev]
        others = [resp[o][0] for o in resp if o != dev]
        mine = ca.select_regions(recall, precision, np.max(others, axis=0) if others else None)
        rep: dict[str, Any] = {
            "detections": len(d),
            "responsive_leds": [regions[k]["id"] for k in mine],
            "follows_loud_hits_only": [
                regions[k]["id"]
                for k in range(lum.shape[1])
                if recall[k] >= RESPONSIVE and k not in mine
            ],
            "budget": ca.timing_budget(
                frame_ns=period_ns,
                exposure_us=summary["settings"]["exposure_us"],
                chip_fps=min((f for f in fps.get(dev, []) if f), default=None),
                clock=(raws[dev].get("clock") or None),
            ),
        }
        if profile["events"] and mine:
            sig = lum_w[:, mine].sum(axis=1)
            row = float(np.mean([rows[k] for k in mine]))
            on = ca.visual_onsets(sig, ts_w, row=row)
            # Mid-interval: the light came on between the previous frame's exposure and this
            # one's, so its best estimate is half a period before this frame (plus exposure).
            mid = (summary["settings"]["exposure_us"] * 1e3 - period_ns / 2) / 1e9
            on_s = np.array([t_w[o["frame"]] + ca.row_offset_ns(row) / 1e9 + mid for o in on])
            pairs, unlit, orphans = ca.match_events(d, on_s)
            strength = np.array([o["strength"] for o in on])
            lags = [(on_s[j] - d[i]) * 1000.0 for i, j in pairs]
            rep.update(
                lit_share=round(len(pairs) / len(d), 3) if len(d) else None,
                unlit=len(unlit),
                unlit_split=ca.split_unlit(d, unlit),
                flashes=len(on),
                flashes_without_detection=len(orphans),
                orphan_strength_median=round(float(np.median(strength[orphans])), 3)
                if orphans
                else None,
                matched_strength_median=(
                    round(float(np.median(strength[[j for _, j in pairs]])), 3) if pairs else None
                ),
                lag_ms=ca.distribution(lags),
            )
        if profile["freezes"]:
            region = mine or list(range(lum.shape[1]))
            sig = lum_w[:, region].sum(axis=1)
            fr = ca.freezes(sig, ts_w, noise=ca.sensor_noise(sig))
            rep["freezes"] = {"count": len(fr), "longest_ms": max((f["ms"] for f in fr), default=0)}
        devices[dev] = rep
    out["devices"] = devices
    out["ends_at"] = hi - 2.0  # the last detection: the next run starts after it
    return out


def markdown(report: dict[str, Any]) -> str:
    lines = [
        f"# Camera review {report['stamp']}",
        "",
        (
            f"Generator `{report['generator']}`, tracks {', '.join(report['tracks'])}. "
            "Secondary check: camera and chips are unsynced; read lags as distributions within "
            "the budget."
        ),
        "",
        (
            f"Capture: {report['capture']['timing']}, control errors "
            f"{report['capture']['control_errors']}, frames with saturation "
            f"{report['capture']['frames_with_saturation']}."
        ),
        "",
    ]
    for run in report["runs"]:
        lines.append(f"## {run['track']} run {run['run']}")
        if "invalid" in run:
            lines += [f"INVALID: {run['invalid']}", ""]
            continue
        if run.get("note"):
            lines.append(f"_{run['note']}_")
        for dev, r in run["devices"].items():
            lines.append(
                f"- **{dev}**: {r['detections']} detections, {len(r['responsive_leds'])} LEDs follow them"
            )
            if "lag_ms" in r:
                lag = r["lag_ms"]
                lines.append(
                    f"  - lit {r['lit_share']:.0%} ({r['unlit']} unlit: "
                    f"{r['unlit_split']['during_previous_flash']} during the previous flash, "
                    f"{r['unlit_split']['isolated']} isolated); "
                    f"{r['flashes_without_detection']} of {r['flashes']} flashes had no detection "
                    f"(median strength {r['orphan_strength_median']} vs {r['matched_strength_median']} matched)"
                )
                if lag.get("n"):
                    lines.append(
                        f"  - detection to light: median {lag['median']} ms, IQR {lag['iqr']}, "
                        f"p10/p90 {lag['p10']}/{lag['p90']} (n {lag['n']})"
                    )
            if "freezes" in r:
                lines.append(f"  - freezes: {r['freezes']}")
            lines.append(f"  - budget: {json.dumps(r['budget'])}")
        lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--device", action="append", help="device id (default: every connected one)")
    p.add_argument("--track", action="append", help="edm/ track name (repeatable)")
    p.add_argument("--track-dir", type=Path, default=CORPUS)
    p.add_argument("--generator", default="audio", choices=sorted(PROFILES))
    p.add_argument("--duration-ms", type=float, default=35000)
    p.add_argument("--runs", type=int, default=1)
    p.add_argument(
        "--regions", type=Path, required=True, help="region map (camera_analysis regions)"
    )
    p.add_argument("--exposure-us", type=int, default=1000)
    p.add_argument("--cpu", type=int, default=3)
    p.add_argument("--max-seconds", type=float, default=1800)
    p.add_argument("--out", type=Path, help=f"output dir (default {OUT_ROOT}/<stamp>)")
    p.add_argument("--analyze", type=Path, help="re-analyze an earlier review dir, no capture")
    args = p.parse_args(argv)
    if not args.device:
        args.device = [d["id"] for d in api("GET", "/devices") if d["state"] == "connected"]
    if not args.track:
        args.track = ["techno-machine-drum"]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = args.analyze or args.out or OUT_ROOT / stamp
    out_dir.mkdir(parents=True, exist_ok=True)
    regions_npz = np.load(args.regions)
    regions = json.loads(str(regions_npz["regions"]))
    if args.analyze:
        job = json.loads((out_dir / "job.json").read_text())
        fps = json.loads((out_dir / "fps.json").read_text())
    else:
        # The chip's frame rate depends on the generator and the music: read it before and
        # after the run, and let the budget use the lower (the wider chip-frame spread).
        fps = {d[:12]: [chip_info(d).get("fps")] for d in args.device}
        before = {d: generator(d) for d in args.device}
        try:
            _, job = run_capture(args, out_dir)
            for d in args.device:
                fps[d[:12]].append(chip_info(d).get("fps"))
        finally:
            restore_generators(before)
        (out_dir / "fps.json").write_text(json.dumps(fps))
    capture = dict(np.load(out_dir / "capture.npz"))
    summary = json.loads(str(capture["summary"]))
    profile = PROFILES[args.generator]
    runs = []
    search_from = float(job["created_at"])
    for r in job["result"]["results"]:  # in the order they ran
        rev = review_run(r, capture, summary, regions, profile, fps, args, job, search_from)
        search_from = float(rev.pop("ends_at", search_from))
        runs.append(rev)
    report = {
        "stamp": stamp,
        "generator": args.generator,
        "tracks": args.track,
        "devices": args.device,
        "capture": summary,
        "runs": runs,
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, default=float))
    (out_dir / "report.md").write_text(markdown(report))
    print(markdown(report))
    print(f"\nreview in {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
