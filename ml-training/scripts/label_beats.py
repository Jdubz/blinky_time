#!/usr/bin/env python3
"""Multi-system beat labeling with parallel execution.

Runs multiple beat tracking algorithms on audio files and saves per-system
labels as separate JSON files. Supports parallel workers for CPU-bound systems
while GPU systems run sequentially with shared models.

Supported systems:
  - beat_this    : Beat This! (ISMIR 2024 SOTA), GPU-accelerated
  - essentia     : essentia RhythmExtractor2013, CPU
  - librosa      : librosa beat_track, CPU
  - madmom       : madmom RNN+DBN beat/downbeat tracking, CPU
                   (requires separate Python 3.11 venv at venv311/)
  - beatnet      : BeatNet CRNN + particle filtering, beat/downbeat/meter
                   (requires venv311 — depends on madmom)
  - allin1       : All-In-One structure-aware beat/downbeat + segment analysis
                   (requires venv311 — depends on madmom)
  - demucs_beats : Demucs drum separation → Beat This! on isolated drum stem
                   (GPU, provides independent beat/downbeat from drums only)

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
        --systems beat_this,demucs_beats,beatnet

    # Only new systems (add to existing labels)
    python scripts/label_beats.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir data/labels/multi \
        --systems beatnet,allin1,demucs_beats
"""

import argparse
import json
import os
import select
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

ALL_SYSTEMS = ["beat_this", "essentia", "librosa", "madmom",
               "beatnet", "allin1", "demucs_beats"]
# Systems that benefit from parallel workers (CPU-bound, no shared model)
CPU_SYSTEMS = {"essentia", "librosa", "madmom"}
# Systems that run via venv311 subprocess (madmom dependency, Python 3.11 only)
VENV311_SYSTEMS = {"madmom", "beatnet"}
# allin1 uses a long-lived batch subprocess (not per-track subprocess)
# to avoid ~4s/track initialization overhead (Python + torch + model loading)
BATCH_SYSTEMS = {"allin1"}
# Systems that use GPU sequentially (shared model or heavy GPU use)
GPU_SYSTEMS = {"beat_this", "demucs_beats"}


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


_demucs_separator = None


