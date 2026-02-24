# Blinky Time - Improvement Plan

*Last Updated: February 24, 2026 (Feature testing complete: ODF mean sub, diffframes, per-band thresholds — defaults are optimal)*

## Current Status

### Completed (February 23-24, 2026)

**Bayesian Tempo Fusion + CBSS Adaptive Threshold (SETTINGS_VERSION 20):**
- Replaced sequential override chain (~400 lines) with unified Bayesian posterior estimation over 20 tempo bins (60-180 BPM)
- Four observation signals (autocorrelation, Fourier tempogram, comb filter bank, IOI histogram) contribute per-bin likelihoods multiplied with Viterbi transition prior
- Removed 17 old parameters (HPS, pulse train, harmonic thresholds, cross-validation thresholds), added 9 Bayesian/beat params
- Per-sample ACF harmonic disambiguation: after MAP extraction, checks raw autocorrelation at lag/2 (2x BPM, >50% thresh) and lag*2/3 (1.5x BPM, >60% thresh) — fixes sub-harmonic lock on minimal-01 (BPM 70→124)
- CBSS adaptive threshold (`cbssthresh=0.4`): beat fires only if CBSS > factor * running mean (EMA tau ~2s). Prevents phantom beats during silence/breakdowns
- Optional ongoing static prior (`bayespriorw`, default off) — tested but static prior hurts tracks far from center
- Removed dead code: `evaluatePulseTrains()`, `generateAndCorrelate()`, old sequential override chain
- Fixed pre-existing bug: IOI onset ring buffer not shifted during sampleCounter_ wrap (~4.6 hour edge case)

### Completed (February 21-22, 2026)

**Detector & Beat Tracking Optimization:**
- BandWeightedFluxDetector set as sole default detector (all others disabled)
- BandFlux parameters confirmed near-optimal via sweep: gamma=20, bassWeight=2.0, threshold=0.5
- beatoffset recalibrated 9→5, doubling avg Beat F1 (0.216→0.452 across 9 tracks)
- Onset delta filter (minOnsetDelta=0.3): rejects slow-rising pads/swells, improved avg Beat F1 0.452→0.472
- IOI histogram, Fourier tempogram, ODF mean subtraction implemented as independent features (later unified into Bayesian fusion)

### Completed (February 2026)

**Rhythm Tracking:**
- CBSS beat tracking with counter-based beat detection (replaced multi-hypothesis v3)
- Deterministic phase derivation from beat counter
- Adaptive cooldown — tempo-aware, scales with beat period
- BandWeightedFluxDetector Solo (replaced Drummer+Complex ensemble)

**Particle System & Visuals:**
- Frame-rate independent physics (centiseconds, not frames)
- Continuous mode blending (replaced binary `hasRhythm()` threshold)
- Particle variety system (FAST_SPARK, SLOW_EMBER, BURST_SPARK types)
- Smooth 6-stop color gradient for fire (eliminated banding)
- Hardware AGC full range in loud mode (0-80, was 10-80)
- Multi-octave SimplexNoise turbulence wind (replaced sine wave)
- Runtime device configuration (safe mode, JSON registry, serial upload)

### Completed (December 2025)

**Architecture:** Generator → Effect → Renderer, AudioController v3, ensemble detection (6 algorithms), agreement-based fusion, comprehensive testing infrastructure (MCP + param-tuner), calibration completed.

---

## Design Philosophy

See **[VISUALIZER_GOALS.md](VISUALIZER_GOALS.md)** for the guiding design philosophy. Key principle: the goal is visual quality, not metric perfection. Low Beat F1 on ambient, trap, or machine-drum tracks may represent correct visual behavior (organic mode fallback). False positives are the #1 visual problem.

## Outstanding Issues

### Priority 1: False Positive Elimination — RESOLVED (Feb 22, 2026)

**Status:** Closed. Remaining ~16-22 pad FPs on synthetic pattern are **visually acceptable** — timing analysis shows they are on-beat, not random.

**Timing analysis of pad-rejection FPs (Feb 22):**
Analysis of 22 FPs from pad-rejection pattern (80 BPM, 750ms beat period):
- **4 FPs** are direct pad triggers — precisely on the 80 BPM beat grid (within 6-102ms of pad onset)
- **14 FPs** are reverb/echo tails — within 200-700ms of a real event (kick, snare, or pad)
- **4 FPs** are pre-kick room reflections — 230-590ms before a kick
- **0 FPs** are truly random or unrelated to musical events

