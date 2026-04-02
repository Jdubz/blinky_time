#!/usr/bin/env python3
"""Generate deduplicated onset teacher labels from madmom CNN onset detector.

Two output modes:
1. Soft labels (for knowledge distillation): continuous onset probability at each frame
2. Hard labels (for standard training): binary events, strictly deduplicated

Dedup strategy:
- Within a configurable window (default 70ms), only the STRONGEST activation survives
- Surviving onset is snapped to the nearest beat subdivision (if beat grid available)
- Per-instrument dedup is handled by the kick-weighted label pipeline (separate)
- Final validation: onset density must be 2-6 events/second for typical EDM

Usage:
    # Soft teacher labels (for distillation — no dedup needed, raw activations)
    python generate_onset_teacher_labels.py --mode soft --audio-dir audio/combined/ --output-dir labels/onset_teacher_soft/

    # Hard deduplicated labels (for binary training)
    python generate_onset_teacher_labels.py --mode hard --audio-dir audio/combined/ --output-dir labels/onset_teacher_hard/

    # Hard labels with beat grid snapping
    python generate_onset_teacher_labels.py --mode hard --snap-to-grid --beats-dir labels/consensus_v5/ --audio-dir audio/combined/ --output-dir labels/onset_teacher_hard_snapped/
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np


def generate_soft_labels(audio_path: str, fps: int = 100) -> dict:
    """Generate soft onset probability labels using madmom CNN onset detector.

    Returns continuous activation at each frame — no peak-picking or dedup.
    The model learns to reproduce the activation shape (rise/attack/decay).
    """
    try:
        import madmom
    except ImportError:
        raise ImportError(
            "madmom is required for onset teacher labels. "
            "Install via: pip install madmom (requires Python 3.11, use venv311/)"
        ) from None

    proc = madmom.features.onsets.CNNOnsetProcessor()
    activations = proc(audio_path)

    return {
        "type": "onset_teacher_soft",
        "fps": fps,
        "n_frames": len(activations),
        "duration_sec": len(activations) / fps,
        "activations": activations.tolist(),
        "stats": {
            "mean": float(np.mean(activations)),
            "max": float(np.max(activations)),
            "std": float(np.std(activations)),
        },
    }


def generate_hard_labels(
    audio_path: str,
    dedup_window_ms: float = 70,
    threshold: float = 0.3,
    fps: int = 100,
    beats: list[float] | None = None,
    snap_window_ms: float = 30,
) -> dict:
    """Generate strictly deduplicated binary onset labels.

    Process:
    1. Run madmom CNN onset detector → continuous activations
    2. Peak-pick above threshold
    3. Dedup: within each window, keep only the strongest peak
    4. Optionally snap to nearest beat subdivision
    5. Validate onset density (warn if outside 1-8 events/sec)

    Args:
        audio_path: Path to audio file
        dedup_window_ms: Minimum inter-onset interval. Within this window,
            only the strongest activation survives. 70ms matches firmware
            ONSET_DEDUP_SEC and prevents re-triggers.
        threshold: Minimum activation to consider as onset
        fps: Madmom frame rate (100 Hz = 10ms frames)
        beats: Optional beat times in seconds (for grid snapping)
        snap_window_ms: Max distance to snap an onset to a beat subdivision
    """
    try:
        import madmom
        from madmom.features.onsets import OnsetPeakPickingProcessor
    except ImportError:
        raise ImportError(
            "madmom is required for onset teacher labels. "
            "Install via: pip install madmom (requires Python 3.11, use venv311/)"
        ) from None

    proc = madmom.features.onsets.CNNOnsetProcessor()
    activations = proc(audio_path)

    # Peak-pick raw onsets
    picker = OnsetPeakPickingProcessor(threshold=threshold, fps=fps)
    raw_onsets = picker(activations)

    # Get activation strength at each onset
    onset_strengths = []
    for t in raw_onsets:
        frame_idx = int(t * fps)
        if 0 <= frame_idx < len(activations):
            onset_strengths.append(float(activations[frame_idx]))
        else:
            onset_strengths.append(0.0)

    # Strict dedup: within each window, keep only the strongest peak
    dedup_sec = dedup_window_ms / 1000
    deduped_onsets = []
    deduped_strengths = []

    i = 0
    while i < len(raw_onsets):
        # Collect all onsets within the dedup window
        window_onsets = [(raw_onsets[i], onset_strengths[i])]
        j = i + 1
        while j < len(raw_onsets) and (raw_onsets[j] - raw_onsets[i]) < dedup_sec:
            window_onsets.append((raw_onsets[j], onset_strengths[j]))
            j += 1

        # Keep the strongest in this window
        best = max(window_onsets, key=lambda x: x[1])
        deduped_onsets.append(best[0])
        deduped_strengths.append(best[1])
        i = j  # Skip past the window

    # Optional: snap to nearest beat subdivision
    if beats and len(beats) >= 2:
        # Build subdivision grid (8th notes = halfway between beats)
        grid = []
        for k in range(len(beats) - 1):
            grid.append(beats[k])
            grid.append((beats[k] + beats[k + 1]) / 2)  # 8th note
        grid.append(beats[-1])
        grid_arr = np.array(grid)

        snap_sec = snap_window_ms / 1000
        snapped = []
        for t in deduped_onsets:
            dists = np.abs(grid_arr - t)
            nearest_idx = np.argmin(dists)
            if dists[nearest_idx] <= snap_sec:
                snapped.append(float(grid_arr[nearest_idx]))
            else:
                snapped.append(float(t))  # Keep original if no nearby grid point
        deduped_onsets = snapped

    # Validate onset density
    duration = len(activations) / fps
    density = len(deduped_onsets) / duration if duration > 0 else 0
    density_ok = 1.0 <= density <= 8.0

    return {
        "type": "onset_teacher_hard",
        "onsets": [
            {"time": float(t), "strength": float(s)}
            for t, s in zip(deduped_onsets, deduped_strengths)
        ],
        "duration_sec": duration,
        "stats": {
            "raw_count": len(raw_onsets),
            "deduped_count": len(deduped_onsets),
            "dedup_ratio": 1.0 - len(deduped_onsets) / max(len(raw_onsets), 1),
            "density_per_sec": round(density, 1),
            "density_ok": density_ok,
        },
        "params": {
            "threshold": threshold,
            "dedup_window_ms": dedup_window_ms,
            "snap_to_grid": beats is not None,
        },
    }


def load_beats(beats_path: Path) -> list[float] | None:
    """Load beat times from a .beats.json file."""
    if not beats_path.exists():
        return None
    try:
        with open(beats_path) as f:
            data = json.load(f)
        hits = data.get("hits", [])
        return [h["time"] for h in hits if h.get("expect_trigger", True)]
    except (json.JSONDecodeError, KeyError):
        return None


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate onset teacher labels")
    parser.add_argument("--mode", choices=["soft", "hard"], default="hard")
    parser.add_argument("--audio-dir", required=True, help="Directory with audio files")
    parser.add_argument("--output-dir", required=True, help="Output directory for labels")
    parser.add_argument("--beats-dir", help="Beat annotations dir (for grid snapping)")
    parser.add_argument("--snap-to-grid", action="store_true", help="Snap onsets to beat subdivisions")
    parser.add_argument("--threshold", type=float, default=0.3, help="Onset detection threshold")
    parser.add_argument("--dedup-ms", type=float, default=70, help="Dedup window in ms")
    parser.add_argument("--limit", type=int, help="Process only N tracks (for testing)")
    args = parser.parse_args()

    audio_dir = Path(args.audio_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    audio_files = sorted(
        f for f in audio_dir.iterdir()
        if f.suffix.lower() in {".mp3", ".wav", ".flac"}
    )
    if args.limit:
        audio_files = audio_files[: args.limit]

    print(f"Processing {len(audio_files)} tracks → {output_dir}/")
    print(f"Mode: {args.mode}, threshold: {args.threshold}, dedup: {args.dedup_ms}ms")

    t0 = time.time()
    errors = 0
    density_warnings = 0

    for i, audio_path in enumerate(audio_files):
        stem = audio_path.stem
        out_path = output_dir / f"{stem}.onsets_teacher.json"

        if out_path.exists():
            continue  # Skip already processed

        try:
            if args.mode == "soft":
                result = generate_soft_labels(str(audio_path))
            else:
                beats = None
                if args.snap_to_grid and args.beats_dir:
                    beats_path = Path(args.beats_dir) / f"{stem}.beats.json"
                    beats = load_beats(beats_path)

                result = generate_hard_labels(
                    str(audio_path),
                    dedup_window_ms=args.dedup_ms,
                    threshold=args.threshold,
                    beats=beats,
                )

                if not result["stats"]["density_ok"]:
                    density_warnings += 1

            with open(out_path, "w") as f:
                json.dump(result, f)

            if (i + 1) % 100 == 0 or i == 0:
                elapsed = time.time() - t0
                rate = (i + 1) / elapsed
                eta = (len(audio_files) - i - 1) / rate if rate > 0 else 0
                if args.mode == "hard":
                    stats = result.get("stats", {})
                    print(
                        f"  [{i+1}/{len(audio_files)}] {stem}: "
                        f"raw={stats.get('raw_count', '?')} → deduped={stats.get('deduped_count', '?')} "
                        f"({stats.get('density_per_sec', '?')}/sec) "
                        f"[{rate:.1f} tracks/sec, ETA {eta:.0f}s]"
                    )
                else:
                    print(f"  [{i+1}/{len(audio_files)}] {stem} [{rate:.1f} tracks/sec, ETA {eta:.0f}s]")

        except Exception as e:
            print(f"  ERROR {stem}: {e}", file=sys.stderr)
            errors += 1

    elapsed = time.time() - t0
    print(f"\nDone: {len(audio_files) - errors} tracks in {elapsed:.0f}s ({errors} errors)")
    if density_warnings:
        print(f"  {density_warnings} tracks with unusual onset density (outside 1-8/sec)")


if __name__ == "__main__":
    main()
