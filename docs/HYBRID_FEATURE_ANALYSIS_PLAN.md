# Hybrid Feature Analysis Plan

**Status:** In progress (Apr 2026)
**Owner:** Audio/ML
**Supersedes:** ad-hoc `flat`/`rflux` streaming (b132)

## Goal

**Choose a small set of deterministic features that genuinely help the NN discriminate true drum onsets from false ones on-device.**

Read that sentence twice. It has three load-bearing words:

- **"small set"** — not "all features that show a signal". Each NN input channel has to earn its slot against a mel-only baseline. Stacking more inputs on a thin baseline muddies the model, it doesn't help it.
- **"help the NN discriminate"** — the NN is the consumer. The measurement that matters is *does the model's F1 improve with this feature as an input* (versus, say, *does this feature separate onset frames from non-onset frames*, which is a different question).
- **"on-device"** — on-device is the ground truth. Offline |d| and parity checks rule out bugs, but only device measurements decide which features survive the acoustic chain.

**Scope: EDM only.** The fleet's target deployment is EDM-driven installations (techno, trance, breakbeat, dnb, dubstep, garage, amapiano, reggaeton, etc. — everything already in the corpus). This investigation does not need to generalize to rock, jazz, classical, or sparse percussion.

## Working principles — read before adding any feature

These codify mistakes made on this investigation so far and the corrected approach. The plan drifts if they're ignored.

### Mistakes to avoid

1. **Don't drift into deterministic gates.** A gate is a post-NN threshold filter that suppresses firings. It's a different engineering approach from "NN input feature" and has different requirements. The goal is NN inputs; building a gate experiment (crestGateMin sweep in b135) was a digression. Any future "gate" proposal has to be a deliberate detour with explicit justification, not an accidental drift.

2. **Don't use the wrong discrimination metric.** "Onset-vs-non-onset peak |d|" (Phase 1 / Phase 3) measures whether a feature fires at drum times. That's *feature existence*, not NN-input readiness. For NN-input readiness you want **TP-vs-FP |d|**: among frames where the NN fires, does the feature separate correct firings from wrong ones? A feature with strong onset-vs-non-onset |d| can have near-zero TP-vs-FP |d| — both drums AND broadband false triggers produce a peak.

3. **Don't stack without ablation.** v27-hybrid added flatness + raw_flux without a mel-only baseline, so we don't actually know either helped. Repeating that mistake (add four more shape features without the same check) is off the table. Every feature that earns a permanent NN input slot must beat a mel-only baseline under identical training recipe.

4. **Don't ignore redundancy with mel.** Five of the six shape features (centroid, crest, rolloff, hfc, flatness) are functions of the same magnitude spectrum the 30 mel bands already represent. If a feature can be reconstructed from mel bands with R² > 0.95, the NN can learn it for free — adding it as an input duplicates info and wastes a channel. Only raw_flux is temporal (frame-to-frame), so it's the one feature that carries genuinely new information relative to mel.

5. **Don't confuse "feature exists" with "feature helps the NN."** Phase 1 ranking = "signals that exist on-device". That's a necessary but not sufficient condition for being an NN input. The full bar is higher.

### Five gates a feature must pass before it earns an NN input slot

Ordered cheapest first:

a. **Exists on-device** — per-onset-peak |d| ≥ 0.4 on real EDM. (All 6 current candidates pass.)

b. **Discriminates TP from FP at NN-firing moments** — |d| ≥ 0.3 between frames where NN fires AND within ±50 ms of GT vs frames where NN fires AND ≥ 100 ms from any GT. *(Not yet measured. Next step.)*

c. **Non-redundant with mel bands** — fit a linear regressor predicting the feature from the 30 mel bands; R² < 0.95 required. Features the NN can trivially recompute from mel earn no slot.

d. **Non-redundant within the kept set** — pairwise |r| < 0.9 between surviving candidates. Drop the more expensive of any correlated pair.

e. **Marginal F1 gain against mel-only baseline** — ablation training: v28-mel-only vs v28-mel+feature. Require ≥ 0.03 F1 improvement on held-out EDM corpus.

No feature enters the production NN input vector without passing all five. The mel-only baseline must exist and be trained with the same recipe as any hybrid candidate; without it, "feature helped" is unverifiable.

### Sequencing — offline first, on-device for confirmation only

On-device validation runs cost ≈ 15–30 min each; Python evaluations on the same audio cost seconds. The five gates map cleanly onto an offline-first pipeline, with on-device runs reserved for *confirming* candidates that already survived the cheaper tests:

| gate | offline feasible? | on-device required? |
|------|:-----------------:|:-------------------:|
| (a) exists on-device | — | already done (Phase 3) |
| (b) TP-vs-FP \|d\| | **yes** — run v27 offline on held-out GS corpus, split firings by GT | only to confirm top candidates |
| (c) R² vs mel | yes — pure math on any audio | no |
| (d) pairwise \|r\| | yes — pure math on any audio | no |
| (e) ablation F1 | yes — training is always offline | held-out eval only |

**The contamination rule still applies.** For any step that invokes the NN (b, e), the audio must come from the 25-track `edm_holdout` corpus, not the 18 tracks v27 trained / validated on. The offline-feature-definition steps (c, d) don't touch the NN, so any EDM audio works.

**Sequence to execute:**
1. (c) + (d) on any single EDM track — cheapest, kills redundant features first.
2. (b) offline on held-out GS corpus — decides whether Path B is even worth attempting.
3. On-device confirmation of the surviving 2–3 features using the *existing* b134 validation captures (no new device runs needed).
4. (e) ablation training only if ≥ 1 feature survives 1–3.

## Parity gap analysis — 2026-04-20

Before running any offline test whose result is used to pick features for retraining, the harness has to produce numbers that match on-device execution. An offline-to-on-device divergence silently corrupts gate (b), (c), (e), and every ranking built from them. The current parity harness (commit `7e0ceaa1`) closed one gap — the shape-feature math — but many more remain. The list below enumerates every known divergence between my offline pipeline and on-device execution, prioritised by blast radius. The fix plan after it builds a "harness v2" that eliminates each one.

### Known gaps

#### P0 — corrupts investigation results directly

**Gap 1 — FFT is Python, not firmware.** Offline pipeline runs `np.fft.rfft`; device runs `arm_rfft_fast_f32`. Same inputs, different Radix (2 vs 4), different accumulation order — float32 results differ by 1e-5 to 1e-4 per bin. Feature values computed from these mags inherit the drift. Harms every feature-accuracy claim made from the harness, and hides any future FFT-related firmware change.

**Gap 2 — Raw flux and other state-dependent features not covered.** `SharedSpectralAnalysis::rawSpectralFlux_` reads `prevRawMagnitudes_`; the harness injects one frame at a time with no history, so `computeShapeFeaturesRaw` is the only thing testable today. Raw flux, compressed spectral flux, PLP state — all untestable.

**Gap 3 — Mel bands not in harness output.** Gate (c) (mel-redundancy check) needs per-frame mel bands alongside features. Firmware's `computeRawMelBands` is callable; I just haven't added it to the dump path.

**Gap 4 — The flatness the NN actually consumes is unverified.** Three different flatnesses exist, on different magnitude domains:

- `computeShapeFeaturesRaw` computes a "raw" flatness on pre-compressor `preWhitenMagnitudes_`. This is what my parity harness tests.
- `computeDerivedFeatures` computes `spectralFlatness_` on post-compressor (and possibly post-whitening) `magnitudes_`. This is what `SerialConsole` streams as `"flat"` in the `m` sub-object via `spectral.getSpectralFlatness()`.
- The Python training pipeline's `append_hybrid_features` computes flatness on raw STFT magnitudes (no compression, no whitening).

**The three are numerically different.** The NN was trained on the third; the device streams the second; the parity harness validates the first. Which one actually reaches the NN on-device? Unknown without auditing the `FrameOnsetNN` input path. Until that audit runs, *the harness is silently passing a feature path that may not match what the model consumes.* This is the worst failure of the bunch.

#### P1 — matters for final accuracy, not for ranking

**Gap 5 — TFLite runtime vs TFLite Micro.** The harness (once NN-capable) would load `.tflite` via desktop `tflite-runtime`; device uses `tflite-micro`. They *should* match for INT8 ops, but unverified for our specific Conv1D W16 model. If activations diverge, ablation F1 won't predict device F1.

**Gap 6 — Training-pipeline features use Python code, not firmware code.** Even after my off-by-one fix in `features.py`, the *training* pipeline (`scripts/audio.py` `append_hybrid_features`) uses Python implementations that were never run through a parity test. If those drift from the C++ implementations, the model sees one feature distribution at train time and a different one at inference time. Effect size unknown.

**Gap 7 — Hamming window variant not verified.** Firmware's `applyHammingWindow` — is it periodic (`0.54 - 0.46·cos(2πn/N)`), symmetric (`0.54 - 0.46·cos(2πn/(N-1))`), CMSIS's variant, or a stored table? `np.hamming` is a specific choice. Haven't confirmed equivalence. Small numerical divergence possible.

