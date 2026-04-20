"""Phase 1 feature-catalog runner (GT-only labels, no NN inference).

Loads an audio corpus with consensus-onset ground truth, computes the 10
candidate frame features from `analysis/features.py`, and labels every
frame by GT proximity:

  onset     — within ±50 ms of any GT onset
  non-onset — everything else

For each feature it reports per-class mean / std, Cohen's d between
onset and non-onset, ROC-AUC as a standalone onset classifier, and KS
statistic. Per-track JSONs plus a corpus-wide ranked summary land
under the `--out` directory.

Phase 1 deliberately avoids running any NN. The question is whether
the deterministic signal itself separates onsets from non-onsets on
the audio — independent of any model's opinion about those frames.
Model behavior is Phase 4's problem, not this one's.

Usage:
    cd ml-training && ./venv/bin/python -m analysis.run_catalog \
        --corpus ../blinky-test-player/music/edm \
        --out outputs/feature_catalog
"""

from __future__ import annotations

import argparse
import bisect
import json
import logging
import sys
from pathlib import Path
from typing import Any

import librosa
import numpy as np
from scipy import stats
from sklearn.metrics import roc_auc_score

# Make sibling `scripts/` importable when running as a module.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from analysis.features import SR, compute_all_features, frame_times  # noqa: E402

log = logging.getLogger("feature_catalog")

# Half-width of the window used to label a frame as "onset" — matches
# HYBRID_ONSET_WINDOW_SEC in blinky-server scoring.py.
ONSET_WINDOW_SEC = 0.050


# --------------------------------------------------------------------- data loading


def load_consensus_onsets(audio_path: Path) -> list[float]:
    """Load sorted GT onset times (seconds) from *.onsets_consensus.json.

    Falls back to *.beats.json if consensus is absent.
    """
    stem = audio_path.with_suffix("")
    consensus = stem.with_suffix(".onsets_consensus.json")
    beats = stem.with_suffix(".beats.json")
    if consensus.exists():
        data = json.loads(consensus.read_text())
        onsets = [float(o["time"]) for o in data.get("onsets", [])]
    elif beats.exists():
        data = json.loads(beats.read_text())
        onsets = [
            float(h["time"])
            for h in data.get("hits", [])
            if h.get("expectTrigger", True)
        ]
    else:
        return []
    return sorted(onsets)


def discover_tracks(corpus_dir: Path) -> list[Path]:
    """Return audio paths for tracks with at least one GT source."""
    tracks: list[Path] = []
    for pattern in ("*.mp3", "*.wav"):
        for audio in sorted(corpus_dir.glob(pattern)):
            stem = audio.with_suffix("")
            if (
                stem.with_suffix(".onsets_consensus.json").exists()
                or stem.with_suffix(".beats.json").exists()
            ):
                tracks.append(audio)
    return tracks


# --------------------------------------------------------------------- labeling


def onset_mask(
    frame_secs: np.ndarray, gt_onsets: list[float], window_sec: float
) -> np.ndarray:
    """True where a frame's centre is within ±window_sec of any GT onset.

    gt_onsets must be sorted; lookup is O(log m) per frame via bisect.
    """
    mask = np.zeros(len(frame_secs), dtype=bool)
    if not gt_onsets:
        return mask
    for i, t in enumerate(frame_secs):
        idx = bisect.bisect_left(gt_onsets, t)
        for j in (idx - 1, idx):
            if 0 <= j < len(gt_onsets) and abs(t - gt_onsets[j]) < window_sec:
                mask[i] = True
                break
    return mask


# --------------------------------------------------------------------- statistics


def cohens_d(a: np.ndarray, b: np.ndarray) -> float:
    """Cohen's d with pooled variance. Returns 0.0 on insufficient data."""
    if len(a) < 2 or len(b) < 2:
        return 0.0
    a = a[np.isfinite(a)]
    b = b[np.isfinite(b)]
    if len(a) < 2 or len(b) < 2:
        return 0.0
    pooled = np.sqrt((a.var(ddof=1) + b.var(ddof=1)) / 2.0)
    if pooled < 1e-12:
        return 0.0
    return float((a.mean() - b.mean()) / pooled)


def auc_binary(pos: np.ndarray, neg: np.ndarray) -> float:
    """ROC-AUC of a single feature as a pos-vs-neg classifier."""
    if len(pos) < 2 or len(neg) < 2:
        return 0.5
    x = np.concatenate([pos, neg])
    y = np.concatenate([np.ones(len(pos)), np.zeros(len(neg))])
    if np.std(x) < 1e-12:
        return 0.5
    try:
        return float(roc_auc_score(y, x))
    except ValueError:
        return 0.5


def ks_stat(a: np.ndarray, b: np.ndarray) -> float:
    """KS statistic: 0 = identical distributions, 1 = disjoint."""
    if len(a) < 5 or len(b) < 5:
        return 0.0
    return float(stats.ks_2samp(a, b).statistic)


# --------------------------------------------------------------------- per-track run


