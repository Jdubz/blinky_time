"""Phase 1 feature-catalog runner.

Loads an audio corpus with consensus-onset ground truth, computes the 10
candidate frame features from `analysis/features.py`, runs the deployed
v27-hybrid NN to get per-frame activations, and labels every frame as:

  TP  — NN activation >= threshold AND a GT onset within ±50 ms
  FP  — NN activation >= threshold AND no GT onset within ±50 ms
  TN  — NN activation <  threshold AND no GT onset within ±50 ms
  FN  — NN activation <  threshold AND a GT onset within ±50 ms

For each feature it reports per-class means / stds, Cohen's d between TP
and FP (the primary discrimination metric), and ROC-AUC of the feature
alone as a binary TP-vs-FP classifier. Per-track JSONs plus a corpus-wide
ranked summary land under `outputs/feature_catalog/`.

Usage:
    cd ml-training && ./venv/bin/python -m analysis.run_catalog \
        --config configs/conv1d_w16_onset_v27.yaml \
        --model outputs/v27-hybrid/best_model.pt \
        --corpus ../blinky-test-player/music/edm \
        --out outputs/feature_catalog
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import torch
import yaml
from scipy import stats
from sklearn.metrics import roc_auc_score

# Make sibling `scripts/` importable when running as a module.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from analysis.features import HOP, SR, compute_all_features, frame_times  # noqa: E402
from scripts.audio import (  # noqa: E402
    append_hybrid_features,
    build_mel_filterbank_torch,
    firmware_mel_spectrogram_torch,
    load_config,
)

log = logging.getLogger("feature_catalog")

# Matches the on-device pulseOnsetFloor (see CLAUDE.md / AUDIO_ARCHITECTURE.md).
NN_FIRING_THRESHOLD = 0.30
# Window for "near a GT onset"; matches HYBRID_ONSET_WINDOW_SEC on the server.
ONSET_WINDOW_SEC = 0.050


# --------------------------------------------------------------------- data loading


def load_consensus_onsets(path: Path) -> list[float]:
    """Load GT onset times (seconds) from *.onsets_consensus.json.

    Falls back to *.beats.json if consensus is absent. Returns a sorted list.
    """
    consensus = path.with_suffix("").with_suffix(".onsets_consensus.json")
    beats = path.with_suffix("").with_suffix(".beats.json")
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
    """Return audio file paths for tracks with at least one GT source."""
    tracks: list[Path] = []
    for audio in sorted(corpus_dir.glob("*.mp3")):
        if (
            audio.with_suffix(".onsets_consensus.json").exists()
            or audio.with_suffix(".beats.json").exists()
        ):
            tracks.append(audio)
    for audio in sorted(corpus_dir.glob("*.wav")):
        if (
            audio.with_suffix(".onsets_consensus.json").exists()
            or audio.with_suffix(".beats.json").exists()
        ):
            tracks.append(audio)
    return tracks


# --------------------------------------------------------------------- NN inference


def load_v27_model(model_path: Path, cfg: dict, device: torch.device) -> torch.nn.Module:
    """Instantiate the frame_conv1d architecture from cfg and load weights."""
    from models.onset_conv1d import build_onset_conv1d  # lazy: only in PyTorch envs

    n_mels = int(cfg["audio"]["n_mels"])
    use_hybrid = bool(cfg.get("features", {}).get("use_hybrid", False))
    input_features = n_mels + (2 if use_hybrid else 0)
    model = build_onset_conv1d(
        n_mels=input_features,
        channels=cfg["model"]["channels"],
        kernel_sizes=cfg["model"]["kernel_sizes"],
        dropout=cfg["model"].get("dropout", 0.1),
        num_tempo_bins=cfg["model"].get("num_tempo_bins", 0),
        freq_pos_encoding=cfg["model"].get("freq_pos_encoding", False),
        num_output_channels=cfg["model"].get("num_output_channels", 0),
    ).to(device)
    state = torch.load(model_path, map_location=device, weights_only=True)
    if isinstance(state, dict) and "state_dict" in state:
        state = state["state_dict"]
    model.load_state_dict(state)
    model.eval()
    return model


def nn_activations(
    audio: np.ndarray,
    model: torch.nn.Module,
    cfg: dict,
    mel_fb: torch.Tensor,
    window: torch.Tensor,
    device: torch.device,
) -> np.ndarray:
    """Run the model over `audio` and return (n_frames,) activations in [0, 1]."""
    audio_gpu = torch.from_numpy(audio).to(device)
    mel = firmware_mel_spectrogram_torch(audio_gpu, cfg, mel_fb, window)
    mel = append_hybrid_features(
        mel, audio=audio, mel_db_range=cfg["audio"].get("mel_db_range", 60.0)
    )
    chunk_frames = int(cfg["model"]["window_frames"])
    n_frames = mel.shape[0]
    activations = np.zeros(n_frames, dtype=np.float32)
    counts = np.zeros(n_frames, dtype=np.float32)
    stride = max(1, chunk_frames // 2)
    mel_tensor = torch.from_numpy(mel).float().to(device)
    with torch.no_grad():
        for start in range(0, max(1, n_frames - chunk_frames + 1), stride):
            end = start + chunk_frames
            if end > n_frames:
                chunk = torch.zeros(
                    chunk_frames, mel.shape[1], device=device, dtype=torch.float32
                )
                chunk[: n_frames - start] = mel_tensor[start:n_frames]
                actual = n_frames - start
            else:
                chunk = mel_tensor[start:end]
                actual = chunk_frames
            pred = model(chunk.unsqueeze(0))[0]
            activations[start : start + actual] += pred[:actual, 0].cpu().numpy()
            counts[start : start + actual] += 1
    return activations / np.maximum(counts, 1.0)


# --------------------------------------------------------------------- labeling


def classify_frames(
    frame_secs: np.ndarray,
    activations: np.ndarray,
    gt_onsets: list[float],
    *,
    threshold: float,
    window_sec: float,
) -> np.ndarray:
    """Return an int8 array: 1=TP, 2=FP, 3=TN, 4=FN per frame.

    GT onsets are time-sorted; classification uses bisect for O(log m) lookup.
    """
    import bisect

    out = np.zeros(len(frame_secs), dtype=np.int8)
    if not gt_onsets:
        # All frames are TN/FP depending on threshold
        return np.where(activations >= threshold, 2, 3).astype(np.int8)
    for i, t in enumerate(frame_secs):
        idx = bisect.bisect_left(gt_onsets, t)
        near = False
        for j in (idx - 1, idx):
            if 0 <= j < len(gt_onsets) and abs(t - gt_onsets[j]) < window_sec:
                near = True
                break
        fires = activations[i] >= threshold
        if near and fires:
            out[i] = 1  # TP
        elif fires:
            out[i] = 2  # FP
        elif near:
            out[i] = 4  # FN
        else:
            out[i] = 3  # TN
    return out


# --------------------------------------------------------------------- statistics


def cohens_d(a: np.ndarray, b: np.ndarray) -> float:
    """Cohen's d with pooled variance. NaN-safe; returns 0.0 on empty input."""
    if len(a) < 2 or len(b) < 2:
        return 0.0
    a = a[np.isfinite(a)]
    b = b[np.isfinite(b)]
    if len(a) < 2 or len(b) < 2:
        return 0.0
    va = a.var(ddof=1)
    vb = b.var(ddof=1)
    pooled = np.sqrt((va + vb) / 2.0)
    if pooled < 1e-12:
        return 0.0
    return float((a.mean() - b.mean()) / pooled)


