#!/usr/bin/env python3
"""Download free room impulse responses for acoustic environment augmentation.

Sources:
  - OpenAIR: Large venue RIRs (concert halls, churches, tunnels, warehouses)
  - EchoThief: 115 diverse real-world impulse responses (CC0)
  - MIT IR Survey: Various room types

For loud venue simulation, we want:
  - Large reverberant spaces (warehouses, arenas)
  - Outdoor spaces (minimal reverb, crowd noise)
  - Clubs/small venues (short RT60, bass-heavy)

Usage:
    python scripts/download_rir.py --output-dir /mnt/storage/blinky-ml-data/rir
"""

import argparse
import subprocess
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

# EchoThief: 115 free RIRs from real spaces, CC0 license
ECHOTHIEF_URL = "http://www.echothief.com/wp-content/uploads/2016/06/EchoThiefImpulseResponseLibrary.zip"


def download_echothief(output_dir: Path):
    """Download EchoThief impulse response library."""
    zip_path = output_dir / "echothief.zip"
    echothief_dir = output_dir / "echothief"

    if echothief_dir.exists() and any(echothief_dir.rglob("*.wav")):
        n = len(list(echothief_dir.rglob("*.wav")))
        print(f"  EchoThief already downloaded ({n} RIRs)")
        return

    print("  Downloading EchoThief IR library...")
    subprocess.run(["curl", "-L", "-o", str(zip_path), "--progress-bar", ECHOTHIEF_URL], check=True)

    print("  Extracting...")
    import zipfile
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(echothief_dir)

    n = len(list(echothief_dir.rglob("*.wav")))
    print(f"  Extracted {n} impulse responses")


def generate_synthetic_rir(sr: int, rt60: float, room_type: str,
                           rng: np.random.Generator) -> np.ndarray:
    """Generate a simple synthetic RIR using exponential decay + early reflections.

    This is a rough approximation — real RIRs are better, but these cover
    parameter ranges not in the EchoThief set.
    """
    n_samples = int(sr * rt60 * 1.5)

    # Direct sound
    rir = np.zeros(n_samples, dtype=np.float32)
    rir[0] = 1.0

    # Early reflections (5-50ms)
    n_reflections = rng.integers(3, 12)
    for _ in range(n_reflections):
        delay = rng.integers(int(sr * 0.005), int(sr * 0.05))
        amplitude = rng.uniform(0.1, 0.6)
        if delay < n_samples:
            rir[delay] += amplitude * rng.choice([-1, 1])

    # Late reverb (exponential decay noise)
    decay = np.exp(-6.9 * np.arange(n_samples) / (rt60 * sr))
    late_reverb = rng.standard_normal(n_samples).astype(np.float32) * decay
    # Start late reverb after early reflections
    late_start = int(sr * 0.05)
    rir[late_start:] += late_reverb[late_start:] * 0.3

    # Room-type specific adjustments
    if room_type == "warehouse":
        # Strong low-frequency resonance
        from scipy.signal import butter, sosfilt
        sos = butter(2, 300, btype="low", fs=sr, output="sos")
        bass_rir = sosfilt(sos, rir)
        rir = rir + bass_rir * 0.5
    elif room_type == "outdoor":
        # Very short RT60, minimal reverb
        rir[int(sr * 0.1):] *= 0.1
    elif room_type == "arena":
        # Extra long decay, diffuse
        pass

    return rir / (np.max(np.abs(rir)) + 1e-10)


def main():
    parser = argparse.ArgumentParser(description="Download/generate room impulse responses")
    parser.add_argument("--output-dir", default="/mnt/storage/blinky-ml-data/rir")
    parser.add_argument("--sr", type=int, default=16000, help="Target sample rate")
    parser.add_argument("--skip-download", action="store_true", help="Only generate synthetic RIRs")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Download real RIRs
    if not args.skip_download:
        print("Downloading real room impulse responses...")
        try:
            download_echothief(output_dir)
        except Exception as e:
            print(f"  Warning: EchoThief download failed ({e})")
            print("  Continuing with synthetic RIRs only. You can manually download")
            print("  RIRs from https://www.openair.hosted.york.ac.uk/ and place .wav files")
            print(f"  in {output_dir}/echothief/")

    # Resample all downloaded RIRs to target SR and save as .npy
    processed_dir = output_dir / "processed"
    processed_dir.mkdir(exist_ok=True)

    real_rir_dirs = [output_dir / "echothief"]
    n_processed = 0
    for rir_dir in real_rir_dirs:
        if not rir_dir.exists():
            continue
        for wav_path in sorted(rir_dir.rglob("*.wav")):
            npy_path = processed_dir / f"{wav_path.stem}.npy"
            if npy_path.exists():
                n_processed += 1
                continue
            try:
                rir, _ = librosa.load(str(wav_path), sr=args.sr, mono=True)
                # Trim silence from end
                end = np.max(np.where(np.abs(rir) > 0.001)[0]) + int(args.sr * 0.1)
                rir = rir[:min(end, len(rir))]
                np.save(npy_path, rir.astype(np.float32))
                n_processed += 1
            except Exception as e:
                print(f"  Warning: couldn't process {wav_path.name}: {e}")

    print(f"  {n_processed} real RIRs processed to {processed_dir}")

    # Generate synthetic RIRs for environments not well-represented
    print("\nGenerating synthetic RIRs...")
    rng = np.random.default_rng(42)

    synthetic_configs = [
        # (name, rt60, room_type)
        ("warehouse-small", 1.5, "warehouse"),
        ("warehouse-large", 3.0, "warehouse"),
        ("warehouse-huge", 5.0, "warehouse"),
        ("outdoor-open", 0.1, "outdoor"),
        ("outdoor-canopy", 0.3, "outdoor"),
        ("arena-small", 2.0, "arena"),
        ("arena-large", 4.0, "arena"),
        ("club-tight", 0.4, "club"),
        ("club-medium", 0.8, "club"),
        ("festival-tent", 1.2, "warehouse"),
    ]

    for name, rt60, room_type in synthetic_configs:
        npy_path = processed_dir / f"synthetic-{name}.npy"
        if npy_path.exists():
            continue
        rir = generate_synthetic_rir(args.sr, rt60, room_type, rng)
        np.save(npy_path, rir)
        print(f"  Generated: {name} (RT60={rt60}s, type={room_type})")

    total = len(list(processed_dir.glob("*.npy")))
    print(f"\nTotal RIRs available: {total} in {processed_dir}")
    print("Use with: python scripts/prepare_dataset.py --augment --rir-dir " + str(processed_dir))


if __name__ == "__main__":
    main()