**Gap 8 — AdaptiveMic / audio scaling chain is skipped offline.** Device: PDM → hardware gain (32) → `AdaptiveMic` window/range normalisation → float samples. Offline: `librosa.load` → `target_rms_db` scale. Different input distributions reach `process()`. For scale-invariant features (flatness, crest, ratios) this doesn't matter. For scale-dependent ones (hfc, raw_flux raw magnitudes, and the NN's mel inputs after log compression) it does.

#### P2 — methodology gaps

**Gap 9 — Offline "NN fires" ≠ on-device "NN fires".** Device fires via `AudioTracker::updatePulseDetection` — local maxima past `pulseOnsetFloor`, gated by bass-ratio, softened by PLP pattern bias, rate-limited by cooldown. The offline "firing" criterion I'd write in Python would be a simple threshold. Different populations → different TP/FP splits. Relative feature rankings might still be valid, absolute numbers aren't.

**Gap 10 — Clean WAV vs mic-captured audio.** Running the offline harness on WAV tests the computation chain but skips the acoustic chain (speaker → room → mic → ADC). `replay_device_capture.py` infrastructure already exists to replay recorded mic audio through the offline pipeline. The true "as-accurate-as-possible without a device" test uses those captures, not clean WAVs.

**Gap 11 — Per-device mic variance averaged away.** Offline tests one configuration; on-device measurements show 8-12 / 18 tracks of cross-device sign agreement. Any feature ranking from one offline config may not transfer. Mitigated by replay (gap 10) using captures from each physical device.

#### P3 — acknowledged but not worth fixing

- PDM mic simulation: unneeded when real device captures exist.
- Absolute bit-for-bit CMSIS-DSP on desktop: float precision is sufficient, full bit-parity is over-engineering.

### Harness v2 architecture — the fix

"The most-parity-accurate offline harness" is the one where the only code that isn't firmware C++ is the WAV reader, the stat aggregation, and the TFLite runtime (which is the same library as TFLite Micro at the op level). Everything else: firmware.

Binary architecture:

```
                 ┌────────────────────────────────────────────┐
                 │                parity_harness               │
                 │                                            │
  WAV/capture ──►│ wav_reader.cpp                             │
                 │     │                                       │
                 │     ▼                                       │
                 │ normalize (match AdaptiveMic chain) ◄── target_rms_db
                 │     │                                       │
                 │     ▼                                       │
                 │ SharedSpectralAnalysis::process() full      │
                 │     ├── applyHammingWindow  (firmware)     │
                 │     ├── computeFFT          (vendored CMSIS-DSP on desktop)
                 │     ├── computeMagnitudesAndPhases (firmware)
                 │     ├── computeRawMelBands  (firmware)     │
                 │     ├── computeShapeFeaturesRaw (firmware) │
                 │     ├── raw_flux            (firmware, with prev-state maintained)
                 │     ├── applyCompressor     (firmware)     │
                 │     ├── computeDerivedFeatures (firmware)  │
                 │     └── spectral_flux        (firmware)    │
                 │     ▼                                       │
                 │ FrameOnsetNN assemble input vector         │
                 │   (exact path audit — gap 4)               │
                 │     │                                       │
                 │     ▼                                       │
                 │ tflite-runtime inference on .tflite        │
                 │     │                                       │
                 │     ▼                                       │
                 │ AudioTracker::updatePulseDetection         │
                 │   (firmware peak-picking + gates)          │
                 │     │                                       │
                 │     ▼                                       │
                 └───► per-frame CSV: audio-sample-start,      │
                       mags, mels, all features, nn_activation,│
                       fire?, gate_states                      │
                                                              │
                  Python driver: stats only (R², |d|, F1)      │
                                                              │
```

All firmware logic runs in-process via compiled firmware sources. The only "Python reference" that remains is stat aggregation.

### Incremental fix order

Each step gets its own commit. Ordered by cost/benefit ratio:

| # | step | fixes gap(s) | rough cost | status |
|---|------|--------------|-----------|--------|
| 1 | Audit which flatness the NN consumes; fix firmware if wrong | 4 | half day | **✅ closed (b136)** |
| 2 | Extend harness to output mel + raw_flux with state + all features | 2, 3 | half day | **✅ closed (b137)** |
| 3 | Audit `scripts/audio.py` feature code against firmware | 6 | 1 day | **✅ closed (2026-04-20)** |
| 4 | Hamming window variant check | 7 | 10 min | **✅ closed by inspection** |
| 5 | AdaptiveMic chain (match or document as intentional skip) | 8 | half day | **⚠ documented, intentionally skipped — see below** |
| 6 | Port `AudioTracker::updatePulseDetection` into harness | 9 | 1 day | pending |
| 7 | Integrate tflite-runtime, verify vs tflite-micro | 5 | half day | pending |
| 8 | Replay-capture mode: feed device-captured raw audio | 10, 11 | 1-2 days (needs firmware raw-PCM capture) | deferred — infra doesn't exist yet |
| 9 | Vendor CMSIS-DSP FFT into harness | 1 | 1-2 days | deferred — current parity holds at 1e-6 without it |

Every commit from this point forward must keep the parity test green (`tests/parity/run_parity_test`) and extend its coverage to any new feature / path it claims to test. A future Claude that sees green "parity OK" output must be able to trust it across the full pipeline, not just the one function the current harness covers.

### Gap closure log

**Gap 4 (flatness routing) — closed b136.** `computeShapeFeaturesRaw` now computes `rawFlatness_` on pre-compressor magnitudes (Wiener entropy with 1e-10 floor per bin, over all 127 non-DC bins — matches Python training code bit-for-bit). `AudioTracker::update` now passes `getRawFlatness()` into `FrameOnsetNN::infer` instead of the compressed `getSpectralFlatness()`. Debug stream gains a new `rflat` field; NN-diagnostic stream's `flat` field now reports the raw flatness the NN actually consumes. The deployed v27-hybrid model was trained on raw-mag flatness but ran on compressed-mag flatness — this was gap 4 in operational form. Any F1 number measured on pre-b136 firmware is contaminated.

**Gaps 2, 3 (state-dependent features + mel in harness output) — closed b137.** Extracted the SuperFlux loop from `process()` into `SharedSpectralAnalysis::computeRawSpectralFluxAndSavePrev()`; added `runRawFeaturesForTest()` public hook that runs the full pre-compressor chain (mel bands → raw flux with prev-state → shape features) on injected magnitudes. Harness dumps all 6 shape features + raw_flux + 30 mel bands per frame. Raw-flux parity: MAE 2e-10 (essentially bit-perfect).

**Gap 6 (training-feature parity) — closed 2026-04-20.** Code-inspection + empirical audit confirms training pipeline feature math matches firmware:

- Flatness: `scripts/audio.py append_hybrid_features` → Wiener entropy on `np.maximum(mags, 1e-10)` over 127 bins = b136 firmware's `rawFlatness_`. Same bit-for-bit.
- Raw flux: training uses bin slices `[0:6]`, `[6:32]`, `[32:]` with weights 0.5 / 0.2 / 0.3 and BASS_COUNT=6, MID_COUNT=26, HIGH_COUNT=95; firmware uses identical constants (BASS_MAX_BIN=6, MID_MAX_BIN=32; bassFluxWeight/midFluxWeight/highFluxWeight defaults 0.5/0.2/0.3). Match.
- Mel bands: already verified MAE=0.0017 / 0.44 INT8 levels (AUDIO_ARCHITECTURE.md).
- Parity harness across 3 tracks (breakbeat-drive, techno-minimal-01, edm-trap-electro) shows all 6 features pass at float-rounding precision.

**Gap 7 (Hamming window) — closed by inspection.** Firmware `initHammingWindow()` computes `0.54 − 0.46·cos(2π·i / (N−1))` for i=0..N−1. `np.hamming(N)` uses the same symmetric formula. The two match within `cosf` vs `np.cos` float precision (negligible).