**Visual impact:** Since all FPs cluster around actual musical events on the beat grid, they appear as "slightly extra busy but still rhythmic" rather than "random sparks with no musical cause." The pad-rejection pattern is a worst case (isolated pads with no other content). In real music, pads are mixed with kicks/snares and the extra triggers are masked.

**Approaches tested and rejected (Feb 22):**
1. **Bass-ratio gate** — Fails: snares are mid-dominant like pads
2. **Band-dominance gate** (max band / total) — Fails: room acoustics smear spectral content
3. **Post-onset decay gate** (decayRatio + decayFrames) — Discriminates kicks vs pads, but synth stabs share sustained envelope → regresses synth-stabs 1.000→0.59
4. **Min-flux decay variant** — Same result as above
5. **Spectral crest factor gate** (crestGate) — Eliminates all pad FPs but kills kicks through room resonances (recall 0.31 on strong-beats)

**Active gates (production config):**
1. Hi-hat rejection gate (high-only flux suppression)
2. Onset delta filter (minOnsetDelta=0.3, rejects slow-rising signals)

**Available but disabled gates** (for future experimentation):
- `bandflux_dominance` — band-dominance gate (0.0=disabled)
- `bandflux_decayratio` + `bandflux_decayframes` — post-onset decay gate (0.0=disabled)
- `bandflux_crestgate` — spectral crest factor gate (0.0=disabled)

### Priority 2: CBSS Beat Tracking + Bayesian Tempo Fusion — Active Tuning

BTrack-style predict+countdown CBSS beat detection with Bayesian tempo fusion. Tempo estimated via unified posterior over 20 bins (60-180 BPM) fusing autocorrelation, Fourier tempogram, comb filter bank, and IOI histogram. Per-sample ACF harmonic disambiguation after MAP extraction. CBSS adaptive threshold prevents phantom beats.

**Pre-Bayesian baseline (sequential override chain, Feb 21):** avg Beat F1 **0.459** on 9 tracks.

**Bayesian v20 performance — tested Feb 24 (CBSS thresh=0.4, per-sample ACF disambig):**

| Track | Old F1 | Bayes F1 | BPM | Expected | Notes |
|-------|:------:|:--------:|:---:|:--------:|-------|
| trance-party | 0.775 | **0.813** | 130.2 | 136 | Stable improvement |
| infected-vibes | 0.691 | **0.841** | 137.5 | 143.6 | Big improvement |
| minimal-01 | 0.695 | **0.721** | 124.0 | 129.2 | Sub-harmonic fixed by ACF disambig |
| goa-mantra | 0.571 | 0.461 | 138.5 | 136 | Phase drift, high run-to-run variance |
| minimal-emotion | 0.372 | 0.307 | 129.1 | 129.2 | Phase offset (~80ms) |
| deep-ambience | 0.435 | 0.378 | 122.0 | 123 | Slight regression |
| dub-groove | 0.176 | 0.208 | 163.8 | 123 | Double-time lock |
| machine-drum | 0.224 | 0.089 | 123.8 | 143.6 | Sub-harmonic BPM lock |
| trap-electro | 0.190 | 0.036 | 131.5 | 112.3 | Very few beat events |
| **Average** | **0.459** | **0.421** | | | Structural gap: 20-bin resolution |

**Bayesian tunable parameters (9 total):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Lambda | `bayeslambda` | 0.1 | Transition tightness (how fast tempo can change) |
| Prior center | `bayesprior` | 128 | Static prior Gaussian center BPM |
| Prior width | `priorwidth` | 50 | Static prior Gaussian sigma |
| Prior weight | `bayespriorw` | 0.0 | Ongoing static prior strength (off by default) |
| ACF weight | `bayesacf` | 1.0 | Autocorrelation observation weight |
| FT weight | `bayesft` | 0.8 | Fourier tempogram observation weight |
| Comb weight | `bayescomb` | 0.7 | Comb filter bank observation weight |
| IOI weight | `bayesioi` | 0.5 | IOI histogram observation weight |
| CBSS threshold | `cbssthresh` | 0.4 | Adaptive beat threshold (CBSS > factor * mean, 0=off) |

