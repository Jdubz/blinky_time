# Tracker harness

Runs the firmware's real `AudioTracker`, `SharedSpectralAnalysis` and `AdaptiveMic` code on the host. It feeds them recorded audio with a simulated main loop, so you can measure rhythm tracking offline and deterministically.

Use it to check that tempo, phase and beat count don't depend on loop rate and don't drift. Phase drift that depends on loop rate is the failure mode the b208 frame-clock rework fixed.

**Default build:** no TFLite, and the onset NN is off (`BLINKY_ONSET_NN_ENABLED=0`), exactly as on devices: onset pulses come from spectral flux.
- Devices ship with the NN off because tuned flux beats the v33 NN on edm/ (F1 0.546 vs 0.480) at 3.7x the frame rate. See the CPU cost model below.

**With the real onset NN (#203):** `tests/tracker/build_host_nn.sh` compiles the Arduino TFLite Micro library for the host and links the harness with `BLINKY_HOST_NN`, so `FrameOnsetNN` runs the embedded model exactly as the device does.
- The kernels are TFLite reference kernels, plus CMSIS-NN's portable C paths (no ARM intrinsics on the host).
- It finds the library installed by `tools/setup_tflite_lib.sh`, or you can pass `--lib <dir>/src`.
- Use `CXX`/`AR` to pick a toolchain, and `JOBS` to set compile parallelism (default: `nproc`).
- With zig, pin `pip install ziglang==0.13.0` (clang 18). Clang 19 and later reject the library's bundled flatbuffers (`stl_emulation.h`), which GCC accepts.
- On Windows (Git Bash), the output is `tracker_harness_nn.exe`, and `run_tracker_eval.py` needs absolute harness paths (`$(cygpath -w ...)`).

```bash
CXX="python -m ziglang c++" AR="python -m ziglang ar" tests/tracker/build_host_nn.sh --out build/host_nn
```

```bash
tests/tracker/build_host_nn.sh --out build/host_nn
python tests/tracker/run_tracker_eval.py --harness build/host_nn/tracker_harness_nn --compare build/tracker_harness
```

CI's `tracker-harness-nn` job builds it and runs the stress set with it. It also fails if:
- the embedded model is refused (the NN harness exits 5);
- the simulated frame rate with the NN falls below 35 fps (the b208-b210 drain-loop livelock);
- the corpus gate with the NN falls below its floors.

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
| `excess` | `lock` minus the `lock` of the same phase trace sampled at random times. ~0 means no real alignment. Use this, not raw `lock`, to compare builds |
| `tempo` | Fraction of post-warm-up time where the tracked period is a valid multiple (1/2, 1, 2 or 4 beats, ±4%) of the track's tempo from `.pattern_gt.json` |
| `worst20s` | Worst 20 s window of `lock`, which catches drift |
| `recall` | Strong onsets with a fired onset within ±50 ms |
| `bpm` | Median BPM after warm-up |

Compare the same track across `--fps` values: the numbers should barely move.

## CI

The `tracker-harness` job in `.github/workflows/pr-validation.yml` runs on every PR. It does three things:

- **UBSan stress:** builds the harness with `-fsanitize=undefined,float-cast-overflow,float-divide-by-zero -fno-sanitize-recover=all` and runs `run_stress.py`. That covers synthetic silence, an 8 s gap, clipping, white noise, 50 Hz hum and 128 BPM clicks, each at 15, 60 and 250 loop fps. Any UB, crash, firmware `BLINKY_ASSERT` after `begin()` (the harness exits 3), empty output or non-finite phase/bpm fails the job. The corpus gate uses the same harness, so a tripped assert fails it too. It also checks the level normalizer on a smooth noise riser: `level` must not stay pinned at 1.0 for 2 s or more (#176).
- **Frame-rate gate:** runs the release build with the CPU cost model below (NN off, as on devices) and fails if the simulated frame rate is under 150 fps.
- **Corpus gate:** runs `run_tracker_eval.py` with `--min-excess`, `--min-tempo` and `--min-recall` floors set just under the current baseline. Raise the floors in the workflow when the tracker improves.

To run the stress set locally:

```bash
python tests/tracker/run_stress.py --harness build/tracker_harness_ubsan
```

## CPU cost model

The harness clock normally moves only by the simulated loop period, so audio work is free. Pass any of these to make the loop period emergent, as on the device: each loop costs the render time, and every spectral frame and NN inference advances the clock while the PDM "DMA" keeps delivering mic blocks.

```bash
build/tracker_harness in.pcm out.csv 60 0.25 1 --nn-cost-us=12000 --frame-cost-us=3200 --render-cost-us=4500
# stderr: [COST] loops=... sim_s=... fps=... nn_per_s=... nn_cpu=...%
```

The values above are calibrated to hat_v1 (89 LEDs, 64 MHz nRF52840):
- **NN:** 12 ms per v33 inference, measured with `set nnprofile 1`.
- **Calibration check:** without the NN the model gives 177.7 fps (device: 179 fps on b190). With b210's drain loop and the NN on, it gives 11.2 fps and 62.5 inferences/s (device: 12.4 fps, 62.4/s).

This is how the b208-b210 drain-loop livelock was reproduced. `update()` used to chase samples that arrived during a ~12 ms inference, so it ran the NN on every frame and starved the renderer. It now drains only what was waiting at entry.