**Gap 8 (AdaptiveMic chain) — documented as intentional skip.** On-device: PDM mic → hardware gain 32 → AdaptiveMic window/range normalisation → float samples fed to `SharedSpectralAnalysis::process()`. Offline harness / training pipeline: `librosa.load` → `target_rms_db` RMS scale → float samples. These produce different absolute scales but the same *spectral shape* on the same audio source. Every shape feature in the shortlist (centroid, flatness, crest, rolloff, kick_ratio) is scale-invariant; raw_flux and hfc are scale-dependent but we care about discrimination (Cohen's d), which is also scale-invariant. Porting AdaptiveMic to Python would be ~1 day of work and wouldn't change any discrimination metric. Flagged here for awareness; not a blocker.

### Gaps remaining

- **Gap 5 (tflite-runtime vs tflite-micro)** — pending. Matters for any offline NN inference we use to classify TP vs FP. Half day.
- **Gap 9 (peak-picking semantics)** — pending. Matters for TP vs FP classification. 1 day.
- **Gap 10 (device audio capture)** — deferred. Requires new firmware raw-PCM capture stream to exist.
- **Gap 1 (CMSIS-DSP FFT)** — deferred. Float-precision FFT drift is below feature precision; no immediate correctness issue.

## Design decisions

### D1: Selective feature computation + streaming (not ring buffer)

Every candidate feature has an independent runtime flag that gates **both computation and streaming**:

```
stream signal flatness on    # compute AND stream
stream signal centroid off   # compute skipped, field absent from the stream
```

- **Deployed firmware cost:** zero (all flags default off, branch-predicted out).
- **Test-time cost:** proportional to what's enabled.
- **Bandwidth:** only enabled features consume stream bytes.

Rejected alternative: fixed ring buffer of all candidates dumped post-run. That requires always-computing every feature during tests, which changes the detection CPU budget relative to deployment and poisons the baseline we're measuring against.

### D2: Labels come from ground truth, not the NN

The NN's own opinion is explicitly **not** a label source. We want to know whether a feature separates real onsets from real non-onsets, independent of any model. Labeling runs off `onsets_consensus.json` / `.beats.json` only:

- **Onset frame:** within ±50 ms of any GT onset.
- **Non-onset frame:** everything else.

No "true positive" vs "false positive" language in Phase 1 — that framing bakes in NN bias and answers a different question ("which features explain the NN's mistakes"). That question is interesting but downstream: if a feature passes Phase 1–3, *then* we can ask whether adding it to the NN reduces FPs (Phase 4 Path B).

### D3: Firmware-to-Python parity is a first-class acceptance criterion

Every signal the firmware computes must match its Python reference within a documented MAE. The existing mel-pipeline parity (MAE = 0.44 INT8 levels, see `AUDIO_ARCHITECTURE.md`) is the template. Without parity, any offline-vs-device discrepancy is ambiguous: it could be acoustic-chain drift OR an implementation bug. Parity tests rule out the implementation side before we invest in on-device analysis.

## Candidate signal catalog

Ten candidates grouped by family.

| Signal | Family | Rationale | Cost estimate (nRF52840 frame) |
|--------|--------|-----------|---------------------------------|
| Spectral flatness (Wiener entropy) | Shape | Tonal↔noise discriminator; drums peaky, sustained tones flat | ~0.3 ms (deployed) |
| Raw spectral flux (SuperFlux) | Transient | Magnitude attack rate; peaks at broadband onsets | ~0.2 ms (deployed) |
| Complex spectral difference | Transient | Phase-aware flux; suppresses steady-state tones even at loud levels | ~0.8 ms (phase history needed) |
| High-frequency content (HFC) | Shape | Percussion has broadband high-freq energy; tonal impulses are low-freq-dominant | ~0.2 ms |
| Weighted phase deviation (WPD) | Phase | Measures how far phase drifts from linear prediction; tonal tones are phase-stable | ~0.7 ms |
| Kick-band / total ratio | Energy | Explicit bass-dominance test (generalizes current bass-gate) | ~0.1 ms |
| Crest factor (peak / RMS) | Shape | Transients have high crest; sustained tones have low crest | ~0.3 ms |
| Rényi entropy (α=2) | Shape | Alternative noisiness measure, more robust to quantization than flatness | ~0.4 ms |
| Spectral centroid | Shape | Where the spectral "mass" is; drums shift centroid per hit | ~0.2 ms |
| Spectral rolloff (85th percentile) | Shape | Bandwidth proxy; tonal impulses are narrow-band | ~0.3 ms |

Total if all enabled: ~3.5 ms/frame. At 62.5 Hz that's 22 % CPU — unacceptable for deployment but fine when measuring a subset during tests.

## Phase plan

### Phase 1 — Does the signal exist? (Python, no NN)

**Input:** GT corpus (EDM tracks + consensus onsets) and, optionally, synthetic tonal clips (pure chords, synth pads, vocals).
**Compute:** all 10 candidate features per frame.
**Label:** onset (within ±50 ms of any GT onset) or non-onset.
**Report per feature:** mean / std / distribution per class, Cohen's d (onset vs non-onset), ROC-AUC as a standalone classifier, KS statistic.
**Aggregate across corpus:** per-track-weighted mean of |d|, AUC, KS; rank by |d|.
**Keep:** top 6 for Phase 2. Drop anything with |d| < 0.1 or inconsistent signed d across tracks.

**Deliverable:** ranked shortlist appended to this doc. Code in `ml-training/analysis/`.

### Phase 2 — Make it measurable on-device

Three chunks of work, in this order:

**2a. Firmware-to-Python parity tests.** For each shortlisted signal: a Python reference matching the firmware implementation line-by-line, a short audio fixture, and an assertion on MAE (target ≤ 0.005 in normalized units, ≈ INT8 granularity). Reuse the pattern from `ml-training/verify_mel_pipeline.py`. A feature fails this stage if the firmware version disagrees with its Python reference — that's an implementation bug, not an acoustic-chain issue, and it must be fixed before any on-device measurement.

**2b. Selective streaming infrastructure.**

- Firmware: `SignalRegistry` with one entry per candidate (name, compute_fn, flag). `SerialConsole` gets `stream signal <name> on|off`. Stream tick iterates the registry and emits `"signals":{"flat":0.34, "hfc":0.81, ...}` with only enabled signals. Compute gated by flag — zero cost when disabled.
- Server: `test_session.py` ingests arbitrary signal fields under `signals` into a generic `SignalFrame` list (replacing the hardcoded `NNFrame`). `scoring.py` generalizes `HybridMetrics` so every streamed signal gets its own onset-vs-non-onset stats. `test_runner.py` accepts `signals: [str]` in validate/sweep requests and issues `stream signal X on` before the run.

**2c. Acceptance check.** A validate request with `signals=["flatness","hfc"]` produces a capture with both fields populated; `signals=[]` produces a capture with no signal fields (confirms zero-cost baseline when no investigation is active).

### Phase 3 — Does the signal survive the acoustic chain?

Play the same GT tracks through two paths:

- **Offline:** Python (Phase 1 code).
- **On-device:** validation harness with the signal flags enabled on all 4 devices.

Align frames by timestamp (within 1 hop = 16 ms). Per signal, compute:

- **Pearson correlation** between offline and on-device values on matched frames — primary "does the signal survive?" metric.
- **KS statistic** on the two distributions — catches distribution-level drift that correlation misses.
- **On-device Cohen's d** (onset vs non-onset, same labels as Phase 1) — does the offline discrimination carry over?
- **Cross-device std** across the 4 devices — are the devices seeing the same signal?

**Deployability score:**

```
score = |d_ondevice| × corr_offline_device × (1 / max(ε, cross_device_std_normalized))
```

with `ε = 0.01` to prevent a perfectly consistent signal from scoring infinitely. High score = real signal, survives the chain, consistent across devices.

**Diagnostic for low correlation:** if a feature has good offline |d| but poor offline-vs-device correlation, investigate before dropping. Typical suspects are (a) a firmware normalization path the offline code doesn't replicate, (b) noise-subtraction that kills the signal differently on device, (c) INT8 quantization clipping the dynamic range. Fixing any of these closes the on-device gap for this feature and often for others too. Log findings under "gap-closing candidates" in the Phase 3 writeup.

**Deliverable:** decision matrix appended to this doc listing per-signal offline |d|, on-device |d|, offline↔device correlation, cross-device std, final score, and any gap-closing notes.

### Phase 4 — Act on the shortlist

Two paths run in parallel; whichever lands first wins.

**Path A: deterministic FP gates.** For each top-ranked signal, add a threshold-based gate in the onset detection code (extending the current bass-gate pattern). Sweep thresholds via `run_param_sweep`. Cheap to try, cheap to revert.

**Path B: retrain NN with new features.** Add the top 2–3 signals as additional NN input channels (bumps 32 → 34–35). Retrain v28, evaluate, deploy if F1 improves by ≥ 0.03.

Expected outcome: one new deterministic gate (cheap) + one new NN input feature (expensive but higher ceiling). Neither is guaranteed — a shortlist that all fails Phase 3 means the acoustic chain is eating every signal and we need to work on that chain instead.

**Phase 4 exit gate — strip losing candidates from firmware.** Before any Phase 4 work merges, every candidate feature that did **not** make the shortlist must be deleted from the firmware (computation + flag + stream field), not just flag-defaulted-off. Run `grep -r 'SignalRegistry' blinky-things/` to confirm only the shipped signals remain. Investigation-only code that lingers in deployment firmware is how flash budget leaks — see the `v21`–`v26` cleanup in b132 for a recent cautionary tale.

## Open questions / risks

1. **Parity cost.** Each firmware feature needs a Python reference and a parity test. For 6 signals that's ~6 days of careful work. Reuse `ml-training/verify_mel_pipeline.py` as a template to keep per-signal cost to ~1 day.

2. **Phase history RAM.** Complex SD and WPD both need per-bin phase memory (128 floats × 2 = 1 KB RAM each). Acceptable on nRF52840 (256 KB total, ~20 KB globals + arena currently), but only if the buffer is shared across signals that need it.

3. **Label noise from unlabeled onsets.** GT labels are kick/snare-weighted. An NN or feature firing on a hi-hat counts as "non-onset" under current labels but is actually a real onset. Mitigations: (a) use drum-stem-labeled consensus onsets (already in use — `onsets_consensus.json`), (b) include synthetic tonals as a clean "all firings are wrong" corpus, (c) spot-check the strongest apparent disagreements manually.

4. **Investigation ≠ deployment.** The test-time firmware has feature flags and compute paths that deployed firmware shouldn't carry. When Phase 4 picks winners, strip unused candidates — see Phase 4 exit gate above.

5. **Scope creep.** The catalog could grow indefinitely (50+ onset-detection features in the literature). Start with the 10 listed; only expand if Phase 3 scores are disappointingly low across the entire shortlist.

## References

- `docs/AUDIO_ARCHITECTURE.md` — current v27-hybrid architecture and the 32-input feature set.
- `docs/ML_IMPROVEMENT_PLAN.md` — NN training roadmap.
- `blinky-server/blinky_server/testing/scoring.py` `HybridMetrics` — prototype scoring shipped in b132 for `flat`/`rflux`. Generalize in Phase 2.
- `ml-training/verify_mel_pipeline.py` — parity template for Phase 2a.
- Bello et al., *A Tutorial on Onset Detection in Music Signals*, IEEE TSAP 2005 — most of the candidate catalog derives from Section III.

---

## Phase 1 results — 2026-04-20

**Percussion corpus:** 18 EDM tracks, 9,763 GT onsets total.
**Tonal corpus:** 9 synthetic tracks (`generate_tonal_corpus.py`), 720 impulses — sine/saw/FM stabs, detuned leads, harmonic stacks, bass note attacks, vocal-formant bursts, pad+stab mix. Pure tonal content only, known impulse times.
**Labeling:** per-onset peak — one sample per GT onset = max feature value within ±50 ms. Non-onset frames = at least 100 ms from every GT onset. No NN.
**Code:** `ml-training/analysis/features.py` + `generate_tonal_corpus.py` + `run_catalog.py`.
Regenerate with

```
./venv/bin/python -m analysis.generate_tonal_corpus --out ../blinky-test-player/music/synthetic_tonals
./venv/bin/python -m analysis.run_catalog --perc-corpus ../blinky-test-player/music/edm --tonal-corpus ../blinky-test-player/music/synthetic_tonals --out outputs/feature_catalog
```

### Primary metric: |d| between percussion and tonal-impulse peaks

|d| and the AUC distance from 0.5 measure **how separable the two classes are**. Sign is metadata — "higher on tonals" is exactly as discriminative as "higher on drums", you just flip the comparison direction. For an NN input, the model learns direction automatically; for a deterministic gate, you invert the threshold. Sign only disqualifies a feature if it's **unstable across different tonal contexts** (a threshold tuned on one corpus breaks on another). That's what Phase 2d below tests.

| rank | feature | \|d\| | AUC distance from 0.5 | sign (perc − tonal) | perc_mean | tonal_mean |
|-----:|---------|-----:|-----:|:----:|---------:|---------:|
| 1 | renyi | 2.05 | 0.368 | − | 2.10 | 16.27 |
| 2 | centroid | 1.37 | 0.331 | + | 37.12 | 19.66 |
| 3 | flatness | 1.23 | 0.313 | + | 0.490 | 0.260 |
| 4 | raw_flux | 1.15 | 0.316 | − | 0.143 | 0.385 |
| 5 | kick_ratio | 1.13 | 0.310 | + | 0.887 | 0.547 |
| 6 | hfc | 1.13 | 0.357 | − | 64.7 | 313.4 |
| 7 | crest | 1.00 | 0.265 | + | 9.03 | 7.43 |
| 8 | complex_sd | 0.89 | 0.252 | − | 13.4 | 22.6 |
| 9 | rolloff | 0.80 | 0.125 | + | 36.98 | 16.96 |
| 10 | wpd | 0.29 | 0.070 | − | 1.58 | 1.67 |

Nine features have |d| ≥ 0.8 and AUC distance ≥ 0.13. All nine are usable for discrimination; they differ in *how* they can be used and *how stable* their direction is.

**Within-corpus sanity checks** (per-onset peak vs non-onset frame) confirm every feature fires at real onsets — percussion-corpus |d| ranges 0.59–1.21 with AUC 0.71–0.86 across the full catalog. That's a sanity check that the feature implementation is doing something, not a discrimination score.

### Findings

1. **Nine features with useful |d|, in two groups by sign.** Positive-sign (`centroid`, `flatness`, `kick_ratio`, `crest`, `rolloff`): drums score higher than tonals. Negative-sign (`renyi`, `raw_flux`, `hfc`, `complex_sd`): tonals score higher than drums on this corpus. All measure something real.

2. **The sign split has a mechanical explanation.** The negative-sign features all measure *total spectral activity* — flux sums energy changes, HFC sums bin-weighted energy, complex-SD sums residuals, Rényi on power distribution. The synthetic corpus has silence between impulses and sharp digital attacks with rich harmonic stacks, so a tonal impulse produces an enormous total-energy jump from zero. Real music has continuous baseline energy and softer attacks, so the relative jump is smaller. The positive-sign features (centroid, flatness, kick_ratio, crest) are spectral *shape* metrics — normalized ratios that describe what the spectrum looks like at the peak, independent of total energy. Shape signs are likely more stable across corpora; activity signs probably aren't.

3. **`raw_flux` is both an NN input AND context-dependent.** Already shipping (b132). The NN can and presumably does learn some useful direction from it, but if the sign direction differs between "synthetic silence-interrupted tonals" and "real continuous music with tonal stabs", a raw_flux-driven decision boundary inside the model is fragile. Worth checking by ablation (Phase 2d) whether raw_flux actually helps model F1 or whether it's neutral/hurtful — especially since it's one of just two non-mel input channels.

### Phase 2 shortlists by use case

**NN input candidates (Path B) — all 9.** The model learns direction. All nine features with |d| ≥ 0.8 are on the table; the 4 not yet in the NN (centroid, kick_ratio, crest, plus any of the reversed-sign candidates that survive Phase 2d) are the concrete additions.

**Deterministic gate candidates (Path A) — 4 sign-stable ones.** Until Phase 2d proves otherwise, only the shape-metric features get a gate: `centroid`, `flatness`, `kick_ratio`, `crest`. All are low-cost (~0.1–0.3 ms) and need no phase history. A gate with an unstable direction across corpora is a bug waiting to happen.

**Firmware parity tests (Phase 2a) priority order:**
  1. `flatness` — already shipping, parity-test first to confirm the firmware implementation matches the Python reference (fast, high-confidence win).
  2. `kick_ratio` — cheap and directly generalizes the existing bass-gate.
  3. `centroid` — cheap, unlocked new NN input channel.
  4. `crest` — cheap, transient-character signal missing from current NN inputs.
  5–9. Reversed-sign features — only after Phase 2d confirms sign stability.

### Phase 2d — Sign stability test

Purpose: confirm that the sign of each feature's cross-corpus d is stable when the tonal corpus changes. A feature whose sign flips between different tonal contexts cannot be used as a single-threshold gate — the threshold direction is context-dependent, which breaks under any deployment that doesn't match the tuning corpus.

Concrete test:
1. Regenerate the tonal corpus with a second variant: `generate_tonal_corpus.py --variant embedded` — adds a continuous tonal bed under every impulse and multiplies attack times by 4×, simulating tonal content inside continuous music rather than silence-interrupted stabs.
2. Run `run_catalog.py` against each variant separately.
3. For each feature, record sign and |d| on both corpora. **Stable** = signs agree; **unstable** = signs differ.

### Phase 2d results — 2026-04-20

| feature | d (clean) | d (embedded) | AUC (clean) | AUC (embedded) | verdict |
|---------|----------:|------------:|------------:|--------------:|---------|
| centroid | +1.37 | +0.81 | 0.831 | 0.714 | **stable +** |
| flatness | +1.23 | +0.53 | 0.813 | 0.672 | **stable +** |
| crest | +1.00 | +0.66 | 0.765 | 0.709 | **stable +** |
| rolloff | +0.80 | +0.99 | 0.625 | 0.679 | **stable +** |
| hfc | −1.13 | −1.01 | 0.143 | 0.189 | **stable −** |
| raw_flux | −1.15 | −0.53 | 0.184 | 0.298 | **stable −** |
| complex_sd | −0.89 | −0.50 | 0.248 | 0.346 | **stable −** |
| kick_ratio | +1.13 | −0.66 | 0.810 | 0.333 | **UNSTABLE** (+ → −) |
| renyi | −2.05 | +0.32 | 0.132 | 0.587 | **UNSTABLE** (− → +) |
| wpd | −0.29 | +0.29 | 0.430 | 0.601 | weak (|d|<0.3 both) |

**Two genuine surprises.**

1. **`kick_ratio` is sign-unstable.** On silence-separated synthetic tonals, drums have a higher bass/total ratio than tonals (kicks dominate the spectrum). But when the tonal corpus has a low-frequency bed (my embedded variant uses 180 Hz + 270 Hz sustained voices), the bass/total ratio is permanently high regardless of what impulses fire — so the sign flips. This invalidates the "generalized bass-gate" hypothesis: the on-device bass gate works well against silence backgrounds but would fail against bass-heavy musical context. A kick_ratio gate tuned on one corpus would fire the wrong way on the other.

2. **`rolloff` is the cheapest sign-stable discriminator that wasn't on any prior shortlist.** Clean |d|=0.80 was modest; embedded |d|=0.99 actually *improved*. Rolloff cost is ~0.3 ms, same family as the other shape metrics. Promote it to the gate shortlist.

**Unexpected stability.** The three "reversed" features (`raw_flux`, `hfc`, `complex_sd`) all kept their sign when the context changed — tonal impulses consistently produced a larger total spectral activity change than drum hits on both corpora. Magnitude |d| dropped in the embedded variant (context-energy reduces the relative delta) but the direction held. That makes the reversed sign a robust signal, not a synthetic-corpus artifact.

**Three feature groups for Phase 2:**

| Group | Features | Use |
|-------|----------|-----|
| **Stable +** (drum > tonal) | centroid, flatness, crest, rolloff | Deterministic gates (all directions intuitive). NN inputs. |
| **Stable −** (tonal > drum) | raw_flux, hfc, complex_sd | NN inputs. Gates with inverted threshold (fire if LOW). |
| **Unstable** | kick_ratio, renyi | NN-only. Do NOT use as single-threshold gates. |

**Dropped for low |d| on both corpora:** `wpd` (|d| 0.29 / 0.29, mixed sign).

### Revised Phase 2 shortlist

**Phase 2a firmware parity tests (priority order):**
1. `flatness` — already shipping, validate existing implementation against Python reference.
2. `centroid` — cheap (~0.2 ms), no phase history, new NN input candidate.
3. `crest` — cheap (~0.3 ms), new NN input candidate.
4. `rolloff` — cheap (~0.3 ms), surprise winner from Phase 2d.
5. `hfc` — cheap (~0.2 ms), stable − so usable as inverted gate. NN input candidate once sign direction is confirmed in training.

Seven parity tests total (the four stable-+ plus raw_flux, hfc, complex_sd). Three implementations already exist in firmware (flatness, raw_flux, plus the bass-energy math used by kick_ratio and the existing gate). Four new implementations: centroid, crest, rolloff, hfc, complex_sd.

**Phase 2b selective streaming** then covers these seven. `kick_ratio` and `renyi` get flagged as NN-only and are excluded from gate-infrastructure work.

**Phase 2d ablation of raw_flux in the NN** remains a parallel task. The feature is stable-negative (useful to the model if it learns the inverted direction), but we don't yet know whether the current v27-hybrid model is learning that or is just noise-fitting on it.

### Held-out stability check — 2026-04-20

Concern: we'd iterated methodology on the same 18-track corpus three times (per-onset peak vs frame-mean, GT-only vs NN-oracle labels, clean vs embedded tonal variants). Each round could be tightening results to this specific corpus rather than finding generalizable discriminators.

Test: random 13/5 split (seed=42) of the percussion corpus, recompute cross-corpus d against both tonal variants for each half independently. Sign and magnitude should be stable across splits if the ranking isn't corpus-overfit.

Result — every feature's sign verdict is identical across full / kept-13 / held-out-5, and |d| values agree within ~0.15 on every feature. `renyi`, `kick_ratio`, and `wpd` remain classified UNSTABLE (sign flips between variants) on every split; the other seven remain STABLE. **The shortlist is not an overfit artifact** of which 18 EDM tracks we picked. EDM-only is the target scope (see Goal), so this is sufficient stability evidence to proceed.

### Training-set contamination — 2026-04-20 — **ACTION REQUIRED**

While setting up the held-out check, discovered: the 18 EDM tracks in `blinky-test-player/music/edm/` are byte-identical with files inside the v27-hybrid training corpus at `/mnt/storage/blinky-ml-data/audio/combined/`, with the same filenames.

Per `/mnt/storage/blinky-ml-data/processed_v27/.prep_splits.json`:

- **14 of 18** are in **train**: afrobeat-feelgood-groove, amapiano-vibez, breakbeat-background, breakbeat-drive, dnb-energetic-breakbeat, edm-trap-electro, garage-uk-2step, techno-deep-ambience, techno-dub-groove, techno-machine-drum, techno-minimal-01, techno-minimal-emotion, trance-goa-mantra, trance-party
- **4 of 18** are in **val** (used for model selection): dnb-liquid-jungle, dubstep-edm-halftime, reggaeton-fuego-lento, trance-infected-vibes
- **0** are in a held-out test split. The v27 pipeline split `train` + `val` and ran no blind-test evaluation.

**What this affects.**

- **Phase 1 feature ranking (this document):** unaffected. No NN is involved in feature computation or labeling, so training exposure does not contaminate the onset-vs-tonal Cohen's d.
- **On-device F1 = 0.63 reported for v27-hybrid (`AUDIO_ARCHITECTURE.md`):** *optimistic*. The reported number was measured (directly or indirectly) on tracks the model trained on or was selected against. Expect lower F1 on truly unseen music.
- **Phase 2d raw_flux ablation:** would be contaminated if run on these 18 tracks. Needs a held-out corpus (see below).
- **Any future Path B retrain evaluation:** must use an explicit held-out test split, or the F1 improvement threshold loses meaning.

**Action items.**

1. **Carve a true test split for future NN evaluations (EDM only).** Source EDM tracks that were not in `processed_v27/.prep_splits.json` and generate `onsets_consensus.json` GT for them. Re-split with an explicit `test` bucket in the next data-prep run. The v27 training should be considered "train + val only — no blind test"; any F1 number from it is an upper bound.
2. **Before any Phase 2d NN ablation**, obtain a held-out EDM set. Candidates inside `/mnt/storage/blinky-ml-data/`: `labels/edm-test/` (already has `.beats.json` GT — verify none are in v27 splits), `labels/fma/` EDM-tagged tracks (filter out any already in train/val).
3. **Update `AUDIO_ARCHITECTURE.md` and `IMPROVEMENT_PLAN.md`** to reflect that v27-hybrid F1 numbers are on train+val, not a blind test. Current numbers should not be compared against published benchmarks that use held-out test sets.
4. **Phase 3 feature-level offline-vs-device comparison is still valid** on these 18 tracks — features are computed from audio directly, not through the model — so on-device feature capture work doesn't need to wait.

This investigation's feature rankings stand. The NN evaluation methodology around them does not.

### Held-out EDM validation — 2026-04-20

Located 25 GiantSteps tracks inside the local ML-data storage that are **not** in `processed_v27/.prep_splits.json` (neither train nor val). Each has `.beats.json` GT already generated. Materialized as symlinks under `blinky-test-player/music/edm_holdout/` via `analysis/setup_holdout_corpus.py`.

Re-ran Phase 1 cross-corpus Cohen's d against both tonal variants on this truly-unseen corpus:

| feature | seen d_clean | seen d_emb | **holdout d_clean** | **holdout d_emb** | verdict |
|---------|-----:|-----:|-----:|-----:|---------|
| renyi | −2.05 | +0.32 | −2.02 | +0.60 | UNSTABLE (confirmed) |
| centroid | +1.37 | +0.81 | **+1.62** | **+1.07** | STABLE + (stronger on holdout) |
| flatness | +1.23 | +0.53 | **+1.43** | **+0.81** | STABLE + (stronger on holdout) |
| raw_flux | −1.15 | −0.53 | −1.16 | −0.50 | STABLE − |
| kick_ratio | +1.13 | −0.66 | +0.87 | −0.89 | UNSTABLE (confirmed) |
| hfc | −1.13 | −1.01 | −1.02 | −0.86 | STABLE − |
| crest | +1.00 | +0.66 | +0.77 | +0.38 | STABLE + (weaker on holdout) |
| complex_sd | −0.89 | −0.50 | −0.56 | **−0.12** | STABLE − but near-zero on holdout+embedded |
| rolloff | +0.80 | +0.99 | **+1.13** | **+1.34** | STABLE + (stronger on holdout) |
| wpd | −0.29 | +0.29 | −0.01 | +0.64 | UNSTABLE (confirmed) |

**Every STABLE/UNSTABLE classification holds** on the held-out corpus. `centroid`, `flatness`, and `rolloff` are *stronger* on unseen data than on the seen corpus — the Phase 1 shortlist isn't an artifact of models or tracks the training pipeline saw.

**One borderline:** `complex_sd` weakens substantially on held-out + embedded (d = −0.115, nearly zero). It's still sign-stable — just barely. Keep it as an NN input candidate but demote it from the deterministic-gate shortlist until the sign magnitude is confirmed against a larger held-out corpus. Cheap to re-test once we expand the held-out pool.

**Phase 2a parity-test priority is unchanged.** `flatness`, `centroid`, `crest`, `rolloff` as stable-+ gates; `hfc`, `raw_flux` as stable-− NN inputs and inverted-threshold gate candidates; `complex_sd` NN-only pending larger held-out corpus; `kick_ratio`, `renyi` NN-only (unstable); `wpd` dropped.

### Phase 3 first measurement — 2026-04-20

b133 deployed to all 4 serial devices with centroid/crest/rolloff/hfc added to the debug stream. Full 18-track validation run against `blinky-test-player/music/edm/` produced 71 per-device-per-track observations (18 tracks × 4 devices, one run per combination). The server's new per-signal `SignalGapStats` computes a Cohen's d for each.

**On-device vs offline Cohen's d (mean across devices and tracks):**

| signal | offline d | on-device d (μ ± σ) | on-device \|d\| μ | signs agree across devices |
|--------|----------:|--------------------:|----------------:|---------------------------:|
| centroid | +1.367 | +0.012 ± 0.237 | 0.161 | 3 / 18 tracks |
| flatness | +1.233 | +0.008 ± 0.230 | 0.159 | 3 / 18 tracks |
| raw_flux | −1.147 | −0.038 ± 0.154 | 0.124 | 3 / 18 tracks |
| hfc | −1.128 | −0.027 ± 0.214 | 0.156 | 5 / 18 tracks |
| crest | +1.000 | −0.003 ± 0.159 | 0.125 | 7 / 18 tracks |
| rolloff | +0.795 | +0.007 ± 0.259 | 0.139 | 6 / 18 tracks |

**Two severe problems.**

1. **|d| is 8-10× smaller on-device.** Offline all six signals had |d| > 0.8; on-device |d| mean is 0.12-0.16. The acoustic chain (speaker → room → 4 different mics → INT8 front-end) is eating most of the discrimination.
2. **Cross-device sign disagreement.** Only 17-39% of tracks have all 4 devices agreeing on even the sign of d. On the remaining 11-15 tracks, individual devices report +d while others report −d on the same signal on the same track. This is not acoustic-chain attenuation — this is the feature responding to different things on different devices.

**Hypotheses, in order of likelihood.**

- **a) Firmware frames vs GT alignment is noisy.** Signal frames are timestamped with server wall-clock (`time.time()`), not firmware `millis()`. Transients use firmware timestamps via `clock_offset`. These live in the same coordinate after conversion but signal frames carry USB jitter (~2–10 ms). The existing `audio_latency_ms` estimate comes from *transients*, which are firmware-timestamped. Applying a transient-derived offset to server-timestamped frames is the wrong coordinate system. With a 50 ms onset window, even 10 ms of systematic misalignment puts a noticeable fraction of frames on the wrong side of the classification. **Cheap fix**: add a `ts` field to the music stream in firmware and use firmware millis for signal frames too.
- **b) Frame-average vs per-onset peak.** Offline analysis used per-onset peak (single sample = max in ±50 ms around each GT onset). On-device scoring uses frame-average over every frame in the onset window. For transient-sharp features (centroid, hfc, rolloff) the peak is 1-2 frames; the ±50 ms window includes many non-peak frames that dilute the onset mean toward the non-onset mean. **Cheap fix**: mirror offline methodology on-device — per-GT-onset-peak.
- **c) Unverified firmware implementations.** Without a parity test (Phase 2a harness still pending), we can't rule out that the new C++ shape features disagree with their Python reference beyond acoustic-chain effects. The existing `flatness` uses compressed magnitudes in firmware while my Python reference uses raw STFT — already a known mismatch. The new ones are on pre-compressor magnitudes and should match, but "should" isn't "verified".
- **d) Real acoustic-chain degradation.** Even if (a)-(c) are all addressed, some portion of the gap is likely genuine — MEMS mics attenuate highs, the room adds reflections, INT8 quantization on widely-varying levels clips dynamic range. Some features might just not survive.

