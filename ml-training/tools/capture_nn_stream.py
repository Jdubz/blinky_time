#!/usr/bin/env python3
"""Capture NN diagnostic stream from firmware for offline validation.

Connects to a blinky device via serial, enables `stream nn` mode, and records
the raw mel bands + onset/NN output at full frame rate (~62.5 Hz). The captured
data can be compared against the training pipeline to verify feature parity.

Usage:
    # Basic capture (30 seconds)
    python scripts/capture_nn_stream.py /dev/ttyACM0

    # Longer capture with output file
    python scripts/capture_nn_stream.py /dev/ttyACM0 --duration 120 --output capture.jsonl

    # Capture while playing test pattern (manual: play audio near mic)
    python scripts/capture_nn_stream.py /dev/ttyACM0 --duration 60 --output steady-120bpm.jsonl

    # Compare captured mel bands vs training pipeline
    python scripts/capture_nn_stream.py --compare capture.jsonl reference.wav

SSH to blinkyhost:
    ssh blinkyhost.local "cd ~/Development/blinky_time/ml-training && python3 scripts/capture_nn_stream.py /dev/ttyACM0"
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np


def capture(port: str, duration: float, output: str, baud: int = 115200):
    """Capture NN diagnostic stream from device."""
    import serial

    print(f"Connecting to {port} at {baud} baud...")
    ser = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)  # Wait for connection

    # Drain any buffered data
    ser.reset_input_buffer()

    # Enable NN stream mode
    ser.write(b"stream nn\n")
    time.sleep(0.2)
    response = ser.readline().decode("utf-8", errors="replace").strip()
    if "OK" not in response:
        print(f"Warning: unexpected response to 'stream nn': {response}")

    print(f"Capturing for {duration}s → {output}")
    frames = []
    start = time.time()
    frame_count = 0
    errors = 0

    try:
        while time.time() - start < duration:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue

            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                errors += 1
                continue

            if data.get("type") != "NN":
                continue

            frames.append(data)
            frame_count += 1

            if frame_count % 100 == 0:
                elapsed = time.time() - start
                rate = frame_count / elapsed
                print(f"  {frame_count} frames ({rate:.1f} Hz), "
                      f"{elapsed:.0f}s / {duration:.0f}s", end="\r")

    except KeyboardInterrupt:
        print("\nCapture interrupted by user")
    finally:
        # Disable streaming
        ser.write(b"stream off\n")
        time.sleep(0.2)
        ser.close()

    elapsed = time.time() - start
    print(f"\nCaptured {frame_count} frames in {elapsed:.1f}s "
          f"({frame_count / elapsed:.1f} Hz avg), {errors} parse errors")

    # Save as JSONL (one JSON object per line)
    with open(output, "w") as f:
        for frame in frames:
            f.write(json.dumps(frame) + "\n")
    print(f"Saved to {output}")

    # Summary statistics
    if frames:
        mels = np.array([f["mel"] for f in frames])
        onsets = np.array([f.get("onset", 0) for f in frames])
        print(f"\nMel band stats:")
        print(f"  Shape: {mels.shape}")
        print(f"  Range: [{mels.min():.4f}, {mels.max():.4f}]")
        print(f"  Mean:  {mels.mean():.4f}")
        print(f"  Bands with signal: {(mels.mean(axis=0) > 0.01).sum()}/26")
        print(f"\nOnset stats:")
        print(f"  Range: [{onsets.min():.4f}, {onsets.max():.4f}]")
        print(f"  Mean:  {onsets.mean():.4f}")
        print(f"  Detections (>0.3): {(onsets > 0.3).sum()}")

        nn_active = frames[0].get("nn", 0)
        print(f"\nNN status: {'active' if nn_active else 'not loaded (using BandFlux ODF)'}")


def compare(capture_file: str, audio_file: str, config: str):
    """Compare captured firmware mel bands vs training pipeline output."""
    import librosa
    import torch
    import yaml

    from prepare_dataset import _build_mel_filterbank, firmware_mel_spectrogram, load_config

    cfg = load_config(config)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # Load capture
    print(f"Loading capture: {capture_file}")
    frames = []
    with open(capture_file) as f:
        for line in f:
            line = line.strip()
            if line:
                frames.append(json.loads(line))

    fw_mels = np.array([f["mel"] for f in frames if f.get("type") == "NN"])
    print(f"  Firmware frames: {fw_mels.shape}")

    # Run training pipeline on reference audio
    print(f"Processing reference: {audio_file}")
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    audio_np, _ = librosa.load(audio_file, sr=cfg["audio"]["sample_rate"], mono=True)
    target_rms_db = cfg["audio"].get("target_rms_db", -35)
    rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
    audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
    audio_gpu = torch.from_numpy(audio_np).to(device)
    train_mels = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)
    print(f"  Training frames: {train_mels.shape}")

    # Align by length (firmware capture may start/end at different points)
    min_frames = min(fw_mels.shape[0], train_mels.shape[0])
    if min_frames < 10:
        print("ERROR: Too few frames to compare. Ensure audio was playing during capture.")
        sys.exit(1)

    # Cross-correlate to find time offset
    fw_energy = fw_mels[:, :].mean(axis=1)
    tr_energy = train_mels[:, :].mean(axis=1)
    correlation = np.correlate(
        fw_energy - fw_energy.mean(),
        tr_energy - tr_energy.mean(),
        mode="full"
    )
    offset = np.argmax(correlation) - len(fw_energy) + 1
    print(f"  Best alignment offset: {offset} frames ({offset / 62.5 * 1000:.0f} ms)")

    # Align
    if offset >= 0:
        fw_aligned = fw_mels[offset:offset + min_frames]
        tr_aligned = train_mels[:min_frames]
    else:
        fw_aligned = fw_mels[:min_frames]
        tr_aligned = train_mels[-offset:-offset + min_frames]

    compare_len = min(len(fw_aligned), len(tr_aligned))
    fw_aligned = fw_aligned[:compare_len]
    tr_aligned = tr_aligned[:compare_len]

    # Per-band comparison
    print(f"\nPer-band comparison ({compare_len} frames):")
    print(f"{'Band':>6} {'MaxDiff':>10} {'MeanDiff':>10} {'Corr':>10}")
    for band in range(26):
        diff = np.abs(fw_aligned[:, band] - tr_aligned[:, band])
        if fw_aligned[:, band].std() > 0.01 and tr_aligned[:, band].std() > 0.01:
            corr = np.corrcoef(fw_aligned[:, band], tr_aligned[:, band])[0, 1]
        else:
            corr = float("nan")
        print(f"{band:>6d} {diff.max():>10.4f} {diff.mean():>10.4f} {corr:>10.4f}")

    # Overall
    all_diff = np.abs(fw_aligned - tr_aligned)
    mask = (fw_aligned.std(axis=0) > 0.01) & (tr_aligned.std(axis=0) > 0.01)
    active_bands = mask.sum()
    if active_bands > 0:
        active_corrs = []
        for b in range(26):
            if mask[b]:
                active_corrs.append(
                    np.corrcoef(fw_aligned[:, b], tr_aligned[:, b])[0, 1])
        mean_corr = np.mean(active_corrs)
    else:
        mean_corr = float("nan")

    print(f"\nOverall:")
    print(f"  Max diff:  {all_diff.max():.4f}")
    print(f"  Mean diff: {all_diff.mean():.4f}")
    print(f"  Active bands: {active_bands}/26")
    print(f"  Mean correlation (active bands): {mean_corr:.4f}")

    if mean_corr > 0.95:
        print("\n  PASS: Firmware and training pipeline mel bands match well")
    elif mean_corr > 0.85:
        print("\n  WARNING: Moderate mismatch — check filterbank or log compression")
    else:
        print("\n  FAIL: Significant mismatch — investigate pipeline differences")


def main():
    parser = argparse.ArgumentParser(
        description="Capture and validate NN diagnostic stream from firmware")
    sub = parser.add_subparsers(dest="command")

    # Capture subcommand (default if port given as positional)
    cap = sub.add_parser("capture", help="Record NN stream from device")
    cap.add_argument("port", help="Serial port (e.g., /dev/ttyACM0)")
    cap.add_argument("--duration", type=float, default=30, help="Capture duration in seconds")
    cap.add_argument("--output", "-o", default="nn_capture.jsonl", help="Output file")
    cap.add_argument("--baud", type=int, default=115200, help="Baud rate")

    # Compare subcommand
    cmp = sub.add_parser("compare", help="Compare capture vs training pipeline")
    cmp.add_argument("capture_file", help="Captured .jsonl file")
    cmp.add_argument("audio_file", help="Reference audio file (.wav/.mp3)")
    cmp.add_argument("--config", default="configs/default.yaml", help="Training config")

    # Allow positional port without subcommand for convenience
    args, remaining = parser.parse_known_args()

    if args.command == "compare":
        compare(args.capture_file, args.audio_file, args.config)
    elif args.command == "capture":
        capture(args.port, args.duration, args.output, args.baud)
    elif args.command is None and remaining:
        # Treat first positional as port for capture
        parser2 = argparse.ArgumentParser()
        parser2.add_argument("port")
        parser2.add_argument("--duration", type=float, default=30)
        parser2.add_argument("--output", "-o", default="nn_capture.jsonl")
        parser2.add_argument("--baud", type=int, default=115200)
        args2 = parser2.parse_args(remaining)
        capture(args2.port, args2.duration, args2.output, args2.baud)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
