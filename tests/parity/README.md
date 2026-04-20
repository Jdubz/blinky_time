# Parity harness — firmware vs Python feature math

This directory hosts a native C++ test that links the real firmware
`SharedSpectralAnalysis.cpp` and invokes its shape-feature math on an
externally supplied magnitude spectrum. A Python script computes the same
features on the same magnitudes via `ml-training/analysis/features.py` and
asserts per-frame MAE is below a tight threshold.

Why not re-implement features in Python only? Because any drift between
firmware and Python is then invisible. The harness makes the firmware C++
the authoritative computation and forces the Python reference to track it.

## What's tested

- `SharedSpectralAnalysis::computeShapeFeaturesRaw()` — centroid, crest,
  rolloff, HFC on pre-compressor magnitudes.

Not yet tested (open follow-ups):
- FFT (firmware uses CMSIS-DSP / arduinoFFT; Python uses numpy). Feeding
  the same magnitudes to both code paths bypasses this difference —
  acceptable because the FFT is well-trodden infrastructure, not the part
  we wrote.
- Compressor / whitening / mel bands / spectral flatness on compressed mags.
  Add in future rounds if those features enter the shortlist.

## Build

```bash
mkdir -p tests/parity/build
cd tests/parity/build
cmake ..
cmake --build . -j
```

Produces `parity_harness`.

## Run via Python wrapper

```bash
cd ml-training
./venv/bin/python -m analysis.run_parity_test \
    --audio ../blinky-test-player/music/edm/breakbeat-drive.mp3 \
    --harness ../tests/parity/build/parity_harness
```

Builds the harness if missing, computes Python mags + features, feeds mags
to the harness, reads harness feature output, compares, prints MAE per
feature. Non-zero exit on MAE above threshold.
