# Blinky Onset Activation — ML Training Pipeline

Train a small NN that outputs a per-frame **onset** activation (one value per 16 ms mel frame) for the Blinky firmware. The task is onset detection: fire on every percussive hit, on or off the beat. It is not beat detection (see "Task is Onset Detection" in the repo's `CLAUDE.md`).

**Current status (October 2026):**
- The firmware embeds the **v33** model (`configs/conv1d_w16_onset_v33_mel_only.yaml`): Conv1D [32, 32], kernel 5, 16-frame window, 50 mel bands. The header is `blinky-things/audio/frame_onset_model_data.h`.
- **Devices ship with the onset NN off** (`BLINKY_ONSET_NN_ENABLED=0` in `blinky-things/audio/AudioTracker.h`). Onsets come from tuned spectral flux. On edm/ with the hat's CPU cost model, flux scores F1 0.546 at 178 fps; v33 scores 0.480 and holds the loop at 48 fps (~12 ms per inference).
- A new model is enabled only when it beats flux under that same measurement (the host harness in `tests/tracker/`). Roadmap and open experiments: `docs/ML_IMPROVEMENT_PLAN.md`.

## Quick Start

```bash
source venv/bin/activate

# train.py refuses to run outside tmux/screen. Run long steps in tmux:
tmux new-session -d -s train "cd ml-training && source venv/bin/activate && \
  make prepare train export eval CONFIG=configs/conv1d_w16_onset_v33_mel_only.yaml 2>&1 | tee train.log"

# Steps:
make prepare CONFIG=...   # Extract mel features + augmentation
make train CONFIG=...     # Train model (auto-named output dir)
make export CONFIG=...    # Export TFLite INT8 → C header
make eval CONFIG=...      # Evaluate on the 18-track edm/ corpus
```

Always pass `CONFIG=` (an onset config): `prepare`, `train`, `export` and `eval` refuse without it (#271).

`./train_pipeline.sh <config> <run-name> [--skip-labels] [--skip-prep]` runs onset labels → prepare → train → eval → export in one go. Launch it in tmux too.

**`make label` generates onset labels** (`scripts/generate_onset_consensus.py`, 5 onset detectors; it resumes). `make merge` refuses: it used to write BEAT consensus into the onset label dir (#271). `make prepare/train/export/eval` require `CONFIG=` (an onset config). Onset labels come from `scripts/generate_onset_consensus.py` and `scripts/generate_kick_weighted_targets.py` / `scripts/clean_kick_weighted.py` (kick/snare weighted labels from drum stems).

## Configuration

Configs use a **base + override** pattern. `configs/base.yaml` holds shared audio, training and data settings. Each versioned config (`configs/conv1d_w16_onset_v*.yaml`) overrides the model, labels, audio and export sections. `load_config()` merges them.

Things that must match the firmware (mel bands, fmin, fmax, `mel_db_range`) live in the `audio` section. Note that `base.yaml` still says `n_mels: 26` / `fmin: 60`; the v33 config overrides these to 50 / 30.

```bash
make train CONFIG=configs/conv1d_w16_onset_v33_mel_only.yaml RUN_NAME=v33-retrain
```

## Directory Structure

```
ml-training/
├── configs/
│   ├── base.yaml                     # Shared settings (audio, training, data paths)
│   └── conv1d_w16_onset_v*.yaml      # Per-experiment overrides (v33 = embedded model)
├── scripts/
│   ├── audio.py                      # Shared mel pipeline (single source of truth)
│   ├── prepare_dataset.py            # Audio → mel features + onset targets (with augmentation)
│   ├── generate_onset_consensus.py   # 5-system onset consensus labels
│   ├── generate_kick_weighted_targets.py  # Kick/snare weighted onset labels
│   ├── clean_kick_weighted.py        # Drop drum-stem bleed from kick_weighted labels
│   ├── export_tflite.py              # PyTorch → TFLite INT8 → C header
│   ├── calibrate_mic.py              # Mic calibration pipeline
│   ├── validate_features.py          # Python vs firmware mel parity check
│   ├── label_beats.py, merge_consensus_labels*.py  # BEAT labeling (reference only, not training input)
│   └── download_fma.py, download_giantsteps.py, download_rir.py
├── models/                           # onset_conv1d.py (current), onset_fc*.py, onset_cnn.py, onset_conv1d_pool.py
├── analysis/                         # Offline diagnostics (peak picker, gate checks, offline-vs-device)
├── tools/label-reviewer/             # Browser tool for hand-correcting onset labels
├── train.py                          # Training loop (refuses to run outside tmux/screen)
├── evaluate.py                       # Offline onset F1 on edm/ vs .onsets_consensus.json
├── train_pipeline.sh                 # prepare → train → eval → export
├── Makefile                          # Pipeline targets
└── data/                             # (gitignored) calibration, device captures
```

## Output Convention

Each `train_pipeline.sh` run writes to `${BLINKY_WORK}/outputs/<run-name>/` (`BLINKY_WORK` defaults to `/mnt/nvme`; see `docs/ML_STORAGE_LAYOUT.md`). The default run name is `{config}-{timestamp}`; override with `RUN_NAME=`.

```
outputs/<run-name>/
├── best_model.pt            # Best checkpoint (by early_stopping_metric)
├── final_model.pt           # Last epoch
├── model_checkpoint.pt      # state_dict + config + metadata (incl. mel_frontend)
├── mel_frontend.json        # the dataset's mel front end, for export (#291)
├── training_log.csv         # Per-epoch metrics
└── eval/eval_results.json   # Offline edm/ onset F1
```

**`export_tflite.py` also overwrites the firmware's active header** `blinky-things/audio/frame_onset_model_data.h`, not just the versioned one named in the config. Check `git diff` before committing, and run `python tools/check_model_input.py` (repo root) to confirm the firmware will accept the model.

## Key Constraint: Feature Parity

The model input **must exactly match** the firmware's `SharedSpectralAnalysis::getRawMelBands()` (`blinky-things/audio/SharedSpectralAnalysis.h`):
- 16 kHz sample rate, Hamming window, FFT-256, hop 256 (62.5 frames/s)
- 50 mel bands from the firmware's integer table `MEL_BANDS[]` (30–8000 Hz), each a weighted average; **not** an HTK filterbank (#295)
- Log compression: `10*log10(x + 1e-7)` with a 1e-6 silence floor, mapped [-60, 0] dB to [0, 1]

The spec is `spec/mel_frontend_v1.json`, and `scripts/mel_frontend.py` builds training's front end from it; CI's mel parity test holds both to the firmware. Changing the mel config needs a matching firmware change (`NUM_MEL_BANDS`, `MEL_MIN_FREQ`, `MEL_MAX_FREQ` triplet), a new spec version and a model the firmware accepts. The v36 config (80 mel, 31.25 kHz) does not match the firmware.

**The model carries its front end (#291).** `prepare_dataset.py` writes `mel_frontend.json` into the processed dir, `train.py` copies it beside the model (and into `model_checkpoint.pt`), and `export_tflite.py` writes it into the header as `FRAME_ONSET_MODEL_DATA_MEL_FRONTEND`. The firmware build refuses a header whose front end isn't `SpectralConstants::MEL_FRONTEND_VERSION`, or that records none. The one exception is v33, trained on the librosa filterbank (front end 0) and waived by hash in `FrameOnsetNN.h`. To export a model trained before the record existed, state its front end with `--mel-frontend librosa_htk`; the firmware will refuse it unless it's retrained.

## Hardware Target

- **XIAO nRF52840 Sense** (Cortex-M4F @ 64 MHz, 256 KB RAM, 1 MB Flash)
- v33: ~20 KB INT8, 16-frame window, ~12 ms per inference on device
- Runtime: TFLite Micro + CMSIS-NN

## Results (edm/, 18 tracks, offline onset F1 vs `.onsets_consensus.json`, ±50 ms)

| Version | Change | Onset F1 | Status |
|---------|--------|----------|--------|
| v33 | 50 mel, 30–8000 Hz, kick_weighted labels | 0.617 | Embedded in firmware; NN off on devices |
| v34d | kick_weighted_clean labels | 0.616 | Flat; not deployed |
| v36 | 80 mel, 14 kHz fmax, 31.25 kHz | 0.537 | Regressed; not deployed |

On-device, the decision metric is the host tracker harness (`tests/tracker/run_tracker_eval.py`), where tuned flux currently wins.

## Acoustic Environment Augmentation

`prepare_dataset.py --augment`:
- **Volume variation** (gain augmentation)
- **Background noise** at varied SNR
- **Low-pass filter** (boomy rooms)
- **Room impulse responses** (`download_rir.py`)
- **Time stretch and pitch shift** (factors in `base.yaml`)
- **Mic profile**: gain-aware noise floor from calibration data (`--mic-profile`)