def score_track(
    audio_path: Path,
    *,
    window_sec: float,
    target_rms_db: float,
) -> dict[str, Any]:
    """Compute features and onset/non-onset stats for a single track."""
    audio, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    rms = np.sqrt(np.mean(audio**2) + 1e-10)
    audio = audio * (10 ** (target_rms_db / 20) / rms)

    features = compute_all_features(audio)
    n = min(len(v) for v in features.values())
    features = {k: v[:n] for k, v in features.items()}
    ts = frame_times(n)

    gt = load_consensus_onsets(audio_path)
    mask = onset_mask(ts, gt, window_sec)
    onset_count = int(mask.sum())
    non_count = int((~mask).sum())

    per_feature: dict[str, dict[str, float]] = {}
    for name, values in features.items():
        onset_vals = values[mask]
        non_vals = values[~mask]
        per_feature[name] = {
            "onset_mean": float(onset_vals.mean()) if onset_vals.size else 0.0,
            "onset_std": float(onset_vals.std(ddof=1)) if onset_vals.size > 1 else 0.0,
            "non_mean": float(non_vals.mean()) if non_vals.size else 0.0,
            "non_std": float(non_vals.std(ddof=1)) if non_vals.size > 1 else 0.0,
            "cohens_d": cohens_d(onset_vals, non_vals),
            "auc": auc_binary(onset_vals, non_vals),
            "ks": ks_stat(onset_vals, non_vals),
        }

    return {
        "track": audio_path.stem,
        "n_frames": int(n),
        "gt_onsets": len(gt),
        "onset_frames": onset_count,
        "non_onset_frames": non_count,
        "features": per_feature,
    }


# --------------------------------------------------------------------- aggregation


def aggregate(reports: list[dict[str, Any]]) -> dict[str, Any]:
    """Pool across tracks, weighted by per-track onset+non-onset frame count."""
    pooled: dict[str, dict[str, list[float]]] = {}
    for r in reports:
        weight = r["onset_frames"] + r["non_onset_frames"]
        for name, stats_d in r["features"].items():
            bucket = pooled.setdefault(name, {"d": [], "auc": [], "ks": [], "w": []})
            bucket["d"].append(stats_d["cohens_d"])
            bucket["auc"].append(stats_d["auc"])
            bucket["ks"].append(stats_d["ks"])
            bucket["w"].append(weight)

    ranked: list[dict[str, Any]] = []
    for name, b in pooled.items():
        w = np.array(b["w"], dtype=np.float64)
        if w.sum() < 1:
            continue
        d = np.array(b["d"])
        ranked.append(
            {
                "feature": name,
                "mean_abs_cohens_d": float(np.average(np.abs(d), weights=w)),
                "mean_cohens_d_signed": float(np.average(d, weights=w)),
                "positive_tracks": int((d > 0).sum()),
                "mean_auc": float(np.average(b["auc"], weights=w)),
                "mean_ks": float(np.average(b["ks"], weights=w)),
                "tracks": len(b["d"]),
            }
        )
    ranked.sort(key=lambda r: r["mean_abs_cohens_d"], reverse=True)
    return {"ranking": ranked}


def format_summary_md(agg: dict[str, Any], total_frames: int, total_tracks: int) -> str:
    lines = [
        "# Phase 1 Feature Catalog — Ranked Summary",
        "",
        f"Tracks: {total_tracks} · total frames: {total_frames}",
        f"Onset window: ±{int(ONSET_WINDOW_SEC * 1000)} ms around GT onsets",
        "Labels: onset = within ±window of any GT onset; non-onset = everything else.",
        "",
        "Rank by per-track-weighted mean |Cohen's d| between onset and non-onset frames.",
        "Positive signed d means the feature is larger on onset frames than on non-onset frames.",
        "",
        "| rank | feature | \\|d\\| | d (signed) | pos tracks | AUC | KS |",
        "|-----:|---------|-----:|-----------:|-----------:|----:|----:|",
    ]
    for i, row in enumerate(agg["ranking"], start=1):
        lines.append(
            f"| {i} | {row['feature']} | {row['mean_abs_cohens_d']:.3f} "
            f"| {row['mean_cohens_d_signed']:+.3f} "
            f"| {row['positive_tracks']}/{row['tracks']} "
            f"| {row['mean_auc']:.3f} "
            f"| {row['mean_ks']:.3f} |"
        )
    lines.append("")
    lines.append(
        "Cohen's d interpretation (Cohen 1988): 0.2 weak, 0.5 moderate, 0.8 strong."
    )
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--tracks",
        type=str,
        default=None,
        help="Comma-separated track stem list (defaults to all)",
    )
    parser.add_argument("--window-sec", type=float, default=ONSET_WINDOW_SEC)
    parser.add_argument(
        "--target-rms-db",
        type=float,
        default=-35.0,
        help="Normalize each clip to this RMS level before feature extraction.",
    )
    parser.add_argument("--log-level", type=str, default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    tracks = discover_tracks(args.corpus)
    if args.tracks:
        wanted = set(args.tracks.split(","))
        tracks = [t for t in tracks if t.stem in wanted]
    if not tracks:
        log.error("No tracks found in %s", args.corpus)
        return 2
    log.info("Analysing %d tracks (GT-only labels, no NN)", len(tracks))

    args.out.mkdir(parents=True, exist_ok=True)
    per_track_dir = args.out / "per_track"
    per_track_dir.mkdir(exist_ok=True)

    reports: list[dict[str, Any]] = []
    total_frames = 0
    for path in tracks:
        log.info("  %s", path.stem)
        try:
            report = score_track(
                path,
                window_sec=args.window_sec,
                target_rms_db=args.target_rms_db,
            )
        except Exception:
            log.exception("  failed: %s", path.stem)
            continue
        (per_track_dir / f"{path.stem}.json").write_text(json.dumps(report, indent=2))
        reports.append(report)
        total_frames += report["n_frames"]

    if not reports:
        log.error("No tracks scored successfully")
        return 3

    agg = aggregate(reports)
    (args.out / "ranking.json").write_text(json.dumps(agg, indent=2))
    (args.out / "summary.md").write_text(
        format_summary_md(agg, total_frames, len(reports))
    )
    log.info("Wrote %s", args.out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
