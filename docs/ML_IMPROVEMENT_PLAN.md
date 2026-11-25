# ML plan

**Status (2026-11-23):** the onset NN is shelved on the nRF52840 and training is paused. The only active ML direction is ESP32-S3 feasibility, which is priority P3 (after tooling, audio consistency and LED smoothness). The dated experiment logs that used to be here (v26–v37, 2026-04 to 2026-05) are in git history.

The task is drum-hit (kick/snare) detection, not beat or all-onset detection (CLAUDE.md). Labels, scoring and the corpus rules in CLAUDE.md apply to any ML work.

## Why the NN is off on the nRF52840

- Device builds set `BLINKY_ONSET_NN_ENABLED=0` (blinky-firmware `blinky-things/audio/AudioTracker.h`). Onsets come from SuperFlux with the Böck 2012 online picker. The firmware still embeds the v33 model (`frame_onset_model_data.h`), and blinky-firmware's CI keeps the NN path building and scored (`tracker-harness-nn`, `tools/check_model_input.py`) so it doesn't rot.
- **A tie goes to flux.** Speed only has to fit the frame budget; past that, the comparison is accuracy alone, and flux is cheaper, needs no training and is plain code. Without CPU cost, v33 scores drum F 0.672 against SuperFlux's 0.657 on the 18 edm/ tracks, measured against labels known to be late and wrong at the time. That is a tie. At ~12 ms per inference, v33 also can't hold the frame rate.
- Compute was not the deciding factor. A streaming export (per-layer state, newest frame only) is estimated at about 13k MACs, roughly 0.75 ms, against v33's 210k. That estimate is unmeasured. The blocker is accuracy.

The consistency work that replaced it is firmware work: [blinky-firmware#8](https://github.com/Jdubz/blinky-firmware/issues/8) (the same hit sometimes triggers and sometimes doesn't). The frame-phase wobble ([blinky-firmware#20](https://github.com/Jdubz/blinky-firmware/issues/20)) was fixed with the overlapped onset STFT and closed on 2026-11-23; what's left of it moved to #8.

## The gate to turn a model back on

A model is enabled only when it clearly beats SuperFlux on accuracy, measured:

1. against hand-curated ground truth for the 18 edm/ tracks ([blinky-corpus#4](https://github.com/Jdubz/blinky-corpus/issues/4)), not consensus or generated labels;
2. with the drum metrics (kick/snare recall at ±30/±50 ms, on-drum share, drum F), not all-onset F1;
3. in blinky-firmware's host tracker harness (`tests/tracker/run_tracker_eval.py`), through the same `AudioTracker::pickOnset` picker, with the CPU cost model set to the model's measured inference time (`--nn-cost-us`, from `set nnprofile 1` on a device), so frame rate is part of the result.

Offline `evaluate.py` F1 is not the gate: it ignores CPU cost and the firmware's peak picking.

## Active: ESP32-S3 feasibility (P3)

The ESP32-S3's second core is the only place an NN is being considered (blinky-firmware's [NEXTGEN_ESP32S3_PLAN.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/NEXTGEN_ESP32S3_PLAN.md)). The question is whether a model there can clear the gate above while the LEDs never drop frames. Training for it runs on the DGX Spark as a `gpu_task` job (`ml-training/scripts/spark_jobs.py train`, CLAUDE.md "Long-Running Scripts"); nothing trains on the build host or the test box.

## Paused

| Item | State |
|---|---|
| [#305](https://github.com/Jdubz/blinky_time/issues/305) Submit training as a `gpu_task` | Open, paused. The client is built and tested; the platform side is ready (my-spark #1550 closed). Resumes for the ESP32-S3 or once blinky-corpus#4 gives a reason to expect a model to beat flux. |
| [#324](https://github.com/Jdubz/blinky_time/issues/324) Drum labels from GPU-runnable transcription models | Open. ADTOF (PyTorch port) is the baseline; the label move waits until labels are needed again. |
| [blinky-corpus#4](https://github.com/Jdubz/blinky-corpus/issues/4) Hand-curated ground truth for edm/ | Open (owner). Tooling is done (`ml-training/tools/label-reviewer/`, `.onsets_human.json` overlays); curation isn't. |
| [#284](https://github.com/Jdubz/blinky_time/issues/284) Late, mislabelled drum labels | Closed: the generator was replaced by ADTOF. Corpus labels made before that are not training targets. |
| [#288](https://github.com/Jdubz/blinky_time/issues/288) Spark submit/poll client | Closed: done, in blinky-server (`blinky_server/spark/`). |

## Don't believe until measured

From the 2026-05-02 literature review (citations in blinky-firmware's [ONSET_DETECTION_LITERATURE_2026_05_02.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/ONSET_DETECTION_LITERATURE_2026_05_02.md)):

- **"Architecture isn't the constraint."** Not supported at our scale. Published small-CNN onset detectors with F1 ≥ 0.85 run at 25k–100k parameters (Schlüter & Böck 2014; Böck et al. 2012; Stahl & Sturm 2023). Our ~10k INT8 budget is below anything published, and the internal "wider doesn't help" ablation was run entirely below that range.
- **"Label cleanup doesn't help."** What v34d showed is narrower: cleaning kick labels doesn't add coverage. Label noise and label coverage are separate axes (Schlüter 2014; Vogl 2017; Wu 2018).
- **"F1 ≥ 0.70 is reachable" or any ceiling quoted against consensus labels.** Inter-annotator F1 on hand-curated drum onsets is about 0.92–0.95 on clean material and lower on dense electronic music (Cartwright 2018; Gillick 2019). Against consensus labels the measurable ceiling may be much lower. Until blinky-corpus#4 exists, no model number is calibrated.
- **Offline gains carry to the device.** They haven't: v36's validation F1 rose 0.13 over v34d while its edm/ F1 fell 0.08 below v33, and v33 itself scored 0.617 offline against 0.570 on a device. Only the gate above counts.