def auc_binary(tp_values: np.ndarray, fp_values: np.ndarray) -> float:
    """ROC-AUC of a single feature as a TP-vs-FP binary classifier.

    Returns 0.5 if one class is empty or all values are identical.
    """
    if len(tp_values) < 2 or len(fp_values) < 2:
        return 0.5
    x = np.concatenate([tp_values, fp_values])
    y = np.concatenate([np.ones(len(tp_values)), np.zeros(len(fp_values))])
    if np.std(x) < 1e-12:
        return 0.5
    try:
        return float(roc_auc_score(y, x))
    except ValueError:
        return 0.5


def ks_stat(a: np.ndarray, b: np.ndarray) -> float:
    """KS statistic (distribution-level separation). 0 = identical, 1 = disjoint."""
    if len(a) < 5 or len(b) < 5:
        return 0.0
    return float(stats.ks_2samp(a, b).statistic)


# --------------------------------------------------------------------- per-track run


def score_track(
    audio_path: Path,
    model: torch.nn.Module,
    cfg: dict,
    mel_fb: torch.Tensor,
    window: torch.Tensor,
    device: torch.device,
    *,
    threshold: float,
    window_sec: float,
) -> dict[str, Any]:
    """Compute features, NN activations, labels, and per-feature stats for one track."""
    audio, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    target_rms_db = cfg["audio"].get("target_rms_db", -35)
    rms = np.sqrt(np.mean(audio**2) + 1e-10)
    audio = audio * (10 ** (target_rms_db / 20) / rms)

    features = compute_all_features(audio)
    # Align features and activations to the same frame count. Hybrid mel may
    # drop the trailing partial frame; truncate every array to the shortest.
    activations = nn_activations(audio, model, cfg, mel_fb, window, device)
    n = min(len(activations), *(len(v) for v in features.values()))
    activations = activations[:n]
    features = {k: v[:n] for k, v in features.items()}
    ts = frame_times(n)

    gt = load_consensus_onsets(audio_path)
    labels = classify_frames(
        ts, activations, gt, threshold=threshold, window_sec=window_sec
    )
    tp_mask = labels == 1
    fp_mask = labels == 2
    tn_mask = labels == 3
    fn_mask = labels == 4

    per_feature: dict[str, dict[str, float]] = {}
    for name, values in features.items():
        tp_v = values[tp_mask]
        fp_v = values[fp_mask]
        tn_v = values[tn_mask]
        per_feature[name] = {
            "tp_mean": float(tp_v.mean()) if tp_v.size else 0.0,
            "tp_std": float(tp_v.std(ddof=1)) if tp_v.size > 1 else 0.0,
            "fp_mean": float(fp_v.mean()) if fp_v.size else 0.0,
            "fp_std": float(fp_v.std(ddof=1)) if fp_v.size > 1 else 0.0,
            "tn_mean": float(tn_v.mean()) if tn_v.size else 0.0,
            "cohens_d_tp_fp": cohens_d(tp_v, fp_v),
            "auc_tp_fp": auc_binary(tp_v, fp_v),
            "ks_tp_fp": ks_stat(tp_v, fp_v),
        }

    return {
        "track": audio_path.stem,
        "n_frames": int(n),
        "gt_onsets": len(gt),
        "counts": {
            "tp": int(tp_mask.sum()),
            "fp": int(fp_mask.sum()),
            "tn": int(tn_mask.sum()),
            "fn": int(fn_mask.sum()),
        },
        "nn": {
            "mean": float(activations.mean()),
            "std": float(activations.std(ddof=1)) if n > 1 else 0.0,
            "firing_rate": float((activations >= threshold).mean()),
        },
        "features": per_feature,
    }


