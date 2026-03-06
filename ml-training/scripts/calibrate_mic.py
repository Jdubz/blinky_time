#!/usr/bin/env python3
"""Capture mic transfer function for ML training data augmentation.

Plays reference signals (frequency sweeps, white/pink noise, music) through
speakers, captures what the device mic hears via `stream nn`, and compares
against the theoretical mel bands of the original audio to derive a per-band
transfer function (gain + noise floor).

The resulting mic_profile.npz is consumed by prepare_dataset.py --mic-profile
to transform clean mel spectrograms into realistic mic-captured equivalents.

Dependencies:
    - numpy (required)
    - librosa (for mel filterbank computation)
    - scipy (for audio I/O and windowing)
    - pyserial (for device communication)
    - ffplay/ffprobe (for audio playback and duration detection)

Subcommands:
    generate    - Create reference audio files (sweep, white, pink, silence)
    capture     - Play audio through speakers + record device mel bands
    capture-all - Capture all reference signals on all ports
    gain-sweep  - Measure noise floor + signal at each HW gain level
    analyze     - Compare captures vs originals -> mic_profile.npz

Usage:
    # 1. Generate reference signals
    python scripts/calibrate_mic.py generate --output-dir data/calibration

    # 2. Capture each signal (run on host with speakers + devices)
    python scripts/calibrate_mic.py capture --port /dev/ttyACM0 \
        --audio data/calibration/sweep_16k.wav \
        --output data/calibration/sweep_capture_ACM0.jsonl

    # 2b. Capture all signals on all ports at once
    python scripts/calibrate_mic.py capture-all \
        --ports /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2 \
        --audio-dir data/calibration \
        --output-dir data/calibration

    # 3. Sweep HW gain levels to characterize noise floor vs gain
    python scripts/calibrate_mic.py gain-sweep --port /dev/ttyACM0 \
        --output data/calibration/gain_sweep_ACM0.npz

    # 3b. With reference audio to also measure signal quality at each gain
    python scripts/calibrate_mic.py gain-sweep --port /dev/ttyACM0 \
        --audio data/calibration/pink_noise_16k.wav \
        --output data/calibration/gain_sweep_ACM0.npz

    # 4. Analyze captures vs originals
    python scripts/calibrate_mic.py analyze \
        --captures data/calibration \
        --output data/calibration/mic_profile.npz
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import shutil

import numpy as np


def _check_imports(*modules):
    """Check that required Python modules are importable."""
    for mod in modules:
        try:
            __import__(mod)
        except ImportError:
            print(f"ERROR: {mod} is required. Install with: pip install {mod}", file=sys.stderr)
            sys.exit(1)


def _check_ffplay():
    """Check that ffplay is available on PATH."""
    if shutil.which("ffplay") is None:
        print("ERROR: ffplay not found. Install ffmpeg (includes ffplay).", file=sys.stderr)
        sys.exit(1)


def _check_ffprobe():
    """Check that ffprobe is available on PATH."""
    if shutil.which("ffprobe") is None:
        print("ERROR: ffprobe not found. Install ffmpeg (includes ffprobe).", file=sys.stderr)
        sys.exit(1)


# Must match firmware SharedSpectralAnalysis exactly
SAMPLE_RATE = 16000
N_FFT = 256
HOP_LENGTH = 256
N_MELS = 26
FMIN = 60
FMAX = 8000
FRAME_RATE = SAMPLE_RATE / HOP_LENGTH  # 62.5 Hz

# Capture timing
SETTLE_SECONDS = 3.0  # AGC settle time before recording
POST_SILENCE_SECONDS = 1.0  # Silence after audio ends


def _build_mel_filterbank() -> np.ndarray:
    """Build mel filterbank matching firmware (librosa HTK, no norm)."""
    import librosa
    return librosa.filters.mel(
        sr=SAMPLE_RATE, n_fft=N_FFT, n_mels=N_MELS,
        fmin=FMIN, fmax=FMAX, htk=True, norm=None,
    ).astype(np.float32)


def _firmware_mel_from_audio(audio: np.ndarray) -> np.ndarray:
    """Compute mel spectrogram matching firmware pipeline exactly.

    Returns (n_frames, 26) array with values in [0, 1].

    NOTE: Must stay in sync with firmware_mel_spectrogram() in prepare_dataset.py.
    Both implement: Hamming window → FFT-256 → HTK mel (26 bands, 60-8000 Hz) →
    10*log10(x+1e-10) → [-60,0] dB → [0,1]. This copy exists to avoid importing
    torch/config dependencies into the calibration tool.
    """
    from scipy.signal.windows import hamming

    window = hamming(N_FFT, sym=True).astype(np.float32)
    mel_fb = _build_mel_filterbank()

    n_frames = len(audio) // HOP_LENGTH
    mels = np.zeros((n_frames, N_MELS), dtype=np.float32)

    for i in range(n_frames):
        frame = audio[i * HOP_LENGTH:(i * HOP_LENGTH) + N_FFT]
        if len(frame) < N_FFT:
            break
        windowed = frame * window
        spectrum = np.abs(np.fft.rfft(windowed))
        mel_spec = mel_fb @ spectrum
        log_mel = 10.0 * np.log10(mel_spec + 1e-10)
        log_mel = (log_mel + 60.0) / 60.0
        mels[i] = np.clip(log_mel, 0.0, 1.0)

    return mels


def generate(output_dir: str, duration: float = 10.0):
    """Generate reference calibration audio files."""
    _check_imports("scipy", "librosa")
    from scipy.io import wavfile
    from scipy.signal import chirp

    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)

    n_samples = int(SAMPLE_RATE * duration)
    t = np.linspace(0, duration, n_samples, endpoint=False)

    # 1. Logarithmic frequency sweep (20 Hz -> 8000 Hz)
    sweep = chirp(t, f0=20, f1=8000, t1=duration, method='logarithmic')
    sweep = (sweep * 0.8 * 32767).astype(np.int16)
    path = out / "sweep_16k.wav"
    wavfile.write(str(path), SAMPLE_RATE, sweep)
    print(f"  Generated: {path} ({duration}s, 20-8000 Hz log sweep)")

    # 2. White noise
    rng = np.random.default_rng(42)
    white = rng.standard_normal(n_samples)
    white = (white / np.abs(white).max() * 0.8 * 32767).astype(np.int16)
    path = out / "white_noise_16k.wav"
    wavfile.write(str(path), SAMPLE_RATE, white)
    print(f"  Generated: {path} ({duration}s)")

    # 3. Pink noise (1/f spectrum)
    white_f = np.fft.rfft(rng.standard_normal(n_samples))
    freqs = np.fft.rfftfreq(n_samples, d=1 / SAMPLE_RATE)
    freqs[0] = 1  # avoid div by zero
    pink_f = white_f / np.sqrt(freqs)
    pink = np.fft.irfft(pink_f, n=n_samples)
    pink = (pink / np.abs(pink).max() * 0.8 * 32767).astype(np.int16)
    path = out / "pink_noise_16k.wav"
    wavfile.write(str(path), SAMPLE_RATE, pink)
    print(f"  Generated: {path} ({duration}s)")

    # 4. Silence (for noise floor measurement)
    silence = np.zeros(n_samples, dtype=np.int16)
    path = out / "silence_16k.wav"
    wavfile.write(str(path), SAMPLE_RATE, silence)
    print(f"  Generated: {path} ({duration}s)")

    # 5. Per-band tone bursts (pure tones at mel band center frequencies)
    # Each tone plays for 0.5s with 0.1s gaps — tests individual band response
    from scipy.signal.windows import hann

    mel_centers = _get_mel_center_freqs()
    burst_dur = 0.4
    gap_dur = 0.1
    total_dur = N_MELS * (burst_dur + gap_dur)
    n_total = int(SAMPLE_RATE * total_dur)
    tones = np.zeros(n_total, dtype=np.float64)

    for band_idx, freq in enumerate(mel_centers):
        start = int(band_idx * (burst_dur + gap_dur) * SAMPLE_RATE)
        n_burst = int(burst_dur * SAMPLE_RATE)
        t_burst = np.arange(n_burst) / SAMPLE_RATE
        tone = np.sin(2 * np.pi * freq * t_burst)
        # Apply Hann envelope to avoid clicks
        envelope = hann(n_burst, sym=True)
        tone *= envelope * 0.8
        tones[start:start + n_burst] = tone

    tones = (tones * 32767).astype(np.int16)
    path = out / "tone_bursts_16k.wav"
    wavfile.write(str(path), SAMPLE_RATE, tones)
    print(f"  Generated: {path} ({total_dur:.1f}s, {N_MELS} bands)")

    # Save mel center frequencies for reference
    np.save(out / "mel_centers.npy", mel_centers)
    print(f"\n  Mel band center frequencies (Hz):")
    for i, f in enumerate(mel_centers):
        print(f"    Band {i:2d}: {f:7.1f} Hz")

    # Also compute and save theoretical mel bands for each signal
    print(f"\nComputing theoretical mel bands for reference signals...")
    for wav_name in ["sweep_16k.wav", "white_noise_16k.wav",
                     "pink_noise_16k.wav", "silence_16k.wav",
                     "tone_bursts_16k.wav"]:
        wav_path = out / wav_name
        _, audio_int16 = wavfile.read(str(wav_path))
        audio_f32 = audio_int16.astype(np.float32) / 32768.0
        mel = _firmware_mel_from_audio(audio_f32)
        ref_path = out / f"{wav_name.replace('.wav', '_reference.npy')}"
        np.save(ref_path, mel)
        print(f"  {ref_path}: {mel.shape}")


def _get_mel_center_freqs() -> np.ndarray:
    """Get center frequencies of the 26 mel filter bands."""
    mel_fb = _build_mel_filterbank()
    freqs = np.fft.rfftfreq(N_FFT, d=1 / SAMPLE_RATE)
    centers = np.zeros(N_MELS)
    for i in range(N_MELS):
        weights = mel_fb[i]
        if weights.sum() > 0:
            centers[i] = np.average(freqs[:len(weights)], weights=weights)
    return centers


def _capture_serial(port: str, duration: float, baud: int = 115200) -> list[dict]:
    """Capture NN stream frames from device for given duration."""
    import serial

    ser = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    ser.write(b"stream nn\n")
    time.sleep(0.2)
    resp = ser.readline().decode("utf-8", errors="replace").strip()
    if "OK" not in resp:
        print(f"  Warning [{port}]: unexpected response: {resp}")

    frames = []
    start = time.time()
    errors = 0

    while time.time() - start < duration:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            errors += 1
            continue
        if data.get("type") == "NN":
            frames.append(data)

    ser.write(b"stream off\n")
    time.sleep(0.2)
    ser.close()

    elapsed = time.time() - start
    rate = len(frames) / elapsed if elapsed > 0 else 0
    if errors > 0:
        print(f"  [{port}] {len(frames)} frames ({rate:.1f} Hz), {errors} parse errors")
    return frames


def _play_audio(audio_path: str, wait: bool = True) -> subprocess.Popen:
    """Play audio file via ffplay. Returns process handle."""
    proc = subprocess.Popen(
        ["ffplay", "-nodisp", "-autoexit", "-loglevel", "error", str(audio_path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    if wait:
        proc.wait()
    return proc


def capture(port: str, audio: str, output: str, baud: int = 115200,
            settle: float = SETTLE_SECONDS):
    """Play audio through speakers and capture device mel bands simultaneously."""
    _check_imports("scipy", "serial")
    _check_ffplay()
    from scipy.io import wavfile

    audio_path = Path(audio)
    if not audio_path.exists():
        print(f"ERROR: Audio file not found: {audio_path}", file=sys.stderr)
        sys.exit(1)

    # Get audio duration
    sr, audio_data = wavfile.read(str(audio_path))
    audio_duration = len(audio_data) / sr
    total_capture = settle + audio_duration + POST_SILENCE_SECONDS

    print(f"Calibration capture: {audio_path.name}")
    print(f"  Port: {port}")
    print(f"  Audio duration: {audio_duration:.1f}s")
    print(f"  Settle time: {settle:.1f}s")
    print(f"  Total capture: {total_capture:.1f}s")

    # Start serial capture in background thread
    capture_result = {"frames": []}

    def capture_thread():
        capture_result["frames"] = _capture_serial(port, total_capture, baud)

    thread = threading.Thread(target=capture_thread)
    thread.start()

    # Wait for AGC to settle, then play audio
    print(f"  Waiting {settle:.0f}s for AGC settle...")
    time.sleep(settle)

    print(f"  Playing {audio_path.name}...")
    play_start = time.time()
    _play_audio(str(audio_path), wait=True)
    play_elapsed = time.time() - play_start
    print(f"  Playback finished ({play_elapsed:.1f}s)")

    # Wait for post-silence capture
    time.sleep(POST_SILENCE_SECONDS)

    # Wait for capture thread
    thread.join(timeout=5)
    frames = capture_result["frames"]

    if not frames:
        print("ERROR: No frames captured!", file=sys.stderr)
        sys.exit(1)

    # Mark frames with timing relative to audio start
    # Audio started after settle_seconds of capture
    capture_start_ts = frames[0]["ts"]
    audio_start_ts = capture_start_ts + int(settle * 1000)

    for f in frames:
        f["audio_offset_ms"] = f["ts"] - audio_start_ts

    # Save capture
    out_path = Path(output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        for frame in frames:
            f.write(json.dumps(frame) + "\n")

    # Summary
    mels = np.array([f["mel"] for f in frames])
    audio_frames = [f for f in frames if 0 <= f["audio_offset_ms"] <= audio_duration * 1000]
    settle_frames = [f for f in frames if f["audio_offset_ms"] < 0]

    print(f"\n  Saved: {out_path}")
    print(f"  Total frames: {len(frames)}")
    print(f"  Settle frames: {len(settle_frames)}")
    print(f"  Audio frames: {len(audio_frames)}")
    print(f"  Mel range: [{mels.min():.4f}, {mels.max():.4f}]")
    print(f"  Gain: {frames[-1].get('gain', '?')}")

    return frames


def capture_all(ports: list[str], audio_dir: str, output_dir: str,
                baud: int = 115200, settle: float = SETTLE_SECONDS,
                music_dir: str | None = None):
    """Capture all reference signals + optional music tracks on all ports.

    Plays one audio file at a time (shared acoustic space), captures from
    all ports in parallel.
    """
    _check_imports("scipy", "serial")
    _check_ffplay()
    _check_ffprobe()
    from scipy.io import wavfile

    audio_path = Path(audio_dir)
    out_path = Path(output_dir)
    out_path.mkdir(parents=True, exist_ok=True)

    # Collect reference signals
    ref_files = sorted(audio_path.glob("*_16k.wav"))
    if not ref_files:
        print(f"ERROR: No reference WAV files in {audio_dir}", file=sys.stderr)
        print(f"  Run 'generate' first to create them.", file=sys.stderr)
        sys.exit(1)

    # Optionally add music tracks
    all_files = list(ref_files)
    if music_dir:
        music_path = Path(music_dir)
        music_files = sorted(music_path.glob("*.mp3")) + sorted(music_path.glob("*.wav"))
        all_files.extend(music_files)
        print(f"Including {len(music_files)} music tracks from {music_dir}")

    print(f"Capture-all: {len(all_files)} files, {len(ports)} ports")
    print(f"  Ports: {', '.join(ports)}")
    print(f"  Files: {', '.join(f.name for f in all_files)}")
    print()

    for file_idx, audio_file in enumerate(all_files):
        print(f"=== [{file_idx + 1}/{len(all_files)}] {audio_file.name} ===")

        # Get duration
        if audio_file.suffix == ".wav":
            sr, data = wavfile.read(str(audio_file))
            audio_duration = len(data) / sr
        else:
            # For mp3, use ffprobe
            result = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries",
                 "format=duration", "-of", "csv=p=0", str(audio_file)],
                capture_output=True, text=True
            )
            if result.returncode != 0:
                err = result.stderr.strip() or result.stdout.strip()
                print(f"  WARNING: ffprobe failed for {audio_file.name} "
                      f"(rc={result.returncode}): {err}")
                print(f"  Skipping file.\n")
                continue
            try:
                audio_duration = float(result.stdout.strip())
            except (ValueError, TypeError):
                print(f"  WARNING: Could not parse duration from ffprobe for "
                      f"{audio_file.name}: {result.stdout.strip()!r}")
                print(f"  Skipping file.\n")
                continue

        total_capture = settle + audio_duration + POST_SILENCE_SECONDS

        # Start capture on all ports in parallel
        results = {}

        def port_capture(p):
            results[p] = _capture_serial(p, total_capture, baud)

        threads = []
        for port in ports:
            t = threading.Thread(target=port_capture, args=(port,))
            t.start()
            threads.append(t)

        # Wait for AGC settle, then play
        print(f"  Settling {settle:.0f}s...")
        time.sleep(settle)
        print(f"  Playing ({audio_duration:.1f}s)...")
        _play_audio(str(audio_file), wait=True)
        time.sleep(POST_SILENCE_SECONDS)

        # Wait for all capture threads
        for t in threads:
            t.join(timeout=5)

        # Save each port's capture
        for port in ports:
            port_name = Path(port).name  # e.g., "ttyACM0"
            frames = results.get(port, [])
            if not frames:
                print(f"  WARNING: No frames from {port}")
                continue

            # Mark audio timing
            capture_start_ts = frames[0]["ts"]
            audio_start_ts = capture_start_ts + int(settle * 1000)
            for f in frames:
                f["audio_offset_ms"] = f["ts"] - audio_start_ts

            out_file = out_path / f"{audio_file.stem}_{port_name}.jsonl"
            with open(out_file, "w") as fh:
                for frame in frames:
                    fh.write(json.dumps(frame) + "\n")

            audio_frames = [f for f in frames
                            if 0 <= f["audio_offset_ms"] <= audio_duration * 1000]
            print(f"  {port_name}: {len(audio_frames)} audio frames -> {out_file.name}")

        # Brief pause between files
        print()
        time.sleep(1)

    print("Capture-all complete.")


def _send_command(ser, cmd: str, expect: str = "OK", timeout: float = 2.0) -> str:
    """Send a serial command and wait for response containing expect string."""
    ser.reset_input_buffer()
    ser.write(f"{cmd}\n".encode())
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line:
            lines.append(line)
            if expect and expect in line:
                return line
    resp = "; ".join(lines) if lines else "(no response)"
    if expect and expect not in resp:
        print(f"  WARNING: expected '{expect}' in response to '{cmd}', got: {resp}")
    return resp


def _capture_nn_frames(ser, duration: float) -> list[dict]:
    """Capture NN stream frames from an already-open serial connection."""
    frames = []
    errors = 0
    start = time.time()
    while time.time() - start < duration:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            errors += 1
            continue
        if data.get("type") == "NN":
            frames.append(data)
    if errors > 0:
        elapsed = time.time() - start
        rate = len(frames) / elapsed if elapsed > 0 else 0
        print(f"    {len(frames)} frames ({rate:.1f} Hz), {errors} parse errors")
    return frames


def gain_sweep(port: str, output: str, gains: list[int] | None = None,
               silence_duration: float = 5.0, signal_duration: float = 8.0,
               audio: str | None = None, baud: int = 115200):
    """Sweep HW gain levels, measuring noise floor and optionally signal quality.

    At each gain level:
      1. Lock HW gain via 'test lock hwgain N'
      2. Wait for mic to settle (2s)
      3. Capture silence mel bands (noise floor)
      4. Optionally play reference audio and capture signal mel bands
      5. Compute per-band noise floor, and SNR if audio provided

    Produces gain_sweep.npz with:
      - gains:        (G,) gain levels tested
      - noise_floor:  (G, 26) per-band noise floor at each gain
      - noise_std:    (G, 26) noise floor std dev
      - signal_mean:  (G, 26) mean signal level (if audio provided, else zeros)
      - snr_db:       (G, 26) signal-to-noise ratio in dB (if audio provided)
      - dynamic_range_db: (G, 26) usable dynamic range above noise floor
    """
    _check_imports("serial")
    if audio:
        _check_ffplay()
    import serial

    if gains is None:
        gains = list(range(0, 81, 5))  # 0, 5, 10, ..., 80

    n_gains = len(gains)
    noise_floor = np.zeros((n_gains, N_MELS), dtype=np.float32)
    noise_std = np.zeros((n_gains, N_MELS), dtype=np.float32)
    signal_mean = np.zeros((n_gains, N_MELS), dtype=np.float32)
    snr_db = np.zeros((n_gains, N_MELS), dtype=np.float32)

    has_audio = audio is not None
    if has_audio:
        audio_path = Path(audio)
        if not audio_path.exists():
            print(f"ERROR: Audio file not found: {audio_path}", file=sys.stderr)
            sys.exit(1)

    total_per_step = 2.0 + silence_duration + (signal_duration + 4.0 if has_audio else 0)
    est_total = total_per_step * n_gains
    print(f"Gain sweep: {n_gains} levels on {port}")
    print(f"  Gains: {gains}")
    print(f"  Silence capture: {silence_duration:.0f}s per level")
    if has_audio:
        print(f"  Signal capture: {signal_duration:.0f}s per level ({audio_path.name})")
    print(f"  Estimated total: {est_total / 60:.1f} min")
    print()

    ser = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    try:
        for idx, gain in enumerate(gains):
            print(f"[{idx + 1}/{n_gains}] Gain = {gain}")

            # Lock hardware gain
            resp = _send_command(ser, f"test lock hwgain {gain}")
            print(f"  Locked: {resp}")

            # Start NN stream
            _send_command(ser, "stream nn")

            # Settle time for mic adaptation
            print(f"  Settling 2s...")
            time.sleep(2.0)

            # Capture silence
            print(f"  Capturing silence ({silence_duration:.0f}s)...")
            silence_frames = _capture_nn_frames(ser, silence_duration)

            if silence_frames:
                mels = np.array([f["mel"] for f in silence_frames])
                noise_floor[idx] = mels.mean(axis=0)
                noise_std[idx] = mels.std(axis=0)
                nf_db = -60.0 * (1.0 - noise_floor[idx])
                print(f"  Silence: {len(silence_frames)} frames, "
                      f"noise floor [{noise_floor[idx].min():.4f}, {noise_floor[idx].max():.4f}] "
                      f"({nf_db.max():.1f} to {nf_db.min():.1f} dB)")
            else:
                print(f"  WARNING: No silence frames captured!")

            # Capture with audio if provided
            if has_audio:
                print(f"  Playing {audio_path.name} + capturing ({signal_duration:.0f}s)...")
                # Start playback in background
                play_proc = _play_audio(str(audio_path), wait=False)
                # Capture during playback
                sig_frames = _capture_nn_frames(ser, signal_duration)
                # Stop playback if still running
                play_proc.terminate()
                play_proc.wait()

                if sig_frames:
                    sig_mels = np.array([f["mel"] for f in sig_frames])
                    signal_mean[idx] = sig_mels.mean(axis=0)

                    # SNR = signal_mean / noise_floor (in mel [0,1] space)
                    # Convert to dB: both are already log-compressed mel values
                    # Dynamic range = how much the signal rises above noise floor
                    for b in range(N_MELS):
                        nf = noise_floor[idx, b]
                        sig = signal_mean[idx, b]
                        if nf > 0.001 and sig > nf:
                            # Both in [0,1] mapped from [-60, 0] dB
                            # Convert back to linear power, compute ratio, back to dB
                            nf_power = 10.0 ** ((nf * 60.0 - 60.0) / 10.0)
                            sig_power = 10.0 ** ((sig * 60.0 - 60.0) / 10.0)
                            snr_db[idx, b] = 10.0 * np.log10(sig_power / nf_power)
                        else:
                            snr_db[idx, b] = 0.0

                    print(f"  Signal: {len(sig_frames)} frames, "
                          f"mean [{signal_mean[idx].min():.4f}, {signal_mean[idx].max():.4f}], "
                          f"SNR [{snr_db[idx].min():.1f}, {snr_db[idx].max():.1f}] dB")

                # Brief pause between signal and next gain level
                time.sleep(1.0)

            # Stop NN stream before changing gain
            _send_command(ser, "stream off")
            time.sleep(0.3)

        # Restore AGC
        _send_command(ser, "test unlock hwgain")
        print(f"\nAGC unlocked.")

    finally:
        # Always try to clean up
        try:
            ser.write(b"stream off\n")
            time.sleep(0.1)
            ser.write(b"test unlock hwgain\n")
            time.sleep(0.1)
        except Exception:
            pass
        ser.close()

    # Compute dynamic range (dB above noise floor, out of 60 dB total)
    dynamic_range_db = 60.0 * (1.0 - noise_floor)

    # Save results
    out_path = Path(output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    gains_arr = np.array(gains, dtype=np.int32)
    np.savez(out_path,
             gains=gains_arr,
             noise_floor=noise_floor,
             noise_std=noise_std,
             signal_mean=signal_mean,
             snr_db=snr_db,
             dynamic_range_db=dynamic_range_db)

    # Print summary table
    mel_centers = _get_mel_center_freqs()
    print(f"\n{'='*80}")
    print(f"Gain Sweep Results: {out_path}")
    print(f"{'='*80}")

    # Compact noise floor table: gains as columns, bands as rows
    print(f"\nNoise Floor (mel [0-1]) — rows=bands, cols=gain levels:")
    header = f"{'Band':>5} {'Hz':>7} |" + "".join(f" {g:>4}" for g in gains)
    print(header)
    print("-" * len(header))
    for b in range(N_MELS):
        row = f"{b:>5d} {mel_centers[b]:>7.0f} |"
        for g_idx in range(n_gains):
            row += f" {noise_floor[g_idx, b]:.2f}"
        print(row)

    # Dynamic range summary
    print(f"\nDynamic Range (dB above noise floor):")
    header = f"{'Band':>5} {'Hz':>7} |" + "".join(f" {g:>4}" for g in gains)
    print(header)
    print("-" * len(header))
    for b in range(N_MELS):
        row = f"{b:>5d} {mel_centers[b]:>7.0f} |"
        for g_idx in range(n_gains):
            dr = dynamic_range_db[g_idx, b]
            row += f" {dr:>4.0f}"
        print(row)

    if has_audio:
        print(f"\nSNR (dB) with {Path(audio).name}:")
        header = f"{'Band':>5} {'Hz':>7} |" + "".join(f" {g:>4}" for g in gains)
        print(header)
        print("-" * len(header))
        for b in range(N_MELS):
            row = f"{b:>5d} {mel_centers[b]:>7.0f} |"
            for g_idx in range(n_gains):
                row += f" {snr_db[g_idx, b]:>4.0f}"
            print(row)

    # Recommend optimal gain range
    print(f"\n{'='*80}")
    print("Gain Range Recommendations:")
    print(f"{'='*80}")

    # For each gain, compute average dynamic range across "useful" bands (3-25)
    useful_bands = slice(3, N_MELS)  # skip bands 0-2 (often noise-dominated)
    avg_dr = dynamic_range_db[:, useful_bands].mean(axis=1)
    avg_nf = noise_floor[:, useful_bands].mean(axis=1)

    if has_audio:
        avg_snr = snr_db[:, useful_bands].mean(axis=1)
        # Best gain = highest SNR
        best_snr_idx = np.argmax(avg_snr)
        print(f"  Best SNR:  gain={gains[best_snr_idx]} "
              f"(avg SNR={avg_snr[best_snr_idx]:.1f} dB across bands 3-25)")

    # Minimum gain where all useful bands have >= 20 dB dynamic range
    min_dr_target = 20.0
    for g_idx, gain in enumerate(gains):
        min_dr = dynamic_range_db[g_idx, useful_bands].min()
        if min_dr >= min_dr_target:
            print(f"  Min gain for {min_dr_target:.0f} dB DR everywhere: gain={gain} "
                  f"(worst band has {min_dr:.1f} dB)")
            break
    else:
        print(f"  WARNING: No gain level achieves {min_dr_target:.0f} dB DR across all useful bands")

    # Gain where noise floor < 0.5 for all useful bands
    for g_idx, gain in enumerate(reversed(gains)):
        ridx = n_gains - 1 - g_idx
        max_nf = noise_floor[ridx, useful_bands].max()
        if max_nf < 0.5:
            print(f"  Max gain with noise < 0.5: gain={gain} "
                  f"(worst band noise={max_nf:.3f})")
            break

    print(f"\nTo use: np.load('{out_path}') -> gains, noise_floor, dynamic_range_db, ...")


def analyze(captures_dir: str, output: str, config: str = None,
            gain_sweep_file: str | None = None):
    """Analyze captures vs theoretical mel bands to derive mic profile.

    For each captured signal, computes the per-band gain ratio:
        gain[band] = mean(captured[band]) / mean(theoretical[band])

    And the noise floor from silence capture:
        noise_floor[band] = mean(silence_capture[band])

    If --gain-sweep is provided, produces a gain-aware profile with:
      - noise_floor_by_gain: (G, 26) noise floor indexed by HW gain
      - hw_gain_levels: (G,) the gain levels
      - recommended_min/max: suggested AGC range

    Output: mic_profile.npz
    """
    _check_imports("librosa")
    cap_dir = Path(captures_dir)
    out_path = Path(output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Load reference mel bands (generated by 'generate' step)
    ref_files = {
        "sweep": "sweep_16k_reference.npy",
        "white": "white_noise_16k_reference.npy",
        "pink": "pink_noise_16k_reference.npy",
        "silence": "silence_16k_reference.npy",
        "tones": "tone_bursts_16k_reference.npy",
    }

    refs = {}
    for name, fname in ref_files.items():
        ref_path = cap_dir / fname
        if ref_path.exists():
            refs[name] = np.load(ref_path)
            print(f"  Reference loaded: {name} {refs[name].shape}")

    # Find capture files (pattern: *_ttyACMN.jsonl)
    capture_files = sorted(cap_dir.glob("*.jsonl"))
    if not capture_files:
        print(f"ERROR: No .jsonl capture files in {captures_dir}", file=sys.stderr)
        sys.exit(1)

    # Group captures by signal type and port
    captures = {}  # {signal_name: {port: frames}}
    for cf in capture_files:
        stem = cf.stem  # e.g., "sweep_16k_ttyACM0"
        parts = stem.rsplit("_", 1)
        if len(parts) == 2:
            signal_name, port_name = parts
        else:
            signal_name = stem
            port_name = "default"

        frames = []
        with open(cf) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        data = json.loads(line)
                        if data.get("type") == "NN":
                            frames.append(data)
                    except json.JSONDecodeError:
                        pass

        if frames:
            captures.setdefault(signal_name, {})[port_name] = frames
            print(f"  Capture loaded: {cf.name} ({len(frames)} frames)")

    # Extract audio-region frames (where audio_offset_ms >= 0)
    def get_audio_mels(frames):
        audio_frames = [f for f in frames
                        if f.get("audio_offset_ms", 0) >= 0
                        and f.get("audio_offset_ms", float("inf")) < 999999]
        if not audio_frames:
            audio_frames = frames  # fallback: use all frames
        return np.array([f["mel"] for f in audio_frames])

    # Compute per-band gain ratios from noise signals
    all_gains = []
    signal_gains = {}

    for sig_key, ref_key in [("sweep_16k", "sweep"),
                              ("white_noise_16k", "white"),
                              ("pink_noise_16k", "pink")]:
        if sig_key not in captures or ref_key not in refs:
            continue

        ref_mel = refs[ref_key]
        ref_mean = ref_mel.mean(axis=0)  # (26,)
        ref_mean = np.maximum(ref_mean, 0.001)  # avoid div by zero

        for port_name, frames in captures[sig_key].items():
            cap_mel = get_audio_mels(frames)
            if len(cap_mel) == 0:
                continue

            # Time-align: truncate to shorter length
            min_len = min(len(cap_mel), len(ref_mel))
            cap_mean = cap_mel[:min_len].mean(axis=0)

            gain = cap_mean / ref_mean
            all_gains.append(gain)
            signal_gains[f"{sig_key}_{port_name}"] = gain

            print(f"\n  {sig_key} ({port_name}):")
            print(f"    Ref mean:  [{ref_mean.min():.3f}, {ref_mean.max():.3f}]")
            print(f"    Cap mean:  [{cap_mean.min():.3f}, {cap_mean.max():.3f}]")
            print(f"    Gain:      [{gain.min():.3f}, {gain.max():.3f}]")

    # Noise floor from silence captures
    noise_floors = []
    for sig_key in ["silence_16k"]:
        if sig_key not in captures:
            continue
        for port_name, frames in captures[sig_key].items():
            cap_mel = get_audio_mels(frames)
            if len(cap_mel) > 0:
                nf = cap_mel.mean(axis=0)
                noise_floors.append(nf)
                print(f"\n  Noise floor ({port_name}):")
                print(f"    Mean:  [{nf.min():.4f}, {nf.max():.4f}]")

    # Aggregate across ports and signal types
    if all_gains:
        band_gain = np.median(np.array(all_gains), axis=0)
    else:
        print("WARNING: No gain data — using unity gain", file=sys.stderr)
        band_gain = np.ones(N_MELS, dtype=np.float32)

    if noise_floors:
        noise_floor = np.median(np.array(noise_floors), axis=0)
    else:
        print("WARNING: No silence captures — using zero noise floor", file=sys.stderr)
        noise_floor = np.zeros(N_MELS, dtype=np.float32)

    # Build output profile
    profile = {
        "band_gain": band_gain.astype(np.float32),
        "noise_floor": noise_floor.astype(np.float32),
    }

    # Incorporate gain sweep data if available
    if gain_sweep_file:
        gain_sweep_path = Path(gain_sweep_file)
        sweep_files = []

        # Prefer the explicitly specified path
        if gain_sweep_path.is_file():
            sweep_files.append(gain_sweep_path)

        # Also include any other sweep files from captures dir, avoiding duplicates
        for sf in sorted(Path(captures_dir).glob("gain_sweep*.npz")):
            if gain_sweep_path.is_file() and sf.resolve() == gain_sweep_path.resolve():
                continue
            sweep_files.append(sf)

        if sweep_files:
            # Average across all sweep files (multiple devices)
            all_nf = []
            all_hw_gains = None
            for sf in sweep_files:
                sweep = np.load(sf)
                all_nf.append(sweep["noise_floor"])
                if all_hw_gains is None:
                    all_hw_gains = sweep["gains"]
                print(f"  Gain sweep loaded: {sf.name} ({len(sweep['gains'])} levels)")

            nf_by_gain = np.median(np.array(all_nf), axis=0)  # (G, 26)

            # Recommend AGC range: max gain where band 3 noise < 0.5
            recommended_max = int(all_hw_gains[-1])
            for g_idx in range(len(all_hw_gains) - 1, -1, -1):
                if nf_by_gain[g_idx, 3:].max() < 0.50:
                    recommended_max = int(all_hw_gains[g_idx])
                    break

            # Min gain: lowest where we still get > 30 dB DR on band 15 (mid)
            recommended_min = int(all_hw_gains[0])
            for g_idx in range(len(all_hw_gains)):
                dr_mid = 60.0 * (1.0 - nf_by_gain[g_idx, 15])
                if dr_mid >= 30.0:
                    recommended_min = int(all_hw_gains[g_idx])
                    break

            profile["noise_floor_by_gain"] = nf_by_gain.astype(np.float32)
            profile["hw_gain_levels"] = all_hw_gains.astype(np.int32)
            profile["recommended_gain_min"] = np.int32(recommended_min)
            profile["recommended_gain_max"] = np.int32(recommended_max)

            print(f"\n  Gain-aware profile: {len(all_hw_gains)} levels")
            print(f"  Recommended AGC range: {recommended_min} - {recommended_max}")

    # Save profile
    np.savez(out_path, **profile)

    print(f"\n=== Mic Profile: {out_path} ===")
    print(f"{'Band':>6} {'Freq(Hz)':>10} {'Gain':>8} {'NoiseFlr':>10}")
    mel_centers = _get_mel_center_freqs()
    for i in range(N_MELS):
        print(f"{i:>6d} {mel_centers[i]:>10.1f} {band_gain[i]:>8.3f} {noise_floor[i]:>10.4f}")

    print(f"\n  Band gain range:   [{band_gain.min():.3f}, {band_gain.max():.3f}]")
    print(f"  Noise floor range: [{noise_floor.min():.4f}, {noise_floor.max():.4f}]")
    if "recommended_gain_max" in profile:
        print(f"  Recommended AGC:   [{profile['recommended_gain_min']} - {profile['recommended_gain_max']}]")
    print(f"\nUsage: python scripts/prepare_dataset.py --mic-profile {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Mic transfer function calibration for ML training")
    sub = parser.add_subparsers(dest="command")

    # Generate reference signals
    gen = sub.add_parser("generate", help="Create reference calibration audio")
    gen.add_argument("--output-dir", default="data/calibration",
                     help="Output directory for WAV files")
    gen.add_argument("--duration", type=float, default=10.0,
                     help="Duration of each signal in seconds")

    # Single capture
    cap = sub.add_parser("capture", help="Play audio + capture device mel bands")
    cap.add_argument("--port", required=True, help="Serial port (e.g., /dev/ttyACM0)")
    cap.add_argument("--audio", required=True, help="Audio file to play")
    cap.add_argument("--output", "-o", required=True, help="Output .jsonl file")
    cap.add_argument("--baud", type=int, default=115200, help="Baud rate")
    cap.add_argument("--settle", type=float, default=SETTLE_SECONDS,
                     help="AGC settle time in seconds")

    # Capture all signals on all ports
    cap_all = sub.add_parser("capture-all",
                             help="Capture all reference signals on all ports")
    cap_all.add_argument("--ports", nargs="+", required=True,
                         help="Serial ports (e.g., /dev/ttyACM0 /dev/ttyACM1)")
    cap_all.add_argument("--audio-dir", default="data/calibration",
                         help="Directory with reference WAV files")
    cap_all.add_argument("--output-dir", default="data/calibration",
                         help="Output directory for captures")
    cap_all.add_argument("--music-dir", default=None,
                         help="Optional: directory with music tracks to also capture")
    cap_all.add_argument("--baud", type=int, default=115200, help="Baud rate")
    cap_all.add_argument("--settle", type=float, default=SETTLE_SECONDS,
                         help="AGC settle time per file")

    # Gain sweep
    gsw = sub.add_parser("gain-sweep",
                         help="Sweep HW gain levels to measure noise vs gain")
    gsw.add_argument("--port", required=True, help="Serial port")
    gsw.add_argument("--output", "-o", default="data/calibration/gain_sweep.npz",
                     help="Output .npz file")
    gsw.add_argument("--gains", type=int, nargs="+", default=None,
                     help="Gain levels to test (default: 0,5,10,...,80)")
    gsw.add_argument("--silence-duration", type=float, default=5.0,
                     help="Seconds of silence capture per gain level")
    gsw.add_argument("--signal-duration", type=float, default=8.0,
                     help="Seconds of signal capture per gain level (if --audio)")
    gsw.add_argument("--audio", default=None,
                     help="Optional: reference audio to play at each gain level")
    gsw.add_argument("--baud", type=int, default=115200, help="Baud rate")

    # Analyze captures
    ana = sub.add_parser("analyze", help="Derive mic profile from captures")
    ana.add_argument("--captures", required=True,
                     help="Directory with .jsonl captures and _reference.npy files")
    ana.add_argument("--output", default="data/calibration/mic_profile.npz",
                     help="Output mic profile (.npz)")
    ana.add_argument("--config", default="configs/default.yaml",
                     help="Training config (for reference)")
    ana.add_argument("--gain-sweep", default=None,
                     help="Gain sweep .npz file(s) to incorporate into profile")

    args = parser.parse_args()

    if args.command == "generate":
        generate(args.output_dir, args.duration)
    elif args.command == "capture":
        capture(args.port, args.audio, args.output, args.baud, args.settle)
    elif args.command == "capture-all":
        capture_all(args.ports, args.audio_dir, args.output_dir,
                    args.baud, args.settle, args.music_dir)
    elif args.command == "gain-sweep":
        gain_sweep(args.port, args.output, args.gains,
                   args.silence_duration, args.signal_duration,
                   args.audio, args.baud)
    elif args.command == "analyze":
        analyze(args.captures, args.output, getattr(args, "config", None),
                getattr(args, "gain_sweep", None))
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