**Next priority:**

1. **Fix frame timestamp coordinate system** (hypothesis a). Add firmware `millis()` to the music stream, thread through `test_session.py`, retest. Cheapest first.
2. **Implement per-onset peak on-device** (hypothesis b). Mirrors offline Phase 1 methodology exactly. Should show whether features are merely diluted or actually lost.
3. **Standalone parity harness** (hypothesis c). Still worth building — rules out implementation-vs-reference bugs and becomes permanent CI regression protection.

Only after all three are in place can we confidently say (d) is the residual. Until then, Phase 4 gate / retrain decisions have no reliable signal.

### Phase 3 second measurement — 2026-04-20

b134 firmware (ts in music stream) + server with per-onset-peak scoring alongside frame mode. Same 18-track corpus, 4 devices, 1 run.

| signal | offline d | **frame mode on-device d** | **peak mode on-device d** | sign agree (peak) |
|--------|----------:|---------------------------:|--------------------------:|------------------:|
| centroid | +1.367 | +0.006 (\|d\| 0.12) | **+0.634** (\|d\| 0.67, 46%) | 9/18 tracks |
| flatness | +1.233 | +0.007 (\|d\| 0.12) | **+0.664** (\|d\| 0.71, 54%) | 9/18 |
| raw_flux | −1.147 | +0.010 (\|d\| 0.12) sign FLIP | **+0.622** (\|d\| 0.67, sign FLIP vs offline) | 10/18 |
| hfc | −1.128 | +0.040 sign FLIP | **+0.572** (sign FLIP vs offline) | 8/18 |
| crest | +1.000 | −0.002 sign FLIP | **+0.706** (\|d\| 0.74, 71%) | 12/18 |
| rolloff | +0.795 | +0.005 | **+0.448** (\|d\| 0.48, 56%) | 11/18 |

