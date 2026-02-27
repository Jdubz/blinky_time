# Blinky Time - Improvement Plan

*Last Updated: February 27, 2026*

## Current Status

### Completed (February 25, 2026)

**Bayesian Weight Tuning (SETTINGS_VERSION 21-22, refined in 24):**
- Multi-device parallel sweep of all 6 Bayesian parameters using beat event scoring
- Root cause analysis: ACF/FT/IOI observations have fundamental implementation issues (compared to BTrack, madmom, librosa)
- v21: ACF/FT/IOI=0 (disabled), comb=0.7, lambda=0.15, cbssthresh=1.0 → F1 0.590
- v22: Combined validation found bayesacf=0.3 needed to prevent half-time lock → F1 0.519
- v24: Post-spectral re-tuning re-enabled FT=2.0 and IOI=2.0 (spectral processing fixes normalization issues)

**Multi-Device Testing Infrastructure:**
- 3 devices (Long Tube) connected simultaneously via `/dev/ttyACM0,1,2`
- Replaced Playwright browser-based audio playback with `ffplay` — works headless on Raspberry Pi, no X server needed
- Fixed serial port leak in MCP server: `sendCommand('stream fast')` left `this.streaming=false` while firmware was actively streaming, causing `disconnect()` to skip `stream off` and lock the port. Fix: track streaming commands in `sendCommand()` + always send `stream off` in `disconnect()` as safety net
- Multi-device variation test: all 3 devices capture simultaneously from single audio playback. F1 spread 0.014 on techno-minimal-01 — devices are highly consistent
- Multi-device parallel sweep: batches parameter values across N devices (3x speedup with 3 devices)
- Audio routed to USB speakers (JBL Pebbles) via `.asoundrc`
- Music test files (`.mp3` + `.beats.json` ground truth) auto-discovered from `music/` directory

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
- `bfdominance` — band-dominance gate (0.0=disabled)
- `bfdecayratio` + `bfconfirmframes` — post-onset decay gate (0.0=disabled)
- `bfcrestgate` — spectral crest factor gate (0.0=disabled)

### Priority 2: CBSS Beat Tracking + Bayesian Tempo Fusion — Validated (SETTINGS_VERSION 25)

BTrack-style predict+countdown CBSS beat detection with Bayesian tempo fusion. Tempo estimated via unified posterior over 20 bins (60-180 BPM). Comb filter bank is primary observation; harmonic-enhanced ACF (weight 0.8, v25) with 4-harmonic comb and Rayleigh prior prevents sub-harmonic lock. FT and IOI re-enabled at weight 2.0 after spectral processing (v24). CBSS adaptive threshold (1.0) prevents phantom beats. Bidirectional disambiguation (2x, 1.5x, 0.5x checks). Lambda tightened to 0.07 (v25).