**Ongoing static prior — tested and disabled (Feb 23):**
Static Gaussian prior (centered at `bayesprior`, sigma `priorwidth`) multiplied into posterior each frame. Helps tracks near 128 BPM (minimal-01: 69.8→97.3 BPM) but actively hurts tracks far from center (trap-electro: 112→131 BPM, machine-drum: 144→124 BPM). Fundamental limitation — can't distinguish correct off-center tempo from sub-harmonic. Per-sample ACF harmonic disambiguation is the proper fix.

**Known limitations:**
- DnB half-time detection — both librosa and firmware detect ~117 BPM instead of ~170 BPM. Acceptable for visual purposes
- trap/syncopated low F1 — energy-reactive mode is the correct visual response
- deep-ambience low F1 — organic mode fallback is correct for ambient content

**What NOT to do (tested and rejected):**
- Phase correction (phasecorr): Destroys BPM on syncopated tracks
- ODF width > 5: Variable delay destroys beatoffset calibration
- Ensemble transient input to CBSS: Only works for 4-on-the-floor
- Ongoing static prior as sole sub-harmonic fix: Helps near 128 BPM, hurts far from it (Feb 23)
- Posterior-based harmonic disambig: Posterior already sub-harmonic dominated, check never triggers (Feb 24)
- Soft bin-level ACF penalty: Helped trance-party (0.856) but destroyed goa-mantra (0.246), collateral damage (Feb 24)
- HPS (both ratio-based and additive): Penalizes correct peaks or boosts wrong sub-harmonics (Feb 22)
- Pulse train cross-correlation: Sub-harmonics produce similar onset alignment (Feb 22)
- Sequential override chain: Features interact negatively, combined avg F1 dropped 0.472→0.381 (Feb 23)

### Priority 2: BandWeightedFluxDetector — COMPLETE (Feb 21, 2026)

**Status:** Default detector. All parameters confirmed near-optimal. beatoffset recalibrated.

**Algorithm:** Log-compress FFT magnitudes (`log(1 + 20 * mag[k])`), 3-bin max-filter (SuperFlux vibrato suppression), band-weighted half-wave rectified flux (bass 2.0x, mid 1.5x, high 0.1x), additive threshold (`mean + delta`), asymmetric threshold update, hi-hat rejection gate.

**Parameter sweep results (Feb 21):** All defaults confirmed optimal:
- gamma: 10 (-0.06), 20 (baseline), 30 (-0.01) — 20 is sweet spot
- bassWeight: 1.5 (worse), 2.0 (baseline), 3.0 (-0.02) — 2.0 is sweet spot
- threshold: 0.3 (-0.03), 0.5 (baseline), 0.7 (-0.15) — 0.5 is sweet spot

**Beat tracking with beatoffset=5 (9 tracks, Feb 21):**

| Metric | Value |
|--------|:-----:|
| Avg Beat F1 | 0.452 |
| Best track | minimal-01 (0.704) |
| Avg BPM accuracy | 0.942 |
| Avg transient F1 | 0.440 |

### Priority 2b: False Positive Results — COMPLETE (Feb 21, 2026)

BandFlux synthetic pattern evaluation completed. Major improvement on lead-melody, regression on pad-rejection:

| Pattern | BandFlux F1 | Old (HFC+D) F1 | Delta |
|---------|:-:|:-:|:-:|
| lead-melody | 0.785 | 0.286 | **+0.499** |
| bass-line | 0.891 | 0.630 | +0.261 |
| chord-rejection | 0.727 | 0.698 | +0.029 |
| synth-stabs | 0.600 | 1.000 | -0.400 |
| pad-rejection | 0.314 | 0.696 | **-0.382** |

### Priority 3: Microphone Sensitivity

**Problem:** Raw ADC level is 0.01-0.02 at maximum hardware gain (80 = +20 dB) with music playing from speakers. This is ~60 dB SPL at the mic — conversation level, not music level.

**Hardware findings:**
- Mic: MSM261D3526H1CPM, -26 dBFS sensitivity, 64 dB SNR — industry standard, equivalent to Arduino Nano 33 BLE Sense's MP34DT05
- PDM clock: nRF52840 uses 1.28 MHz; mic's optimal is 2.4 MHz. Operating below optimal but within spec (768 kHz–3.072 MHz). May cost 1-2 dB SNR.
- Hardware gain: 0-80 range (±20 dB in 0.5 dB steps), firmware already uses full range with AGC

