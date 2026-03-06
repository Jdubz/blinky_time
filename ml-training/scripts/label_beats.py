#!/usr/bin/env python3
"""Multi-system beat labeling with parallel execution.

Runs multiple beat tracking algorithms on audio files and saves per-system
labels as separate JSON files. Supports parallel workers for CPU-bound systems
(essentia, librosa, madmom) while Beat This! runs on GPU sequentially.

Supported systems:
  - beat_this  : Beat This! (ISMIR 2024 SOTA), GPU-accelerated
  - essentia   : essentia RhythmExtractor2013, CPU
  - librosa    : librosa beat_track, CPU
  - madmom     : madmom RNN+DBN beat/downbeat tracking, CPU
                 (requires separate Python 3.11 venv at venv311/)

Output: For each audio file and system, creates {stem}.{system}.beats.json
containing beat times, downbeat times, and estimated tempo.

Usage:
    # Label with all systems (parallel CPU workers)
    python scripts/label_beats.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir data/labels/multi

    # Label with specific systems only
    python scripts/label_beats.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir data/labels/multi \
        --systems beat_this,librosa

    # More parallel workers for CPU-bound systems
    python scripts/label_beats.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir data/labels/multi \
        --systems madmom --workers 5
"""

import argparse
import json
import os
import subprocess
import sys
import textwrap
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import numpy as np
from tqdm import tqdm

# Root of ml-training (two levels up from this script)
ML_ROOT = Path(__file__).resolve().parent.parent
VENV311_PYTHON = ML_ROOT / "venv311" / "bin" / "python"

ALL_SYSTEMS = ["beat_this", "essentia", "librosa", "madmom"]
# Systems that benefit from parallel workers (CPU-bound, no shared model)
CPU_SYSTEMS = {"essentia", "librosa", "madmom"}


# ---------------------------------------------------------------------------
# Per-system labeling functions
# ---------------------------------------------------------------------------

def _bpm_from_beats(beats: np.ndarray) -> float:
    """Estimate BPM from median inter-beat interval."""
    if len(beats) < 2:
        return 0.0
    ibis = np.diff(beats)
    return round(60.0 / float(np.median(ibis)), 1)


_beat_this_model = None


def label_beat_this(audio_path: Path, device: str = "cuda") -> dict:
    """Label beats using Beat This! (GPU-accelerated)."""
    global _beat_this_model
    if _beat_this_model is None:
        _init_beat_this(device)
    beats, downbeats = _beat_this_model(str(audio_path))
    return {
        "system": "beat_this",
        "beats": [round(float(t), 4) for t in beats],
        "downbeats": [round(float(t), 4) for t in downbeats],
        "tempo": _bpm_from_beats(beats),
    }


def _init_beat_this(device: str = "cuda"):
    """Initialize Beat This! model (call once before labeling loop)."""
    global _beat_this_model
    from beat_this.inference import File2Beats
    _beat_this_model = File2Beats(device=device, dbn=False)


def label_essentia(audio_path: Path, **_kwargs) -> dict:
    """Label beats using essentia RhythmExtractor2013."""
    import essentia.standard as es
    audio = es.MonoLoader(filename=str(audio_path), sampleRate=44100)()
    rhythm_extractor = es.RhythmExtractor2013(method="multifeature")
    bpm, beats, beats_confidence, _, beats_intervals = rhythm_extractor(audio)
    return {
        "system": "essentia",
        "beats": [round(float(t), 4) for t in beats],
        "downbeats": [],
        "tempo": round(float(bpm), 1),
    }


def label_librosa(audio_path: Path, **_kwargs) -> dict:
    """Label beats using librosa beat_track."""
    import librosa
    y, sr = librosa.load(str(audio_path), sr=22050, mono=True)
    tempo, beat_frames = librosa.beat.beat_track(y=y, sr=sr)
    beat_times = librosa.frames_to_time(beat_frames, sr=sr)
    tempo_val = float(np.atleast_1d(tempo)[0])
    return {
        "system": "librosa",
        "beats": [round(float(t), 4) for t in beat_times],
        "downbeats": [],
        "tempo": round(tempo_val, 1),
    }


