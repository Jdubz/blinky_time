"""Gate (b) — TP-vs-FP |d| per candidate feature, from raw held-out captures.

This is the *real* gate (b) — the one the earlier run_gate_check.py could only
approximate. It measures whether each shape feature distinguishes transients
that landed near a GT onset (TP) from transients that fired far from any GT
onset (FP). That's the signal a new NN input would need to carry for the
model to learn "when flatness looks like X, I'm probably wrong".

Inputs:
  --job-result   A held-out validation JSON produced with persist_raw=true.
                 Each track row must include `raw_capture` per device_id12
                 containing `signal_frames` (with `t`, `v`, `nn`) and
                 `transients` (with `t`, `strength`).
  --gt-dir       Directory holding *.onsets_consensus.json / *.beats.json
                 sidecars keyed by track name. Falls back to any GT in the
                 validation job's `tracks` manifest if omitted.

For each transient in each (track, device) pair we:
  1. Find the GT onset nearest in time.
  2. Label TP if |Δt| < 50 ms, FP otherwise.
  3. Look up the signal_frame at the transient timestamp (nearest frame
     within 32 ms = 2 firmware hops).
  4. Collect the signal value for every configured signal.

We then compute per-signal |d|(TP, FP) overall and per-device, and report
whether each candidate passes the gate-(b) threshold (|d| ≥ 0.3). The
threshold is documented in the memory
`project_hybrid_feature_principles.md` and
`docs/HYBRID_FEATURE_ANALYSIS_PLAN.md`.

Usage:
    cd ml-training && ./venv/bin/python -m analysis.run_gate_b \
        --job-result outputs/validation/holdout_<id>_raw.json \
        --gt-dir ../blinky-test-player/music/edm_holdout \
        --out outputs/gate_b/<run-label>
"""

from __future__ import annotations

import argparse
import bisect
import json
import logging
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

log = logging.getLogger("gate_b")

# Tolerance: how close a transient must be to a GT onset to count as TP.
# Matches onset F1 @ 50 ms from scoring.py; keeps the two measurements on
# the same class-boundary convention.
TP_WINDOW_SEC = 0.050

# Max distance between a transient time and the nearest signal_frame we'll
# accept for feature lookup. 2 firmware hops = 32 ms — anything further means
# the stream dropped frames and the reading would be misleading.
FRAME_LOOKUP_TOL_SEC = 0.032

# Gate-(b) threshold. A feature with TP-vs-FP |d| below this is not informative
# about the NN's mistakes at firing moments.
GATE_B_MIN_ABS_D = 0.3


def _sidecar(audio_stem: str, gt_dir: Path, suffix: str) -> Path | None:
    p = gt_dir / f"{audio_stem}{suffix}"
    return p if p.exists() else None


def load_gt_onsets(track_name: str, gt_dir: Path) -> list[float]:
    """Return sorted GT onsets (seconds) for a track stem.

    Prefers `*.onsets_consensus.json`; falls back to `.beats.json` hits with
    expectTrigger=true. Returns empty list if neither exists.
    """
    consensus = _sidecar(track_name, gt_dir, ".onsets_consensus.json")
    if consensus is None:
        # Try stripping common extensions if `track_name` came back with one.
        for ext in (".mp3", ".wav"):
            if track_name.endswith(ext):
                return load_gt_onsets(track_name[: -len(ext)], gt_dir)
    if consensus is not None:
        data = json.loads(consensus.read_text())
        return sorted(float(o["time"]) for o in data.get("onsets", []))
    beats = _sidecar(track_name, gt_dir, ".beats.json")
    if beats is not None:
        data = json.loads(beats.read_text())
        return sorted(
            float(h["time"]) for h in data.get("hits", []) if h.get("expectTrigger", True)
        )
    return []


def nearest_frame_values(
    frame_t: list[float],
    frame_vals: list[dict[str, float]],
    t_sec: float,
) -> dict[str, float] | None:
    """Return the signal_frame.values dict nearest to t_sec, or None if too far.

    frame_t must be sorted ascending. Uses bisect to find the neighbour, then
    returns whichever of (left, right) is closer iff within FRAME_LOOKUP_TOL_SEC.
    """
    if not frame_t:
        return None
    idx = bisect.bisect_left(frame_t, t_sec)
    candidates = []
    if idx > 0:
        candidates.append(idx - 1)
    if idx < len(frame_t):
        candidates.append(idx)
    best = None
    best_dist = math.inf
    for j in candidates:
        d = abs(frame_t[j] - t_sec)
        if d < best_dist:
            best = j
            best_dist = d
    if best is None or best_dist > FRAME_LOOKUP_TOL_SEC:
        return None
    return frame_vals[best]


def cohens_d(a: np.ndarray, b: np.ndarray) -> float:
    """Cohen's d with pooled variance, filtering non-finite values."""
    a = a[np.isfinite(a)]
    b = b[np.isfinite(b)]
    if len(a) < 2 or len(b) < 2:
        return 0.0
    pooled = np.sqrt((a.var(ddof=1) + b.var(ddof=1)) / 2.0)
    if pooled < 1e-12:
        return 0.0
    return float((a.mean() - b.mean()) / pooled)


