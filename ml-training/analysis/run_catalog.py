"""Phase 1 feature-catalog runner.

Evaluates each candidate feature in `features.py` against two questions,
both GT-only (no NN inference):

  1. **Within-corpus discrimination.** For each track: extract the peak
     feature value in a ±window around every GT onset ("onset peak"),
     and the feature values at every frame far from any GT onset
     ("non-onset frame"). Cohen's d between the two is the standard
     onset-vs-non-onset score — a sanity check that the feature fires
     at onsets at all.

  2. **Cross-corpus discrimination — percussion vs tonal impulse.**
     Aggregate onset peaks across a percussion corpus (e.g. EDM GT)
     and across a tonal-impulse corpus (see generate_tonal_corpus.py).
     Cohen's d and ROC-AUC between those two distributions measures
     the feature's ability to distinguish a real drum hit from a
     tonal impulse — the false-positive case the NN actually struggles
     with on-device.

The cross-corpus metric is the primary Phase 1 deliverable; the
within-corpus metric is a cross-check that catches features that
don't fire at onsets at all.

Usage:
    cd ml-training && ./venv/bin/python -m analysis.run_catalog \
        --perc-corpus ../blinky-test-player/music/edm \
        --tonal-corpus ../blinky-test-player/music/synthetic_tonals \
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

# Half-width used to label a frame as "onset" and to collect the per-onset peak.
ONSET_WINDOW_SEC = 0.050

# Half-width used to mark a frame as "non-onset": the frame must be at least
# this far from ANY GT onset. Larger than ONSET_WINDOW_SEC to avoid mixing
# the tails of onsets into the non-onset pool.
NON_ONSET_GUARD_SEC = 0.100


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


def min_distance_to_onset(frame_secs: np.ndarray, gt_onsets: list[float]) -> np.ndarray:
    """For each frame, minimum absolute distance (seconds) to any GT onset.

    Uses bisect for O(log m) per frame. Returns +inf-filled array if the
    onset list is empty.
    """
    out = np.full(len(frame_secs), np.inf, dtype=np.float64)
    if not gt_onsets:
        return out
    for i, t in enumerate(frame_secs):
        idx = bisect.bisect_left(gt_onsets, t)
        best = np.inf
        for j in (idx - 1, idx):
            if 0 <= j < len(gt_onsets):
                d = abs(t - gt_onsets[j])
                if d < best:
                    best = d
        out[i] = best
    return out


def per_onset_peaks(
    feature_values: np.ndarray,
    frame_secs: np.ndarray,
    gt_onsets: list[float],
    window_sec: float,
) -> np.ndarray:
    """For each GT onset, extract the max feature value within ±window."""
    peaks = np.empty(len(gt_onsets), dtype=np.float32)
    if not len(gt_onsets):
        return np.zeros(0, dtype=np.float32)
    starts = np.searchsorted(frame_secs, np.array(gt_onsets) - window_sec, side="left")
    ends = np.searchsorted(frame_secs, np.array(gt_onsets) + window_sec, side="right")
    for i, (s, e) in enumerate(zip(starts, ends)):
        if s >= e:
            peaks[i] = 0.0
        else:
            peaks[i] = float(feature_values[s:e].max())
    return peaks


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
    """ROC-AUC. 0.5 = no separation, 1.0 = perfect pos-above-neg."""
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
    guard_sec: float,
    target_rms_db: float,
) -> dict[str, Any]:
    """Compute features and per-onset peaks + non-onset frames for one track."""
    audio, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    rms = np.sqrt(np.mean(audio**2) + 1e-10)
    audio = audio * (10 ** (target_rms_db / 20) / rms)

    features = compute_all_features(audio)
    n = min(len(v) for v in features.values())
    features = {k: v[:n] for k, v in features.items()}
    ts = frame_times(n)
    gt = load_consensus_onsets(audio_path)

    # Per-onset peak: max feature value in ±window_sec around each GT onset
    onset_peaks: dict[str, np.ndarray] = {
        name: per_onset_peaks(v, ts, gt, window_sec) for name, v in features.items()
    }
    # Non-onset frames: at least guard_sec from every GT onset
    dists = min_distance_to_onset(ts, gt)
    non_mask = dists >= guard_sec

    per_feature: dict[str, dict[str, Any]] = {}
    for name, values in features.items():
        peaks = onset_peaks[name]
        non = values[non_mask]
        per_feature[name] = {
            "onset_peaks": peaks.tolist(),
            "non_onset_values": non.tolist() if non.size < 8000 else non[::4].tolist(),
            "onset_peak_mean": float(peaks.mean()) if peaks.size else 0.0,
            "onset_peak_std": float(peaks.std(ddof=1)) if peaks.size > 1 else 0.0,
            "non_mean": float(non.mean()) if non.size else 0.0,
            "non_std": float(non.std(ddof=1)) if non.size > 1 else 0.0,
            "cohens_d_peak_vs_non": cohens_d(peaks, non),
            "auc_peak_vs_non": auc_binary(peaks, non),
        }

    return {
        "track": audio_path.stem,
        "n_frames": int(n),
        "gt_onsets": len(gt),
        "non_onset_frames": int(non_mask.sum()),
        "features": per_feature,
    }


# --------------------------------------------------------------------- aggregation


def per_corpus_ranking(reports: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Per-feature mean onset-peak-vs-non-onset d/AUC across tracks.

    Weighted by per-track (gt_onsets + non_onset_frames).
    """
    pooled: dict[str, dict[str, list[float]]] = {}
    for r in reports:
        weight = r["gt_onsets"] + r["non_onset_frames"]
        for name, s in r["features"].items():
            bucket = pooled.setdefault(name, {"d": [], "auc": [], "w": []})
            bucket["d"].append(s["cohens_d_peak_vs_non"])
            bucket["auc"].append(s["auc_peak_vs_non"])
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
                "tracks": len(b["d"]),
                "mean_auc": float(np.average(b["auc"], weights=w)),
            }
        )
    ranked.sort(key=lambda r: r["mean_abs_cohens_d"], reverse=True)
    return ranked


