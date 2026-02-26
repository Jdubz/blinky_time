# Blinky Time - Improvement Plan

*Last Updated: February 25, 2026*

## Current Status

### Completed (February 25, 2026)

**Bayesian Weight Tuning (SETTINGS_VERSION 21):**
- Multi-device parallel sweep of all 6 Bayesian parameters using beat event scoring
- Root cause analysis: ACF/FT/IOI observations have fundamental implementation issues (compared to BTrack, madmom, librosa)
- Applied new defaults: ACF/FT/IOI=0 (disabled), comb=0.7, lambda=0.15, cbssthresh=1.0
- Best F1 0.590 (up from 0.421 at v20 defaults, 0.209 at cbssthresh=0.4)

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
- `bandflux_dominance` — band-dominance gate (0.0=disabled)
- `bandflux_decayratio` + `bandflux_decayframes` — post-onset decay gate (0.0=disabled)
- `bandflux_crestgate` — spectral crest factor gate (0.0=disabled)

### Priority 2: CBSS Beat Tracking + Bayesian Tempo Fusion — Validated (SETTINGS_VERSION 22)

BTrack-style predict+countdown CBSS beat detection with Bayesian tempo fusion. Tempo estimated via unified posterior over 20 bins (60-180 BPM). Comb filter bank is primary observation; ACF contributes at low weight (0.3) to prevent sub-harmonic lock. FT and IOI disabled (weight=0). CBSS adaptive threshold (1.0) prevents phantom beats.