**Pre-Bayesian baseline (sequential override chain, Feb 21):** avg Beat F1 **0.472** on 9 tracks.
**Bayesian v20 (all observations on, cbssthresh=0.4, Feb 24):** avg Beat F1 **0.421**.
**Bayesian v21 (comb-only, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.590** (best independent cbssthresh sweep).
**Combined validation (comb+ACF 0.3, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.519** (4-device validated).

**Why v21 independent sweep was misleading:** Each parameter was swept independently against device-saved defaults (which had bayesacf=1). The cbssthresh=1.0 result (F1 0.590) was achieved WITH ACF at weight 1.0, not 0. When all v21 changes were applied together (ACF=0), half-time lock occurred on most tracks (avg F1 0.410). A 4-device bayesacf sweep with v21 base params found 0.3 optimal.

**Bayesian tunable parameters (9 total, SETTINGS_VERSION 25 defaults):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Lambda | `bayeslambda` | **0.07** | Transition tightness (tightened v25 to prevent octave jumps) |
| Prior center | `bayesprior` | 128 | Static prior Gaussian center BPM |
| Prior width | `priorwidth` | 50 | Static prior Gaussian sigma |
| Prior weight | `bayespriorw` | 0.0 | Ongoing static prior strength (off by default) |
| ACF weight | `bayesacf` | **0.8** | Harmonic-enhanced ACF (raised v25 — 4-harmonic comb + Rayleigh prior) |
| FT weight | `bayesft` | **2.0** | Fourier tempogram observation (re-enabled by spectral processing, v24) |
| Comb weight | `bayescomb` | 0.7 | Comb filter bank observation weight |
| IOI weight | `bayesioi` | **2.0** | IOI histogram observation (re-enabled by spectral processing, v24) |
| CBSS threshold | `cbssthresh` | **1.0** | Adaptive beat threshold (CBSS > factor * mean, 0=off) |

**4-device validation results (Feb 25, bayesacf sweep with v21 base params):**

| Track | v20 baseline | acf=0 | acf=0.3 | acf=0.7 | acf=1.0 |
|-------|:-----------:|:-----:|:-------:|:-------:|:-------:|
| trance-party | 0.775 | 0.583 | 0.596 | 0.326 | 0.409 |
| minimal-01 | 0.695 | 0.467 | **0.679** | 0.540 | 0.557 |
| infected-vibes | 0.691 | 0.563 | **0.764** | 0.574 | 0.615 |
| goa-mantra | 0.605 | 0.596 | **0.769** | 0.540 | 0.318 |
| minimal-emotion | 0.486 | 0.484 | **0.678** | 0.588 | 0.571 |
| deep-ambience | 0.404 | 0.437 | **0.636** | 0.489 | 0.263 |
| machine-drum | 0.224 | 0.000 | 0.032 | 0.000 | 0.000 |
| trap-electro | 0.190 | 0.159 | 0.065 | 0.269 | 0.242 |
| dub-groove | 0.176 | 0.405 | **0.452** | 0.118 | 0.031 |
| **Average** | **0.472** | **0.410** | **0.519** | **0.383** | **0.334** |

#### FT and IOI Observation History (Reference Implementation Comparison)

**Originally disabled (v21-22, Feb 25):** Comparison with BTrack, madmom, and librosa identified normalization issues.

**Re-enabled (v24, Feb 26):** Spectral compressor + whitening (v23) fixed the normalization problems that made FT and IOI observations unreliable. Both re-enabled at weight 2.0 with +49% avg Beat F1 vs control.

**ACF (bayesacf=0.8, v25):** Harmonic-enhanced ACF with 4-harmonic comb summation and Rayleigh tempo prior. BTrack-style approach: for each candidate period T, sum ACF at 1T, 2T, 3T, 4T with spread windows — fundamental gets 4x advantage over sub-harmonics. Rayleigh weighting peaked at ~120 BPM. Weight raised from 0.3 to 0.8 because the harmonic comb makes ACF a reliable signal. Previous 0.3 was necessary because raw single-point ACF had sub-harmonic bias.

**FT (bayesft=2.0):** Originally disabled — Goertzel magnitude-squared with mean normalization produced near-flat observation vectors. Spectral compressor normalizes gross signal level, making the Goertzel output discriminative again. Re-enabled at weight 2.0 in v24.

**IOI (bayesioi=2.0):** Originally disabled — unnormalized onset counts (1-10+ range) dominated the multiplicative posterior. Spectral whitening produces more consistent onset detection across frequency bands, stabilizing IOI interval counts. Re-enabled at weight 2.0 in v24.

**Architectural note:** Multiplicative fusion (`posterior = prediction × ACF × FT × comb × IOI`) assumes comparably-scaled signals. The spectral processing pipeline (compressor + whitening) produces more normalized inputs to all downstream detectors, improving the fusion assumption. BTrack uses a sequential pipeline; madmom uses a joint DBN; librosa uses windowed ACF with log-normal prior.

**Known limitations:**
- DnB half-time detection — both librosa and firmware detect ~117 BPM instead of ~170 BPM. Acceptable for visual purposes
- trap/syncopated low F1 — energy-reactive mode is the correct visual response
- deep-ambience low F1 — organic mode fallback is correct for ambient content

**Ongoing static prior — tested and disabled (Feb 23):**
Static Gaussian prior (centered at `bayesprior`, sigma `priorwidth`) multiplied into posterior each frame. Helps tracks near 128 BPM (minimal-01: 69.8→97.3 BPM) but actively hurts tracks far from center (trap-electro: 112→131 BPM, machine-drum: 144→124 BPM). Fundamental limitation — can't distinguish correct off-center tempo from sub-harmonic. Per-sample ACF harmonic disambiguation is the proper fix.

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
- Raw ACF observation in Bayesian: Sub-harmonic bias without inverse-lag normalization (Feb 25) — **fixed**: inverse-lag normalization added, then harmonic comb + Rayleigh prior added (v25), ACF weight raised to 0.8
- Fourier tempogram at full weight without spectral processing: Mean normalization destroys discriminability (Feb 25) — **fixed**: spectral compressor+whitening (v23) enabled re-activation at weight 2.0 (v24)
- IOI histogram at full weight without spectral processing: Unnormalized counts dominate posterior (Feb 25) — **fixed**: spectral whitening stabilized onset detection, re-enabled at weight 2.0 (v24)

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

### Priority 3: Microphone Sensitivity — RESOLVED (Feb 2026)

**Problem:** Raw ADC level is 0.01-0.02 at maximum hardware gain (80 = +20 dB) with music playing from speakers. This is ~60 dB SPL at the mic — conversation level, not music level.

**Solution: Spectral compressor + per-bin adaptive whitening (v23+, SETTINGS_VERSION 23-24)**

Instead of the originally proposed raw-sample pre-amplification, a spectral-domain approach was implemented that addresses the root cause more effectively:

1. **Soft-knee compressor** (Giannoulis/Massberg/Reiss 2012) — Frame-level RMS-based compression applied to FFT magnitudes. Threshold -30dB, ratio 3:1, soft knee 15dB, **6dB makeup gain**, 1ms attack, 2s release. Boosts quiet signals more than loud ones, normalizing gross signal level differences caused by mic placement and room acoustics.

2. **Per-bin adaptive whitening** (Stowell & Plumbley 2007) — Each FFT bin normalized by its running maximum (decay 0.997, ~5s memory). Makes change-based detectors (BandFlux) invariant to sustained spectral content regardless of absolute signal level.

**Why spectral-domain beats raw pre-amp:**
- Raw pre-amp (multiplying int16 samples by 2-4x) amplifies noise equally with signal — no SNR improvement
- Spectral compressor applies frequency-selective gain based on frame energy — quiet frames get more boost
- Per-bin whitening auto-scales each frequency band independently — works at any mic level
- Makeup gain (+6dB) provides a fixed boost in the spectral domain where it matters for detection

**Impact:** Spectral processing fixed FT and IOI normalization issues, enabling their re-activation at weight 2.0 in v24 (avg Beat F1 +49% vs control). The pipeline also improved BandFlux onset detection by normalizing spectral dynamics across different acoustic environments.

**Hardware findings (for reference):**
- Mic: MSM261D3526H1CPM, -26 dBFS sensitivity, 64 dB SNR — industry standard
- PDM clock: nRF52840 uses 1.28 MHz; mic's optimal is 2.4 MHz. Below optimal but within spec. May cost 1-2 dB SNR.
- Hardware gain: 0-80 range (±20 dB in 0.5 dB steps), firmware uses full range with AGC
- Physical mic placement remains the single highest-impact variable for raw signal level

**Spectral pipeline parameters (10 total, all tunable via serial):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Whitening enabled | `whitenenabled` | true | Per-bin adaptive whitening |
| Whitening decay | `whitendecay` | 0.997 | Running max decay (~5s memory) |
| Whitening floor | `whitenfloor` | 0.001 | Noise floor for normalization |
| Compressor enabled | `compenabled` | true | Soft-knee compressor |
| Threshold | `compthresh` | -30.0 dB | Compression onset level |
| Ratio | `compratio` | 3.0 | Compression ratio (3:1) |
| Knee | `compknee` | 15.0 dB | Soft knee width |
| Makeup gain | `compmakeup` | 6.0 dB | Post-compression gain boost |
| Attack | `compattack` | 0.001s | Attack time constant |
| Release | `comprelease` | 2.0s | Release time constant |

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

#### 6b. CBSS Adaptive Threshold — TUNED (Feb 25, 2026)

**Status:** Implemented as `cbssThresholdFactor` (default **1.0**, raised from 0.4 in SETTINGS_VERSION 21). Beat fires only if `CBSS > factor * cbssMean_` where cbssMean_ is an EMA with tau ~120 frames (~2s). Setting to 0 disables the threshold (countdown-only, old behavior).

**Impact:** Multi-device sweep (Feb 25) found thresh=1.0 is optimal (F1 0.590 vs 0.209 at 0.4). Higher threshold = fewer phantom beats during low-energy sections = more stable BPM tracking = paradoxically better recall.

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

### Priority 7: Onset Detection — Gap Analysis vs State of the Art (Feb 27, 2026)

Comprehensive comparison against SuperFlux, BTrack (ComplexSpectralDifferenceHWR), madmom, aubio, and CNN-based detectors. Our BandWeightedFlux detector is well-aligned with established best practices but has specific gaps in peak picking, frequency resolution, and ODF design.

#### Onset Detection Performance Context

| Method | F1 (50ms) | F1 (25ms) | Type | Embedded |
|--------|-----------|-----------|------|----------|
| CNN ensemble (Schlüter 2014) | ~0.90 | ~0.87 | Offline neural | No |
| SuperFlux (Böck 2013) | ~0.85-0.88 | ~0.84-0.85 | Offline DSP | Yes |
| ComplexFlux | ~0.86-0.89 | — | Offline DSP | Yes |
| Complex domain (Duxbury 2003) | ~0.78-0.82 | — | DSP | Yes |
| Standard spectral flux | ~0.80-0.83 | ~0.78-0.80 | DSP | Yes |
| **BandWeightedFlux (ours)** | **~0.44** | — | **Real-time DSP** | **Yes** |

Our lower scores vs literature benchmarks are primarily due to: (1) room acoustics via microphone vs studio recordings, (2) 16 kHz / FFT-256 vs 44.1 kHz / FFT-2048, (3) single-threshold peak picking without local-max confirmation. Percussion onset detection on clean recordings is considered "solved" (F1 > 0.85) — our challenge is the degraded real-world acoustic environment.

#### What BandWeightedFlux Does Right (Validated by Literature)

1. **Log-compressed spectral flux** — Standard in SuperFlux, madmom, librosa. Any compression improves F1 by 5-10+ points.
2. **Per-bin adaptive whitening** (Stowell & Plumbley 2007) — Exactly the aubio algorithm. 10+ F1 point improvement in literature.
3. **Soft-knee spectral compressor** (Giannoulis 2012) — More sophisticated than most onset detectors (typically just log compression).
4. **SuperFlux 3-bin max filter** — Reduces false positives up to 60% on vibrato-heavy material (Böck & Widmer 2013).
5. **Half-wave rectified spectral flux** — Standard in all top non-neural systems.
6. **Additive threshold** (`mean + delta`) — BTrack, SuperFlux, and madmom all use additive/subtractive thresholds.
7. **Asymmetric threshold update** (skip update on detection frames) — Prevents loud onsets from inflating noise floor. Novel but sound engineering.
8. **Band weighting** (bass=2.0, mid=1.5, high=0.1) — Aligned with literature on drum-focused onset detection (Scheirer 1998).
9. **Onset delta filter** (minOnsetDelta=0.3) — Addresses the "gradual onset" failure mode identified in the literature as a key challenge.

#### 7a. Per-Band Independent Thresholds — TESTED, KEEP OFF (Feb 24, 2026)

Independent adaptive thresholds per band (bass/mid/high). Detection fires if ANY band exceeds its own threshold × multiplier. **Disabled by default** (`bfperbandthresh=0`).

- **Full 9-track regression:** avg F1 0.421→0.354 (**-0.067**) — major regressions on quiet/sparse tracks
- **Verdict:** Keep disabled. Literature supports this finding — splitting detection by band without per-band calibration increases false positives.

#### 7b. Multi-Frame Temporal Reference — TESTED, KEEP AT 1 (Feb 24, 2026)

Configurable `diffframes` (1-3). Default remains 1. diffframes=2 generates too many transients (avg -0.098 F1).

#### 7c. Dual-Threshold Peak Picking — NEW, HIGH PRIORITY

**The biggest gap in our onset detection.** SuperFlux, madmom, and librosa all use dual-threshold peak picking requiring BOTH:
1. ODF sample is a **local maximum** within a window
2. ODF sample **exceeds local mean + delta**

We use threshold-only (`combinedFlux > averageFlux + threshold`). No local maximum check. This means:
- We fire on the **rising edge** of flux peaks rather than the true peak (imprecise timing)
- Consecutive frames above threshold all fire (suppressed only by cooldown, not peak detection)
- The cooldown does double duty: rate-limiting AND peak-selecting

**SuperFlux default parameters:**
```
pre_max=10ms, post_max=50ms   // local max window (post_max provides look-ahead)
pre_avg=150ms, post_avg=0ms   // local mean window (causal)
combine=30ms                   // minimum inter-onset interval
delta=1.1                      // threshold above local mean
```

**Causal adaptation for our system:**
- Local max: `ODF[t] >= ODF[t-1] && ODF[t] >= ODF[t+1]` with 1-frame look-ahead (16ms)
- Local mean: existing `averageFlux` EMA (causal, no look-ahead needed)
- Combine: existing adaptive cooldown
- Even 1-frame look-ahead (16ms) is imperceptible for visualization

**Implementation:** Buffer 1-2 frames of ODF output. Only report detection when the buffered frame is confirmed as a local maximum. The current detection fires at frame N; the new logic would fire at frame N+1 after confirming N is a peak.

- **Effort:** Low (~40 lines). 1-2 frame ring buffer + local max check before emitting detection.
- **Expected impact:** Medium-High. Literature shows dual-threshold adds ~2-5% F1 over threshold-only. Improves timing precision of every detection.
- **References:** SuperFlux (Böck & Widmer 2013), madmom `peak_picking`, librosa `peak_pick`

#### 7d. Hi-Res Bass via Goertzel — ALREADY IMPLEMENTED, TEST ENABLING

The hi-res bass path (`hiResBassEnabled`) is already coded in BandWeightedFluxDetector. It uses 512-sample Goertzel for 12 bass bins at 31.25 Hz/bin (vs 6 FFT bins at 62.5 Hz/bin). This doubles bass frequency resolution, giving 2-4 bins for kick drum fundamental (40-80 Hz) vs 1-2 bins currently.

At FFT-256/16kHz, kick drum fundamental (40-80 Hz) and bass guitar (80-250 Hz) can share the same 1-2 bins. The kick's attack is masked by the bass's sustain. Hi-res bass separates them.

- **Effort:** Trivial (set `hiResBassEnabled=true`, run 9-track sweep)
- **Expected impact:** Medium for kick-specific detection
- **References:** Multi-resolution spectral flux (Bello 2005), Böck CNN multi-scale input (ICASSP 2014)

#### 7e. Complex Spectral Difference for Rhythm ODF — MEDIUM PRIORITY

BTrack's default ODF is ComplexSpectralDifferenceHWR, which uses both magnitude AND phase:
```
phaseDeviation[k] = phase[k] - 2*prevPhase[k] + prevPhase2[k]
CSD[k] = sqrt(mag[k]² + prevMag[k]² - 2*mag[k]*prevMag[k]*cos(phaseDeviation[k]))
ODF = Σ max(0, CSD[k])  // half-wave rectified
```

This catches pitched onsets at constant energy (chord changes, bass note changes) that magnitude flux misses. We already compute and store phase data in SharedSpectralAnalysis.

**Caveat:** Phase is extremely sensitive to noise. Through a microphone in a reverberant room, phase coherence degrades rapidly. Dixon (2006) found CSD only slightly outperforms spectral flux on clean recordings and performs worse on noisy signals. **Recommended for CBSS rhythm ODF only, not for visual transient detection.**

- **Memory:** ~512 bytes (one extra frame of phase history)
- **CPU:** ~0.5% (128 trig ops per frame)
- **Effort:** Medium (~80 lines)
- **Expected impact:** Uncertain for mic-in-room. Test as CBSS ODF only.
- **References:** Duxbury 2003, Dixon 2006 (Onset Detection Revisited), Bello 2005

#### 7f. Log-Spaced Sub-Band Filterbank — MEDIUM PRIORITY

SuperFlux uses 24 bands/octave (~216 total). madmom CNN uses 80 mel bands. We use 3 bands (bass/mid/high) with raw FFT bins averaged per band. This is very coarse — a kick at 60 Hz and a bass note at 300 Hz share the same "bass" band.

With 128 FFT bins at 62.5 Hz/bin, we could create 12-24 log-spaced bands. At low frequencies, each band maps to 1-2 FFT bins (limited by resolution); at high frequencies, bands span many bins. The finer grouping would:
- Separate kick fundamental from bass guitar
- Separate snare crack from vocal energy
- Give per-band flux more discriminative power

**However:** Our 3-band approach works well for the specific kick/snare visual use case. The coarse grouping is a deliberate simplification. The literature filterbanks are for general-purpose onset detection across all instrument types.

- **Effort:** Medium (~80 lines, new band definition table + per-band flux loop)
- **Expected impact:** Low-Medium for kick/snare use case. Higher for general onset detection.
- **Risk:** Requires per-band threshold calibration. May interact with existing `bassWeight/midWeight/highWeight`.

#### 7g. Knowledge-Distilled TinyML Onset Detector — LONG TERM

Train CNN on desktop with labeled EDM onset data, distill to tiny student model (~1K-5K params), quantize to INT8, deploy via TensorFlow Lite Micro. The only approach that can learn complex spectral patterns (kicks vs bass vs pads vs room modes).

Performance gap between DSP and neural onset detection: ~10-15 F1 points on standard benchmarks. CNN F1 ~0.90 vs SuperFlux ~0.85-0.88. This gap is the performance ceiling that DSP improvements cannot cross.

- **Memory:** ~5-15KB (model + activations)
- **CPU:** ~200-500us/frame (with CMSIS-NN on Cortex-M4F)
- **Flash:** ~5-20KB model weights
- **Risk:** High — requires training data, model development, TFLM integration
- **References:** Böck & Schlüter (ICASSP 2014), efficient CNN (81K params, 2018), TinyML distillation (Nature 2025)

#### Onset Detection: Approaches Tested and Rejected

| Approach | Why Not | Tested |
|----------|---------|--------|
| Per-band independent thresholds | Increases FPs on sparse tracks (-0.067 avg F1) | Feb 24 |
| Multi-frame diffframes=2 | Too many transients, hurts phase (-0.098 avg F1) | Feb 24 |
| Post-onset decay confirmation | Adds latency, rejects synth stabs | Feb 22 |
| Band-dominance gate | Redundant with high-weight suppression | Feb 22 |
| Spectral crest factor gate | Kills kicks through room resonances | Feb 22 |
| CNN/RNN on nRF52840 | Not feasible (64 MHz, no matrix acceleration) | Research |
| Complex domain for visual transients | Phase too noisy via microphone in room | Research |
| Overlapping FFT windows (125 fps) | Doubles FFT CPU cost for marginal timing gain | Research |

#### Onset Detection: What NOT to Change (Validated)

| Feature | Rationale |
|---------|-----------|
| BandFlux Solo | Single detector outperforms ensemble (+14% Beat F1). Literature confirms cleaner signal > multi-detector voting |
| Additive threshold | Correct for low-signal environments. Used by BTrack, SuperFlux, madmom |
| High-band suppression (0.1) | Correct for kick/snare visual use case |
| Onset delta filter (0.3) | Valid solution for gradual onset rejection |
| Asymmetric threshold update | Prevents onset self-inflation. Sound engineering |
| Adaptive cooldown | Maps to SuperFlux's `combine` parameter. Tempo-awareness correct for visualization |
| Log compression (gamma=20) | Aggressive but appropriate for low-SNR mic input |
| Per-bin whitening (decay=0.997) | Faster than aubio default (250s) but appropriate for live music |

---

## Calibration Status (Feb 27, 2026)

**Bayesian fusion v25 with BTrack-style improvements.** Comb filter bank is the primary tempo observation. Harmonic-enhanced ACF (weight 0.8) with 4-harmonic comb summation and Rayleigh prior replaces raw single-point ACF. Lambda tightened to 0.07 to prevent octave jumps. FT and IOI re-enabled at weight 2.0 (spectral processing fixed normalization, v24). CBSS adaptive threshold at 1.0 prevents phantom beats. Bidirectional harmonic disambiguation (2x, 1.5x, 0.5x checks). **Needs 18-track validation** — see NEXT_TESTS.md.

**Multi-device sweep capability:** 4 devices sweep parameters in parallel (4x speedup). Example: `param-tuner multi-sweep --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM3 --params bayesacf --duration 30`. Uses real music files with ground truth annotations, ffplay for headless audio playback.

| Feature | Parameters | Status |
|---------|:----------:|--------|
| **Spectral pipeline** | compenabled, compthresh=-30, compratio=3, compknee=15, compmakeup=6, whitenenabled, whitendecay=0.997 | **Validated** (SETTINGS_VERSION 23-24) — resolves mic sensitivity, enables FT/IOI |
| **Bayesian weights** | bayesacf=0.8, bayesft=0.0, bayescomb=0.7, bayesioi=0.0, bayeslambda=0.07, bayespriorw=0 | **v29** — FT+IOI disabled (v28), harmonic comb ACF + Rayleigh prior (v25). Needs 18-track validation |
| **CBSS adaptive threshold** | cbssthresh=1.0 | **Validated** (SETTINGS_VERSION 22) — prevents phantom beats |
| ODF Mean Subtraction | odfmeansub (toggle) | **Essential** — keep ON. OFF destroys BPM (Feb 24) |
| Per-band thresholds | bfperbandthresh, bfpbmult | **Tested, keep OFF** — hurts weak tracks (Feb 24) |
| Multi-frame diffframes | bfdiffframes | **Tested, keep at 1** — diffframes=2 too many transients (Feb 24) |
| BandFlux core params | gamma, bassWeight, threshold, onsetDelta | **Calibrated** (Feb 21) |

## State-of-the-Art Gap Analysis (Feb 27, 2026)

Comprehensive comparison against BTrack, madmom DBN, BeatNet, Essentia, and published academic systems. Current avg Beat F1: **0.519**. BTrack (nearest comparable DSP-only system): **~65-75%**. The ~15-20 point gap is explained by architectural differences documented below.

### Performance Context

| System | Type | Beat F1 | Hardware |
|--------|------|---------|----------|
| Beat This! (2024) | Offline transformer | ~89% | GPU |
| madmom DBN (offline) | Offline RNN+DBN | ~88% | CPU |
| BeatNet+ (2024) | Online CRNN+PF | ~81% | CPU |
| madmom forward (online) | Online RNN+HMM | ~74% | CPU |
| BTrack (online) | Online DSP | ~65-75% | Embedded OK |
| **Blinky (online)** | **Online DSP** | **~52%** | **nRF52840 64MHz** |

### What We're Doing Right (Validated by Literature)

These are confirmed best practices — keep them:

1. **Continuous ODF → CBSS** — CBSS is fed by `computeSpectralFluxBands()` (continuous spectral flux), not binary transient events. Matches BTrack architecture.
2. **Adaptive spectral whitening** (Stowell & Plumbley 2007) — Per-bin normalization. Literature shows 10+ F1 point improvement.
3. **Soft-knee spectral compression** (Giannoulis 2012) — Standard in all top systems.
4. **Inverse-lag ACF normalization** — Already implemented (`AudioController.cpp:491-494`). Corrects sub-harmonic bias. BTrack does the same.
5. **SuperFlux-style max filtering** — In both BandWeightedFlux and computeSpectralFluxBands. Exactly the Böck & Widmer 2013 technique.
6. **Comb filter bank** (Scheirer 1998) — Best single non-neural tempo estimator.
7. **BTrack-style predict+countdown CBSS** — Standard non-neural real-time beat tracking.
8. **Band-weighted spectral flux** — Emphasizing bass/mid over high is well-supported.
9. **ODF mean subtraction** — Essential for ACF discriminability (tested Feb 24, OFF destroys BPM).
10. **CBSS adaptive threshold** — Prevents phantom beats. Standard in BTrack (adaptive threshold on cumulative score).

---

## Next Actions

### Phase 1: Simplify — Remove Wasteful/Detrimental Features

Each removal must be tested with a 9-track sweep before/after to confirm no regression.

#### 1a. Test Disabling FT and IOI Observations — NEEDS VALIDATION

**Problem:** FT and IOI have documented algorithmic issues (see root cause analysis in `blinky-test-player/PARAMETER_TUNING_HISTORY.md`). No reference implementation (BTrack, madmom, librosa) uses Fourier tempogram or IOI histograms in real-time beat tracking. The +49% improvement attributed to their v24 re-enablement may be confounded by simultaneous changes (spectral processing, cbssthresh tuning) rather than FT/IOI themselves.

**Evidence against FT:**
- Mean normalization in Goertzel produces near-flat observation vectors (all bins ≈ 1.0)
- Independent sweep found bayesft=0 optimal
- BTrack does not use Fourier tempogram — uses comb filter on ACF instead
- madmom uses RNN activations, not Fourier tempogram, for real-time

**Evidence against IOI:**
- Unnormalized counts (1-10+ range) can dominate multiplicative posterior
- O(n²) complexity with onset count (up to 48×48×20 operations)
- 2x folding biases toward fast tempos
- No reference implementation uses IOI histograms for polyphonic beat tracking

**Test plan:**
1. Set `bayesft=0, bayesioi=0` (disable observations)
2. Run 9-track beat F1 sweep vs current defaults (`bayesft=2.0, bayesioi=2.0`)
3. If F1 is equal or better: make the removal permanent, delete the code
4. If F1 is worse: keep enabled but document which tracks benefit and why

**Effort:** Trivial (parameter change). **Impact:** Removes 2 fragile signals from multiplicative fusion + ~1-2% CPU.

#### 1b. Simplify Ensemble Infrastructure for Solo Detector

**Problem:** EnsembleFusion runs agreement-based confidence scaling, weighted averaging, and multi-detector cooldown logic — all designed for N detectors. With BandFlux Solo (1 detector enabled), this is pure overhead. The agreementBoosts array, minConfidence filtering, and dominant detector tracking serve no purpose.

**Action:** Simplify EnsembleFusion to pass BandFlux output directly when only 1 detector is enabled. Keep the multi-detector path as dead code for future experimentation, but bypass it at runtime.

**Test plan:** Before/after 9-track sweep to confirm no regression from simplification.

**Effort:** Low (~30 lines). **Impact:** Cleaner code, marginally less CPU.

#### 1c. Evaluate Adaptive Band Weighting Cost/Benefit

**Problem:** ~1600 lines of code for adaptive band weighting (per-band OSS buffers, cross-band correlation, peakiness crest factor, per-band autocorrelation). Consumes 2.9 KB RAM for per-band OSS buffers + ~1% CPU. The conditions for adaptive weights to activate (periodicity > 0.1, avgEffective > 0.15, bandSynchrony > 0.3) may rarely be met, causing the system to fall back to fixed defaults most of the time.

**Test plan:**
1. Set `adaptiveBandWeightEnabled=false`
2. Run 9-track beat F1 sweep vs enabled
3. If F1 is equal: remove the feature (reclaim 2.9 KB RAM + 1% CPU + ~1600 lines)
4. If F1 is worse on specific tracks: document which tracks and evaluate if the complexity is justified

**Effort:** Trivial (parameter toggle). **Impact:** Potentially major simplification.

#### 1d. Remove Dual ODF Computation (Consolidate with Phase 2.4)

**Problem:** Two parallel spectral flux computations on the same data:
- `computeSpectralFluxBands()` in AudioController — raw band-weighted flux for CBSS
- `BandWeightedFluxDetector::computeBandFlux()` — log-compressed flux with thresholding for transients

These run independently on the same spectral magnitudes, producing different views of "what just happened." This is wasteful AND architecturally harmful (see Phase 2.4).

**Action:** Defer to Phase 2.4 (Unify ODF). The fix for waste and the fix for the architectural gap are the same change.

---

### Phase 2: Improve — Close Gaps with State of the Art

Each improvement must be tested and calibrated independently before combining. Use 9-track (or 18-track) beat F1 sweep with 4-device parallel testing.

#### 2.1. Only Update beatPeriodSamples at Beat Boundaries

**Gap:** Tempo changes can happen at arbitrary times (every 250ms during autocorrelation), causing CBSS to use a beat period that changed mid-prediction. BTrack only calls `calculateTempo()` when a beat fires — tempo and beat timing are synchronized.

**What BTrack does:** `calculateTempo()` runs inside the `if (timeToNextBeat == 0)` block. The beat period used by CBSS stays constant between beats.

**What we do:** `runAutocorrelation()` runs every 250ms regardless of beat phase. The resulting BPM immediately updates `beatPeriodSamples_`, which CBSS uses for its log-Gaussian window. A mid-beat tempo change shifts the window while a prediction is in flight.

**Fix:** Continue running autocorrelation every 250ms (the Bayesian posterior needs frequent updates to track changes). But defer applying the new `beatPeriodSamples_` to CBSS until the next beat fires. Store the pending value in `pendingBeatPeriod_` and apply it in `detectBeat()` when `timeToNextBeat <= 0`.

**Test plan:**
1. Implement pending beat period
2. Run 9-track beat F1 sweep before/after
3. Expect improvement on tracks with stable tempo (fewer mid-beat period discontinuities)
4. Watch for regression on tracks with rapid tempo changes (deferred update = slower adaptation)

**Effort:** Low (~20 lines). **Impact:** High — synchronizes tempo and beat tracking.

#### 2.2. Increase Tempo Resolution (20 → 40+ Bins)

**Gap:** 20 bins over 60-180 BPM = 6 BPM per bin. At 120 BPM, 1 BPM error = 4.2ms per beat — after 10 beats, 42ms cumulative drift. BTrack uses 41 bins (80-160 BPM, 2 BPM steps). madmom uses frame-level resolution (~80 distinct periods at 100fps).

**Fix:** Increase `NUM_TEMPO_BINS` from 20 to 40 (3 BPM steps) or 60 (2 BPM steps).

**Cost analysis:**
- Transition matrix: 40×40×4 = 6.4 KB (vs current 20×20×4 = 1.6 KB). Precomputed once.
- Per-update: O(N²) = 1600 multiplies at 40 bins vs 400 at 20 bins. At 250ms intervals on 64 MHz, negligible.
- Comb filter bank: 40 filters × 60 samples × 4 bytes = 9.6 KB (vs current 4.8 KB). This is the main memory cost.
- Total: ~+8 KB RAM. Within budget (14 KB used of 256 KB available).

**Test plan:**
1. Implement 40 bins, re-run 9-track sweep
2. If better: try 60 bins
3. Check memory usage stays under 25 KB total
4. Re-calibrate Bayesian weights if needed (more bins may change optimal exponents)

**Effort:** Low-Medium (~50 lines, mainly constants and array sizes). **Impact:** High — finer tempo resolution reduces cumulative phase drift.

#### 2.3. Adaptive ODF Threshold Before ACF (BTrack-style)

**Gap:** BTrack applies a sliding window adaptive threshold to the ODF before computing the ACF. This removes slowly-varying energy envelopes (verse/chorus dynamics, crescendos), leaving only impulsive onsets for the ACF to find periodicity in. Our ODF goes into the ACF with only 5-point causal smoothing — the ACF can find periodicity in arrangement-level dynamics rather than beat-level dynamics.

**BTrack algorithm:**
```
For each ODF sample:
    localMean = mean(ODF[i-8 : i+7])  // 16-sample window (pre=8, post=7)
    thresholded[i] = max(0, ODF[i] - localMean)
```

**Adaptation for causal (real-time) operation:** BTrack's threshold uses a post-window (looks 7 samples ahead), which isn't possible in real-time at frame rate. Options:
- Use fully causal window (pre=15, post=0) — trades look-ahead for latency
- Apply threshold only to the OSS buffer retrospectively before ACF — the buffer contains 6s of history, so a centered window is fine for the ACF computation

**Test plan:**
1. Implement local-mean subtraction on OSS buffer before autocorrelation (centered window, applied to buffer not real-time stream)
2. Run 9-track beat F1 sweep before/after
3. Expect improvement on tracks with dynamic energy (verse/chorus, breakdowns)
4. Check periodicity strength stability (should be more consistent)

**Effort:** Low (~30 lines). **Impact:** Medium — cleaner ACF input = more reliable tempo estimation.

#### 2.4. Unify ODF — Feed BandFlux Pre-Threshold Activation to Beat Tracker

**Gap:** The transient detector (BandWeightedFlux) and beat tracker (computeSpectralFluxBands) compute spectral flux independently with different preprocessing. The transient detector has log compression, onset delta filtering, hi-hat rejection — all tuned for visual aesthetics. The beat tracker's ODF has none of these. This means the beat tracker might lock onto energy patterns that the transient detector suppresses (or vice versa).

**What BTrack does:** Single ODF (ComplexSpectralDifferenceHWR) feeds BOTH onset detection (peak picking) and beat tracking (CBSS). One representation, two consumers.

**Fix:** Extract the continuous (pre-threshold) activation value from BandWeightedFlux and use it as the ODF for CBSS. Apply thresholding/cooldown only for the visual pulse output. This:
- Eliminates the duplicate `computeSpectralFluxBands()` computation (~100 lines)
- Unifies what the system "hears" — transient detection and beat tracking see the same signal
- The log compression in BandFlux may actually help the ACF (compresses dynamic range, standard in the literature)
- Keeps BandFlux's vibrato suppression and band weighting for the shared ODF

**Test plan:**
1. Add a `getPreThresholdFlux()` accessor to BandWeightedFluxDetector
2. Replace `computeSpectralFluxBands()` call with pre-threshold BandFlux value
3. Run 9-track beat F1 sweep
4. If BandFlux's log compression hurts ACF: add a configurable compression bypass for the CBSS path
5. Re-calibrate `beatoffset` (timing may shift due to different ODF characteristics)

**Effort:** Medium (~50 lines of plumbing, ~100 lines removed). **Impact:** High — eliminates ODF disagreement between transient detection and beat tracking.

#### 2.5. Simplify Bayesian Fusion (Conditional on 1a Results)

**Gap:** Multiplicative fusion of 4 independent estimators is fragile. If any estimator produces near-zero for the correct bin, the posterior collapses. BTrack uses a sequential pipeline (ACF → comb filter on ACF → Rayleigh → Viterbi). madmom uses a single observation model into a DBN. No reference system uses multiplicative fusion of 4 independent estimators.

**Options (evaluate after Phase 1a):**

**Option A — Reduce to Comb + ACF only:**
If Phase 1a confirms FT/IOI don't help, simplify to `posterior = prediction × combObs × acfObs`. Two well-understood signals. Removes ~150 lines of FT/IOI observation code.

**Option B — Switch to log-domain additive fusion:**
`logPosterior[i] = w_comb * log(combObs[i]) + w_acf * log(acfObs[i]) + ...`
Numerically more stable. Individual estimators contribute proportionally rather than multiplicatively vetoing. Equivalent to weighted geometric mean.

**Option C — BTrack-style pipeline:**
Apply comb filter to ACF values (not to raw ODF), then Rayleigh weight, then Viterbi-like DP. Each stage refines the previous one's output. More robust because failure in one stage doesn't multiply through.

**Test plan:** Implement the option that aligns with Phase 1a results. 9-track sweep. Compare stability (F1 variance across tracks) not just average.

**Effort:** Medium. **Impact:** Medium — reduces fragile interaction effects.

#### 2.6. Dual-Threshold Peak Picking for BandFlux (Priority 7c)

**Gap:** The biggest gap in our onset detection. SuperFlux, madmom, and librosa all use dual-threshold peak picking requiring BOTH a local maximum AND exceeding the threshold. We use threshold-only, which fires on rising edges rather than true peaks, with cooldown doing double-duty as rate limiter and peak selector.

**Fix:** Buffer 1-2 frames of ODF output. Only report detection when the buffered frame is confirmed as a local maximum. Add 1-frame look-ahead (16ms latency, imperceptible for visualization).

```
Detection requires ALL of:
1. combinedFlux[t] > averageFlux + threshold        (existing threshold check)
2. combinedFlux[t] >= combinedFlux[t-1]              (rising or peak)
3. combinedFlux[t] >= combinedFlux[t+1]              (confirmed peak, 1-frame delay)
4. Cooldown elapsed                                   (existing adaptive cooldown)
```

**Test plan:**
1. Implement 1-frame look-ahead ring buffer in BandWeightedFluxDetector
2. Add local max condition: detection deferred 1 frame, emitted only if confirmed as peak
3. Run 9-track transient F1 sweep (timing precision should improve)
4. Run 9-track beat F1 sweep (better timing → better beat tracking)
5. Re-calibrate `beatoffset` if timing shifts

**Effort:** Low (~40 lines). **Impact:** Medium-High — improves timing precision of every detection, reduces double-fires.

#### 2.7. Enable Hi-Res Bass (Priority 7d)

**Already implemented** in BandWeightedFluxDetector (`hiResBassEnabled`). 512-sample Goertzel for 12 bass bins at 31.25 Hz/bin. Doubles bass resolution vs 6 FFT bins at 62.5 Hz/bin.

**Test plan:**
1. Set `hiResBassEnabled=true`
2. Run 9-track transient F1 sweep (focus on kick-heavy tracks)
3. Run 9-track beat F1 sweep
4. If improvement: make default, update SETTINGS_VERSION

**Effort:** Trivial (parameter toggle). **Impact:** Medium for kick discrimination.

#### 2.8. Complex Spectral Difference ODF for Rhythm Tracking (Priority 7e)

BTrack's default ODF (ComplexSpectralDifferenceHWR) uses both magnitude AND phase. We already have phase data in SharedSpectralAnalysis. CSD catches pitched onsets at constant energy that magnitude flux misses. **For CBSS rhythm ODF only** — phase is too noisy via microphone for visual transient detection.

**Test plan:**
1. Implement CSD in AudioController (or SharedSpectralAnalysis)
2. Test as standalone ODF for CBSS (replace spectral flux)
3. Test as weighted blend: `ODF = α*flux + (1-α)*CSD`
4. 9-track beat F1 sweep for each variant

**Effort:** Medium (~80 lines + 512 bytes). **Impact:** Uncertain — phase noisy via mic in room.

#### 2.9. Tempo Transition Constraints (Apply at Beat Boundaries Only)

**Gap:** madmom's DBN allows tempo changes ONLY at beat boundaries (lambda=100). BTrack runs `calculateTempo()` only at beat time. Our system applies tempo changes every 250ms regardless of beat phase.

**This is a generalization of 2.1** — not just deferring the beat period update, but constraining the entire Bayesian posterior to only transition at beat boundaries. Between beats, the posterior should be frozen (carry forward the previous posterior as-is without prediction spreading).

**Test plan:** After implementing 2.1, evaluate whether further constraining posterior updates to beat boundaries helps. If 2.1 already addresses the phase discontinuity issue, this may be unnecessary overhead.

**Effort:** Low (conditional on 2.1). **Impact:** Medium.

---

### Phase 3: Architecture — Major Changes (Future)

These require significant design work and should only be attempted after Phase 1+2 gains are realized and calibrated.

#### 3.1. Joint Tempo-Phase HMM (Bar Pointer Model)

**The biggest architectural gap.** madmom's DBN jointly tracks `(position_within_beat, beat_period)`. Position advances deterministically by 1 each frame. Tempo can only change at beat boundaries. Phase and tempo are structurally coupled. This is fundamentally different from our decoupled approach (Bayesian tempo every 250ms + CBSS phase independently).

**Feasibility on nRF52840:**
- 40 tempo bins × 15 position steps = 600 states
- Forward algorithm: 600 × 600 = 360K multiplies per frame (but transition matrix is sparse — only ~10 states reachable per step → ~6K multiplies)
- At 60 Hz: 6K × 60 = 360K ops/second = ~0.5% CPU at 64 MHz
- Memory: 600 × 4 bytes (forward vector) + sparse transition matrix (~2 KB) = ~5 KB

**What it provides:**
- Frame-by-frame beat probability (not just countdown)
- Structural enforcement of even beat spacing
- Implicit tempo smoothing through transition probabilities
- Beat and tempo locked together — no phase discontinuities

**Would replace:** Bayesian tempo fusion + CBSS + predict-and-countdown. Essentially the entire rhythm tracking backend.

**Risk:** High — complete rewrite of core beat tracking. Must prove improvement before committing.

**Approach:** Prototype in Python first (offline, against ground truth). Compare to current system on 18-track test set. Only port to firmware if clearly better.

#### 3.2. Log-Spaced Sub-Band Filterbank (Previously Priority 7f)

Replace 3-band grouping (bass/mid/high) with 12-24 log-spaced bands for finer frequency discrimination. SuperFlux uses 24 bands/octave. With 128 FFT bins at 62.5 Hz/bin, 12-16 log-spaced bands is realistic. Separates kick from bass guitar, snare crack from vocal energy.

- **Effort:** Medium (~80 lines)
- **Risk:** Medium — requires per-band threshold calibration, may interact with band weights

**Deferred:** Phase 2.7 (hi-res bass Goertzel, already implemented) addresses the most critical bass resolution gap. Sub-band filterbank is a further refinement if bass resolution alone is insufficient.

#### 3.3. Particle Filter Beat Tracking (Previously Priority 6c)

100-200 particles tracking `(beat_period, beat_position)`. Naturally handles multi-modal tempo distributions. Octave investigator injects particles at 2x/0.5x median tempo.

- **CPU:** ~1% (100 particles × weight update per frame + periodic resampling)
- **Memory:** ~2 KB
- **Complexity:** ~100-150 lines

**Deferred:** Evaluate after Phase 3.1 (joint HMM) prototyping. If the HMM achieves BTrack-level performance, a particle filter adds diminishing returns. If the HMM is too rigid, particles offer more flexibility.

#### 3.4. Knowledge-Distilled TinyML Onset Detector (Previously Priority 7e)

The only approach that can learn complex spectral patterns (kicks vs bass vs pads vs room modes). Performance gap between DSP and neural onset detection is ~10-15 F1 points.

- **Memory:** ~5-15 KB (model + activations)
- **CPU:** ~200-500us/frame (with CMSIS-NN on Cortex-M4F)
- **Risk:** High — requires training data, model development, TFLM integration

**Deferred:** Long-term research project. Phase 2 improvements may close enough of the gap.

---

### Features Identified for Removal/Simplification

| Feature | Status | Action | Rationale |
|---------|--------|--------|-----------|
| **FT observation** | Enabled (bayesft=2.0) | **Test disabling (Phase 1a)** | Suspected algorithmic issues; v24 improvement needs validation; no reference system uses FT for real-time beat tracking |
| **IOI observation** | Enabled (bayesioi=2.0) | **Test disabling (Phase 1a)** | Suspected algorithmic issues; O(n²) complexity; v24 improvement needs validation; no reference system uses IOI for polyphonic beat tracking |
| **Adaptive band weighting** | Enabled | **Test disabling (Phase 1c)** | ~1600 lines + 2.9 KB RAM for potentially minimal benefit |
| **computeSpectralFluxBands()** | Active | **Replace with unified ODF (Phase 2.4)** | Duplicate computation; disagrees with BandFlux transient detector |
| **6 disabled detectors** | Code present, disabled | **Keep as-is** | Zero runtime cost; useful for future experimentation |
| **Phase correction** | Disabled (phasecorr=0) | **Keep disabled** | Documented failure on syncopated tracks |
| **Static Bayesian prior** | Disabled (bayespriorw=0) | **Keep disabled** | Hurts tracks far from 128 BPM center |
| **Ensemble fusion complexity** | Active | **Simplify for solo detector (Phase 1b)** | Agreement scaling, weighted averaging unnecessary with 1 detector |
| **ODF smooth width=5** | Active | **Re-evaluate after Phase 2.4** | 83ms latency; may be unnecessary once ODF is unified |
| **Disabled BandFlux gates** | Code present, disabled | **Keep as-is** | Zero runtime cost; available for future testing |
| **Per-band thresholds** | Disabled | **Keep disabled** | Tested, -0.067 avg F1 regression |
| **diffframes > 1** | Set to 1 | **Keep at 1** | Tested, -0.098 avg F1 regression |

---

## Next Actions (Priority Order)

### Active — Phase 1: Simplify
1. **Test disable FT+IOI** (Phase 1a) — 9-track sweep with bayesft=0, bayesioi=0 vs current
2. **Test disable adaptive band weighting** (Phase 1c) — 9-track sweep with adaptiveBandWeightEnabled=false
3. **Simplify ensemble fusion** (Phase 1b) — bypass multi-detector logic for solo mode

### Next — Phase 2: Improve (after Phase 1 validated)
4. **Beat-boundary tempo updates** (Phase 2.1) — defer beatPeriodSamples_ changes to beat fire
5. **Increase tempo bins** (Phase 2.2) — 20 → 40 bins, +~5 KB RAM
6. **Adaptive ODF threshold** (Phase 2.3) — local-mean subtraction on OSS buffer before ACF
7. **Unify ODF** (Phase 2.4) — replace computeSpectralFluxBands with BandFlux pre-threshold value
8. **Simplify Bayesian fusion** (Phase 2.5) — reduce to Comb+ACF or switch to log-domain
9. **Dual-threshold peak picking** (Phase 2.6) — add local-max confirmation to BandFlux with 1-frame look-ahead
10. **Enable hi-res bass** (Phase 2.7) — test `hiResBassEnabled=true` (already implemented)
11. **Complex spectral difference** (Phase 2.8) — phase-based ODF for CBSS rhythm tracking only

### Future — Phase 3: Architecture
12. **Joint tempo-phase HMM** (Phase 3.1) — prototype in Python, port if clearly better
13. **Log-spaced sub-band filterbank** (Phase 3.2) — 12-24 bands, evaluate after Phase 2.7 hi-res bass
14. **Particle filter** (Phase 3.3) — evaluate after Phase 3.1 HMM prototyping
15. **TinyML onset detector** (Phase 3.4) — long-term research

### Completed
- ~~**Microphone sensitivity**~~ — ✅ Spectral compressor + per-bin adaptive whitening (v23+)
- ~~**Post-spectral Bayesian re-calibration**~~ — ✅ FT/IOI re-enabled at 2.0, cbssthresh=1.0 confirmed (v24)
- ~~**ACF inverse-lag normalization**~~ — ✅ `acf[i] /= lag` (BTrack-style, confirmed)
- ~~**Combined defaults validation**~~ — ✅ bayesacf=0.3 prevents half-time lock (4-device validated, v22)
- ~~**Bayesian weight sweep**~~ — ✅ All 6 params swept (Feb 25)
- ~~**Multi-device testing infrastructure**~~ — ✅ 4-device parallel capture (Feb 25)
- ~~**Feature testing sweep**~~ — ✅ ODF mean sub, diffframes, per-band thresholds tested (Feb 24)
- ~~**CBSS Adaptive Threshold**~~ — ✅ Implemented (v20), tuned to 1.0 (v22)
- ~~**Per-sample ACF Harmonic Disambiguation**~~ — ✅ Fixed minimal-01 sub-harmonic
- ~~**Bayesian Tempo Fusion**~~ — ✅ Implemented (v18-21), replaced sequential override chain
- ~~**Onset density tracking**~~ — ✅ Done
- ~~**Diverse test music library**~~ — ✅ 18 tracks

---

## Bayesian Tempo Fusion — IMPLEMENTED + TUNED (Feb 23-25, 2026)

**Status:** Implemented in SETTINGS_VERSION 18-25. Sequential override chain replaced with Bayesian posterior estimation. v25: harmonic comb ACF (0.8) + Rayleigh prior, tighter lambda (0.07), bidirectional disambiguation. FT/IOI re-enabled at 2.0 after spectral processing (v24), cbssthresh=1.0.

**Architecture summary (v24 — full fusion with spectral processing):**
```
Every 250ms:
  1. Compute autocorrelation of OSS buffer (with inverse-lag normalization, for periodicityStrength + harmonic disambig)
  2. FOR EACH of 20 tempo bins (60-180 BPM from CombFilterBank):
       - ACF observation: weight=0.8 (harmonic-enhanced: 4-harmonic comb + Rayleigh prior, v25)
       - FT observation: weight=2.0 (re-enabled — spectral compressor fixes normalization)
       - Comb observation: comb filter energy at bin, raised to 0.7
       - IOI observation: weight=2.0 (re-enabled — spectral whitening stabilizes onset detection)
  3. Viterbi transition: spread prior through Gaussian (sigma = 0.07 * BPM)
  4. Posterior = prediction × ACF^0.8 × FT^2.0 × comb^0.7 × IOI^2.0
  5. MAP estimate with quadratic interpolation → BPM
  6. Per-sample ACF harmonic disambiguation: check raw ACF at lag/2 (>50%), lag*2/3 (>60%), and lag*2 (0.5x downward, v25)
  7. CBSS adaptive threshold: beat fires only if CBSS > 1.0 * running mean
  8. EMA smoothing (tempoSmoothingFactor) → CBSS beat period update
```

**Key findings:**
1. Bayesian fusion alone cannot prevent sub-harmonic locking — per-sample ACF harmonic disambiguation (step 6) required.
2. Spectral processing (compressor + whitening, v23) fixed normalization issues that made FT/IOI unreliable. Re-enabling both at weight 2.0 yielded +49% avg Beat F1 vs control (v24).
3. Comb filter bank (Scheirer 1998 resonators) remains the primary tempo observation — continuous resonance, phase-sensitive, self-normalizing. Closest to BTrack's primary method.
4. Higher CBSS threshold (1.0 vs 0.4) is the single biggest improvement — fewer phantom beats = more stable BPM = better recall.
5. ACF inverse-lag normalization (BTrack-style `acf[i] /= lag`) reduces sub-harmonic bias. v25 added 4-harmonic comb summation + Rayleigh prior, raising ACF weight to 0.8.

**Resources:** 257KB flash (31%), ~17KB RAM (7%).

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
| [Beat This!](https://github.com/CPJKU/beat_this) | Foscarin, Schlüter, Widmer | 2024 | 20M param transformer (SOTA) | No |
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
