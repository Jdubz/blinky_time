# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 17, 2026 (AudioTracker + Conv1D W16 onset-only deployed, AudioController deleted)

## Current Config (v75, SETTINGS_VERSION 74)

**Onset detection (deployed):** Conv1D W16 onset-only model, ~13 KB INT8, ~7ms inference, single-channel onset activation. Detects acoustic onsets (kicks/snares) — drives visual pulse + PLL phase refinement. Cannot distinguish on-beat from off-beat onsets.
- Deployed on all 7 devices (3 nRF52840 + 2 ESP32-S3 on blinkyhost, 1 nRF52840 tube + 1 ESP32-S3 display local).
- Supersedes W64 Conv1D (27ms, 15.1 KB) and FC W32 (56.8 KB). Dual-model (OnsetNN + RhythmNN) abandoned.
- BandFlux/EnsembleDetector fully removed (v67). AudioController deleted (v74).

**BPM estimation:** Spectral flux (HWR, NN-independent) → contrast^2 → OSS buffer → ACF + comb bank validation.

**Beat tracking:** AudioTracker (spectral flux → ACF + Comb filter bank + PLL)
- ~35 tunable parameters persisted via ConfigStorage (v74, was serial-only)
- pllkp=0.15, pllki=0.005 (PLL phase correction)
- rayleighBpm=140 (Rayleigh prior peak)
- combFeedback=0.92 (comb bank IIR resonance)
- acfPeriodMs=150 (ACF recomputation interval)
- tempoSmoothing=0.85 (BPM EMA)
- odfContrast=2.0 (spectral flux power-law sharpening before ACF)
- percival2=0.5, percival4=0.25 (ACF harmonic fold weights)
- bassFlux=0.5, midFlux=0.2, highFlux=0.3 (spectral flux band weighting)
- emicweight=0.30, emelweight=0.30, eodfweight=0.40 (energy synthesis blend)

## SOTA Context (March 2026)