def collect_peaks(reports: list[dict[str, Any]]) -> dict[str, np.ndarray]:
    """Pool onset peaks from every track into one array per feature."""
    buckets: dict[str, list[float]] = {}
    for r in reports:
        for name, s in r["features"].items():
            buckets.setdefault(name, []).extend(s["onset_peaks"])
    return {k: np.asarray(v, dtype=np.float32) for k, v in buckets.items()}


def cross_corpus_ranking(
    perc_peaks: dict[str, np.ndarray],
    tonal_peaks: dict[str, np.ndarray],
) -> list[dict[str, Any]]:
    """Per-feature d / AUC / KS between percussion and tonal onset-peak pools."""
    ranked: list[dict[str, Any]] = []
    for name in sorted(perc_peaks.keys() & tonal_peaks.keys()):
        perc = perc_peaks[name]
        tonal = tonal_peaks[name]
        ranked.append(
            {
                "feature": name,
                "cohens_d": cohens_d(perc, tonal),
                "auc": auc_binary(perc, tonal),
                "ks": ks_stat(perc, tonal),
                "perc_mean": float(perc.mean()) if perc.size else 0.0,
                "perc_std": float(perc.std(ddof=1)) if perc.size > 1 else 0.0,
                "tonal_mean": float(tonal.mean()) if tonal.size else 0.0,
                "tonal_std": float(tonal.std(ddof=1)) if tonal.size > 1 else 0.0,
                "perc_n": int(perc.size),
                "tonal_n": int(tonal.size),
            }
        )
    ranked.sort(key=lambda r: abs(r["cohens_d"]), reverse=True)
    return ranked


def format_summary_md(
    perc_rank: list[dict[str, Any]],
    tonal_rank: list[dict[str, Any]] | None,
    cross_rank: list[dict[str, Any]] | None,
    perc_tracks: int,
    tonal_tracks: int,
) -> str:
    out = [
        "# Phase 1 Feature Catalog — Ranked Summary",
        "",
    ]
    if cross_rank is not None:
        out += [
            "## Cross-corpus: percussion onset peak vs tonal impulse peak",
            "",
            f"Percussion tracks: {perc_tracks} · tonal tracks: {tonal_tracks}.",
            "Each GT onset contributes one sample: the max feature value within",
            f"±{int(ONSET_WINDOW_SEC * 1000)} ms. Positive signed d ⇒ feature is larger",
            "on percussion onsets than on tonal impulses (the direction we want).",
            "",
            "| rank | feature | d (perc−tonal) | AUC | KS | perc_mean | tonal_mean | perc_n | tonal_n |",
            "|-----:|---------|---------------:|----:|---:|---------:|---------:|------:|------:|",
        ]
        for i, r in enumerate(cross_rank, start=1):
            out.append(
                f"| {i} | {r['feature']} | {r['cohens_d']:+.3f} "
                f"| {r['auc']:.3f} | {r['ks']:.3f} "
                f"| {r['perc_mean']:.4g} | {r['tonal_mean']:.4g} "
                f"| {r['perc_n']} | {r['tonal_n']} |"
            )
        out += [
            "",
            "Interpretation: |d| ≥ 0.5 is a moderate discriminator; AUC ≥ 0.7",
            "means the feature alone classifies percussion-vs-tonal with useful",
            "skill. AUC < 0.55 is basically coin-flip.",
            "",
        ]
    out += [
        "## Percussion corpus: per-onset peak vs non-onset frames",
        "",
        "Sanity check that features fire at drum onsets at all.",
        "",
        "| rank | feature | \\|d\\| | d (signed) | pos tracks | AUC |",
        "|-----:|---------|-----:|-----------:|-----------:|----:|",
    ]
    for i, r in enumerate(perc_rank, start=1):
        out.append(
            f"| {i} | {r['feature']} | {r['mean_abs_cohens_d']:.3f} "
            f"| {r['mean_cohens_d_signed']:+.3f} "
            f"| {r['positive_tracks']}/{r['tracks']} | {r['mean_auc']:.3f} |"
        )
    if tonal_rank is not None:
        out += [
            "",
            "## Tonal corpus: per-impulse peak vs non-impulse frames",
            "",
            "Useful mainly as a cross-check — the feature should still fire on",
            "tonal impulses (d > 0), otherwise it's simply not responding to any",
            "attack in that corpus.",
            "",
            "| rank | feature | \\|d\\| | d (signed) | pos tracks | AUC |",
            "|-----:|---------|-----:|-----------:|-----------:|----:|",
        ]
        for i, r in enumerate(tonal_rank, start=1):
            out.append(
                f"| {i} | {r['feature']} | {r['mean_abs_cohens_d']:.3f} "
                f"| {r['mean_cohens_d_signed']:+.3f} "
                f"| {r['positive_tracks']}/{r['tracks']} | {r['mean_auc']:.3f} |"
            )
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------- main