**Pre-Bayesian baseline (sequential override chain, Feb 21):** avg Beat F1 **0.472** on 9 tracks.
**Bayesian v20 (all observations on, cbssthresh=0.4, Feb 24):** avg Beat F1 **0.421**.
**Bayesian v21 (comb-only, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.590** (best independent cbssthresh sweep).
**Combined validation (comb+ACF 0.3, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.519** (4-device validated).

**Why v21 independent sweep was misleading:** Each parameter was swept independently against device-saved defaults (which had bayesacf=1). The cbssthresh=1.0 result (F1 0.590) was achieved WITH ACF at weight 1.0, not 0. When all v21 changes were applied together (ACF=0), half-time lock occurred on most tracks (avg F1 0.410). A 4-device bayesacf sweep with v21 base params found 0.3 optimal.

**Bayesian tunable parameters (9 total, SETTINGS_VERSION 22 defaults):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Lambda | `bayeslambda` | **0.15** | Transition tightness (how fast tempo can change) |
| Prior center | `bayesprior` | 128 | Static prior Gaussian center BPM |
| Prior width | `priorwidth` | 50 | Static prior Gaussian sigma |
| Prior weight | `bayespriorw` | 0.0 | Ongoing static prior strength (off by default) |
| ACF weight | `bayesacf` | **0.3** | Autocorrelation observation (low weight prevents sub-harmonic lock) |
| FT weight | `bayesft` | **0** | Fourier tempogram observation (disabled — mean-norm kills discriminability) |
| Comb weight | `bayescomb` | 0.7 | Comb filter bank observation weight |
| IOI weight | `bayesioi` | **0** | IOI histogram observation (disabled — unnormalized counts dominate posterior) |
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

#### Why FT and IOI Are Disabled (Reference Implementation Comparison, Feb 25)

Comprehensive comparison with BTrack, madmom, and librosa identified fundamental implementation problems:

**ACF (bayesacf=0.3):** Raw autocorrelation has sub-harmonic bias without inverse-lag normalization. At full weight (1.0) it overwhelms comb filters. At low weight (0.3) it provides just enough periodicity signal to prevent sub-harmonic lock without dominating. Combined validation confirmed 0.3 as optimal across 9 tracks on 4 devices.

**FT (bayesft=0):** Goertzel magnitude-squared normalized by mean across bins. Mean normalization produces near-flat observation vectors (~1.0 everywhere), destroying discriminability. Uses mag² (amplifies noise). No reference implementation (BTrack, madmom, librosa) uses Fourier tempogram for real-time beat tracking.

**IOI (bayesioi=0):** Pairwise onset interval matching with raw integer counts (1-10+ range) that dominate the multiplicative posterior. 2x folding (skipped-beat matching) systematically biases toward fast tempos. Fixed ±2 sample tolerance creates tempo-dependent precision. No reference implementation uses IOI histograms for polyphonic beat tracking.

**Architectural mismatch:** Our multiplicative fusion (`posterior = prediction × ACF × FT × comb × IOI`) assumes independent, comparably-scaled signals. They are neither — they share the same input (OSS buffer) and have different scales and biases. BTrack uses a sequential pipeline (ACF → comb filter → Rayleigh weighting → Viterbi), not multiplicative fusion. madmom uses a joint DBN with ~5500 states. librosa uses windowed ACF with log-normal prior.

**Potential future fixes (if re-enabling):**
- ACF: Add inverse-lag normalization + L-inf norm (max=1.0), or process through harmonic comb filter summation (BTrack-style)
- FT: Use magnitude (not squared), max-normalize instead of mean, apply log compression
- IOI: Normalize by max count, remove 2x folding, use proportional tolerance
- Architecture: Consider pipeline approach (sequential transforms) instead of multiplicative fusion

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
- Raw ACF observation in Bayesian: Sub-harmonic bias without inverse-lag normalization (Feb 25)
- Fourier tempogram observation: Mean normalization destroys discriminability (Feb 25)
- IOI histogram observation: Unnormalized counts dominate multiplicative posterior (Feb 25)

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

## Calibration Status (Feb 25, 2026)

**Bayesian fusion calibrated and validated via 4-device sweep.** Comb filter bank is the primary tempo observation. ACF contributes at low weight (0.3) to prevent sub-harmonic lock. FT and IOI disabled. CBSS adaptive threshold at 1.0 prevents phantom beats. Combined defaults validated across 9 tracks on 4 devices: avg Beat F1 **0.519** (+10% vs pre-Bayesian baseline).

**Multi-device sweep capability:** 4 devices sweep parameters in parallel (4x speedup). Example: `param-tuner multi-sweep --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM3 --params bayesacf --duration 30`. Uses real music files with ground truth annotations, ffplay for headless audio playback.

| Feature | Parameters | Status |
|---------|:----------:|--------|
| **Bayesian weights** | bayesacf=0.3, bayesft=0, bayescomb=0.7, bayesioi=0, bayeslambda=0.15, bayespriorw=0 | **Validated** (SETTINGS_VERSION 22) — comb+low ACF, FT/IOI disabled |
| **CBSS adaptive threshold** | cbssthresh=1.0 | **Validated** (SETTINGS_VERSION 22) — prevents phantom beats |
| ODF Mean Subtraction | odfmeansub (toggle) | **Essential** — keep ON. OFF destroys BPM (Feb 24) |
| Per-band thresholds | bandflux_perbandthresh, perbandmult | **Tested, keep OFF** — hurts weak tracks (Feb 24) |
| Multi-frame diffframes | bandflux_diffframes | **Tested, keep at 1** — diffframes=2 too many transients (Feb 24) |
| BandFlux core params | gamma, bassWeight, threshold, onsetDelta | **Calibrated** (Feb 21) |

## Next Actions

### Active
1. **Microphone sensitivity investigation** (Priority 3) — Transient F1 scores on real music are low (0.19-0.21 on techno-minimal-01 via multi-device test, Feb 25). Software pre-amplification (2x-4x gain on raw samples) is the simplest experiment.
2. **FFT-512 bass-focused analysis** (Priority 7c) — Better kick detection to feed better data upstream. ~200 lines, ~5KB RAM.
3. **Particle filter beat tracking** (Priority 6c) — Fundamentally different approach that handles multi-modal tempo distributions. ~100-150 lines, ~2KB RAM.
4. **ACF inverse-lag normalization** — Add `acf[i] /= lag` and L-inf normalization, then re-sweep bayesacf. May allow higher ACF weight for better sub-harmonic disambiguation.

### Completed
- ~~**Combined defaults validation**~~ — ✅ 4-device sweep of bayesacf (0, 0.3, 0.7, 1.0) with v21 base params (Feb 25). Found bayesacf=0 causes half-time lock; 0.3 optimal (F1 0.519). Independent sweep results were misleading — interaction between bayesacf=0 and cbssthresh=1.0 caused regression. Updated to SETTINGS_VERSION 22.
- ~~**Bayesian weight sweep + firmware defaults**~~ — ✅ Swept all 6 Bayesian params via multi-device parallel sweep (Feb 25). Root cause analysis via BTrack/madmom/librosa comparison identified sub-harmonic bias (ACF), mean-norm (FT), and unnormalized counts (IOI).
- ~~**Multi-device testing infrastructure**~~ — ✅ 4-device parallel capture with ffplay audio, variation + sweep commands (Feb 25). Inter-device F1 spread 0.014.
- ~~**Feature testing sweep**~~ — ✅ ODF mean sub OFF, diffframes=2, per-band thresholds ON all tested (Feb 24). None improve overall avg. Current defaults are optimal.
- ~~**CBSS Adaptive Threshold**~~ — ✅ Implemented (SETTINGS_VERSION 20). Prevents phantom beats during silence.
- ~~**Per-sample ACF Harmonic Disambiguation**~~ — ✅ Fixed minimal-01 sub-harmonic (BPM 70→124).
- ~~**Bayesian Tempo Fusion**~~ — ✅ Implemented (SETTINGS_VERSION 18-21). Replaced sequential override chain.
- ~~**Design unified feature cooperation framework**~~ — ✅ Done (Bayesian fusion is the framework).
- ~~**Onset density tracking**~~ — ✅ Done.
- ~~**Diverse test music library**~~ — ✅ 18 tracks (9 original + 9 syncopated).
- ~~**HPS, Pulse train, IOI, FT, ODF mean sub**~~ — ✅ All implemented; IOI/FT/comb now feed Bayesian fusion. HPS/pulse train removed.

---

## Bayesian Tempo Fusion — IMPLEMENTED + TUNED (Feb 23-25, 2026)

**Status:** Implemented in SETTINGS_VERSION 18-21. Sequential override chain replaced with Bayesian posterior estimation. Weights tuned via multi-device sweep (Feb 25): ACF/FT/IOI disabled, comb-only, cbssthresh=1.0.

**Architecture summary (v21 — comb-only):**
```
Every 250ms:
  1. Compute autocorrelation of OSS buffer (for periodicityStrength + harmonic disambig)
  2. FOR EACH of 20 tempo bins (60-180 BPM from CombFilterBank):
       - ACF observation: weight=0, returns 1.0 (disabled — sub-harmonic bias)
       - FT observation: weight=0, returns 1.0 (disabled — mean-norm kills discriminability)
       - Comb observation: comb filter energy at bin, raised to 0.7
       - IOI observation: weight=0, returns 1.0 (disabled — unnormalized counts dominate)
  3. Viterbi transition: spread prior through Gaussian (sigma = 0.15 * BPM)
  4. Posterior = prediction × combObs  (effectively comb-only inference)
  5. MAP estimate with quadratic interpolation → BPM
  6. Per-sample ACF harmonic disambiguation: check raw ACF at lag/2 (>50%) and lag*2/3 (>60%)
  7. CBSS adaptive threshold: beat fires only if CBSS > 1.0 * running mean
  8. EMA smoothing (tempoSmoothingFactor) → CBSS beat period update
```

**Key findings:**
1. Bayesian fusion alone cannot prevent sub-harmonic locking — per-sample ACF harmonic disambiguation (step 6) required.
2. Three of four observations actively hurt beat tracking (Feb 25 sweep). Root cause: multiplicative fusion assumes independent, comparably-scaled signals; ACF/FT/IOI violate both assumptions.
3. Comb filter bank (Scheirer 1998 resonators) provides the best tempo observation — continuous resonance, phase-sensitive, self-normalizing. Closest to BTrack's primary method.
4. Higher CBSS threshold (1.0 vs 0.4) is the single biggest improvement — fewer phantom beats = more stable BPM = better recall.

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
