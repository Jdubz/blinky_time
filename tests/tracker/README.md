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

## CI

The `tracker-harness` job in `.github/workflows/pr-validation.yml` runs on every PR. It does two things:

- **UBSan stress:** builds the harness with `-fsanitize=undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=all` and runs `run_stress.py`. That covers synthetic silence, an 8 s gap, clipping, white noise, 50 Hz hum and 128 BPM clicks, each at 15, 60 and 250 loop fps. Any UB, crash, firmware `BLINKY_ASSERT` after `begin()` (the harness exits 3), empty output or non-finite phase/bpm fails the job. The corpus gate uses the same harness, so a tripped assert fails it too. It also checks the level normalizer on a smooth noise riser: `level` must not stay pinned at 1.0 for 2 s or more (#176).
- **Corpus gate:** runs `run_tracker_eval.py` with `--min-excess`, `--min-tempo` and `--min-recall` floors set just under the current baseline. Raise the floors in the workflow when the tracker improves.

To run the stress set locally:

```bash
python tests/tracker/run_stress.py --harness build/tracker_harness_ubsan
```
