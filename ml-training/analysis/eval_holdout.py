"""Evaluate a trained onset model on the edm_holdout corpus (.beats.json only).

evaluate.py's sweep_thresholds needs .onsets.json / .kick_weighted sidecars
that edm_holdout doesn't carry. This helper treats .beats.json hits with
expectTrigger=true as the onset reference — same convention the on-device
validation uses (see blinky-server/blinky_server/testing/scoring.py
`estimate_audio_latency` and `ref_onsets = [h.time for h in ...]`).

Usage:
    ./venv/bin/python -m analysis.eval_holdout \
        --config configs/conv1d_w16_onset_v28_mel_only.yaml \
        --model outputs/v28_mel_only/best_model.pt \
        --audio-dir ../blinky-test-player/music/edm_holdout \
        --out outputs/v28_mel_only/eval_holdout_beats
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import librosa
import mir_eval
import numpy as np
import torch

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from evaluate import (  # noqa: E402
    _build_mel_filterbank,
    _peak_pick,
    firmware_mel_spectrogram,
    load_model,
)
from scripts.audio import (  # noqa: E402
    append_hybrid_features,
    load_config,
    resolve_hybrid_features,
)


def load_ref_onsets_from_beats(beats_path: Path) -> np.ndarray:
    data = json.loads(beats_path.read_text())
    return np.array(
        sorted(float(h["time"]) for h in data.get("hits", []) if h.get("expectTrigger", True))
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--audio-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument(
        "--thresholds",
        type=str,
        default="0.2,0.3,0.4,0.5,0.6,0.7",
        help="Comma-separated threshold sweep",
    )
    parser.add_argument(
        "--window-sec",
        type=float,
        default=0.05,
        help="mir_eval onset F1 tolerance (default 50 ms)",
    )
    args = parser.parse_args(argv)

    cfg = load_config(args.config)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"device={device}")

    model, _pool = load_model(args.model, cfg, device)
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]
    chunk_frames = cfg["training"]["chunk_frames"]
    target_rms_db = cfg["audio"].get("target_rms_db", -35)

    hybrid_names = resolve_hybrid_features(cfg)
    print(f"Hybrid features used: {hybrid_names or '(none)'}")

    audio_paths = sorted(
        f for f in args.audio_dir.rglob("*") if f.suffix.lower() in {".mp3", ".wav", ".flac"}
    )
    tracks: list[tuple[str, np.ndarray, np.ndarray]] = []
    for ap in audio_paths:
        beats = ap.parent / f"{ap.stem}.beats.json"
        if not beats.exists():
            print(f"skip {ap.stem}: no .beats.json")
            continue
        ref_onsets = load_ref_onsets_from_beats(beats)
        if ref_onsets.size == 0:
            print(f"skip {ap.stem}: beats.json has no onset-trigger hits")
            continue

        audio_np, _ = librosa.load(str(ap), sr=sr, mono=True)
        rms = float(np.sqrt(np.mean(audio_np**2) + 1e-10))
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)
        if hybrid_names:
            mel = append_hybrid_features(
                mel,
                audio=audio_np,
                mel_db_range=cfg["audio"].get("mel_db_range", 60.0),
                features=hybrid_names,
            )

        n_frames = mel.shape[0]
        n_feat = mel.shape[1]
        mel_tensor = torch.from_numpy(mel).float().to(device) if isinstance(mel, np.ndarray) else mel.float().to(device)

        activations = np.zeros(n_frames, dtype=np.float32)
        counts = np.zeros(n_frames, dtype=np.float32)
        stride = chunk_frames // 2
        with torch.no_grad():
            for start in range(0, max(1, n_frames - chunk_frames + 1), stride):
                end = start + chunk_frames
                if end > n_frames:
                    chunk = torch.zeros(
                        chunk_frames, n_feat, device=device, dtype=torch.float32
                    )
                    chunk[: n_frames - start] = mel_tensor[start:n_frames]
                    actual = n_frames - start
                else:
                    chunk = mel_tensor[start:end]
                    actual = chunk_frames
                pred = model(chunk.unsqueeze(0))[0]
                activations[start : start + actual] += pred[:actual, 0].cpu().numpy()
                counts[start : start + actual] += 1
        activations /= np.maximum(counts, 1)

        tracks.append((ap.stem, activations, ref_onsets))
        print(f"  {ap.stem}: {len(ref_onsets)} GT onsets, {n_frames} frames")

    if not tracks:
        print("No tracks evaluated — aborting")
        return 1

    thresholds = [float(t) for t in args.thresholds.split(",")]
    args.out.mkdir(parents=True, exist_ok=True)
    rows: list[dict] = []
    print(
        f"\n{'thresh':>6} {'mean F1':>8} {'median':>8} {'min':>6} {'max':>6}"
        f" {'est/ref':>8}"
    )
    best_t, best_f1 = thresholds[0], -1.0
    for thresh in thresholds:
        f1s: list[float] = []
        ratios: list[float] = []
        for _name, act, ref in tracks:
            est = _peak_pick(act, thresh, frame_rate)
            f1 = (
                mir_eval.onset.f_measure(ref, est, window=args.window_sec)[0]
                if len(est) > 0 and len(ref) > 0
                else 0.0
            )
            f1s.append(f1)
            ratios.append(len(est) / max(len(ref), 1))
        mean_f1 = float(np.mean(f1s))
        rows.append(
            {
                "threshold": thresh,
                "mean_f1": mean_f1,
                "median_f1": float(np.median(f1s)),
                "min_f1": float(np.min(f1s)),
                "max_f1": float(np.max(f1s)),
                "est_per_ref": float(np.mean(ratios)),
                "n_tracks": len(tracks),
            }
        )
        print(
            f"{thresh:>6.2f} {mean_f1:>8.3f} {np.median(f1s):>8.3f} {np.min(f1s):>6.3f}"
            f" {np.max(f1s):>6.3f} {np.mean(ratios):>7.2f}x"
        )
        if mean_f1 > best_f1:
            best_f1, best_t = mean_f1, thresh

    print(f"\nBest threshold: {best_t:.2f}  mean F1: {best_f1:.3f}")
    (args.out / "sweep.json").write_text(
        json.dumps({"best_threshold": best_t, "best_f1": best_f1, "sweep": rows}, indent=2)
    )
    print(f"Saved {args.out / 'sweep.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