def _label_cpu_worker(audio_path: str, output_path: str, system: str,
                      threads: int) -> tuple[str, str, bool, str]:
    """Process a single (file, system) pair in a worker process.

    Restricts BLAS threads to avoid oversubscription across workers.
    Returns (audio_path, system, success, error_message).
    """
    env = os.environ.copy()
    for var in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                "NUMEXPR_NUM_THREADS", "VECLIB_MAXIMUM_THREADS"):
        env[var] = str(threads)

    if system == "madmom":
        return _label_madmom_subprocess(audio_path, output_path, env)

    # For essentia/librosa, run in-process (already in a worker process)
    # but set thread limits first
    for var, val in env.items():
        if var.endswith("_THREADS"):
            os.environ[var] = val

    try:
        if system == "essentia":
            result = label_essentia(Path(audio_path))
        elif system == "librosa":
            result = label_librosa(Path(audio_path))
        else:
            return (audio_path, system, False, f"unknown CPU system: {system}")

        with open(output_path, "w") as f:
            json.dump(result, f, indent=2)
        return (audio_path, system, True, "")
    except Exception as e:
        return (audio_path, system, False, str(e)[:200])


def _label_madmom_subprocess(audio_path: str, output_path: str,
                             env: dict) -> tuple[str, str, bool, str]:
    """Run madmom via subprocess to venv311."""
    helper_script = Path(__file__).resolve().parent / "_madmom_helper.py"
    if not VENV311_PYTHON.exists():
        return (audio_path, "madmom", False,
                f"venv311 not found at {VENV311_PYTHON}")
    if not helper_script.exists():
        return (audio_path, "madmom", False,
                f"helper not found at {helper_script}")

    try:
        result = subprocess.run(
            [str(VENV311_PYTHON), str(helper_script), audio_path],
            capture_output=True, text=True, timeout=300, env=env,
        )
        if result.returncode != 0:
            return (audio_path, "madmom", False, result.stderr.strip()[:200])

        data = json.loads(result.stdout)
        with open(output_path, "w") as f:
            json.dump(data, f, indent=2)
        return (audio_path, "madmom", True, "")
    except subprocess.TimeoutExpired:
        return (audio_path, "madmom", False, "timeout (300s)")
    except Exception as e:
        return (audio_path, "madmom", False, str(e)[:200])


# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------

def collect_audio_files(audio_dir: Path, extensions: set) -> list[Path]:
    """Recursively collect audio files matching the given extensions."""
    if audio_dir.is_file():
        return [audio_dir]
    if audio_dir.is_dir():
        return sorted(f for f in audio_dir.rglob("*") if f.suffix.lower() in extensions)
    print(f"Error: {audio_dir} not found", file=sys.stderr)
    sys.exit(1)


