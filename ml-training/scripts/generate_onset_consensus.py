#!/usr/bin/env python3
"""Generate multi-system onset consensus labels for training.

Runs 5 onset detection systems on each track and merges their detections
into a consensus label set, analogous to the 7-system beat consensus but
for acoustic onsets (not metrical beats).

Systems:
  1. librosa onset_detect (spectral flux peak-picking)
  2. essentia HFC (high-frequency content — percussive onsets)
  3. essentia complex domain (pitched onsets, polyphonic content)
  4. madmom CNNOnsetProcessor (neural, trained on 26K manual annotations)
  5. madmom RNNOnsetProcessor (recurrent neural, complementary to CNN)

Consensus: For each detected onset, count how many systems agree within
a ±30ms tolerance window. The "strength" field encodes agreement (1/5 to 5/5).
More systems = higher confidence that the onset is real.

Output: /mnt/storage/blinky-ml-data/labels/onsets_consensus/{stem}.onsets.json
Format: {"onsets": [{"time": 0.1, "strength": 0.4, "systems": 2}, ...],
         "count": 87, "total_systems": 5, "tolerance_ms": 30}

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
TOLERANCE_SEC = 0.070  # 70ms consensus window (half a 16th note at common tempos)
TOTAL_SYSTEMS = 5      # Fixed denominator for strength (even if some fail)


def detect_librosa(audio_path: Path) -> np.ndarray:
    """Detect onsets using librosa (spectral flux peak-picking)."""
    import librosa
    y, _ = librosa.load(str(audio_path), sr=SR, mono=True)
    onsets = librosa.onset.onset_detect(
        y=y, sr=SR, hop_length=HOP, backtrack=False, units="time"
    )
    return np.array(onsets, dtype=np.float64)


def detect_essentia_hfc(audio: np.ndarray) -> np.ndarray:
    """Detect onsets using essentia HFC (high-frequency content).

    Good for percussive onsets (kicks, snares, hi-hats).
    """
    import essentia.standard as es
    od = es.OnsetDetection(method='hfc')
    w = es.Windowing(type='hann')
    fft = es.FFT()
    c2p = es.CartesianToPolar()
    onset_func = []
    for frame in es.FrameGenerator(audio, frameSize=2048, hopSize=HOP):
        mag, phase = c2p(fft(w(frame)))
        onset_func.append(od(mag, phase))
    onset_func = np.array(onset_func)
    onsets = es.Onsets()
    onset_times = onsets(np.array([onset_func]), [1.0])
    return np.array(onset_times, dtype=np.float64)


def detect_essentia_complex(audio: np.ndarray) -> np.ndarray:
    """Detect onsets using essentia complex domain method.

    Complementary to HFC — better for pitched onsets and polyphonic content.
    """
    import essentia.standard as es
    od = es.OnsetDetection(method='complex')
    w = es.Windowing(type='hann')
    fft = es.FFT()
    c2p = es.CartesianToPolar()
    onset_func = []
    for frame in es.FrameGenerator(audio, frameSize=2048, hopSize=HOP):
        mag, phase = c2p(fft(w(frame)))
        onset_func.append(od(mag, phase))
    onset_func = np.array(onset_func)
    onsets = es.Onsets()
    onset_times = onsets(np.array([onset_func]), [1.0])
    return np.array(onset_times, dtype=np.float64)


def detect_madmom_subprocess(audio_path: Path, processor: str = "CNN") -> np.ndarray:
    """Detect onsets using madmom via subprocess.

    madmom requires Python 3.11 (venv311), so we call it as a subprocess.
    Supports CNNOnsetProcessor and RNNOnsetProcessor.
    """
    venv311 = Path(__file__).parent.parent / "venv311"
    python311 = venv311 / "bin" / "python3"
    if not python311.exists():
        return np.array([], dtype=np.float64)

    proc_class = f"{processor}OnsetProcessor"
    # Pass audio path as command-line argument to avoid string injection
    script = (
        "import sys, json, madmom; "
        f"proc = madmom.features.onsets.{proc_class}(); "
        "act = proc(sys.argv[1]); "
        "pp = madmom.features.onsets.OnsetPeakPickingProcessor(fps=100); "
        "onsets = pp(act); "
        "print(json.dumps([round(float(t), 4) for t in onsets]))"
    )
    try:
        result = subprocess.run(
            [str(python311), "-c", script, str(audio_path)],
            capture_output=True, text=True, timeout=120
        )
        if result.returncode != 0:
            return np.array([], dtype=np.float64)
        onsets = json.loads(result.stdout.strip())
        return np.array(onsets, dtype=np.float64)
    except (subprocess.TimeoutExpired, json.JSONDecodeError, Exception):
        return np.array([], dtype=np.float64)


def merge_onsets(all_onsets: list[np.ndarray], tolerance: float,
                 total_systems: int) -> list[dict]:
    """Merge onset detections from multiple systems into consensus.

    For each onset event, find all other detections within ±tolerance seconds.
    Uses sliding-window grouping: each new event must be within tolerance of
    the PREVIOUS event in the group (not the first), preventing long chains
    from merging distant events.

    Strength = number_of_agreeing_systems / total_systems (fixed denominator).
    """
    if not all_onsets:
        return []

    # Flatten all onsets with system labels
    events = []
    for sys_idx, onsets in enumerate(all_onsets):
        for t in onsets:
            events.append((float(t), sys_idx))
    events.sort(key=lambda x: x[0])

    if not events:
        return []

    # Greedy merge with sliding window: each event must be within tolerance
    # of the LAST event added to the group (not the first). This prevents
    # chaining: A(0.100) + B(0.125) + C(0.155) won't merge if C is >30ms
    # from B, even though B was within 30ms of A.
    merged = []
    used = set()
    for i, (t, sys_i) in enumerate(events):
        if i in used:
            continue
        group_times = [t]
        group_systems = {sys_i}
        used.add(i)
        last_time = t
        for j in range(i + 1, len(events)):
            if j in used:
                continue
            if events[j][0] - last_time > tolerance:
                break
            group_times.append(events[j][0])
            group_systems.add(events[j][1])
            used.add(j)
            last_time = events[j][0]

        merged.append({
            "time": round(float(np.median(group_times)), 4),
            "strength": round(len(group_systems) / total_systems, 4),
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

        # 1. librosa onset_detect (use cached if available)
        if librosa_cache_path and librosa_cache_path.exists():
            with open(librosa_cache_path) as f:
                cached = json.load(f)
            all_onsets.append(np.array(cached["onsets"], dtype=np.float64))
        else:
            all_onsets.append(detect_librosa(audio_path))

        # Load audio once for both essentia detectors
        import essentia.standard as es
        audio = es.MonoLoader(filename=str(audio_path), sampleRate=SR)()

        # 2. essentia HFC (percussive onsets)
        hfc_onsets = detect_essentia_hfc(audio)
        all_onsets.append(hfc_onsets)

        # 3. essentia complex domain (pitched onsets)
        complex_onsets = detect_essentia_complex(audio)
        all_onsets.append(complex_onsets)

        # 4. madmom CNNOnsetProcessor (neural, 26K annotations)
        cnn_onsets = detect_madmom_subprocess(audio_path, processor="CNN")
        if len(cnn_onsets) > 0:
            all_onsets.append(cnn_onsets)

        # 5. madmom RNNOnsetProcessor (recurrent, complementary)
        rnn_onsets = detect_madmom_subprocess(audio_path, processor="RNN")
        if len(rnn_onsets) > 0:
            all_onsets.append(rnn_onsets)

        # Use TOTAL_SYSTEMS as denominator regardless of how many succeeded.
        # This ensures strength values are comparable across tracks even if
        # madmom fails on some tracks (e.g., 2/5=0.4 not 2/3=0.67).
        merged = merge_onsets(all_onsets, TOLERANCE_SEC, TOTAL_SYSTEMS)

        result = {
            "onsets": merged,
            "count": len(merged),
            "systems_succeeded": len(all_onsets),
            "total_systems": TOTAL_SYSTEMS,
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
    parser.add_argument("--test-dir", type=Path,
                        default=Path(__file__).parent.parent.parent / "blinky-test-player/music/edm",
                        help="Also generate labels for test tracks (output alongside audio)")
    parser.add_argument("--workers", type=int, default=1,
                        help="Workers (madmom subprocess limits parallelism)")
    args = parser.parse_args()

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect stems from consensus labels (same track set as beat training)
    label_stems = {f.stem.replace(".beats", "") for f in args.labels_dir.glob("*.beats.json")}
    print(f"Found {len(label_stems)} consensus labels")

    # Build audio index (flat dir, no rglob)
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_index: dict[str, Path] = {}
    for f in args.audio_dir.iterdir():
        if f.suffix.lower() in audio_extensions:
            audio_index[f.stem] = f
    print(f"Found {len(audio_index)} audio files")

    # Build work items: training labels
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

    # Also add test tracks (output as .onsets_consensus.json alongside audio)
    test_added = 0
    if args.test_dir and args.test_dir.exists():
        for f in sorted(args.test_dir.iterdir()):
            if f.suffix.lower() not in audio_extensions:
                continue
            out_path = args.test_dir / f"{f.stem}.onsets_consensus.json"
            if out_path.exists():
                already_done += 1
                continue
            librosa_cache = args.librosa_dir / f"{f.stem}.onsets.json"
            work_items.append((f, out_path, librosa_cache))
            test_added += 1

    print(f"  {already_done} already done, {len(work_items)} to process")
    if test_added > 0:
        print(f"  (includes {test_added} test tracks)")
    print(f"  {TOTAL_SYSTEMS} onset detection systems")

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