def label_demucs_beats(audio_path: Path, device: str = "cuda",
                       demix_dir: Path = None) -> dict:
    """Separate drums with Demucs, then run Beat This! on the drum stem.

    Provides beat/downbeat annotations from the isolated percussion track,
    which has fundamentally different error characteristics from full-mix
    analysis. Published research (Beat Transformer ISMIR 2022, Drum-Aware
    Ensemble IEEE 2021) confirms this improves downbeat tracking.

    If demix_dir is provided, saves ALL stems (not just drums) for reuse
    by allin1 which needs bass/drums/other/vocals spectrograms.
    """
    global _demucs_separator, _beat_this_model
    import torch
    import torchaudio

    # Initialize demucs separator (reuse across calls)
    if _demucs_separator is None:
        import demucs.pretrained
        from demucs.apply import apply_model
        _demucs_separator = demucs.pretrained.get_model("htdemucs")
        _demucs_separator.to(device)
        _demucs_separator.eval()

    # Ensure Beat This! is initialized
    if _beat_this_model is None:
        _init_beat_this(device)

    # Load audio for demucs (expects stereo float32 at model's samplerate)
    wav, sr = torchaudio.load(str(audio_path))
    if wav.shape[0] == 1:
        wav = wav.repeat(2, 1)  # mono → stereo
    if sr != _demucs_separator.samplerate:
        wav = torchaudio.functional.resample(wav, sr, _demucs_separator.samplerate)

    # Separate drums
    from demucs.apply import apply_model
    with torch.no_grad():
        sources = apply_model(_demucs_separator, wav.unsqueeze(0).to(device))
    # sources: (1, n_sources, channels, samples)
    # source order matches model.sources: ['drums', 'bass', 'other', 'vocals']
    drum_idx = _demucs_separator.sources.index("drums")
    drums = sources[0, drum_idx].cpu()  # (channels, samples)

    # Save ALL stems for allin1 reuse if demix_dir is provided
    if demix_dir is not None:
        stem_dir = demix_dir / "htdemucs" / audio_path.stem
        stem_dir.mkdir(parents=True, exist_ok=True)
        sr_out = _demucs_separator.samplerate
        for src_name in _demucs_separator.sources:
            src_idx = _demucs_separator.sources.index(src_name)
            src_wav = sources[0, src_idx].cpu()
            torchaudio.save(str(stem_dir / f"{src_name}.wav"), src_wav, sr_out)

    # Save drum stem to temp file for Beat This!
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        torchaudio.save(tmp_path, drums, _demucs_separator.samplerate)

    try:
        beats, downbeats = _beat_this_model(tmp_path)
    finally:
        os.unlink(tmp_path)

    return {
        "system": "demucs_beats",
        "beats": [round(float(t), 4) for t in beats],
        "downbeats": [round(float(t), 4) for t in downbeats],
        "tempo": _bpm_from_beats(beats),
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

    # venv311 subprocess systems (madmom dependency, Python 3.11 only)
    if system in VENV311_SYSTEMS:
        return _label_venv311_subprocess(audio_path, output_path, system, env)

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


def _run_allin1_batch(work: list[tuple[Path, Path]], device: str,
                      demix_dir: Path) -> tuple[int, list]:
    """Run allin1 via a single long-lived batch subprocess.

    Eliminates the ~4s per-track overhead of subprocess-per-track mode
    (Python startup + torch import + 8-fold model loading).

    Args:
        work: List of (audio_file, output_path) tuples.
        device: 'cuda' or 'cpu'.
        demix_dir: Directory containing pre-computed Demucs stems (from
                   demucs_beats). allin1 will skip Demucs for tracks
                   whose stems already exist here.

    Returns:
        (success_count, error_list) where error_list contains
        (filename, system, error_message) tuples.
    """
    if not work:
        return 0, []

    helper_script = Path(__file__).resolve().parent / "_allin1_batch_helper.py"
    if not VENV311_PYTHON.exists():
        return 0, [(w[0].name, "allin1",
                     f"venv311 not found at {VENV311_PYTHON}") for w in work]
    if not helper_script.exists():
        return 0, [(w[0].name, "allin1",
                     f"batch helper not found at {helper_script}") for w in work]

    spec_dir = demix_dir.parent / "spec"

    print(f"\n[allin1] Starting batch subprocess ({len(work)} tracks, "
          f"device={device})...")
    print(f"[allin1] Demix dir: {demix_dir} "
          f"(pre-computed stems will be reused)")

    proc = subprocess.Popen(
        [str(VENV311_PYTHON), str(helper_script),
         "--device", device,
         "--demix-dir", str(demix_dir),
         "--spec-dir", str(spec_dir)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=None,  # let stderr pass through to console
        text=True,
    )

    success = 0
    errors = []

    # Per-track timeout: Demucs (~2s) + NN inference (~1s on GPU, ~90s CPU)
    # + DBN (~0.01s) + overhead. 300s is generous for any single track.
    TRACK_TIMEOUT = 300

    try:
        pbar = tqdm(work, desc="allin1 (batch)")
        for audio_file, output_path in pbar:
            # Send command
            cmd = json.dumps({
                "audio_path": str(audio_file),
                "output_path": str(output_path),
            })
            proc.stdin.write(cmd + "\n")
            proc.stdin.flush()

            # Read response with timeout to prevent indefinite hang
            ready, _, _ = select.select([proc.stdout], [], [], TRACK_TIMEOUT)
            if not ready:
                errors.append((audio_file.name, "allin1",
                               f"timeout ({TRACK_TIMEOUT}s)"))
                tqdm.write(f"  TIMEOUT [allin1] {audio_file.name}")
                # Kill and restart subprocess
                proc.kill()
                proc.wait()
                proc = subprocess.Popen(
                    [str(VENV311_PYTHON), str(helper_script),
                     "--device", device,
                     "--demix-dir", str(demix_dir),
                     "--spec-dir", str(spec_dir)],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=None,
                    text=True,
                )
                continue

            response_line = proc.stdout.readline()
            if not response_line:
                errors.append((audio_file.name, "allin1",
                               "batch subprocess died"))
                # Restart subprocess for remaining tracks
                proc = subprocess.Popen(
                    [str(VENV311_PYTHON), str(helper_script),
                     "--device", device,
                     "--demix-dir", str(demix_dir),
                     "--spec-dir", str(spec_dir)],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=None,
                    text=True,
                )
                continue

            try:
                resp = json.loads(response_line)
                if resp.get("success"):
                    success += 1
                else:
                    err = resp.get("error", "unknown error")
                    errors.append((audio_file.name, "allin1", err))
                    tqdm.write(f"  ERROR [allin1] {audio_file.name}: {err}")
            except json.JSONDecodeError as e:
                errors.append((audio_file.name, "allin1",
                               f"bad JSON response: {e}"))
    finally:
        try:
            proc.stdin.close()
            proc.wait(timeout=30)
        except Exception:
            proc.kill()

    return success, errors


def _label_venv311_subprocess(audio_path: str, output_path: str,
                               system: str,
                               env: dict) -> tuple[str, str, bool, str]:
    """Run a labeling system via subprocess to venv311 (Python 3.11).

    Systems that depend on madmom (which requires Python <=3.11) are run
    via helper scripts in venv311. Each system has its own helper script:
      madmom  → _madmom_helper.py
      beatnet → _beatnet_helper.py
    Note: allin1 uses _run_allin1_batch() instead (batch subprocess mode).
    """
    helper_map = {
        "madmom": "_madmom_helper.py",
        "beatnet": "_beatnet_helper.py",
    }
    helper_name = helper_map.get(system)
    if not helper_name:
        return (audio_path, system, False, f"no venv311 helper for {system}")

    helper_script = Path(__file__).resolve().parent / helper_name
    if not VENV311_PYTHON.exists():
        return (audio_path, system, False,
                f"venv311 not found at {VENV311_PYTHON}")
    if not helper_script.exists():
        return (audio_path, system, False,
                f"helper not found at {helper_script}")

    try:
        result = subprocess.run(
            [str(VENV311_PYTHON), str(helper_script), audio_path],
            capture_output=True, text=True, timeout=600, env=env,
        )
        if result.returncode != 0:
            return (audio_path, system, False, result.stderr.strip()[:200])

        data = json.loads(result.stdout)
        with open(output_path, "w") as f:
            json.dump(data, f, indent=2)
        return (audio_path, system, True, "")
    except subprocess.TimeoutExpired:
        return (audio_path, system, False, "timeout (600s)")
    except Exception as e:
        return (audio_path, system, False, str(e)[:200])


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
    parser.add_argument("--demix-dir", type=str, default=None,
                        help="Directory for Demucs stems (shared between "
                             "demucs_beats and allin1). Default: "
                             "<output-dir>/../demix")
    parser.add_argument("--allin1-device", type=str, default=None,
                        help="Device for allin1 NN inference: cuda or cpu "
                             "(default: same as --device)")
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

    # Resolve shared Demucs stem directory
    if args.demix_dir:
        demix_dir = Path(args.demix_dir)
    else:
        demix_dir = output_dir.parent / "demix"
    # Only create if we need it (demucs_beats or allin1 requested)
    need_demix = bool({"demucs_beats", "allin1"} & set(systems))
    if need_demix:
        demix_dir.mkdir(parents=True, exist_ok=True)

    allin1_device = args.allin1_device or args.device

    # Build work lists: GPU (sequential), CPU (parallel), batch (allin1)
    gpu_work = []      # (file, system) — processed sequentially with shared model
    cpu_work = []      # (file, system) — processed with parallel workers
    allin1_work = []   # (file, output_path) — processed via batch subprocess
    skipped = 0

    for f in files:
        for system in systems:
            out = output_path_for(f, system, output_dir)
            if out.exists() and not args.overwrite:
                skipped += 1
            elif system in BATCH_SYSTEMS:
                allin1_work.append((f, out))
            elif system in CPU_SYSTEMS or system in VENV311_SYSTEMS:
                cpu_work.append((f, system))
            else:
                gpu_work.append((f, system))

    print(f"Found {len(files)} audio files")
    print(f"Systems: {', '.join(systems)}")
    if skipped:
        print(f"Skipping {skipped} already-labeled pairs (use --overwrite to redo)")
    print(f"GPU work (beat_this/demucs_beats, sequential): {len(gpu_work)}")
    print(f"CPU work (parallel, {args.workers} workers): {len(cpu_work)}")
    if allin1_work:
        print(f"allin1 work (batch subprocess, {allin1_device}): {len(allin1_work)}")

    if not gpu_work and not cpu_work and not allin1_work:
        print("All files already labeled for all systems.")
        return

    t0 = time.time()
    success = 0
    errors = []

    # --- GPU work: sequential with shared models ---
    # Run demucs_beats BEFORE allin1 so stems can be reused.
    # If allin1 is also requested, save all Demucs stems to demix_dir.
    save_stems = bool(allin1_work)
    if gpu_work:
        # Group by system to initialize models once
        gpu_systems_needed = {s for _, s in gpu_work}
        if "beat_this" in gpu_systems_needed or "demucs_beats" in gpu_systems_needed:
            print(f"\nLoading Beat This! model on {args.device}...")
            _init_beat_this(device=args.device)
            print("Beat This! model loaded.")

        for audio_file, system in tqdm(gpu_work, desc="GPU (sequential)"):
            try:
                if system == "beat_this":
                    result = label_beat_this(audio_file, device=args.device)
                elif system == "demucs_beats":
                    result = label_demucs_beats(
                        audio_file, device=args.device,
                        demix_dir=demix_dir if save_stems else None)
                else:
                    raise ValueError(f"unknown GPU system: {system}")
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

    # --- allin1 batch work: single long-lived subprocess ---
    if allin1_work:
        a1_success, a1_errors = _run_allin1_batch(
            allin1_work, device=allin1_device, demix_dir=demix_dir)
        success += a1_success
        errors.extend(a1_errors)

    elapsed = time.time() - t0
    total_work = len(gpu_work) + len(cpu_work) + len(allin1_work)
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