def run_corpus(
    corpus_dir: Path,
    *,
    window_sec: float,
    guard_sec: float,
    target_rms_db: float,
    track_filter: set[str] | None,
    out_per_track: Path,
    label: str,
) -> list[dict[str, Any]]:
    """Process every track in one corpus, write per-track JSONs, return reports."""
    tracks = discover_tracks(corpus_dir)
    if track_filter:
        tracks = [t for t in tracks if t.stem in track_filter]
    if not tracks:
        log.warning("No %s tracks found in %s", label, corpus_dir)
        return []
    log.info("Analysing %d %s tracks", len(tracks), label)
    out_per_track.mkdir(parents=True, exist_ok=True)
    reports: list[dict[str, Any]] = []
    for path in tracks:
        log.info("  [%s] %s", label, path.stem)
        try:
            report = score_track(
                path,
                window_sec=window_sec,
                guard_sec=guard_sec,
                target_rms_db=target_rms_db,
            )
        except Exception:
            log.exception("  failed: %s", path.stem)
            continue
        (out_per_track / f"{path.stem}.json").write_text(json.dumps(report))
        reports.append(report)
    return reports


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perc-corpus", required=True, type=Path,
                        help="Directory with percussion/EDM GT tracks")
    parser.add_argument("--tonal-corpus", type=Path, default=None,
                        help="Optional tonal-impulse corpus directory")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--tracks", type=str, default=None,
                        help="Comma-separated stem filter applied to BOTH corpora")
    parser.add_argument("--perc-tracks", type=str, default=None,
                        help="Override --tracks for the percussion corpus only")
    parser.add_argument("--tonal-tracks", type=str, default=None,
                        help="Override --tracks for the tonal corpus only")
    parser.add_argument("--window-sec", type=float, default=ONSET_WINDOW_SEC)
    parser.add_argument("--guard-sec", type=float, default=NON_ONSET_GUARD_SEC)
    parser.add_argument("--target-rms-db", type=float, default=-35.0)
    parser.add_argument("--log-level", type=str, default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    args.out.mkdir(parents=True, exist_ok=True)
    default_filter = set(args.tracks.split(",")) if args.tracks else None
    perc_filter = set(args.perc_tracks.split(",")) if args.perc_tracks else default_filter
    tonal_filter = set(args.tonal_tracks.split(",")) if args.tonal_tracks else default_filter

    perc_reports = run_corpus(
        args.perc_corpus,
        window_sec=args.window_sec,
        guard_sec=args.guard_sec,
        target_rms_db=args.target_rms_db,
        track_filter=perc_filter,
        out_per_track=args.out / "per_track_perc",
        label="perc",
    )
    tonal_reports: list[dict[str, Any]] = []
    if args.tonal_corpus is not None:
        tonal_reports = run_corpus(
            args.tonal_corpus,
            window_sec=args.window_sec,
            guard_sec=args.guard_sec,
            target_rms_db=args.target_rms_db,
            track_filter=tonal_filter,
            out_per_track=args.out / "per_track_tonal",
            label="tonal",
        )

    if not perc_reports:
        log.error("No percussion tracks scored — aborting")
        return 3

    perc_rank = per_corpus_ranking(perc_reports)
    tonal_rank = per_corpus_ranking(tonal_reports) if tonal_reports else None
    cross_rank = None
    if tonal_reports:
        perc_peaks = collect_peaks(perc_reports)
        tonal_peaks = collect_peaks(tonal_reports)
        cross_rank = cross_corpus_ranking(perc_peaks, tonal_peaks)

    (args.out / "ranking.json").write_text(
        json.dumps(
            {
                "cross_corpus": cross_rank,
                "percussion": perc_rank,
                "tonal": tonal_rank,
            },
            indent=2,
        )
    )
    (args.out / "summary.md").write_text(
        format_summary_md(
            perc_rank,
            tonal_rank,
            cross_rank,
            perc_tracks=len(perc_reports),
            tonal_tracks=len(tonal_reports),
        )
    )
    log.info("Wrote %s", args.out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