**Two headline findings.**

1. **Per-onset peak recovers 46-74 % of offline discrimination.** Frame-average was the dominant dilutor. The ±50 ms window at ~100 Hz captures many non-peak frames that look like non-onset content; averaging them pulls the onset mean toward the non-onset mean. Taking one sample per GT onset (the max within the window) eliminates that dilution and the features discriminate with \|d\| 0.5-0.75 — moderate to strong by Cohen's thresholds. The frame-average approach used in the first measurement was giving us \|d\| ≈ 0.1, essentially useless.

2. **The "reversed sign" features from offline cross-corpus analysis were a synthetic-corpus artifact, not a real property.** On-device peak mode shows `raw_flux`, `hfc`, and `complex_sd`-family features all have **positive** gap (drum peaks > ongoing music content). The synthetic tonal corpus had near-instant attacks over silence — so tonal impulse peaks genuinely exceeded drum peaks in those *synthetic* conditions. But real music has continuous tonal content, and drum peaks are louder than the ongoing musical floor for every one of the six measured features. This simplifies the Phase 2 shortlist: **all six features are stable-positive on real music**, usable as direct-threshold gates or NN inputs with the natural direction.

**Cross-device sign agreement** also recovered: frame mode had 5-7/18 tracks with all devices agreeing; peak mode has 8-12/18. Still not perfect, but the signal is genuine per-device — it was being buried by non-peak frames.