Best online/causal beat tracking systems on standard benchmarks (line-in audio):

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | Conv1D W16 ODF + ACF/Comb/PLL (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends (RNN/CRNN/Transformer). Our gap is primarily in tempo estimation signal quality, not the beat tracking backend. BPM now uses spectral flux (decoupled from NN). NN onset quality still matters for visual pulse and PLL phase refinement. AudioTracker (spectral flux → ACF+Comb+PLL) replaces CBSS with a simpler, faster backend (~10 params vs ~56).

**Reference tempo resolutions:** madmom uses 82 lag-domain bins (~2.4 BPM at 120 BPM), BTrack uses 41 bins (2 BPM steps), BeatNet uses 300 discrete levels. Our 20 bins (11.5 BPM at 130 BPM) is far coarser than any reference system.

## A/B Test Results Summary (March 7-8, 2026)

All tests: 18 EDM tracks, blinkyhost.local, middle-of-track seeking, `NODE_PATH=node_modules`.

| Feature | Wins | Losses | Ties | Mean Err | Octave Errs | Verdict |
|---------|:----:|:------:|:----:|:--------:|:-----------:|---------|
| NN onset (nnbeat=1) | **11** | 7 | 0 | **14.8** vs 15.6 | 7 vs 7 | Default ON |
| Forward filter (fwdfilter=1) | 13 | 5 | 0 | **9.3** vs 15.4 | **17/18** | **REMOVED v64** |
| Fwd filter optimized (6-param) | -- | -- | -- | **12.5** vs 14.5 | **7/18** vs 4/18 | **REMOVED v64** |
| Hybrid phase (fwdphase=1) | 8 | 6 | 4 | 14.9 vs 14.8 | same | **REMOVED v64** |
| Noise subtraction (noiseest=1) | 5 | **13** | 0 | 17.1 vs 15.4 | +3 | Default OFF (kept in v64) |
| Template+subbeat (v50) | 8 | **10** | 0 | -- | -- | **REMOVED v64** |

## Literature-Validated A/B Tests (March 17, 2026)

Parameters compared against Percival 2014, BTrack, madmom, librosa, Scheirer 1998, SuperFlux,
BeatNet, BeatRoot, and Davies/Plumbley 2007. Ordered by expected impact.

### 1. Percival Harmonic Weights: 0.5/0.25 → 1.0/1.0

**Divergence:** Every reference implementation (Percival/Essentia, BTrack) uses **equal weights (1.0/1.0)**.
Our diminishing weights (0.5/0.25) give the fundamental less reinforcement from its harmonics,
making it harder to distinguish fundamental from half-time. May contribute to 135 BPM gravity well.

```
# Test: set percival2 1.0 && set percival4 1.0
# Control: percival2=0.5, percival4=0.25 (current)
# Metric: BPM accuracy, octave errors (expect improvement on half-time-locked tracks)
```

### 2. ODF Contrast Exponent: 2.0 → 1.0 or 0.5

**Divergence:** Percival uses exponent **0.5** (compress peaks, reveal weaker periodic components).
No other system applies power-law sharpening. Our 2.0 (squaring) amplifies strong peaks and
suppresses everything else — the **opposite** of the only published approach.

Note: 2.0 was A/B validated vs 1.0 on the old AudioController (v66, 10-6 win). Architecture has
changed significantly — the old test used NN ODF, current system uses spectral flux. Retest needed.

```
# Test A: set odfcontrast 1.0  (no sharpening — baseline)
# Test B: set odfcontrast 0.5  (Percival-style compression)
# Control: odfcontrast=2.0 (current)
# Metric: BPM accuracy, periodicity strength, gravity well tracks
```

### 3. Spectral Flux Band Weighting: 50/20/30 → Uniform

**Divergence:** No reference system uses bass-heavy weighting. SuperFlux, librosa, madmom, Percival,
Scheirer all use **uniform weighting** across frequency bands. Bass emphasis biases BPM estimation
toward kick patterns, which may hurt on tracks with prominent hi-hat/snare periodic patterns.

```
# Test: set bassflux 0.33 && set midflux 0.33 && set highflux 0.33
# Control: bassflux=0.5, midflux=0.2, highflux=0.3 (current)
# Metric: BPM accuracy across genre diversity (especially non-kick-driven tracks)
```

### 4. Comb Feedback: 0.92 → 0.79

**Divergence:** madmom uses **0.79** (deliberately low — "filters with smaller lags produce more peaks,
leading to a more balanced histogram"). Scheirer and Klapuri use variable alpha per lag. Our 0.92
gives much longer resonance memory. Since our comb bank is validation-only (not primary), the higher
alpha may be acceptable, but worth testing.

```
# Test: set combfeedback 0.79
# Control: combfeedback=0.92 (current)
# Metric: Tempo convergence speed, BPM accuracy on tempo-change tracks
```

### 5. Rayleigh Prior: 140 → 120

**Divergence:** BTrack peaks at ~120-130, Davies at ~120, librosa at 120. The perceptual "preferred
tempo" (van Noorden & Moelants 1999) is ~120 BPM. Our 140 was A/B validated (fewer octave errors
at 140 vs 120), but should be retested after other parameter changes.

```
# Test: set rayleighbpm 120
# Control: rayleighbpm=140 (current)
# Metric: BPM accuracy, octave errors (retest after other params settled)
```

### Literature Agreement (No Change Needed)

| Parameter | Our Default | Literature | Verdict |
|-----------|------------|-----------|---------|
| HWR spectral flux | Yes | All systems | Standard |
| BPM range 60-200 | 60-200 | Percival 50-210, madmom 40-250 | Within range |
| ACF window 5.5s | 5.5s | Percival 5.9s, BTrack ~6s, librosa 8s | Short end, OK for real-time |
| Tempo EMA 0.85 | 0.85 | BTrack 0.9 | Reasonable for visualizer |
| Comb lag spacing | Linear | madmom linear, BTrack linear | Standard |
| PLL Kp=0.15 | 0.15 | Novel (no PLL in literature); bandwidth ~1 Hz | Reasonable |
| PLL window ±25% | ±25% | BeatRoot 15-30%, IBT 20-40% | Within agent-based range |
| Onset gate 0.20 | 0.20 | BeatNet 0.4 | Lower OK for single-channel model |
| Hamming window | Hamming | Percival: Hamming | Standard |

### Additional Notes From Literature

- **Spectral flux diff lag:** SuperFlux and madmom both use **2 frames back** (not 1) for vibrato
  suppression. Low priority for BPM (flux feeds ACF not peak-picker), but could improve signal quality.
- **Log compression:** All reference systems (SuperFlux, Percival, librosa, madmom) use log-magnitude
  spectral flux. Our soft-knee compressor is conceptually similar but non-standard. Not easily changed
  (compressor serves multiple pipeline roles).
- **PLL is novel:** No established beat tracking system uses an explicit PI controller. Closest analogs
  are BeatRoot's correctionFactor=50 (2%/beat) and IBT's 25%/beat, but both correct once per beat,
  not at frame rate. Our PLL design is architecturally unique.
- **Comb bank variable alpha:** Scheirer 1998 and Klapuri 2006 both advocate per-lag alpha so all
  filters have equal decay time. Could be a future enhancement if fixed alpha proves limiting.

## Current Bottlenecks

1. ~~**Mel level mismatch (RESOLVED March 13)**~~ — Fixed with cal63 model (target_rms_db=-63 dB). ODF activations ~50% stronger on device.

2. ~~**CBSS parameter re-tuning (RESOLVED March 13)**~~ — Swept `cbssthresh` (0.5-2.0, 6 steps) and `cbsscontrast` (1.0-3.0, 5 steps) on all 3 devices with cal63 ODF. Neither parameter showed significant improvement over current defaults. cbssthresh: mean error 10.1-11.4 (current 1.0 ≈ 11.0). cbsscontrast: mean error 8.9-11.2 (current 2.0 ≈ 10.8). Ratio-based params (cbssTightness, onsetSnapWindow, adaptiveTightness) are self-compensating as expected. **No changes needed.**

3. **~135 BPM gravity well** — Multi-factorial. Not improved by CBSS parameter tuning. Not a tempo bin resolution issue (47 bins tested v61, full-res ACF already evaluates all lags). Literature review (Mar 17) identified three likely contributors: Percival harmonic weights too low (test #1), ODF contrast exponent opposite of literature (test #2), bass-heavy spectral flux weighting without precedent (test #3). See "Literature-Validated A/B Tests" above.

4. **Phase alignment** — correct BPM doesn't translate to correct beat placement. PLL tracks phase via onset-gated correction. PLL params (Kp/Ki/window/decay) now tunable and persisted (v74). PLL design is architecturally novel (no literature precedent), so parameter sweep is the path forward.

5. **Downbeat label ceiling** — Consensus v3 AND-merge improved label quality (65% noisy single-system labels removed), but inter-annotator agreement remains the ceiling. Offline DB F1=0.24. On-device downbeat activations are now functional with cal63 (max 0.37-0.57).

## Future: Heydari 1D State Space

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) showed a 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. Could be a path to explicit phase tracking without the forward filter's octave symmetry problem. ~860 states fits our memory budget.

## Closed Investigations

- **Tempo bins 20→47** (v61): Tested, no improvement. Gravity well is not a bin count issue.
- **Forward filter** (v57-v60): 6-param sweep, optimized but still 7/18 octave errors (vs CBSS 4/18). Half-time bias is fundamental. **Removed from firmware in v64.**
- **Spectral noise subtraction** (v56): A/B tested, hurts BPM accuracy (baseline wins 13/18). **Removed from firmware in v64.**
- **Focal loss** (v5): Identical to v4, no benefit.
- **Template+subbeat** (v50): No net benefit (baseline 10 wins, subbeat 8). **Removed from firmware in v64.**
- **Adaptive tightness, Percival harmonic, bidirectional snap, HMM, particle filter, PLP phase** — all removed as dead code in v64.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** -- tracks lock to ~132 BPM | Literature A/B tests #1-#3 above |
| NN eval inflated | Test set data leakage (18 tracks in training data) | Unknown | Fixed in v6+; v9 DS-TCN in progress |
| Phase alignment limits F1 | PLL tracks phase via onset-gated correction | **High** | PLL param sweep (Kp/Ki/window now tunable v74) |
| Run-to-run variance | Room acoustics, ambient noise | Requires 5+ runs for reliable evaluation | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** -- acceptable for visuals | -- |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** -- organic mode is correct | -- |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** -- energy-reactive acceptable | -- |

## Key References (Added March 2026)

- BEAST: Streaming Transformer for online beat tracking (ICASSP 2024)
- BeatNet+: CRNN + particle filter, auxiliary training (TISMIR 2024, Heydari et al.)
- RNN-PLP-On: Real-time PLP beat tracking (TISMIR 2024, Meier/Chiu/Muller)
- Novel-1D: 1D state space with jump-back reward (ICASSP 2022, Heydari et al.)
- Percival 2014: Enhanced ACF + pulse train evaluation (IEEE/ACM TASLP)
- Krebs/Bock/Widmer 2015: Efficient state-space for joint tempo-meter (ISMIR)
- Davies 2010: Beat Critic octave error identification (ISMIR)
- Scheirer 1998: Comb filter bank tempo estimation