**Possible improvements:**
1. **Software pre-amplification** — Multiply raw int16 samples by a gain factor (2x-4x) before normalization. Amplifies noise too, but for transient detection the SNR penalty may be acceptable. Simple: `sample = constrain((int32_t)sample * gain, -32768, 32767)` in the ISR.
2. **Faster AGC convergence** — Current normal AGC calibrates every 30s. For speaker-based music, the signal level is relatively stable. Reduce to 10-15s for quicker adaptation.
3. **Lower hwTarget** — Current target is 0.35 (35% of ADC range). At max gain the signal only reaches 0.01-0.02. The AGC is already maxed out, so lowering the target won't help. The issue is the acoustic signal is genuinely quiet at the mic.
4. **Physical mic placement** — The hat-mounted device may have the mic facing away from speakers, or covered by fabric/enclosure. Ensuring direct line-of-sight to audio source is the single highest-impact change.

**What NOT to do:**
- External microphone (impractical for wearable hat)
- Change PDM clock/ratio registers (marginal benefit, risks destabilizing PDM driver)
- Increase FFT size for better sensitivity (latency tradeoff, won't help with raw level)

### Priority 4: Startup Latency — IMPLEMENTED (Feb 22, 2026)

**Was:** AudioController required ~3s (180 samples @ 60Hz) before first autocorrelation.
**Now:** Progressive startup — autocorrelation begins after 1s (60 samples). The existing `maxLag = ossCount_ / 2` clamp naturally limits detectable tempo range during warmup:
- At 1s (60 samples): minimum detectable BPM is ~120 (maxLag=30); upper bound is bpmMax (200)
- At 2s (120 samples): full 60-200 BPM range available
- `periodicityStrength_` smoothing (0.7/0.3 EMA) handles early estimate noise

Band autocorrelation (adaptive weighting) also lowered from 120 to 60 minimum samples.

### Priority 5: Music Content Classification (Long-term)

The existing `rhythmStrength` blend works well but could be enhanced with additional content descriptors. Research (Feb 22) identified three cheap features that would improve organic/music mode transitions:

1. **Onset density** — ✅ Implemented (Feb 2026). Windowed counter with EMA smoothing, exposed as `AudioControl::onsetDensity` and `"od"` in streaming JSON. Modulates `rhythmStrength` by ±0.1 centered at 3 onsets/s.
2. **Spectral centroid variability** — variance of spectral centroid over 2-4s window. High variance=dynamic/percussive, low=sustained/ambient. Already have centroid computation.
3. **Energy crest factor** — peak/mean energy ratio over 2-4s. High=percussive with quiet periods, low=continuous drone.

These would modulate the existing `rhythmStrength` for smoother, more appropriate visual responses without hard mode switching.

### Priority 6: Tempo Estimation — Bayesian Fusion (Feb 23, 2026)

**Architecture:** Replaced sequential override chain with Bayesian posterior estimation. All tempo signals (autocorrelation, Fourier tempogram, comb filter bank, IOI histogram) contribute per-bin observation likelihoods multiplied together with a Viterbi transition prior. MAP estimate with quadratic interpolation becomes the tempo. Post-posterior harmonic disambiguation checks 2x and 1.5x BPM bins.

**What the Bayesian refactor eliminated:**
- Sequential override chain (~400 lines of cascading cross-validation)
- HPS code (ratio-based and additive, both rejected Feb 22)
- Pulse train cross-correlation (Percival 2014, rejected Feb 22)
- 17 old tuning parameters (harmonic thresholds, cross-validation thresholds, etc.)
- Negative feature interactions (combined features scored 0.381, worse than 0.472 baseline)

**What it preserved:**
- Autocorrelation computation (core signal, unchanged)
- CBSS beat tracking (receives tempo from Bayesian state)
- Comb filter bank, Fourier tempogram, IOI histogram (now per-bin observations)
- ODF smoothing, beat timing offset (unchanged)

#### 6a. ODF Mean Subtraction — TESTED, KEEP ON (Feb 24, 2026)

**Status:** Tested with Bayesian fusion. Turning OFF causes major regressions:
- minimal-01: 0.610→0.266 (BPM collapses to 89, sub-harmonic lock)
- goa-mantra: 0.565→0.286
- trance-party: 0.836→0.857 (marginal improvement)

ODF mean subtraction is **essential** for Bayesian fusion — without it, DC bias in autocorrelation makes all lags look correlated and the Bayesian posterior can't discriminate. Keep enabled (default).

#### 6b. CBSS Adaptive Threshold — IMPLEMENTED (Feb 24, 2026)

**Status:** Implemented as `cbssThresholdFactor` (default 0.4). Beat fires only if `CBSS > factor * cbssMean_` where cbssMean_ is an EMA with tau ~120 frames (~2s). Setting to 0 disables the threshold (countdown-only, old behavior).

**Impact:** Prevents phantom beats during silence/breakdowns. With thresh=0.4, avg F1 0.421 across 9 tracks. Setting thresh=0.2 helped some tracks (minimal-01 0.460→0.546) but thresh=0.4 is the best overall default.

#### 6c. Lightweight Particle Filter — Future Alternative (Research, Feb 22)

Inspired by BeatNet (Heydari et al., ISMIR 2021). 100-200 particles tracking (beat_period, beat_position). Octave investigator injects particles at 2x/0.5x median tempo at resampling. Could replace or complement Bayesian fusion if it proves insufficient.

- **CPU:** ~1% (100 particles × weight update per frame + periodic resampling)
- **Memory:** ~2KB
- **Complexity:** ~100-150 lines C++

#### 6d. Multi-Agent Beat Tracking — Future Alternative (Research, Feb 22)

5-10 competing tempo/phase agents. Each scores onset events against predicted beats. Best-scoring agent determines output. Agents at different metrical levels compete naturally.

- **CPU:** <1% | **Memory:** ~500 bytes | **Complexity:** ~150-250 lines

#### Approaches tested and rejected

| Approach | Why Not | Tested |
|----------|---------|--------|
| Sequential override chain | Features interact negatively, combined F1 0.381 < 0.472 baseline | Feb 23 |
| Ongoing static prior (Bayesian) | Helps near 128 BPM, hurts tracks at 112/144 BPM | Feb 23 |
| HPS (ratio-based + additive) | Penalizes correct peaks or boosts wrong sub-harmonics | Feb 22 |
| Pulse train (Percival 2014) | Sub-harmonics produce similar onset alignment | Feb 22 |
| Comb bank cross-validation | Both ACF and comb lock to same wrong tempo | Feb 22 |
| IOI bidirectional override | Repackages bad transient data | Feb 22 |
| ODF mean subtraction OFF | Destroys BPM on sparse tracks (minimal-01 BPM→89) | Feb 24 |
| Per-band thresholds ON | Helps strong tracks, destroys weak ones (avg 0.421→0.354) | Feb 24 |
| Multi-frame diffframes=2 | Too many transients, hurts phase (avg -0.098) | Feb 24 |
| Deep learning/CNN/Transformer | Not feasible on nRF52840 (64 MHz, no matrix acceleration) | Research |

### Priority 7: Onset Detection Improvements (Research, Feb 22)

Comprehensive research survey identified several untried improvements to BandWeightedFlux that could improve kick detection rate (currently ~60% recall on some tracks). Better onset detection feeds better data to all BPM tracking systems.

#### 7a. Per-Band Independent Thresholds — TESTED, KEEP OFF (Feb 24, 2026)

Independent adaptive thresholds per band (bass/mid/high). Detection fires if ANY band exceeds its own threshold × multiplier. **Disabled by default** (`bandflux_perbandthresh=0`).

- **Settings:** `bandflux_perbandthresh` (bool), `bandflux_perbandmult` (default 1.5)
- **3-track subset results:** avg F1 0.670→0.715 (+0.045) — improved goa-mantra (+0.064) and minimal-01 (+0.074)
- **Full 9-track regression:** avg F1 0.421→0.354 (**-0.067**) — **major regressions** on quiet/sparse tracks:
  - deep-ambience: 0.378→0.097 (-0.281)
  - minimal-emotion: 0.307→0.140 (-0.167)
  - machine-drum: 0.089→0.014 (-0.075)
- **Root cause:** Per-band detection generates more transients which overwhelms beat tracker on sparse tracks. Helps strong rhythmic tracks but destroys weak ones.
- **Verdict:** Keep disabled. The 3-track subset was misleading.

#### 7b. Multi-Frame Temporal Reference — TESTED, KEEP AT 1 (Feb 24, 2026)

Configurable `diffframes` (1-3) for BandFlux reference frame. Default remains 1 (compare to previous frame). Higher values skip intermediate frames for more robust flux measurement during bass sweeps.

- **Settings:** `bandflux_diffframes` (default 1, range 1-3)
- **diffframes=2 results:** avg F1 0.670→0.572 (**-0.098**) on 3-track subset:
  - trance-party: 0.836→0.783 (-0.053)
  - goa-mantra: 0.565→0.252 (-0.313) — too many transients (349→460)
  - minimal-01: 0.610→0.681 (+0.071) — more transients helped BPM
- **Root cause:** diffframes=2 detects more transients overall (larger flux from skipping a frame), which overwhelms phase alignment on tracks with complex textures.
- **Verdict:** Keep at 1 (default).

#### 7c. FFT-512 Bass-Focused Analysis — HIGH IMPACT, MODERATE EFFORT

The core onset detection problem: at FFT-256/16kHz, kick drum energy (40-80Hz) occupies only **1-2 FFT bins** (bin 1 = 62.5Hz). A sustained bass note fills the same bins, suppressing kick flux. FFT-512 doubles bass resolution (31.25Hz/bin), giving 2-4 bins for kick discrimination.

- **Memory:** ~5KB (FFT buffers + bass magnitude history)
- **CPU:** ~3-4ms every other frame (~1.5ms average)
- **Complexity:** ~200 lines (second FFT path, bass-only flux computation)
- **Risk:** Medium — need to manage two FFT paths, timing, and fusion
- **References:** Multi-resolution spectral flux (Bello 2005), Bock CNN multi-scale input (ICASSP 2014)

#### 7d. Weighted Phase Deviation Fusion — MEDIUM PRIORITY

Phase deviation catches kicks during sustained notes where magnitude flux is suppressed. At an onset, new frequency components appear with unpredictable phases, causing large deviation from the linear phase prediction. We already compute and store phase data in `SharedSpectralAnalysis`.

- **Memory:** ~512 bytes (one extra frame of phase history for prediction)
- **CPU:** ~50us/frame (~128 multiplies + wraps)
- **Complexity:** ~50 lines
- **References:** Dixon 2006 (Onset Detection Revisited), Duxbury 2003 (Complex Domain)

#### 7e. Knowledge-Distilled TinyML Onset Detector — LONG TERM

Train a CNN on desktop with labeled EDM onset data, distill to tiny student model (~1K-5K parameters), quantize to INT8, deploy via TensorFlow Lite Micro. Input: existing 64 log-compressed FFT bins. Output: onset probability per frame. Only approach that can truly learn complex spectral patterns of kicks vs bass vs pads.

- **Memory:** ~5-15KB (model + activations)
- **CPU:** ~200-500us/frame (with CMSIS-NN acceleration on Cortex-M4F)
- **Flash:** ~5-20KB model weights
- **Risk:** High — requires training data, model development, TFLM integration
- **Expected improvement:** Potentially 0.85+ onset F-measure (vs current ~0.60 on difficult tracks)
- **References:** Bock & Schlueter (ICASSP 2014), Knowledge distillation (Nature 2025, 1282-parameter student), TensorFlow Lite Micro

---

## Calibration Status (Feb 24, 2026)

**Bayesian fusion replaced the old calibration gap.** The sequential override chain's 17 independent parameters (each requiring per-feature sweeps) have been replaced by 9 Bayesian/beat weights that interact cooperatively. The old problem of negative feature interactions is solved architecturally.

| Feature | Parameters | Status |
|---------|:----------:|--------|
| **Bayesian weights** | bayesacf, bayesft, bayescomb, bayesioi, bayespriorw, bayeslambda, bayesprior, priorwidth | Defaults set, tuning tested (avg F1 0.421) |
| **CBSS adaptive threshold** | cbssthresh | **Calibrated** (0.4 default, Feb 24) |
| ODF Mean Subtraction | odfmeansub (toggle) | **Essential** — keep ON. OFF destroys BPM (Feb 24) |
| Per-band thresholds | bandflux_perbandthresh, perbandmult | **Tested, keep OFF** — hurts weak tracks (Feb 24) |
| Multi-frame diffframes | bandflux_diffframes | **Tested, keep at 1** — diffframes=2 too many transients (Feb 24) |
| BandFlux core params | gamma, bassWeight, threshold, onsetDelta | **Calibrated** (Feb 21) |

## Next Actions

### Active
1. **FFT-512 bass-focused analysis** (Priority 7c) — Better kick detection to feed better data upstream. ~200 lines, ~5KB RAM.
2. **Particle filter beat tracking** (Priority 6c) — Fundamentally different approach that handles multi-modal tempo distributions. ~100-150 lines, ~2KB RAM.
3. **Weighted phase deviation fusion** (Priority 7d) — Catches kicks during sustained bass notes. ~50 lines, ~512 bytes.

### Completed
- ~~**Feature testing sweep**~~ — ✅ ODF mean sub OFF, diffframes=2, per-band thresholds ON all tested (Feb 24). None improve overall avg. Current defaults are optimal.
- ~~**CBSS Adaptive Threshold**~~ — ✅ Implemented (SETTINGS_VERSION 20). Prevents phantom beats during silence.
- ~~**Per-sample ACF Harmonic Disambiguation**~~ — ✅ Fixed minimal-01 sub-harmonic (BPM 70→124).
- ~~**Bayesian Tempo Fusion**~~ — ✅ Implemented (SETTINGS_VERSION 18-20). Replaced sequential override chain.
- ~~**Design unified feature cooperation framework**~~ — ✅ Done (Bayesian fusion is the framework).
- ~~**Onset density tracking**~~ — ✅ Done.
- ~~**Diverse test music library**~~ — ✅ 18 tracks (9 original + 9 syncopated).
- ~~**HPS, Pulse train, IOI, FT, ODF mean sub**~~ — ✅ All implemented; IOI/FT/comb now feed Bayesian fusion. HPS/pulse train removed.

---

## Bayesian Tempo Fusion — IMPLEMENTED (Feb 23, 2026)

**Status:** Implemented in SETTINGS_VERSION 18-20. Sequential override chain replaced with unified Bayesian posterior estimation. CBSS adaptive threshold added in v20.

**Architecture summary:**
```
Every 250ms:
  1. Compute autocorrelation of OSS buffer (unchanged)
  2. FOR EACH of 20 tempo bins (60-180 BPM from CombFilterBank):
       - ACF observation: normalized correlation at bin's lag, raised to bayesAcfWeight
       - FT observation: Goertzel magnitude at bin's lag, raised to bayesFtWeight
       - Comb observation: comb filter energy at bin, raised to bayesCombWeight
       - IOI observation: pairwise interval count at bin's lag, raised to bayesIoiWeight
  3. Viterbi transition: spread prior through Gaussian (sigma = bayesLambda * BPM)
  4. Posterior = prediction × [static prior] × ACF × FT × comb × IOI
  5. MAP estimate with quadratic interpolation → BPM
  6. Per-sample ACF harmonic disambiguation: check raw ACF at lag/2 (>50%) and lag*2/3 (>60%)
  7. EMA smoothing (tempoSmoothingFactor) → CBSS beat period update
```

**Key finding from implementation:** Bayesian fusion alone cannot prevent sub-harmonic locking. All four observation signals see sub-harmonics (every-other-beat alignment is real signal at half-tempo). Per-sample ACF harmonic disambiguation (step 6) was required — this is consistent with BTrack/madmom which also include explicit octave correction. Posterior-based disambiguation failed because the posterior is already sub-harmonic dominated.

**Resources:** 228KB flash (28%), 51KB RAM (21%). Net ~25% CPU reduction vs old chain (20 Goertzel evaluations vs ~40, no sequential override cascade).

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | **Design philosophy** — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history + CBSS eval results |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |

## Research Sources (February 2026 Survey)

### Beat Tracking Algorithms

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [BTrack](https://github.com/adamstark/BTrack) | Stark, Davies, Plumbley | 2009 | Autocorrelation + CBSS (our baseline) | Yes |
| [madmom DBN](https://madmom.readthedocs.io/en/v0.16/modules/features/beats.html) | Bock, Krebs, Widmer | 2016 | HMM joint tempo+phase tracking | State-space only |
| [BeatNet](https://github.com/mjhydri/BeatNet) | Heydari et al. | 2021 | CRNN + particle filter with octave investigator | Particle filter only |
| [BeatNet+](https://transactions.ismir.net/articles/10.5334/tismir.198) | Heydari et al. | 2024 | Percussion-invariant representations | No |
| [Real-Time PLP](https://github.com/groupmm/real_time_plp) | Meier, Chiu, Muller | 2024 | Sinusoidal kernel beat prediction | Possible |
| [Beat This!](https://github.com/CPJKU/beat_this) | Foscarin, Schluter, Widmer | 2024 | 20M param transformer (SOTA) | No |
| [IBT](https://archives.ismir.net/ismir2010/paper/000050.pdf) | Oliveira, Gouyon, Martins | 2010 | Real-time multi-agent beat tracking | Yes |
| [BeatRoot](https://courses.cs.washington.edu/courses/cse590m/08wi/Dixon%20-%20Evaluation%20of%20BeatRoot%20(2007).pdf) | Dixon | 2007 | Multiple competing agents | Offline only |
| [Particle Filter Tempo](https://www.researchgate.net/publication/26532312_Particle_Filtering_Applied_to_Musical_Tempo_Tracking) | Hainsworth | 2004 | Sequential Monte Carlo beat tracking | Yes |
| [KF-PDA](https://www.researchgate.net/publication/4343925_On-line_Music_Beat_Tracking_with_Kalman_Filtering_and_Probability_Data_Association_KF-PDA) | Cemgil et al. | 2004 | Kalman filter + probabilistic data association | Yes (limited) |

### Tempo Estimation

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [Fourier Tempogram](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S2_TempogramFourier.html) | Grosche & Muller | 2011 | **Sub-harmonic suppression** (DFT of ODF) | Yes |
| [Autocorrelation Tempogram](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S2_TempogramAutocorrelation.html) | Grosche & Muller | 2011 | Harmonic suppression (complement to Fourier) | Yes (current) |
| [Percival & Tzanetakis](https://webhome.csc.uvic.ca/~gtzan/output/taslp2014-tempo-gtzan.pdf) | Percival & Tzanetakis | 2014 | Pulse train cross-correlation (tested, rejected) | Yes |
| [Comb Filter Bank](https://www.adamstark.co.uk/pdf/papers/comb-filter-matrix-ICMC-2011.pdf) | Stark | 2011 | Resonator bank tempo (tested, same wrong answer) | Yes (current) |
| [Multi-Metrical Tracking](https://www.mdpi.com/2076-3417/9/23/5121) | Holzapfel | 2019 | Hierarchical metrical structure constraints | Possible |
| [Klapuri Multi-Scale](https://www.semanticscholar.org/paper/Tempo-and-beat-analysis-of-acoustic-musical-Scheirer/1fa22e54f70de7a3b36e2ffc602f924f47ec9cbb) | Klapuri | 2006 | Tatum/tactus/bar multi-level tracking | Possible |

### Onset Detection

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [SuperFlux](https://github.com/CPJKU/SuperFlux) | Bock & Widmer | 2013 | Max-filter vibrato suppression (partially implemented) | Yes |
| [CNN Onset Detection](https://www.ofai.at/~jan.schlueter/pubs/2014_icassp.pdf) | Bock & Schlueter | 2014 | Multi-scale CNN onset detector (SOTA) | Distilled only |
| [Onset Detection Revisited](https://ofai.at/papers/oefai-tr-2006-12.pdf) | Dixon | 2006 | Comprehensive ODF comparison (flux, phase, complex) | Yes |
| [Complex Domain](https://www.researchgate.net/publication/200806123_Complex_Domain_Onset_Detection_for_Musical_Signals) | Duxbury | 2003 | Combined magnitude + phase onset detection | Yes |
| [Bello Tutorial](https://ieeexplore.ieee.org/document/1495485/) | Bello et al. | 2005 | Foundational onset detection survey | Reference |
| [Peak Picking](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_PeakPicking.html) | AudioLabs Erlangen | — | Local max + local mean dual peak picker | Yes |
| [Adaptive Thresholding](https://link.springer.com/article/10.1007/s11042-020-08780-2) | Springer | 2020 | GA-optimized median+stddev thresholds | Yes |
| [TinyML Distillation](https://www.nature.com/articles/s41598-025-94205-9) | Nature | 2025 | 1282-parameter distilled models for MCUs | Yes |

### Key Insight: Autocorrelation vs Fourier Tempogram

The fundamental mathematical reason for sub-harmonic locking: autocorrelation of a periodic signal at period T produces peaks at T, 2T, 3T... (sub-harmonics appear). The DFT of the same signal produces peaks at 1/T, 2/T, 3/T... (harmonics appear, sub-harmonics suppressed). These are complementary — using both together provides the most robust tempo estimation. This is documented in Grosche & Muller 2011 and the AudioLabs Erlangen FMP tutorials.