**Crest is the most robust feature on-device:** highest \|d\| (0.74), best cross-device agreement (12/18), fully positive on all 4 devices across most tracks.

### Revised shortlist (post-Phase-3)

Unified direction: all six features are positive-sign discriminators on real music.

| feature | on-device peak \|d\| | sign-agree tracks | notes |
|---------|---------------------:|------------------:|-------|
| crest | 0.74 | 12/18 | Cheapest compute, strongest on-device signal. **Primary.** |
| flatness | 0.71 | 9/18 | Already NN input. |
| centroid | 0.67 | 9/18 | Cheap, new NN input candidate. |
| raw_flux | 0.67 | 10/18 | Already NN input; sign direction corrected from synthetic offline. |
| hfc | 0.62 | 8/18 | New NN input candidate. |
| rolloff | 0.48 | 11/18 | Weakest but most stable cross-device. |

**Phase 2d "sign stability" test is retrospectively invalidated** for features whose synthetic-offline sign disagreed with real-music behavior. The on-device real-music measurement is the source of truth; offline Phase 1 is still useful for ranking magnitude and catching implementation bugs, but not for direction.

**Phase 2a parity harness still wanted** to rule out any residual C++ / Python implementation drift. With \|d\| in the 0.5-0.7 range, a 10-20 % implementation error could still matter for gate-threshold selection in Phase 4.

### Post-gap-4 measurement on b137 — 2026-04-20 (the honest numbers)

After the flatness-routing fix (b136) and the native parity-harness
expansion (b137), re-ran validation twice — once on the 18-track
in-corpus set, once on the 25-track held-out GS set (edm_holdout).
The held-out numbers are the authoritative measurement; the in-corpus
ones are shown for comparison to earlier rounds only.

**Peak-mode per-signal |d| — 3 rounds side by side:**

| signal | b134 in-corpus \|d\| | b137 in-corpus \|d\| | **b137 held-out \|d\|** | sign+ on held-out |
|--------|:-:|:-:|:-:|:-:|
| crest | 0.735 | 0.931 | **0.964** | 100/100 |
| flatness | 0.708 | 0.857 | **0.887** | 100/100 |
| raw_flux | 0.671 | 0.779 | **0.843** | 98/100 |
| centroid | 0.668 | 0.826 | **0.767** | 100/100 |
| hfc | 0.615 | 0.679 | **0.673** | 95/100 |
| rolloff | 0.476 | 0.535 | **0.622** | 99/100 |

**Three things changed at once between b134 and b137 held-out**, so the +0.10 to +0.23 |d| gains can't be cleanly attributed to just the gap-4 fix. All three moved in the same direction so it doesn't matter for decisions:

1. Flatness routing fixed (gap 4 — the big one).
2. Feature bin-indexing off-by-one in Python reference corrected (doesn't affect on-device numbers but affects Python↔firmware comparisons).
3. Tested on held-out content instead of training-contaminated.

**Cross-device sign agreement is now near-perfect.** The b134 numbers showed only 5-12/18 tracks where all 4 devices agreed on sign — that was the "features don't work reliably fleet-wide" problem. Held-out b137 shows 95-100% sign agreement across all 100 observations. The fleet-wide consistency was not a feature problem; it was a flatness-routing contamination problem.

**On-device onset F1 = 0.398 on held-out**, vs 0.628 that CLAUDE.md was reporting. That earlier number was measured on the 18 training-contaminated tracks and is an upper bound. Every subsequent F1 claim must use the held-out corpus.

**Net finding:** with the harness-v2 parity gaps closed, all six shortlist features produce strong, sign-stable on-device discrimination at the peak moment. The question is no longer "do the features carry signal?" (they do) — it's "which combinations help the NN?" — gates (b), (c), (d), (e) below.

### Phase 4 Path A — crest-gate sweep (NULL result) — 2026-04-20

b135 adds a single-threshold `crestGateMin` parameter that suppresses NN-triggered pulses when the current-frame crest factor is below the threshold. Sweep on one device (ABFBC4) across 6 EDM tracks at values [0, 2, 4, 5, 6, 7]:

| crestGateMin | F1 | precision | recall | detections | notes |
|-------------:|---:|----------:|-------:|-----------:|-------|
| 0 (disabled) | **0.573** | 0.551 | 0.610 | 110.7 | baseline |
| 2 | 0.556 | 0.528 | 0.599 | 114.0 | noise — gate barely fires |
| 4 | 0.547 | 0.519 | 0.592 | 114.5 | same |
| 5 | 0.529 | 0.528 | 0.563 | 108.7 | F1 drops |
| 6 | 0.530 | 0.546 | 0.539 | 102.2 | F1 still below baseline |
| 7 | 0.475 | 0.541 | 0.450 | 86.5 | heavy suppression, F1 tanks |

**At every threshold, F1 is below baseline.** Precision barely improves (best case +0 %), recall drops hard (up to −26 %). At value=7 the gate kills 27 % of TPs while only killing 16 % of FPs — it's *anti-selective*.

### Why — methodological gap in Phase 1 / Phase 3

Phase 1 and Phase 3 measured crest |d| ≈ 0.7 between:
- **onset pool**: frames within ±50 ms of a GT onset (regardless of NN behavior), and
- **non-onset pool**: frames ≥ 100 ms from any GT onset.

A deterministic gate fires at the moment the NN triggers — which is a subset of frames, heavily concentrated near real onsets AND at broadband spectral events the NN mistakes for onsets. Within that subset, the interesting discrimination is **TP (NN fires, near GT) vs FP (NN fires, far from GT)**, not GT-near vs GT-far.

Those are different questions. The NN fires on broadband spectral events — drums have sharp magnitude peaks (high crest), but so do synth stabs, vocal consonants, and chord changes. When the NN picks a moment to fire, crest is already high whether that's a TP or an FP. No gating threshold separates them because both populations cluster at the high end of crest.

### Corrected next step — TP-vs-FP targeted measurement

Before trying any more gate thresholds, measure per-feature |d| between two GT-labeled sub-populations: *frames where the NN fires and it's a TP* vs *frames where the NN fires and it's an FP*. This is the actual signal a gate needs.

Possible outcomes:

- Some other feature shows strong TP-vs-FP |d| → worth a gate sweep for that feature.
- No feature shows strong TP-vs-FP |d| → the NN is making its errors on genuinely ambiguous events, and no deterministic shape feature can save it. Path A is dead; move to Path B (retrain with new input features) or upstream content classification.

The Phase 1 |d| ranking was correct about which features exist as signals, but measured the wrong class boundary for FP-reduction purposes. Keep it as a feature-existence ranking, not as a gate-readiness ranking.

### Gate (b) result — b137 held-out, real TP-vs-FP — 2026-04-20

Server validation extended with `persist_raw=true` (`/api/test/validate`
body flag) so `raw_capture.signal_frames` and `raw_capture.transients`
survive into the job JSON. Job `67325bff3b8b` ran all 25 GiantSteps LOFI
held-out tracks across all four serial devices. Analysis script:
`ml-training/analysis/run_gate_b.py`. Per-transient TP/FP labelling:
±50 ms to nearest GT onset = TP, else FP. Feature value = nearest
signal_frame within ±32 ms (= 2 firmware hops).

**Population (pooled across 4 devices, 25 tracks):** 2953 TP, 8991 FP
transients — consistent with held-out onset F1 = 0.398 (~25 % of NN
firings match a GT onset).

**Pooled TP-vs-FP |d|:**

| signal | \|d\| | d (signed) | gate b (≥ 0.3) |
|--------|-----:|-----------:|:--------------:|
| flatness | 0.098 | +0.098 | ❌ |
| centroid | 0.079 | +0.079 | ❌ |
| raw_flux | 0.050 | +0.050 | ❌ |
| crest    | 0.039 | +0.039 | ❌ |
| hfc      | 0.016 | +0.016 | ❌ |
| rolloff  | 0.009 | -0.009 | ❌ |

Per-device |d| tops out at 0.17 (flatness on ABFBC41283E2). Every single
candidate fails the 0.3 threshold by a large margin. This matches the
Phase 4 Path A null result for crest gating and generalises it to every
other candidate.

**What this means for v28:**

1. **Do not ship v28_4hybrid as configured.** Adding
   `[flatness, raw_flux, crest, hfc]` to the NN input vector does not
   give the model new information about when it's currently wrong.
   Gate (e)'s +0.03 F1 threshold would (correctly) fail, and we'd have
   spent GPU hours learning that.
2. **v27-hybrid's existing `[flatness, raw_flux]` inputs are not the
   leverage point either.** Their pooled |d| at firing moments is 0.10
   and 0.05. Whatever lift they provided to v27 was not via
   TP-vs-FP discrimination at decision time.
3. **The NN's FP population is spectrally indistinguishable from its
   TP population on these six deterministic shape features.** The
   mistakes happen at moments that look like drum onsets by every
   cheap magnitude-spectrum statistic we have. Whatever is different
   between "drum hit" and "drum-like synth stab / vocal consonant
   / chord change" is either (a) longer-time-scale than a single
   frame, (b) context-dependent in a way that needs sequence memory,
   or (c) not present in the magnitude spectrum at all (phase, mic
   directionality, dynamics over seconds).

**What to try instead:**

- **Temporal context.** Widen the NN's input window (currently 16
  frames ≈ 256 ms) to 32 or 48 frames. The argument: the difference
  between a kick and a synth kick-drum stab might be what happens in
  the preceding ~500 ms. Cheap experiment — only needs a window-size
  sweep in the existing training pipeline.
- **Onset-density priors.** A kick drum is rarely isolated — it
  co-occurs with a snare or hi-hat within ~250 ms. Add a learned
  feature that captures recent onset density; a false snare fired by
  a vocal consonant would be isolated in time.
- **Spectro-temporal learned features.** If a single-frame shape
  feature can't discriminate, the answer may be to let the model
  learn its own shape feature from a wider temporal receptive field.
  A 2-layer Conv1D with kernel=5 and dilation=2 already does this;
  going to dilation=4 or adding a third layer is the cheapest step.
- **Stop adding deterministic features.** The Phase 4 Path A / Path B
  investigation as originally framed is a dead end for *this* failure
  mode (FP reduction on held-out real music). Further features will
  fail the same gate (b) check until we identify a fundamentally
  different discriminator.

**Gate (b) is working as designed.** It killed a proposed experiment
that would have wasted GPU time and produced no F1 gain. The 2–3
hours spent building persist_raw + run_gate_b.py paid for themselves
on the first run.

Data: `ml-training/outputs/validation/holdout_67325bff3b8b_raw.json`,
analysis: `ml-training/outputs/gate_b/b137_holdout/summary.{md,json}`.

### Gate (e) blocked — 2026-04-22

Gate (e) (ablation F1 ≥ +0.03 vs mel-only baseline) assumes a sound baseline. Training the v28_mel_only baseline exposed a loss-function regression that had quietly been present since v26: switching from plain weighted BCE to `asymmetric_focal` with `gamma_neg=4.0` collapsed precision from 0.47 (v21) to 0.23 (v26). v27-hybrid and v28_mel_only both inherited this loss and both score F1 ≈ 0.40 with P ≈ 0.25 — pathological.

Full diagnosis in `docs/ML_IMPROVEMENT_PLAN.md` §"2026-04-22 — focal-loss regression + v29 reset".

**What this means for the ablation queue:**

