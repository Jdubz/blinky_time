#!/usr/bin/env python3
"""Generate kick-weighted onset targets for training.

Creates per-track target arrays where:
  - Kick onsets (<200 Hz) get target = 1.0
  - Snare onsets (200-4000 Hz) get target = 1.0 (equally important as kicks)
  - Hi-hat/cymbal onsets (>4000 Hz) get target = 0.0 (suppress)

This teaches the NN to fire strongly on kicks and snares (equally important),
and not at all on hi-hats — producing the 2-4 major events per bar
density target needed for smooth LED animations.

Usage:
    python scripts/generate_kick_weighted_targets.py \\
        --audio-dir "$BLINKY_DATA/audio/drums_only" \\
        --output-dir "$BLINKY_DATA/labels/kick_weighted_drums_g1" \\
        --tracks-from "$BLINKY_DATA/labels/onsets_consensus" --workers 4

--algorithm adtof (the default since #284) labels with the ADTOF transcription model
(scripts/adtof_labels.py), on full mixes or drum stems; --algorithm bandpass is the original
band-onset generator below, kept so the suite can compare against it.

Every label file and the directory's .provenance.json carry the generator: the algorithm, its
version and every parameter. A run resumes only over files from the same generator and refuses
a directory written by another, so labels from two generators never mix in one dir (#284).
Score any change with scripts/drum_label_suite.py first.

Output format (per track):
    {
        "onsets": [{"time": 0.5, "weight": 1.0, "type": "kick"}, ...],
        "kick_count": 42,
        "snare_count": 38,
        "hihat_count": 85,
        "source": "bandpass_onset_detect"
    }
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from multiprocessing import Pool
from pathlib import Path

import librosa
import numpy as np
from scipy.signal import butter, sosfilt

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # `scripts.` for adtof_labels

# Frequency bands for onset classification
# Kick: 40-200 Hz (below 40 Hz is sub-bass rumble / mp3 noise artifacts)
# Snare: 200-4000 Hz (body + wire rattle)
# Hihat: 4000-7500 Hz (capped below Nyquist/2 for 16 kHz SR to avoid aliasing)
KICK_LOW = 40
KICK_HIGH = 200
SNARE_LOW = 200
SNARE_HIGH = 4000
HIHAT_LOW = 4000
HIHAT_HIGH = 7500

# Minimum RMS threshold for drum stem audio (below = silent/failed separation)
MIN_RMS = 0.005

# Target weights per onset type
# Kicks and snares are equally important — both are primary visual events.
# Snares cut through the mix ("the talking drum") and drive accents just
# as strongly as kicks drive the downbeat.
KICK_WEIGHT = 1.0
SNARE_WEIGHT = 1.0
HIHAT_WEIGHT = 0.0

SR = 16000
HOP = 256
MERGE_TOL_S = 0.03

# What produced a label. Bump `version` with any change to how labels are made, which also
# makes every existing output dir refuse the new generator until pointed at a new one.
GENERATOR = {
    "algorithm": "bandpass_onset_detect",
    "version": 1,
    "params": {
        "sr": SR,
        "hop": HOP,
        "bands_hz": {
            "kick": [KICK_LOW, KICK_HIGH],
            "snare": [SNARE_LOW, SNARE_HIGH],
            "hihat": [HIHAT_LOW, HIHAT_HIGH],
        },
        "weights": {"kick": KICK_WEIGHT, "snare": SNARE_WEIGHT, "hihat": HIHAT_WEIGHT},
        "min_rms": MIN_RMS,
        "merge_tol_s": MERGE_TOL_S,
        "filter": "butter4 sosfilt",
        "onset": "librosa.onset.onset_detect(hop=256, backtrack=False)",
    },
}


def bandpass_onsets(y: np.ndarray, sr: int, low_hz: float, high_hz: float) -> np.ndarray:
    """Detect onsets in a frequency band."""
    nyq = sr / 2
    low = max(low_hz / nyq, 0.001)
    high = min(high_hz / nyq, 0.999)
    sos = butter(4, [low, high], btype='band', output='sos')
    filtered = sosfilt(sos, y)
    times = librosa.onset.onset_detect(
        y=filtered, sr=sr, hop_length=HOP,
        backtrack=False, units='time'
    )
    return times


def merge_weighted_onsets(kick_times, snare_times, hihat_times, merge_tol=MERGE_TOL_S):
    """Merge onset times from different bands with priority weighting.

    When onsets from different bands overlap (within merge_tol seconds),
    the highest-priority band wins:  kick > snare > hihat.

    This prevents double-counting when a kick triggers onsets in both
    the kick and snare bands (which is common due to spectral leakage).
    """
    events = []
    for t in kick_times:
        events.append({"time": float(t), "weight": KICK_WEIGHT, "type": "kick"})
    for t in snare_times:
        events.append({"time": float(t), "weight": SNARE_WEIGHT, "type": "snare"})
    for t in hihat_times:
        events.append({"time": float(t), "weight": HIHAT_WEIGHT, "type": "hihat"})

    # Sort by time
    events.sort(key=lambda e: e["time"])

    # Merge overlapping events: highest weight wins
    merged = []
    for evt in events:
        if merged and abs(evt["time"] - merged[-1]["time"]) < merge_tol:
            # Overlapping — keep the higher weight
            if evt["weight"] > merged[-1]["weight"]:
                merged[-1] = evt
        else:
            merged.append(evt)

    return merged


def label_audio(y: np.ndarray, sr: int) -> dict:
    """The labels for one drum stem, as written to its .kick_weighted.json (without the file).
    Resamples to SR first, as loading does, so a caller can pass audio at any rate."""
    if sr != SR:
        y = librosa.resample(y, orig_sr=sr, target_sr=SR)
        sr = SR
    bands = {
        "kick": [KICK_LOW, KICK_HIGH],
        "snare": [SNARE_LOW, SNARE_HIGH],
        "hihat": [HIHAT_LOW, HIHAT_HIGH],
    }
    weights = {"kick": KICK_WEIGHT, "snare": SNARE_WEIGHT, "hihat": HIHAT_WEIGHT}

    # Validate drum stem has actual content (not silent/failed separation)
    rms = float(np.sqrt(np.mean(y ** 2)))
    if rms < MIN_RMS:
        return {
            "onsets": [],
            "kick_count": 0,
            "snare_count": 0,
            "hihat_count": 0,
            "total": 0,
            "source": "bandpass_onset_detect",
            "rms": rms,
            "skipped": True,
            "skip_reason": f"silent drum stem (RMS={rms:.6f} < {MIN_RMS})",
            "bands_hz": bands,
            "weights": weights,
            "generator": GENERATOR,
        }

    kick_times = bandpass_onsets(y, sr, KICK_LOW, KICK_HIGH)
    snare_times = bandpass_onsets(y, sr, SNARE_LOW, SNARE_HIGH)
    hihat_times = bandpass_onsets(y, sr, HIHAT_LOW, HIHAT_HIGH)

    merged = merge_weighted_onsets(kick_times, snare_times, hihat_times)

    return {
        "onsets": merged,
        "kick_count": sum(1 for e in merged if e["type"] == "kick"),
        "snare_count": sum(1 for e in merged if e["type"] == "snare"),
        "hihat_count": sum(1 for e in merged if e["type"] == "hihat"),
        "total": len(merged),
        "source": "bandpass_onset_detect",
        "rms": rms,
        "weights": weights,
        "bands_hz": bands,
        "generator": GENERATOR,
    }


ALGORITHMS = ("adtof", "bandpass")


def generator_for(algorithm: str) -> dict:
    """GENERATOR for an algorithm. adtof's lives in adtof_labels.py (importing it needs no TensorFlow)."""
    if algorithm == "bandpass":
        return GENERATOR
    from scripts.adtof_labels import GENERATOR as ADTOF

    return ADTOF


