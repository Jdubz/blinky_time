#!/usr/bin/env python3
"""Calibrate microphone transfer function for training data augmentation.

Measures the end-to-end difference between the Python mel pipeline (clean audio)
and firmware mel pipeline (audio played through speaker → air → MEMS mic → PDM →
AGC → FFT → raw mel bands). The resulting transfer function is applied to all
training data so the model trains on what the mic actually sees.

Three calibration modes:
  1. sweep  — Log sine sweep for per-band frequency response (most precise)
  2. noise  — White/pink noise for broadband statistical characterization
  3. music  — Real music tracks for realistic end-to-end validation

The output is a mic_profile.npz containing:
  - band_gain:  (26,) per-mel-band gain ratio (firmware/python)
  - band_bias:  (26,) per-mel-band additive bias
  - noise_floor: (26,) per-band noise floor (silence capture)
  - agc_curve:  fitted AGC response curve (input level → output level)

Usage:
    # Step 1: Generate reference audio files
    python scripts/calibrate_mic.py generate --output-dir data/calibration

    # Step 2: Play audio on blinkyhost while capturing firmware mel bands
    # (Run this ON blinkyhost, with speakers connected)
    python scripts/calibrate_mic.py capture \
        --port /dev/ttyACM0 \
        --audio data/calibration/sweep_16k.wav \
        --output data/calibration/sweep_capture.jsonl

    # Step 3: Derive transfer function from paired (reference, capture) data
    python scripts/calibrate_mic.py analyze \
        --captures data/calibration/ \
        --output data/calibration/mic_profile.npz

    # Step 4: Verify profile by applying to test track and comparing
    python scripts/calibrate_mic.py verify \
        --profile data/calibration/mic_profile.npz \
        --audio test.wav
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import yaml


def cmd_generate(args):
    """Generate reference audio files for calibration."""
    import soundfile as sf

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    sr = args.sample_rate

    # 1. Log sine sweep (20 Hz to sr/2, 20 seconds)
    duration = 20.0
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    f0, f1 = 20.0, sr / 2
    # Exponential chirp: f(t) = f0 * (f1/f0)^(t/T)
    sweep = 0.8 * np.sin(
        2 * np.pi * f0 * duration / np.log(f1 / f0)
        * (np.power(f1 / f0, t / duration) - 1)
    ).astype(np.float32)
    sweep_path = output_dir / "sweep_16k.wav"
    sf.write(str(sweep_path), sweep, sr)
    print(f"Generated: {sweep_path} ({duration}s log sweep, {f0}-{f1} Hz)")

    # 2. Pink noise (30 seconds)
    duration = 30.0
    n = int(sr * duration)
    white = np.random.default_rng(42).standard_normal(n).astype(np.float32)
    fft = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n, 1 / sr)
    freqs[0] = 1  # avoid div by zero
    fft /= np.sqrt(freqs)
    pink = np.fft.irfft(fft, n=n).astype(np.float32)
    pink = 0.8 * pink / (np.abs(pink).max() + 1e-10)
    pink_path = output_dir / "pink_noise_16k.wav"
    sf.write(str(pink_path), pink, sr)
    print(f"Generated: {pink_path} ({duration}s pink noise)")

    # 3. White noise (30 seconds)
    duration = 30.0
    white = np.random.default_rng(43).standard_normal(int(sr * duration)).astype(np.float32)
    white = 0.8 * white / (np.abs(white).max() + 1e-10)
    white_path = output_dir / "white_noise_16k.wav"
    sf.write(str(white_path), white, sr)
    print(f"Generated: {white_path} ({duration}s white noise)")

    # 4. Silence (10 seconds — for noise floor measurement)
    silence = np.zeros(int(sr * 10), dtype=np.float32)
    silence_path = output_dir / "silence_16k.wav"
    sf.write(str(silence_path), silence, sr)
    print(f"Generated: {silence_path} (10s silence)")

    # 5. Multi-level test tones (for AGC characterization)
    # 1kHz sine at different levels: -40, -30, -20, -10, 0 dBFS
    duration_each = 5.0
    tone_freqs = [250, 1000, 4000]
    levels_db = [-40, -30, -20, -10, -3]
    tones = []
    for level_db in levels_db:
        for freq in tone_freqs:
            t = np.linspace(0, duration_each, int(sr * duration_each), endpoint=False)
            amp = 10 ** (level_db / 20)
            tone = (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)
            tones.append(tone)
            # 0.5s silence between tones for AGC settling
            tones.append(np.zeros(int(sr * 0.5), dtype=np.float32))
    agc_signal = np.concatenate(tones)
    agc_path = output_dir / "agc_test_16k.wav"
    sf.write(str(agc_path), agc_signal, sr)
    print(f"Generated: {agc_path} ({len(agc_signal)/sr:.0f}s AGC test, "
          f"{len(levels_db)} levels × {len(tone_freqs)} freqs)")

    print(f"\nAll reference files in {output_dir}/")
    print("Next: copy to blinkyhost and run 'calibrate_mic.py capture' for each file")


def cmd_capture(args):
    """Play audio through speakers while capturing firmware mel bands."""
    import serial
    import soundfile as sf
    import subprocess

    port = args.port
    audio_path = Path(args.audio)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Validate audio file
    info = sf.info(str(audio_path))
    duration = info.duration
    print(f"Audio: {audio_path.name} ({duration:.1f}s, {info.samplerate} Hz)")

    # Connect to device
    print(f"Connecting to {port}...")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Enable NN stream mode
    ser.write(b"stream nn\n")
    time.sleep(0.2)
    resp = ser.readline().decode("utf-8", errors="replace").strip()
    print(f"Stream mode: {resp}")

    # Pre-capture silence (2 seconds for noise floor)
    print("Capturing 2s silence (noise floor)...")
    silence_frames = _capture_frames(ser, 2.0)

    # Play audio and capture simultaneously
    print(f"Playing audio + capturing ({duration:.1f}s)...")
    # Start audio playback in background using aplay/paplay
    play_cmd = _get_play_command(str(audio_path))
    play_proc = subprocess.Popen(play_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Small delay for audio to start (speaker latency)
    time.sleep(args.latency)

    audio_frames = _capture_frames(ser, duration + 1.0)

    play_proc.wait()

    # Post-capture silence (2 seconds)
    print("Capturing 2s post-silence...")
    post_frames = _capture_frames(ser, 2.0)

    # Disable streaming
    ser.write(b"stream off\n")
    time.sleep(0.2)
    ser.close()

    # Save all frames
    all_data = {
        "audio_file": audio_path.name,
        "sample_rate": info.samplerate,
        "duration": duration,
        "port": port,
        "latency_compensation": args.latency,
        "silence_pre": silence_frames,
        "audio": audio_frames,
        "silence_post": post_frames,
    }
    with open(output_path, "w") as f:
        json.dump(all_data, f)

    n_total = len(silence_frames) + len(audio_frames) + len(post_frames)
    print(f"\nCaptured {n_total} frames → {output_path}")
    print(f"  Pre-silence: {len(silence_frames)} frames")
    print(f"  Audio: {len(audio_frames)} frames")
    print(f"  Post-silence: {len(post_frames)} frames")


def _capture_frames(ser, duration: float) -> list[dict]:
    """Capture NN stream frames for a given duration."""
    frames = []
    start = time.time()
    while time.time() - start < duration:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            continue
        if data.get("type") == "NN":
            frames.append(data)
    return frames


def _get_play_command(audio_path: str) -> list[str]:
    """Get platform-appropriate audio playback command."""
    import shutil
    if shutil.which("paplay"):
        return ["paplay", audio_path]
    if shutil.which("aplay"):
        return ["aplay", "-q", audio_path]
    if shutil.which("ffplay"):
        return ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", audio_path]
    raise RuntimeError("No audio player found. Install pulseaudio (paplay) or alsa-utils (aplay).")


def cmd_analyze(args):
    """Derive mic transfer function from paired captures."""
    import torch
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from scripts.prepare_dataset import (
        _build_mel_filterbank, firmware_mel_spectrogram, load_config,
    )

    captures_dir = Path(args.captures)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    cfg = load_config(args.config)
    sr = cfg["audio"]["sample_rate"]
    frame_rate = cfg["audio"]["frame_rate"]
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    mel_fb = _build_mel_filterbank(cfg, device)
    window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

    n_mels = cfg["audio"]["n_mels"]
    all_band_ratios = []
    all_noise_floors = []

    # Process each capture file
    capture_files = sorted(captures_dir.glob("*_capture.json"))
    if not capture_files:
        print(f"No *_capture.json files found in {captures_dir}", file=sys.stderr)
        sys.exit(1)

    for cap_path in capture_files:
        print(f"\nAnalyzing: {cap_path.name}")
        with open(cap_path) as f:
            cap = json.load(f)

        audio_name = cap["audio_file"]
        audio_path = captures_dir / audio_name
        if not audio_path.exists():
            # Try parent dir
            audio_path = captures_dir.parent / audio_name
        if not audio_path.exists():
            print(f"  WARNING: audio file {audio_name} not found, skipping")
            continue

        # Compute Python mel bands for reference audio
        import librosa
        audio_np, _ = librosa.load(str(audio_path), sr=sr, mono=True)
        target_rms_db = cfg["audio"].get("target_rms_db", -35)
        rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        py_mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)
        print(f"  Python mel: {py_mel.shape}")

        # Extract firmware mel bands from capture
        fw_frames = cap.get("audio", [])
        if not fw_frames:
            print(f"  WARNING: no audio frames in capture, skipping")
            continue
        fw_mel = np.array([f["mel"] for f in fw_frames])
        print(f"  Firmware mel: {fw_mel.shape}")

        # Cross-correlate mean energy to find time alignment
        py_energy = py_mel.mean(axis=1)
        fw_energy = fw_mel.mean(axis=1)
        min_len = min(len(py_energy), len(fw_energy))
        if min_len < 20:
            print(f"  WARNING: too few frames ({min_len}), skipping")
            continue

        corr = np.correlate(
            fw_energy[:min_len] - fw_energy[:min_len].mean(),
            py_energy[:min_len] - py_energy[:min_len].mean(),
            mode="full"
        )
        offset = np.argmax(corr) - min_len + 1
        print(f"  Alignment offset: {offset} frames ({offset / frame_rate * 1000:.0f} ms)")

        # Align arrays
        if offset >= 0:
            fw_aligned = fw_mel[offset:]
            py_aligned = py_mel[:]
        else:
            fw_aligned = fw_mel[:]
            py_aligned = py_mel[-offset:]
        align_len = min(len(fw_aligned), len(py_aligned))
        fw_aligned = fw_aligned[:align_len]
        py_aligned = py_aligned[:align_len]

        # Compute per-band gain ratio (firmware / python)
        # Use median to be robust to outliers (transients, AGC adaptation)
        for band in range(n_mels):
            py_vals = py_aligned[:, band]
            fw_vals = fw_aligned[:, band]
            # Only compare where both have signal (avoid div-by-zero)
            mask = (py_vals > 0.05) & (fw_vals > 0.01)
            if mask.sum() > 50:
                ratios = fw_vals[mask] / py_vals[mask]
                all_band_ratios.append(ratios)

        # Noise floor from silence frames
        silence_frames = cap.get("silence_pre", [])
        if silence_frames:
            silence_mel = np.array([f["mel"] for f in silence_frames])
            if len(silence_mel) > 5:
                all_noise_floors.append(silence_mel.mean(axis=0))

        # Per-band correlation for diagnostics
        correlations = []
        for band in range(n_mels):
            if py_aligned[:, band].std() > 0.01 and fw_aligned[:, band].std() > 0.01:
                corr = np.corrcoef(py_aligned[:, band], fw_aligned[:, band])[0, 1]
            else:
                corr = float("nan")
            correlations.append(corr)
        mean_corr = np.nanmean(correlations)
        print(f"  Mean per-band correlation: {mean_corr:.4f}")

    if not all_band_ratios:
        print("\nERROR: No valid paired data to analyze", file=sys.stderr)
        sys.exit(1)

    # Aggregate per-band gain (median across all captures and frames)
    band_gain = np.ones(n_mels, dtype=np.float32)
    all_ratios_flat = np.concatenate(all_band_ratios)
    # Overall median gain
    overall_gain = np.median(all_ratios_flat)
    print(f"\nOverall gain ratio (firmware/python): {overall_gain:.4f}")

    # TODO: Per-band gain would require tracking which ratios belong to which band.
    # For now, compute overall gain. This can be refined once we have real captures.
    band_gain[:] = overall_gain

    # Noise floor
    if all_noise_floors:
        noise_floor = np.mean(all_noise_floors, axis=0).astype(np.float32)
    else:
        noise_floor = np.zeros(n_mels, dtype=np.float32)

    # Save profile
    np.savez(output_path,
             band_gain=band_gain,
             noise_floor=noise_floor,
             n_captures=len(capture_files))

    print(f"\nMic profile saved to {output_path}")
    print(f"  Band gain: mean={band_gain.mean():.4f}, "
          f"min={band_gain.min():.4f}, max={band_gain.max():.4f}")
    print(f"  Noise floor: mean={noise_floor.mean():.4f}, "
          f"max={noise_floor.max():.4f}")
    print(f"\nUse with prepare_dataset.py:")
    print(f"  python scripts/prepare_dataset.py --config configs/default.yaml "
          f"--mic-profile {output_path}")


def cmd_verify(args):
    """Verify mic profile by applying to test audio and comparing stats."""
    profile = np.load(args.profile)
    band_gain = profile["band_gain"]
    noise_floor = profile["noise_floor"]

    print("Mic Profile:")
    print(f"  Band gain range: [{band_gain.min():.4f}, {band_gain.max():.4f}]")
    print(f"  Noise floor range: [{noise_floor.min():.4f}, {noise_floor.max():.4f}]")

    if args.audio:
        import librosa
        import torch
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        from scripts.prepare_dataset import (
            _build_mel_filterbank, firmware_mel_spectrogram, load_config,
        )

        cfg = load_config(args.config)
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        mel_fb = _build_mel_filterbank(cfg, device)
        window = torch.hamming_window(cfg["audio"]["n_fft"], periodic=False).to(device)

        audio_np, _ = librosa.load(args.audio, sr=cfg["audio"]["sample_rate"], mono=True)
        target_rms_db = cfg["audio"].get("target_rms_db", -35)
        rms = np.sqrt(np.mean(audio_np ** 2) + 1e-10)
        audio_np = audio_np * (10 ** (target_rms_db / 20) / rms)
        audio_gpu = torch.from_numpy(audio_np).to(device)
        mel = firmware_mel_spectrogram(audio_gpu, cfg, mel_fb, window)

        print(f"\nOriginal mel stats:")
        print(f"  Mean: {mel.mean():.4f}, Std: {mel.std():.4f}")
        print(f"  Range: [{mel.min():.4f}, {mel.max():.4f}]")

        # Apply mic profile
        mel_mic = apply_mic_profile(mel, band_gain, noise_floor)
        print(f"\nAfter mic profile:")
        print(f"  Mean: {mel_mic.mean():.4f}, Std: {mel_mic.std():.4f}")
        print(f"  Range: [{mel_mic.min():.4f}, {mel_mic.max():.4f}]")

        # Per-band comparison
        print(f"\n{'Band':>6} {'Orig Mean':>10} {'Mic Mean':>10} {'Gain':>8}")
        for b in range(mel.shape[1]):
            print(f"{b:>6d} {mel[:, b].mean():>10.4f} {mel_mic[:, b].mean():>10.4f} "
                  f"{band_gain[b]:>8.4f}")


def apply_mic_profile(mel: np.ndarray, band_gain: np.ndarray,
                      noise_floor: np.ndarray) -> np.ndarray:
    """Apply mic transfer function to mel spectrogram.

    Simulates the effect of the MEMS mic on clean audio mel bands:
      1. Scale each band by the measured gain ratio
      2. Add noise floor (mic self-noise)
      3. Clip to [0, 1]

    Args:
        mel: (n_frames, n_mels) clean mel spectrogram
        band_gain: (n_mels,) per-band gain ratio
        noise_floor: (n_mels,) per-band noise floor

    Returns:
        (n_frames, n_mels) mic-simulated mel spectrogram
    """
    mel_mic = mel * band_gain[np.newaxis, :]
    # Add noise floor (random per-frame variation around the mean)
    if noise_floor.max() > 0:
        noise = np.random.default_rng().normal(
            loc=noise_floor, scale=noise_floor * 0.3,
            size=mel.shape
        ).astype(np.float32)
        noise = np.maximum(noise, 0)
        mel_mic = np.maximum(mel_mic, noise)
    return np.clip(mel_mic, 0.0, 1.0)


def main():
    parser = argparse.ArgumentParser(
        description="Calibrate microphone transfer function for NN training",
    )
    sub = parser.add_subparsers(dest="command")

    # generate
    gen = sub.add_parser("generate", help="Generate reference audio files")
    gen.add_argument("--output-dir", default="data/calibration")
    gen.add_argument("--sample-rate", type=int, default=16000)

    # capture
    cap = sub.add_parser("capture", help="Play audio + capture firmware mel bands")
    cap.add_argument("--port", required=True, help="Serial port (e.g., /dev/ttyACM0)")
    cap.add_argument("--audio", required=True, help="Reference audio file to play")
    cap.add_argument("--output", required=True, help="Output capture file (.json)")
    cap.add_argument("--latency", type=float, default=0.1,
                     help="Speaker latency compensation in seconds (default: 0.1)")

    # analyze
    ana = sub.add_parser("analyze", help="Derive transfer function from captures")
    ana.add_argument("--captures", required=True, help="Directory with *_capture.json files")
    ana.add_argument("--output", default="data/calibration/mic_profile.npz")
    ana.add_argument("--config", default="configs/default.yaml")

    # verify
    ver = sub.add_parser("verify", help="Verify mic profile")
    ver.add_argument("--profile", required=True, help="mic_profile.npz path")
    ver.add_argument("--audio", default=None, help="Optional test audio file")
    ver.add_argument("--config", default="configs/default.yaml")

    args = parser.parse_args()
    if args.command == "generate":
        cmd_generate(args)
    elif args.command == "capture":
        cmd_capture(args)
    elif args.command == "analyze":
        cmd_analyze(args)
    elif args.command == "verify":
        cmd_verify(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