1. The four v28 single-feature ablation configs (`conv1d_w16_onset_v28_mel_{crest,raw_flux,hfc,flatness}.yaml`) are **not cancelled**, but training them *now* would measure F1 deltas on a broken baseline — uninterpretable. A feature that looks like it helps might just be partially compensating for the loss-function bug; a feature that looks flat might be useful on a corrected baseline.
2. **v29_mel_only must complete first.** Three config deltas from v28: `loss.type: bce`, `neighbor_weight: 0.0`, `hard_binary_threshold: 0.5`. Same `processed_v28/` dataset (label shaping is applied at load time, not at prep time).
3. After v29_mel_only proves P ≥ 0.40 in val and on-device eval, re-run the same four ablations as `v29_mel_{crest,raw_flux,hfc,flatness}` with identical three-delta changes. Compare v29-ablation F1 to v29_mel_only F1. That is a valid gate (e) test; prior comparisons are not.
4. Gate (a), (c), (d) results from the `b137_holdout` analysis remain valid — they don't depend on the NN loss or the training recipe. Only gate (b) and gate (e) need to be re-run against a corrected model.

No held-out-contamination concern added by the v29 retrain: `processed_v28/` was prepared with `--exclude-dir blinky-test-player/music/edm_holdout/`, same as v28 and v27-hybrid. The GiantSteps LOFI corpus stays held-out.

**Near-term plan:**

1. Finish the in-flight v28_mel_only held-out validation on blinkyhost. Record before numbers (F1, P, R, TP/FP distribution) as the last data point for the broken-loss generation.
2. Launch v29_mel_only in tmux on the training GPU. ~11 h.
3. Export v29_mel_only to TFLite, flash fleet, run held-out validation with `persist_raw=true`. Record after numbers.
4. If v29_mel_only clears P ≥ 0.40 on held-out: re-queue the four v29 single-feature ablations. Each reuses `processed_v28/` with `feature_indices` column selection (already implemented in `MemmapBeatDataset`).
5. Gate (b) re-check on v29 held-out data is cheap — the persist_raw infra + `run_gate_b.py` are already in place. Expect higher TP/FP |d| simply because the TP/FP split should be less noisy when the model actually discriminates.

The corrected baseline may make some feature that previously looked flat (e.g., raw_flux's |d|=0.05 at v27 firing moments) suddenly discriminate, because the NN's FP population changes when its loss function changes. Treat the 2026-04-20 gate (b) results as "true on the broken model"; they are not a proof that no feature can help.

### Corpus framing revision — 2026-04-23

**Primary corpus is `blinky-test-player/music/edm/` (18 tracks, representative).** `edm_holdout/` (25 GiantSteps LOFI tracks) is an adversarial / stress-test secondary — ambient, phrase-shifting, boom-bap content where VISUALIZER_GOALS §5 explicitly permits low F1 ("organic mode is the correct response").

Every F1 number in this document before this note was measured on one or the other without the primary/secondary distinction. In particular, gate (b)'s 2953-TP / 8991-FP population was captured on edm_holdout — which is both the hardest content AND (as of v28) training-contaminated. Future gate (b) re-runs should report edm/ and edm_holdout separately and compare.

**Argparse bug affecting past exclusion claims.** `prepare_dataset.py --exclude-dir` was single-valued until 2026-04-23 (no `action='append'`). Any prior run that passed `--exclude-dir A --exclude-dir B` silently kept only B. The v30 prep (Apr 23) is the first to genuinely exclude BOTH edm/ and edm_holdout from training. Pre-v30 mel-only baselines on edm/ (v28, v29) are training-contaminated.

### v27 hybrids were a regression — held-out measured 2026-04-22

v28_mel_only (mel-only, 30 channels, same recipe + loss as v27) held-out:

| corpus | F1 | P | R | FP/TP |
|--------|---:|--:|--:|------:|
| v27-hybrid (`flatness + raw_flux`) on edm_holdout | 0.398 | 0.247 | ~0.95 | 3.04 |
| v28_mel_only on edm_holdout | **0.489** | **0.381** | 0.754 | **1.63** |

**Removing the two hybrid features added +0.091 F1 and +0.134 precision on held-out real music.** Same loss, same labels, same recipe, same data — only difference is 30 mel channels instead of 30 mel + 2 hybrid.

This retroactively confirms two of the working principles at the top of this document:

- §"Don't stack without ablation." v27's hybrid-feature add went in without a mel-only comparison. The comparison now exists and shows those hybrids were a net loss. Everything inferred from v27's F1 about "which hybrid feature helps" was overfit noise; the hybrids in aggregate *hurt*.
- §"Don't ignore redundancy with mel." Flatness R² vs mel = 0.85 (gate c). An extra channel carrying 85% redundant information is not "bonus signal," it's noise added to the input — the NN has to learn to partially ignore it. The 0.134 precision loss is quantitatively consistent with that.

**Revised plan for v29 ablations.** When v29 single-feature variants run, the mel-only baseline is the defender, not a floor. A feature must *beat* v29_mel_only's held-out F1 by ≥ 0.03 (gate e) — beating broken v27 is no longer interesting. Features that cleared gate (a) (exists on-device) but fail gate (c) (R² < 0.95 vs mel) should be deprioritised, since the v27 data now shows redundancy hurts rather than just wastes a channel.

**raw_flux remains the one feature worth testing aggressively:** gate (c) R² = 0.19 (lowest — least redundant with mel), temporal-derivative signal the mel bands cannot encode. If any feature survives gate (e) on v29, raw_flux is the strongest prior.

### v29 held-out measured 2026-04-23 — loss fix did not move on-device

Full comparison in `docs/ML_IMPROVEMENT_PLAN.md` §"v29_mel_only held-out results". Summary:

- v29 val P/R: 0.467 / 0.647 (matches v21 baseline exactly)
- v29 held-out F1 / P: 0.484 / 0.384 — **within noise of v28_mel_only** (0.489 / 0.381)

The v26 focal-loss change was a val-metric artifact — frame-level P/R penalized broad-plateau outputs, but firmware peak-picking collapses plateaus to single firings. On-device P/R is determined by the model's discrimination capacity, not output sharpness. That's why the v27→v28 hybrid-removal gain was real (+0.134 P on-device) while the v28→v29 loss-revert gain was imaginary (Δ < 0.003).

**Gate (e) remains unblocked for ablations**, but the expected ceiling is now clearer: stacking more hybrid features onto a mel-only baseline won't help if those features don't carry new discriminative information. Gate-b already showed none of the six current candidates do. Without a feature that fails gate (c) R² < 0.5 (not 0.95 — truly novel signal) and clears gate (b) TP-vs-FP |d| ≥ 0.3, further ablations are wasted GPU time.

### Disproven direction: wider NN input window

Past experiments confirm widening `window_frames` from 16 → 32 produced no F1 gain (0.468-0.482 at W32 vs 0.552 at v21 W16). See ML_IMPROVEMENT_PLAN.md §"Disproven direction: wider input window" for the reason (window_frames ≠ receptive field) and the operational reasons this direction is off the table (onset detection needs attack info from 10-30 ms, not 500 ms of tail).

Dilated Conv1D (longer RF at same parameter count) was never trained. Given gate-b's finding that TP and FP firings look identical in the current 256 ms window, extending RF into the past is unlikely to recover signal that isn't there. If a future experiment uses it, document the hypothesis it's testing (e.g., "isolated FPs differ from rhythmic TPs in onset density over 1-2 seconds") — not generic "more context is better."

### 2026-04-23 — Gate-b's null result has an upstream cause: contaminated labels

Investigation triggered by v29's held-out F1 being unchanged from v28 (loss fix was val-metric cosmetic only) led to a deeper audit of the label-generation pipeline. Finding: **kick_weighted_drums training labels are contaminated by demucs stem bleed.**

- 32% of tracks have >0.7× bleed ratio (substantial >200 Hz energy in the "drum" stem)
- Kick-band vs snare-band energy correlation >0.6 on 10% of tracks
- Label density is 2× the full-mix consensus label density (152.8 events/track vs 75.9) — inconsistent with a clean drum separation
- Spot-check: labeled "kicks" show as little as 0.50 dB low-frequency energy spike, essentially noise

Full details in `docs/ML_IMPROVEMENT_PLAN.md` §"Thread 1: Is the problem the labels?".

**Implication for gate-b.** The null result (no shape feature separates TP from FP at |d| ≥ 0.3 on v27 firings) measured a population where many "TPs" are mis-labeled tonal-content artifacts — acoustically indistinguishable from the "FPs" because they *are* the same kind of event, just labeled differently. Gate-b is correctly saying "no feature separates these populations"; what it couldn't say is "because the populations aren't actually different."

**This reopens the gate-b measurement as a valid experiment once labels are clean** (see ML_IMPROVEMENT_PLAN Step 2, training v30 on `onsets_consensus`). After v30 ships:

1. Re-run held-out validation with `persist_raw=true` on v30.
2. Re-run `run_gate_b.py` on the fresh TP/FP population defined by cleaner labels.
3. If any feature now shows |d| ≥ 0.3: that feature genuinely carries discriminative signal that was masked by v27's label noise. Prime candidate for v30-mel+feature ablation (clean gate-e comparison).
4. If all features still fail: the original conclusion stands (shape features don't discriminate musically rhythmic-vs-random), and the firmware-level PLP AND-gate is the right answer.

Gate-b doesn't need to be rebuilt — just re-run with corrected inputs.