def collect_tp_fp(
    job_result: dict,
    gt_dir: Path,
) -> dict[str, dict[str, dict[str, list[float]]]]:
    """Walk the job result and bucket (device, signal) → {tp:[], fp:[]}.

    Returns a nested dict: {device_id12: {signal: {"tp": [...], "fp": [...]}}}.
    """
    buckets: dict[str, dict[str, dict[str, list[float]]]] = {}
    result = job_result.get("result", {})
    rows = result.get("results", [])
    for row in rows:
        track_name = row["track"]
        gt = load_gt_onsets(track_name, gt_dir)
        if not gt:
            log.warning("No GT onsets found for %s — skipping", track_name)
            continue
        raw = row.get("raw_capture") or {}
        if not raw:
            log.error(
                "Row for %s has no raw_capture — was the job run with persist_raw?",
                track_name,
            )
            continue
        for dev, cap in raw.items():
            frames = cap.get("signal_frames", [])
            transients = cap.get("transients", [])
            if not frames or not transients:
                continue
            frame_t = [float(f["t"]) for f in frames]
            frame_vals = [f["v"] for f in frames]
            dev_bucket = buckets.setdefault(dev, {})
            for tr in transients:
                t_sec = float(tr["t"])
                # Label vs GT.
                idx = bisect.bisect_left(gt, t_sec)
                best_dist = math.inf
                for j in (idx - 1, idx):
                    if 0 <= j < len(gt):
                        d = abs(t_sec - gt[j])
                        if d < best_dist:
                            best_dist = d
                is_tp = best_dist <= TP_WINDOW_SEC
                # Feature lookup at transient time.
                vals = nearest_frame_values(frame_t, frame_vals, t_sec)
                if vals is None:
                    continue
                for sig, v in vals.items():
                    sig_bucket = dev_bucket.setdefault(sig, {"tp": [], "fp": []})
                    (sig_bucket["tp"] if is_tp else sig_bucket["fp"]).append(float(v))
    return buckets


def summarize(buckets: dict[str, dict[str, dict[str, list[float]]]]) -> dict[str, Any]:
    """Compute per-device per-signal |d| and an overall pool per signal."""
    device_tables: dict[str, list[dict[str, Any]]] = {}
    # Pool everything across devices for the overall table.
    pool: dict[str, dict[str, list[float]]] = {}
    for dev, signals in buckets.items():
        rows: list[dict[str, Any]] = []
        for sig, pools in signals.items():
            tp = np.asarray(pools["tp"], dtype=np.float64)
            fp = np.asarray(pools["fp"], dtype=np.float64)
            d = cohens_d(tp, fp)
            rows.append(
                {
                    "signal": sig,
                    "cohens_d": round(d, 3),
                    "abs_d": round(abs(d), 3),
                    "tp_n": int(tp.size),
                    "fp_n": int(fp.size),
                    "tp_mean": round(float(tp.mean()), 4) if tp.size else 0.0,
                    "fp_mean": round(float(fp.mean()), 4) if fp.size else 0.0,
                }
            )
            p = pool.setdefault(sig, {"tp": [], "fp": []})
            p["tp"].extend(pools["tp"])
            p["fp"].extend(pools["fp"])
        rows.sort(key=lambda r: r["abs_d"], reverse=True)
        device_tables[dev] = rows

    overall: list[dict[str, Any]] = []
    for sig, pools in pool.items():
        tp = np.asarray(pools["tp"], dtype=np.float64)
        fp = np.asarray(pools["fp"], dtype=np.float64)
        d = cohens_d(tp, fp)
        overall.append(
            {
                "signal": sig,
                "cohens_d": round(d, 3),
                "abs_d": round(abs(d), 3),
                "tp_n": int(tp.size),
                "fp_n": int(fp.size),
                "tp_mean": round(float(tp.mean()), 4) if tp.size else 0.0,
                "fp_mean": round(float(fp.mean()), 4) if fp.size else 0.0,
                "passes_gate_b": bool(abs(d) >= GATE_B_MIN_ABS_D),
            }
        )
    overall.sort(key=lambda r: r["abs_d"], reverse=True)
    return {"overall": overall, "per_device": device_tables}


def format_summary_md(summary: dict[str, Any]) -> str:
    lines = [
        "# Gate (b) — TP-vs-FP |d| (pooled across devices)",
        "",
        f"Passing threshold: |d| ≥ {GATE_B_MIN_ABS_D}.",
        "",
        "| signal | |d| | d (signed) | TP n | FP n | TP mean | FP mean | gate b |",
        "|--------|----:|-----------:|-----:|-----:|-------:|-------:|:------:|",
    ]
    for r in summary["overall"]:
        tag = "✅ pass" if r["passes_gate_b"] else "❌ fail"
        lines.append(
            f"| {r['signal']} | {r['abs_d']:.3f} | {r['cohens_d']:+.3f} "
            f"| {r['tp_n']} | {r['fp_n']} | {r['tp_mean']:.4g} | {r['fp_mean']:.4g} "
            f"| {tag} |"
        )
    lines += ["", "## Per-device breakdown", ""]
    for dev, rows in summary["per_device"].items():
        lines.append(f"### {dev}")
        lines.append("")
        lines.append("| signal | |d| | d (signed) | TP n | FP n |")
        lines.append("|--------|----:|-----------:|-----:|-----:|")
        for r in rows:
            lines.append(
                f"| {r['signal']} | {r['abs_d']:.3f} | {r['cohens_d']:+.3f} "
                f"| {r['tp_n']} | {r['fp_n']} |"
            )
        lines.append("")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--job-result",
        type=Path,
        required=True,
        help="Validation job JSON with persist_raw=true.",
    )
    parser.add_argument(
        "--gt-dir",
        type=Path,
        required=True,
        help="Directory containing *.onsets_consensus.json / .beats.json sidecars.",
    )
    parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Output directory for summary.md + summary.json",
    )
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    job = json.loads(args.job_result.read_text())
    buckets = collect_tp_fp(job, args.gt_dir)
    if not buckets:
        log.error(
            "No TP/FP samples collected — check that --job-result was produced "
            "with persist_raw=true and --gt-dir matches the track corpus."
        )
        return 2
    summary = summarize(buckets)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2))
    (args.out / "summary.md").write_text(format_summary_md(summary))

    print(format_summary_md(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