# --------------------------------------------------------------------- aggregation


def aggregate(reports: list[dict[str, Any]]) -> dict[str, Any]:
    """Pool across tracks — weighted by TP/FP counts — to rank features."""
    pooled: dict[str, dict[str, list[float]]] = {}
    for r in reports:
        for name, stats_d in r["features"].items():
            bucket = pooled.setdefault(
                name,
                {"d": [], "auc": [], "ks": [], "tp_n": [], "fp_n": []},
            )
            bucket["d"].append(stats_d["cohens_d_tp_fp"])
            bucket["auc"].append(stats_d["auc_tp_fp"])
            bucket["ks"].append(stats_d["ks_tp_fp"])
            bucket["tp_n"].append(r["counts"]["tp"])
            bucket["fp_n"].append(r["counts"]["fp"])

    ranked: list[dict[str, Any]] = []
    for name, b in pooled.items():
        weights = np.array(b["tp_n"], dtype=np.float64) + np.array(b["fp_n"], dtype=np.float64)
        total = weights.sum()
        if total < 1:
            continue
        ranked.append(
            {
                "feature": name,
                "mean_abs_cohens_d": float(np.average(np.abs(b["d"]), weights=weights)),
                "mean_cohens_d_signed": float(np.average(b["d"], weights=weights)),
                "mean_auc": float(np.average(b["auc"], weights=weights)),
                "mean_ks": float(np.average(b["ks"], weights=weights)),
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
        f"NN firing threshold: {NN_FIRING_THRESHOLD} · onset window: ±{int(ONSET_WINDOW_SEC * 1000)} ms",
        "",
        "Rank by |Cohen's d| weighted by per-track TP+FP counts. AUC and KS are",
        "included as cross-checks; a feature that disagrees across metrics is suspect.",
        "",
        "| rank | feature | |d| | d (signed) | AUC | KS | tracks |",
        "|-----:|---------|----:|-----------:|----:|----:|-------:|",
    ]
    for i, row in enumerate(agg["ranking"], start=1):
        lines.append(
            f"| {i} | {row['feature']} | {row['mean_abs_cohens_d']:.3f} "
            f"| {row['mean_cohens_d_signed']:+.3f} "
            f"| {row['mean_auc']:.3f} "
            f"| {row['mean_ks']:.3f} "
            f"| {row['tracks']} |"
        )
    lines.append("")
    lines.append(
        "d sign convention: positive = feature value is higher on TP than on FP. "
        "|d| > 0.5 is a moderate discriminator; |d| > 0.8 is strong (Cohen 1988)."
    )
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--tracks", type=str, default=None,
                        help="Comma-separated track stem list (defaults to all)")
    parser.add_argument("--threshold", type=float, default=NN_FIRING_THRESHOLD)
    parser.add_argument("--window-sec", type=float, default=ONSET_WINDOW_SEC)
    parser.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--log-level", type=str, default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    cfg = load_config(str(args.config))
    device = torch.device(args.device)

    log.info("Loading model %s", args.model)
    model = load_v27_model(args.model, cfg, device)
    mel_fb = build_mel_filterbank_torch(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    tracks = discover_tracks(args.corpus)
    if args.tracks:
        wanted = set(args.tracks.split(","))
        tracks = [t for t in tracks if t.stem in wanted]
    if not tracks:
        log.error("No tracks found in %s", args.corpus)
        return 2
    log.info("Analysing %d tracks", len(tracks))

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
                model,
                cfg,
                mel_fb,
                window,
                device,
                threshold=args.threshold,
                window_sec=args.window_sec,
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
    log.info("Wrote %s and per-track JSONs", args.out / "summary.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
