#!/usr/bin/env python3
"""Generate multi-system onset consensus labels for training.

Runs 3 onset detection systems on each track and merges their detections
into a consensus label set, analogous to the 7-system beat consensus but
for acoustic onsets (not metrical beats).

Systems:
  1. madmom CNNOnsetProcessor (trained on 26K manual annotations)
  2. librosa onset_detect (energy-based, spectral flux)
  3. essentia OnsetDetection (HFC + complex domain)

Consensus: For each detected onset, count how many systems agree within
a ±30ms tolerance window. The "strength" field encodes agreement (1/3, 2/3, 3/3).
Onsets detected by only 1 system are kept but with low strength.

Output: /mnt/storage/blinky-ml-data/labels/onsets_consensus/{stem}.onsets.json
Format: {"onsets": [{"time": 0.1, "strength": 0.667}, ...],
         "count": 87, "systems": 3, "tolerance_ms": 30}

Usage:
    python scripts/generate_onset_consensus.py
    python scripts/generate_onset_consensus.py --workers 4
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import numpy as np

# Defaults
AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio/combined")
LABELS_DIR = Path("/mnt/storage/blinky-ml-data/labels/consensus_v5")
OUTPUT_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_consensus")
LIBROSA_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_librosa")
SR = 16000
HOP = 256
TOLERANCE_SEC = 0.030  # 30ms consensus window


def detect_librosa(audio_path: Path) -> np.ndarray:
    """Detect onsets using librosa (energy-based spectral flux)."""
    import librosa
    y, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    onsets = librosa.onset.onset_detect(
        y=y, sr=SR, hop_length=HOP, backtrack=False, units="time"
    )
    return np.array(onsets, dtype=np.float64)


def detect_essentia(audio_path: Path) -> np.ndarray:
    """Detect onsets using essentia (HFC + complex domain)."""
    import essentia.standard as es
    audio = es.MonoLoader(filename=str(audio_path), sampleRate=SR)()
    # Use HFC (high-frequency content) onset detection — proven for percussive onsets
    od = es.OnsetDetection(method='hfc')
    w = es.Windowing(type='hann')
    fft = es.FFT()
    c2p = es.CartesianToPolar()
    onsets_hfc = []
    for frame in es.FrameGenerator(audio, frameSize=2048, hopSize=HOP):
        mag, phase = c2p(fft(w(frame)))
        onsets_hfc.append(od(mag, phase))
    onsets_hfc = np.array(onsets_hfc)
    # Peak-pick the onset function
    onsets = es.Onsets()
    onset_times = onsets(np.array([onsets_hfc]), [1.0])
    return np.array(onset_times, dtype=np.float64)


def detect_madmom_subprocess(audio_path: Path) -> np.ndarray:
    """Detect onsets using madmom CNNOnsetProcessor via subprocess.

    madmom requires Python 3.11 (venv311), so we call it as a subprocess
    rather than importing directly.
    """
    venv311 = Path(__file__).parent.parent / "venv311"
    python311 = venv311 / "bin" / "python3"
    if not python311.exists():
        return np.array([], dtype=np.float64)

    script = (
        "import sys, json; "
        "import madmom; "
        "proc = madmom.features.onsets.CNNOnsetProcessor(); "
        f"act = proc('{audio_path}'); "
        "pp = madmom.features.onsets.OnsetPeakPickingProcessor(fps=100); "
        "onsets = pp(act); "
        "print(json.dumps([round(float(t), 4) for t in onsets]))"
    )
    try:
        result = subprocess.run(
            [str(python311), "-c", script],
            capture_output=True, text=True, timeout=120
        )
        if result.returncode != 0:
            print(f"  madmom error: {result.stderr.strip()[:200]}", file=sys.stderr)
            return np.array([], dtype=np.float64)
        onsets = json.loads(result.stdout.strip())
        return np.array(onsets, dtype=np.float64)
    except (subprocess.TimeoutExpired, json.JSONDecodeError, Exception) as e:
        print(f"  madmom subprocess failed: {e}", file=sys.stderr)
        return np.array([], dtype=np.float64)


def merge_onsets(all_onsets: list[np.ndarray], tolerance: float) -> list[dict]:
    """Merge onset detections from multiple systems into consensus.

    For each onset event (from any system), find all other detections within
    ±tolerance seconds. Merge these into a single event at the median time.
    Strength = number_of_agreeing_systems / total_systems.
    """
    n_systems = len(all_onsets)
    if n_systems == 0:
        return []

    # Flatten all onsets with system labels
    events = []
    for sys_idx, onsets in enumerate(all_onsets):
        for t in onsets:
            events.append((float(t), sys_idx))
    events.sort(key=lambda x: x[0])

    if not events:
        return []

    # Greedy merge: walk through sorted events, group within tolerance
    merged = []
    used = set()
    for i, (t, _) in enumerate(events):
        if i in used:
            continue
        # Find all events within tolerance of this one
        group_times = [t]
        group_systems = {events[i][1]}
        used.add(i)
        for j in range(i + 1, len(events)):
            if j in used:
                continue
            if events[j][0] - t > tolerance:
                break
            group_times.append(events[j][0])
            group_systems.add(events[j][1])
            used.add(j)

        merged.append({
            "time": round(float(np.median(group_times)), 4),
            "strength": round(len(group_systems) / n_systems, 4),
            "systems": len(group_systems),
        })

    return merged


def process_track(args: tuple) -> str | None:
    """Process one track through all onset detection systems."""
    audio_path, output_path, librosa_cache_path = args

    if output_path.exists():
        return None

    stem = audio_path.stem
    try:
        all_onsets = []

        # 1. librosa (use cached if available)
        if librosa_cache_path and librosa_cache_path.exists():
            with open(librosa_cache_path) as f:
                cached = json.load(f)
            all_onsets.append(np.array(cached["onsets"], dtype=np.float64))
        else:
            all_onsets.append(detect_librosa(audio_path))

        # 2. essentia
        all_onsets.append(detect_essentia(audio_path))

        # 3. madmom (subprocess to venv311)
        madmom_onsets = detect_madmom_subprocess(audio_path)
        if len(madmom_onsets) > 0:
            all_onsets.append(madmom_onsets)

        n_systems = len(all_onsets)
        merged = merge_onsets(all_onsets, TOLERANCE_SEC)

        result = {
            "onsets": merged,
            "count": len(merged),
            "systems": n_systems,
            "tolerance_ms": int(TOLERANCE_SEC * 1000),
        }
        with open(output_path, "w") as f:
            json.dump(result, f)
        return stem

    except Exception as e:
        print(f"ERROR {stem}: {e}", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(
        description="Generate multi-system onset consensus labels"
    )
    parser.add_argument("--audio-dir", type=Path, default=AUDIO_DIR)
    parser.add_argument("--labels-dir", type=Path, default=LABELS_DIR)
    parser.add_argument("--output-dir", type=Path, default=OUTPUT_DIR)
    parser.add_argument("--librosa-dir", type=Path, default=LIBROSA_DIR)
    parser.add_argument("--workers", type=int, default=1,
                        help="Workers (madmom subprocess limits parallelism)")
    args = parser.parse_args()

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect stems from consensus labels (same track set as beat training)
    label_stems = {f.stem.replace(".beats", "") for f in args.labels_dir.glob("*.beats.json")}
    print(f"Found {len(label_stems)} consensus labels")

    # Build audio index
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_index: dict[str, Path] = {}
    for f in args.audio_dir.iterdir():
        if f.suffix.lower() in audio_extensions:
            audio_index[f.stem] = f
    print(f"Found {len(audio_index)} audio files")

    # Build work items
    work_items = []
    already_done = 0
    for stem in sorted(label_stems):
        if stem not in audio_index:
            continue
        out_path = output_dir / f"{stem}.onsets.json"
        if out_path.exists():
            already_done += 1
            continue
        librosa_cache = args.librosa_dir / f"{stem}.onsets.json"
        work_items.append((audio_index[stem], out_path, librosa_cache))

    print(f"  {already_done} already done, {len(work_items)} to process")

    if not work_items:
        print("Nothing to do.")
        return

    print(f"Processing with {args.workers} worker(s)...")
    t0 = time.time()
    done = 0

    if args.workers <= 1:
        for item in work_items:
            result = process_track(item)
            done += 1
            if done % 10 == 0:
                elapsed = time.time() - t0
                rate = done / elapsed if elapsed > 0 else 0
                eta = (len(work_items) - done) / rate if rate > 0 else 0
                print(f"  {done}/{len(work_items)} ({rate:.1f}/s, ETA {eta:.0f}s)")
    else:
        with Pool(processes=args.workers) as pool:
            for result in pool.imap_unordered(process_track, work_items, chunksize=1):
                done += 1
                if done % 10 == 0:
                    elapsed = time.time() - t0
                    rate = done / elapsed if elapsed > 0 else 0
                    print(f"  {done}/{len(work_items)} ({rate:.1f}/s)")

    elapsed = time.time() - t0
    total = len(list(output_dir.glob("*.onsets.json")))
    print(f"Done! {done} processed in {elapsed:.1f}s. Total: {total} files.")


if __name__ == "__main__":
    main()