def same_generator(path: Path, generator: dict) -> bool:
    """Whether an existing label file came from this generator (old files carry none)."""
    try:
        return json.loads(path.read_text()).get("generator") == generator
    except (OSError, ValueError):
        return False


def process_track(args):
    """Process a single track."""
    audio_path, output_path, algorithm = args

    if output_path.exists() and same_generator(output_path, generator_for(algorithm)):
        return None  # Resumable: done by this same generator

    try:
        if algorithm == "adtof":
            from scripts.adtof_labels import label_file

            result = label_file(audio_path)
        else:
            y, sr = librosa.load(str(audio_path), sr=SR)
            result = label_audio(y, sr)
    except Exception as e:
        return f"FAILED {audio_path.name}: {type(e).__name__}: {e}"

    # Atomic write: temp file + rename prevents corrupt partial writes
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix('.tmp')
    with open(tmp_path, 'w') as f:
        json.dump(result, f, indent=2)
    tmp_path.rename(output_path)

    if result.get("skipped"):
        return f"SILENT {audio_path.stem}: RMS={result['rms']:.6f}"
    return None


def write_provenance(output_dir: Path, audio_dir: Path, tracks_from: Path | None,
                     track_count: int, generator: dict) -> None:
    """Record what produced this dir: a later run refuses it unless the generator matches."""
    provenance = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "audio_source": str(audio_dir),
        "tracks_from": str(tracks_from) if tracks_from else None,
        "track_count": track_count,
        "script": "generate_kick_weighted_targets.py",
        "generator": generator,
    }
    tmp = output_dir / ".provenance.json.tmp"
    tmp.write_text(json.dumps(provenance, indent=2))
    tmp.rename(output_dir / ".provenance.json")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate kick-weighted onset targets")
    parser.add_argument("--algorithm", choices=ALGORITHMS, default="adtof",
                        help="adtof: the ADTOF transcription model (full mixes or drum stems; "
                             "#284). bandpass: the old band-onset generator (drum stems), kept "
                             "for comparison")
    parser.add_argument("--audio-dir", type=Path, required=True,
                        help="Directory of audio, flat (<track>.<ext>)")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Output dir. Use a new one for a new generator version.")
    parser.add_argument("--tracks-from", type=Path, default=None,
                        help="Only tracks that have a label file here (any <track>.*.json), "
                             "e.g. labels/onsets_consensus. Default: every audio file.")
    parser.add_argument("--workers", type=int, default=4,
                        help="Number of parallel workers")
    parser.add_argument("--force", action="store_true",
                        help="Write into a dir from another audio source or generator, "
                             "regenerating every file not from this generator")
    args = parser.parse_args()

    audio_dir, output_dir = args.audio_dir, args.output_dir
    generator = generator_for(args.algorithm)

    # Labels from two generators, or two audio sources, must never share a dir.
    provenance_path = output_dir / ".provenance.json"
    if provenance_path.exists() and not args.force:
        prev = json.loads(provenance_path.read_text())
        problems = []
        if prev.get("audio_source") != str(audio_dir):
            problems.append(f"audio source {prev.get('audio_source')} != {audio_dir}")
        if prev.get("generator") != generator:
            got = prev.get("generator") or {"algorithm": "unversioned"}
            problems.append(f"generator {got.get('algorithm')} v{got.get('version', '?')} "
                            f"!= {generator['algorithm']} v{generator['version']}")
        if problems:
            print(f"ERROR: {output_dir} holds other labels: " + "; ".join(problems),
                  file=sys.stderr)
            print("Use a new --output-dir (or --force to regenerate in place).", file=sys.stderr)
            return 1
        print(f"Resuming previous run in {output_dir}")
    elif any(output_dir.glob("*.kick_weighted.json")) and not args.force:
        print(f"ERROR: {output_dir} has labels but no .provenance.json, so what made them is "
              f"unknown. Use a new --output-dir (or --force).", file=sys.stderr)
        return 1

    output_dir.mkdir(parents=True, exist_ok=True)

    # Find audio files (non-recursive — combined/ is a flat namespace)
    audio_files = sorted(
        f for f in audio_dir.iterdir()
        if f.suffix.lower() in (".mp3", ".wav", ".flac", ".ogg")
    )
    print(f"Found {len(audio_files)} audio files in {audio_dir}")
    audio_by_stem: dict[str, Path] = {}
    for af in audio_files:
        audio_by_stem.setdefault(af.stem, af)  # full stem: "000397" or "3618176.LOFI"
    if args.tracks_from is not None:
        if not args.tracks_from.is_dir():
            print(f"ERROR: --tracks-from {args.tracks_from} is not a directory", file=sys.stderr)
            return 1
        wanted = {f.name.split(".")[0] for f in args.tracks_from.glob("*.json")}
        audio_by_stem = {s: a for s, a in audio_by_stem.items() if s.split(".")[0] in wanted}
        print(f"Matched {len(audio_by_stem)} audio files to labels in {args.tracks_from}")
    if not audio_by_stem:
        # The old default (--labels-dir consensus_v5, archived in #271) matched nothing and
        # exited 0 with "Nothing to do!": a silent failure.
        print("ERROR: no tracks to label", file=sys.stderr)
        return 1

    work = []
    skipped = 0
    for stem, af in sorted(audio_by_stem.items()):
        out = output_dir / f"{stem}.kick_weighted.json"
        if out.exists() and same_generator(out, generator):
            skipped += 1
        else:
            work.append((af, out, args.algorithm))
    print(f"Processing {len(work)} tracks ({skipped} already done by this generator)")

    errors = 0
    done = 0
    if work:
        with Pool(args.workers) as pool:
            for result in pool.imap_unordered(process_track, work):
                done += 1
                if result:
                    print(f"  {result}")
                    errors += result.startswith("FAILED")
                if done % 100 == 0:
                    print(f"  Progress: {done}/{len(work)} "
                          f"({100*done/len(work):.1f}%)")
        print(f"\nDone! Processed {done} tracks ({errors} failed)")

    # Summary stats
    all_files = list(output_dir.glob("*.kick_weighted.json"))
    kicks = snares = hihats = 0
    for f in all_files:
        with open(f) as fh:
            data = json.load(fh)
        kicks += data["kick_count"]
        snares += data["snare_count"]
        hihats += data["hihat_count"]
    total = kicks + snares + hihats
    print(f"Total labels: {len(all_files)} tracks")
    if total > 0:
        print(f"  Kicks:  {kicks} ({100*kicks/total:.0f}%)")
        print(f"  Snares: {snares} ({100*snares/total:.0f}%)")
        print(f"  HiHats: {hihats} ({100*hihats/total:.0f}%)")
    else:
        print("  No onsets found in any track")

    write_provenance(output_dir, audio_dir, args.tracks_from, len(all_files), generator)
    print(f"\nProvenance written to {output_dir / '.provenance.json'}")
    if errors:
        print(f"ERROR: {errors} track(s) failed to load", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
