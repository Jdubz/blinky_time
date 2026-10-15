# Tracker harness

Runs the firmware's real `AudioTracker`, `SharedSpectralAnalysis` and `AdaptiveMic` code on the host. It feeds them recorded audio with a simulated main loop, so you can measure rhythm tracking offline and deterministically.

Use it to check that tempo, phase and beat count don't depend on loop rate and don't drift. Phase drift that depends on loop rate is the failure mode the b208 frame-clock rework fixed.

**Limitation:** there's no TFLite on the host, so the tracker runs its NN-unavailable path.
- Tempo and phase use the same code either way.
- Onset pulses come from spectral flux, not the NN, so `recall` isn't the device's onset F1.

## Build

The harness needs any C++17 compiler (g++, clang++, or `python -m ziglang c++` after `pip install ziglang`):

```bash
c++ -std=c++17 -O2 -DBLINKY_HOST_HARNESS -DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840 \
    -Iblinky-simulator/include -Iblinky-things \
    tests/tracker/tracker_harness.cpp \
    blinky-things/audio/AudioTracker.cpp blinky-things/audio/SharedSpectralAnalysis.cpp \
    blinky-things/inputs/AdaptiveMic.cpp blinky-things/types/BlinkyAssert.cpp \
    -o build/tracker_harness
```

`BLINKY_HOST_HARNESS` does two things:
- swaps the CMSIS FFT for a plain DFT;
- swaps FrameOnsetNN for its stub.

## Run

```bash
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness \
    [--compare build/tracker_harness_old] [--tracks techno-minimal-01] [--fps 35 60 90]
```

- **Corpus:** `blinky-test-player/music/edm/` (the validation corpus).
- **Ground truth:** `.onsets_consensus.json`.
- **Decoding:** needs `ffmpeg` on PATH or `pip install imageio-ffmpeg`.

### Metrics

| Metric | Meaning |
|---|---|
| `lock` | Phase consistency at strong ground-truth onsets (0..1) |
| `worst20s` | Worst 20 s window of `lock`, which catches drift |
| `recall` | Strong onsets with a fired onset within ±50 ms |
| `bpm` | Median BPM after warm-up |

Compare the same track across `--fps` values: the numbers should barely move.