def output_path_for(audio_file: Path, system: str, output_dir: Path) -> Path:
    return output_dir / f"{audio_file.stem}.{system}.beats.json"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Multi-system beat labeling with parallel execution.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Examples:
              # All systems (parallel CPU workers)
              python scripts/label_beats.py --audio-dir /path/to/audio --output-dir data/labels/multi

              # Specific systems
              python scripts/label_beats.py --audio-dir /path/to/audio --output-dir data/labels/multi --systems beat_this,essentia

              # More workers for madmom
              python scripts/label_beats.py --audio-dir /path/to/audio --output-dir data/labels/multi --systems madmom --workers 5
        """),
    )
    parser.add_argument("--audio-dir", required=True,
                        help="Directory of audio files (or a single file)")
    parser.add_argument("--output-dir", required=True,
                        help="Output directory for per-system .beats.json files")
    parser.add_argument("--systems", default=",".join(ALL_SYSTEMS),
                        help=f"Comma-separated systems (default: {','.join(ALL_SYSTEMS)})")
    parser.add_argument("--device", default="cuda",
                        help="Device for Beat This!: cuda or cpu (default: cuda)")
    parser.add_argument("--extensions", default=".mp3,.wav,.flac,.ogg")
    parser.add_argument("--overwrite", action="store_true",
                        help="Overwrite existing label files")
    parser.add_argument("--workers", type=int, default=5,
                        help="Parallel workers for CPU systems (default: 5)")
    parser.add_argument("--threads-per-worker", type=int, default=4,
                        help="BLAS threads per worker (default: 4)")
    args = parser.parse_args()

    audio_dir = Path(args.audio_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    extensions = set(args.extensions.split(","))
    systems = [s.strip() for s in args.systems.split(",")]
    for s in systems:
        if s not in ALL_SYSTEMS:
            print(f"Error: unknown system '{s}'. Choose from: {', '.join(ALL_SYSTEMS)}",
                  file=sys.stderr)
            sys.exit(1)

    files = collect_audio_files(audio_dir, extensions)
    if not files:
        print(f"No audio files found in {audio_dir}", file=sys.stderr)
        sys.exit(1)

    # Build work lists: separate GPU (sequential) from CPU (parallel)
    gpu_work = []  # (file, system) — processed sequentially with shared model
    cpu_work = []  # (file, system) — processed with parallel workers
    skipped = 0

    for f in files:
        for system in systems:
            out = output_path_for(f, system, output_dir)
            if out.exists() and not args.overwrite:
                skipped += 1
            elif system in CPU_SYSTEMS:
                cpu_work.append((f, system))
            else:
                gpu_work.append((f, system))

    print(f"Found {len(files)} audio files")
    print(f"Systems: {', '.join(systems)}")
    if skipped:
        print(f"Skipping {skipped} already-labeled pairs (use --overwrite to redo)")
    print(f"GPU work (beat_this, sequential): {len(gpu_work)}")
    print(f"CPU work (parallel, {args.workers} workers): {len(cpu_work)}")

    if not gpu_work and not cpu_work:
        print("All files already labeled for all systems.")
        return

    t0 = time.time()
    success = 0
    errors = []

    # --- GPU work: Beat This! (sequential, shared model) ---
    if gpu_work:
        print(f"\nLoading Beat This! model on {args.device}...")
        _init_beat_this(device=args.device)
        print("Beat This! model loaded.")

        for audio_file, system in tqdm(gpu_work, desc="beat_this (GPU)"):
            try:
                result = label_beat_this(audio_file, device=args.device)
                out = output_path_for(audio_file, system, output_dir)
                with open(out, "w") as fp:
                    json.dump(result, fp, indent=2)
                success += 1
            except Exception as e:
                errors.append((audio_file.name, system, str(e)))
                tqdm.write(f"  ERROR [{system}] {audio_file.name}: {e}")

    # --- CPU work: parallel workers ---
    if cpu_work:
        print(f"\nProcessing {len(cpu_work)} CPU jobs with {args.workers} workers "
              f"({args.threads_per_worker} threads each)...")

        with ProcessPoolExecutor(max_workers=args.workers) as executor:
            futures = {}
            for audio_file, system in cpu_work:
                out = str(output_path_for(audio_file, system, output_dir))
                fut = executor.submit(
                    _label_cpu_worker, str(audio_file), out, system,
                    args.threads_per_worker)
                futures[fut] = (audio_file.name, system)

            for future in tqdm(as_completed(futures), total=len(cpu_work),
                               desc="CPU (parallel)"):
                audio_path, system, ok, err = future.result()
                if ok:
                    success += 1
                else:
                    name = Path(audio_path).name
                    errors.append((name, system, err))
                    tqdm.write(f"  ERROR [{system}] {name}: {err}")

    elapsed = time.time() - t0
    total_work = len(gpu_work) + len(cpu_work)
    print(f"\nDone: {success}/{total_work} jobs in {elapsed:.1f}s "
          f"({elapsed / max(total_work, 1):.2f}s/job)")

    if errors:
        print(f"\n{len(errors)} errors:")
        for name, system, err in errors[:20]:
            print(f"  [{system}] {name}: {err}")
        if len(errors) > 20:
            print(f"  ... and {len(errors) - 20} more")


if __name__ == "__main__":
    main()
