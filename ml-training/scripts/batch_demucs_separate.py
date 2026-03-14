#!/usr/bin/env python3
"""Batch Demucs HTDemucs source separation for all training audio.

Loads HTDemucs once on GPU and processes all tracks sequentially, saving
all 4 stems (drums, bass, other, vocals) per track. This serves two purposes:

1. Training data augmentation — drums-only mel spectrograms teach the model
   what kick/snare patterns look like without masking from other instruments.
   Same beat labels apply (beats happen at the same times regardless of mix).

2. allin1 labeling speedup — allin1 runs Demucs internally; pre-separated
   stems skip this step entirely (allin1.demix() auto-detects cached stems).

Performance: ~1s/track on GPU (HTDemucs). ~7000 tracks in ~2 hours.
Disk: ~10 MB/track × 7000 = ~70 GB (4 stems × 30s × 44.1kHz × 2ch × int16).

Usage:
    python scripts/batch_demucs_separate.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir /mnt/storage/blinky-ml-data/stems \
        --device cuda

    # Resume (skips tracks with existing stems)
    python scripts/batch_demucs_separate.py \
        --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
        --output-dir /mnt/storage/blinky-ml-data/stems \
        --device cuda
"""

import argparse
import sys
import time
from pathlib import Path

import torch
import torchaudio
from tqdm import tqdm


def find_audio_files(audio_dir: Path) -> list[Path]:
    """Find all audio files in directory."""
    exts = {".mp3", ".wav", ".flac", ".ogg", ".m4a"}
    files = []
    for ext in exts:
        files.extend(audio_dir.glob(f"*{ext}"))
    return sorted(files)


def separate_track(
    audio_path: Path,
    output_dir: Path,
    separator,
    device: str,
) -> bool:
    """Separate one track into 4 stems and save as WAV files.

    Returns True if separation was performed, False if skipped (already exists).
    """
    # Use htdemucs/ subdirectory to match allin1's expected path structure.
    # allin1.demix() looks for {demix_dir}/htdemucs/{track_stem}/{stem}.wav
    stem_dir = output_dir / "htdemucs" / audio_path.stem
    stems_exist = all(
        (stem_dir / f"{s}.wav").exists()
        for s in separator.sources
    )
    if stems_exist:
        return False

    stem_dir.mkdir(parents=True, exist_ok=True)
    sr_model = separator.samplerate

    # Load and prepare audio
    wav, sr = torchaudio.load(str(audio_path))
    if wav.shape[0] == 1:
        wav = wav.repeat(2, 1)  # mono → stereo
    if sr != sr_model:
        wav = torchaudio.functional.resample(wav, sr, sr_model)

    # Separate
    from demucs.apply import apply_model
    with torch.no_grad():
        sources = apply_model(separator, wav.unsqueeze(0).to(device))
    # sources: (1, n_sources, channels, samples)

    # Save all stems
    for src_name in separator.sources:
        src_idx = separator.sources.index(src_name)
        src_wav = sources[0, src_idx].cpu()
        torchaudio.save(
            str(stem_dir / f"{src_name}.wav"),
            src_wav,
            sr_model,
        )

    # Free GPU memory to prevent fragmentation over 7000 tracks
    del sources, wav
    if torch.cuda.is_available() and str(device) != "cpu":
        torch.cuda.empty_cache()

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Batch Demucs source separation for training data augmentation",
    )
    parser.add_argument("--audio-dir", required=True,
                        help="Directory containing audio files")
    parser.add_argument("--output-dir", required=True,
                        help="Output directory for separated stems")
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu",
                        help="Device for Demucs inference (default: cuda)")
    args = parser.parse_args()

    audio_dir = Path(args.audio_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    files = find_audio_files(audio_dir)
    if not files:
        print(f"No audio files found in {audio_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(files)} audio files in {audio_dir}")
    print(f"Output: {output_dir}")
    print(f"Device: {args.device}")

    # Load HTDemucs model once
    print("Loading HTDemucs model...")
    import demucs.pretrained
    separator = demucs.pretrained.get_model("htdemucs")
    separator.to(args.device)
    separator.eval()
    print(f"Model loaded. Sources: {separator.sources}")

    # Check how many already done
    already_done = sum(
        1 for f in files
        if all(
            (output_dir / "htdemucs" / f.stem / f"{s}.wav").exists()
            for s in separator.sources
        )
    )
    if already_done > 0:
        print(f"Skipping {already_done} already-separated tracks")

    t0 = time.time()
    separated = 0
    errors = 0

    for audio_path in tqdm(files, desc="Separating"):
        try:
            did_work = separate_track(audio_path, output_dir, separator, args.device)
            if did_work:
                separated += 1
        except Exception as e:
            errors += 1
            tqdm.write(f"  ERROR {audio_path.name}: {str(e)[:100]}")

    elapsed = time.time() - t0
    print(f"\nDone: {separated} separated, {already_done} skipped, "
          f"{errors} errors in {elapsed:.0f}s "
          f"({elapsed/max(separated,1):.1f}s/track)")

    # Report disk usage
    import subprocess
    result = subprocess.run(
        ["du", "-sh", str(output_dir)],
        capture_output=True, text=True,
    )
    if result.returncode == 0:
        print(f"Disk usage: {result.stdout.strip()}")


if __name__ == "__main__":
    main()
