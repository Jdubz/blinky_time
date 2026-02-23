# Blinky Time - Improvement Plan

*Last Updated: February 22, 2026 (Comprehensive research survey + IOI histogram implementation)*

## Current Status

### Completed (February 21, 2026)

**Detector & Beat Tracking Optimization:**
- BandWeightedFluxDetector set as sole default detector (all others disabled)
- BandFlux parameters confirmed near-optimal via sweep: gamma=20, bassWeight=2.0, threshold=0.5
- beatoffset recalibrated 9→5, doubling avg Beat F1 (0.216→0.452 across 9 tracks)
- Runtime params persisted to flash: tempoSmoothFactor, odfSmoothWidth, harmup2x, harmup32, peakMinCorrelation (SETTINGS_VERSION 12)
- Sub-harmonic investigation: machine-drum lock is fundamental autocorrelation limitation, not fixable by tuning
- Confidence gating analysis: existing activationThreshold sufficient; cbssConfidence doesn't discriminate

**Onset Delta Filter (minOnsetDelta=0.3):**
- Rejects slow-rising signals (pad swells, stab echoes) by requiring minimum frame-to-frame flux jump
- Fixed synth-stabs regression: F1 0.600→1.000 (16 FPs eliminated)
- Improved pad-rejection: F1 0.314→0.421 (35 FPs→16 FPs, 54% reduction)
- Improved real music avg Beat F1: 0.452→0.472 (best: trance-party +0.098, minimal-emotion +0.104)
- Per-track offset investigation: -58ms to +57ms variation, adaptive offset deferred (oscillation risk)

### Completed (February 2026)

**Rhythm Tracking:**
- CBSS beat tracking with counter-based beat detection (replaced multi-hypothesis v3)
- Deterministic phase derivation from beat counter
- Tempo prior with Gaussian weighting (120 BPM center, 50 width, 0.5 blend)
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

### Priority 2: CBSS Beat Tracking — Stable

BTrack-style predict+countdown CBSS beat detection is working. Beat F1 avg **0.472** on original 9 tracks, **0.221** on 9 new syncopated tracks.

**Current performance — Original tracks (4-on-the-floor, beatoffset=5, onsetDelta=0.3):**

| Track | Beat F1 | BPM Acc | Detected BPM | Visual Behavior |
|-------|:-------:|:-------:|:------------:|-----------------|
| trance-party | 0.775 | 0.993 | ~138 | Beat-sync, sparks on kicks |
| minimal-01 | 0.695 | 0.959 | ~125 | Beat-sync, clean pulsing |
| infected-vibes | 0.691 | 0.973 | ~141 | Beat-sync, strong phase lock |
| goa-mantra | 0.605 | 0.993 | ~138 | Beat-sync, occasional drift |
| minimal-emotion | 0.486 | 0.998 | ~125 | Partial music mode |
| deep-ambience | 0.404 | 0.949 | ~118 | Energy-reactive (correct) |
| machine-drum | 0.224 | 0.825 | ~118 | Half-time lock (see Priority 6) |
| trap-electro | 0.190 | 0.924 | ~130 | Energy-reactive (correct) |
| dub-groove | 0.176 | 0.830 | ~121 | Energy-reactive (correct) |
| **Average** | **0.472** | **0.938** | | |

**Current performance — New syncopated tracks (Feb 22, 2026):**

| Track | Beat F1 | BPM Acc | Detected BPM | Expected BPM | Issue |
|-------|:-------:|:-------:|:------------:|:------------:|-------|
| garage-uk-2step | 0.443 | 0.993 | 128.3 | 129.2 | BPM correct, good F1 |
| dubstep-edm-halftime | 0.273 | 0.942 | 124.3 | 117.5 | Slightly high BPM |
| amapiano-vibez | 0.257 | 0.861 | 127.9 | 112.3 | BPM pulled to prior center |
| breakbeat-drive | 0.216 | 0.654 | 128.8 | 95.7 | **Harmonic lock** (4/3x) |
| breakbeat-background | 0.186 | 0.503 | 128.9 | 86.1 | **Harmonic lock** (3/2x) |
| reggaeton-fuego-lento | 0.186 | 0.626 | 126.8 | 92.3 | **Harmonic lock** (4/3x) |
| dnb-energetic-breakbeat | 0.173 | 0.959 | 122.3 | 117.5 | Librosa half-time too |
| dnb-liquid-jungle | 0.161 | 0.902 | 123.3 | 112.3 | Librosa half-time too |
| afrobeat-feelgood-groove | 0.087 | 0.950 | 123.3 | 117.5 | Phase misalignment |
| **Average** | **0.220** | **0.821** | | | |

**Key finding:** Almost every syncopated track gets pulled to ~123-129 BPM regardless of true tempo. The tempo prior (center=120) dominates when autocorrelation can't find a clean peak. Breakbeat and reggaeton are most affected — true tempos (86-96 BPM) are far from prior center and get overridden by harmonic peaks near 128.

**Known limitations (visually acceptable, potential improvements identified):**
- machine-drum non-harmonic BPM lock (143.6→118 BPM, ratio 6/5) — autocorrelation picks spurious peak. See Priority 6 for correction approaches
- breakbeat/reggaeton harmonic lock — prior favors ~128 BPM harmonic over true ~86-96 BPM fundamental. HPS (Priority 6a) targets this
- DnB half-time detection — both librosa and firmware detect ~117 BPM instead of ~170 BPM. Acceptable for visual purposes
- trap/syncopated low F1 — energy-reactive mode is the correct visual response
- deep-ambience low F1 — organic mode fallback is correct for ambient content
- Per-track offset variation (-58 to +57ms) — invisible at LED update rates

**What NOT to do (tested and rejected):**
- Phase correction (phasecorr): Destroys BPM on syncopated tracks
- ODF width > 5: Variable delay destroys beatoffset calibration
- Ensemble transient input to CBSS: Only works for 4-on-the-floor
- Disabling tempo prior to fix machine-drum: Doesn't fix it, destabilizes other tracks
- Shifting tempo prior center to 128+: Marginal effect on machine-drum, hurts trance
- Comb bank cross-validation (Feb 22): Both autocorrelation AND comb bank lock to the same wrong tempo (~118 BPM) because the audio genuinely has stronger periodicity there. Low correlation threshold (0.15) overcorrects on good tracks (infected-vibes BPM destroyed). Infrastructure remains in code (SETTINGS_VERSION 13, `combxvalconf`/`combxvalcorr` settings, `cbpm`/`cconf` debug streaming) with safe defaults (0.3/0.5)

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

### Priority 6: Non-Harmonic BPM Correction (Research Complete, Feb 22)

**Problem:** machine-drum (143.6 BPM) locks to ~118 BPM. The ratio 143.6/118 = 1.22 (close to 6/5) is a polyrhythmic relationship — not a clean harmonic. Both autocorrelation and the independent comb filter bank agree on the wrong tempo because the audio genuinely has stronger periodicity at ~118 BPM. Existing harmonic disambiguation (2:1, 3:2, 2:3) and comb bank cross-validation cannot fix this.

**Root cause:** Autocorrelation measures self-similarity at a lag, but does NOT measure whether onsets actually occur at regular intervals of that lag. Every 5th beat at 143.6 BPM coincides with every 6th beat at ~118 BPM, creating spurious autocorrelation energy at the lower frequency.

**Candidate approaches (ranked by feasibility and impact):**

#### 6a. Harmonic Product Spectrum — REJECTED (Feb 22, 2026)

**Both ratio-based and additive (Percival-style) HPS tested and rejected:**
- **Ratio-based** (`hpsFactor = 0.5 + ACF[2i]/ACF[i]`): Penalizes correct peaks whose sub-harmonics are naturally weaker. Destroyed trance-party (F1 0.775→0.383) and goa-mantra (F1 0.605→0.121).
- **Additive** (`ACF[i] += ACF[2i]`): Boosts wrong sub-harmonic for goa-mantra (BPM 137→124, F1 0.571→0.070). Machine-drum unchanged with either approach.
- **Root cause:** The problem is NOT that the correct peak lacks sub-harmonic support — it's that autocorrelation genuinely measures stronger periodicity at the wrong tempo. HPS operates on autocorrelation output, which is the wrong place to intervene.
- **Code kept but disabled** (`hps` bool, default false, persisted to flash)

#### 6b. Pulse Train Cross-Correlation — REJECTED

Implemented Percival & Tzanetakis 2014 algorithm: sparse pulse train template (fundamental=1.0, double=0.5, 3/2=0.5), 4 beat cycles, magScore + varScore rank-level fusion. Also tried three-signal fusion (adding autocorrelation+prior score as third term).

**Results:** Both two-signal and three-signal fusion cause regressions on trance/goa tracks:
- trance-party: BPM 135.7→130.4, F1 0.650→0.500 (three-signal) or BPM→113.9 (two-signal)
- goa-mantra: BPM 137→122.5, F1 0.571→0.155
- machine-drum: BPM improved to 86 (still wrong, was 118 baseline)

**Root cause:** In 4-on-the-floor trance with room acoustics, sub-harmonics produce similar onset alignment scores because every other beat still aligns. The onset-based signals (mag+var) outvote autocorrelation even with three-signal fusion. The approach fundamentally cannot distinguish octave-related tempos reliably.

- **Code kept but disabled** (`pulsetrain` bool, default false, persisted to flash)

#### 6c. IOI Histogram Analysis — IMPLEMENTED, LIMITED EFFECT (Feb 22, 2026)

Implemented: 48-entry onset timestamp ring buffer, pairwise interval histogram with skipped-beat folding (2x→1x), 3-bin smoothing, upward-only guard. Cross-validates autocorrelation — if IOI peak suggests faster tempo AND autocorrelation has evidence at that lag, switch.

**Results (A/B testing on device):**
- machine-drum: IOI makes BPM **worse** (118→96) because BandFlux transients don't fire at 143.6 BPM intervals — measured IOIs are 600-850ms (~70-100 BPM), not the expected 418ms
- infected-vibes: No meaningful change (F1 0.850 with or without IOI)
- trance-party: Neutral (F1 ~0.44 both ways)

**Root cause:** IOI repackages transient timing data. If BandFlux only detects ~60% of kicks with highly variable intervals, the IOI histogram reflects that same bad data. The approach is fundamentally limited by onset detection quality.

**Upward-only guard added** to prevent regressions (only allows IOI to push BPM higher, never lower). This makes IOI inert on machine-drum but prevents the 118→96 regression.

- **Code live in firmware** (SETTINGS_VERSION 16, `ioi`/`ioipeakratio`/`ioicorr` settings, `ioi`/`ic` debug streaming)
- **Verdict:** Safe to leave enabled (upward-only guard), but does not solve the core problem

#### 6d. Fourier Tempogram Cross-Validation — HIGH PRIORITY (Research, Feb 22)

**This is the single most promising untried approach for sub-harmonic locking.**

Autocorrelation of a periodic signal at period T inherently produces peaks at T, 2T, 3T... (sub-harmonics). The Fourier tempogram (DFT of the ODF buffer) has the **opposite** property — it shows the fundamental and harmonics (T, T/2, T/3) but **suppresses sub-harmonics**. A 143 BPM signal would show peaks at 143, 286 BPM but NEVER at 72 BPM in the Fourier domain.

**Implementation:** Take existing 360-sample OSS buffer → Hanning window → zero-pad to 512 → FFT → magnitude spectrum → convert bins to BPM (`bpm = bin * frameRate * 60 / fftSize`) → find peak in 60-200 BPM range → cross-validate with autocorrelation result.

- **CPU:** Implemented as per-lag Goertzel (not FFT). O(~190 lags × 360 samples) = ~68K multiply-adds every 500ms. Amortized cost is well within headroom.
- **Memory:** ~800 bytes stack (200 histogram bins for IOI, reusable)
- **Complexity:** ~40-60 lines C++
- **Risk:** Low — cross-validation pattern (same as comb bank, IOI). If Fourier peak agrees with autocorrelation, no change. If it disagrees and autocorrelation has evidence at the Fourier lag, switch.
- **References:** Grosche & Muller 2011 ("What Makes Beat Tracking Difficult?"), AudioLabs Erlangen Fourier Tempogram tutorial

#### 6e. ODF Mean Subtraction — HIGH PRIORITY (Research, Feb 22)

**Gap vs BTrack reference implementation.** Our autocorrelation computes raw cross-products on the OSS buffer without detrending. BTrack subtracts the local mean first:

```
// BTrack: correlation += (odf[i] - mean) * (odf[i+lag] - mean)
// Ours:   correlation += odf[i] * odf[i+lag]   (no mean subtraction)
```

Without mean subtraction, the autocorrelation has a DC component that creates baseline correlation at ALL lags — every lag looks somewhat correlated. This DC floor can bury the true tempo peak and make sub-harmonic peaks appear competitive.

- **CPU:** Near-zero (compute mean once, subtract in inner loop — 2 extra multiplications per iteration)
- **Memory:** 1 float
- **Complexity:** ~10 lines
- **Risk:** Very low — mathematically more correct than current approach
- **References:** BTrack source code (Adam Stark), standard autocorrelation normalization

#### 6f. Lightweight Particle Filter with Octave Investigator — MEDIUM PRIORITY (Research, Feb 22)

Inspired by BeatNet (Heydari et al., ISMIR 2021). Run 100-200 particles, each tracking (beat_period, beat_position). Per frame: advance position, weight by ODF match at predicted beat times. Every ~1 second: resample (kill low-weight, copy high-weight). **Key innovation:** At resampling, inject 10-20 particles at 2x and 0.5x the current median tempo. This explicitly explores tempo octave alternatives and self-corrects.

- **CPU:** ~1% (100 particles × weight update per frame + periodic resampling)
- **Memory:** ~2KB (200 particles × 12 bytes each)
- **Complexity:** ~100-150 lines C++
- **Risk:** Medium — resampling and observation model need tuning
- **References:** BeatNet (Heydari 2021, ISMIR), Cemgil 2004 (Particle Filtering for Tempo), Hainsworth 2004

#### 6g. Multi-Agent Beat Tracking (IBT-style) — MEDIUM PRIORITY (Research, Feb 22)

Maintain 5-10 competing tempo/phase agents. Each independently predicts beats and scores against ODF. Best-scoring agent determines output. When music changes character, a different agent can take over. Agents at different metrical levels compete naturally.

- **CPU:** <1% (10 agents × score update per frame)
- **Memory:** ~500 bytes (10 agents × 50 bytes)
- **Complexity:** ~150-250 lines C++
- **Risk:** Medium — agent lifecycle management (spawn/kill/score) needs tuning
- **References:** IBT (Oliveira et al., ISMIR 2010), BeatRoot (Dixon 2007), Klapuri 2006

#### 6h. CBSS Adaptive Threshold — LOW PRIORITY (Research, Feb 22)

**Gap vs BTrack reference implementation.** Our beat detection fires purely on countdown (`timeToNextBeat_ <= 0`). BTrack adds: `AND CBSS[n] > adaptiveThreshold`. Without this, beats fire during silence and breakdowns where there's no rhythmic content.

- **CPU:** ~5 ops/frame (running mean of CBSS)
- **Memory:** 1 float
- **Complexity:** ~15 lines
- **Risk:** Low — improves beat quality but won't fix sub-harmonic locking
- **References:** BTrack source code (Adam Stark), Davies & Plumbley 2007

#### Approaches evaluated and not recommended

| Approach | Why Not | Tested |
|----------|---------|--------|
| HPS ratio-based | Penalizes correct peaks, severe regressions | Feb 22 |
| HPS additive (Percival) | Boosts wrong sub-harmonic, no help for machine-drum | Feb 22 |
| Pulse train 2-signal (Percival) | Onset alignment overrides autocorrelation to sub-harmonics | Feb 22 |
| Pulse train 3-signal (+ autocorr fusion) | Autocorrelation anchor too weak, still regresses trance/goa | Feb 22 |
| Comb bank cross-validation | Both autocorrelation AND comb bank lock to same wrong tempo | Feb 22 |
| IOI histogram (bidirectional) | Repackages bad transient data; made machine-drum worse (118→96 BPM) | Feb 22 |
| Extended ratio checks (5/6, 5/4) | Fragile, threshold tuning causes regressions on other tracks | Feb 22 |
| Style-aware prior shift | Shifting prior center — marginal effect, hurts other tracks | Feb 22 |
| Deep learning (TCN/CRNN/Transformer) | Not feasible on nRF52840 (64 MHz, no matrix acceleration) | Research |
| Full Boeck CNN (3 FFT sizes) | Needs ~60KB RAM for multi-resolution spectrograms | Research |
| Full 192-band SuperFlux filterbank | Impractical at FFT-256 (only 128 input bins) | Research |
| HMM/Bayesian onset detection | Smoothing layer can't create peaks that don't exist | Research |
| Cyclic tempogram | Only handles octave (2:1) ambiguity, not 6/5 | Research |
| Kalman filter beat tracking | Unimodal Gaussian can't represent multi-modal tempo ambiguity | Research |
| ~~PLP / Fourier tempogram~~ | ~~Moderate CPU cost, similar effect to pulse train~~ **WRONG — see 6d** | Corrected |

**Recommended implementation order:**
1. **ODF mean subtraction (6e)** — ~10 lines, removes DC bias from autocorrelation. Lowest risk, addresses root cause.
2. **Fourier tempogram (6d)** — ~50 lines, mathematically suppresses sub-harmonics. Complementary to autocorrelation.
3. **Particle filter (6f)** — if 6d+6e insufficient. Explicit tempo octave exploration.
4. **Multi-agent (6g)** — alternative to particle filter if competing-hypothesis model preferred.

### Priority 7: Onset Detection Improvements (Research, Feb 22)

Comprehensive research survey identified several untried improvements to BandWeightedFlux that could improve kick detection rate (currently ~60% recall on some tracks). Better onset detection feeds better data to all BPM tracking systems.

#### 7a. Per-Band Independent Thresholds — HIGH PRIORITY

Currently one combined flux threshold gates detection. A kick might produce `combinedFlux = 1.2` when threshold is ~1.2, causing a miss. But the bass band alone might show a clear spike that gets diluted by low mid/high flux. Independent bass-band threshold detection would catch these masked kicks.

- **Memory:** ~20 bytes (3 separate running means)
- **CPU:** Negligible
- **Complexity:** ~30 lines
- **References:** Multi-band onset detection (Bello et al. 2005), SuperFlux per-band variant

#### 7b. Multi-Frame Temporal Reference — MEDIUM PRIORITY

Current BandFlux compares to only 1 previous frame (`prevLogMag_`). SuperFlux paper uses comparison to frame N-2 or N-3 (`diff_frames` parameter), which is more robust to gradual bass buildup and better detects kicks during bass sweeps.

- **Memory:** ~256 bytes per extra frame (64 bins × 4 bytes)
- **CPU:** Negligible
- **Complexity:** ~20 lines
- **References:** SuperFlux (Bock & Widmer, DAFx 2013), librosa `superflux` implementation

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

## Next Actions

1. ~~**Add onset density tracking**~~ — ✅ Done. See Priority 5.
2. ~~**Build diverse test music library**~~ — ✅ Done. 9 syncopated tracks added.
3. ~~**Implement HPS tempo correction**~~ — ❌ Rejected. See Priority 6a.
4. ~~**Implement pulse train cross-correlation**~~ — ❌ Rejected. See Priority 6b.
5. ~~**Implement IOI histogram analysis**~~ — ✅ Implemented, limited effect. See Priority 6c.
6. **Implement ODF mean subtraction** (Priority 6e) — ~10 lines, removes autocorrelation DC bias. Highest priority.
7. **Implement Fourier tempogram cross-validation** (Priority 6d) — ~50 lines, sub-harmonic suppression. Highest priority.
8. **Implement per-band independent thresholds** (Priority 7a) — ~30 lines, unmasks bass kicks.
9. **Implement multi-frame temporal reference** (Priority 7b) — ~20 lines, better kick detection during bass sweeps.

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
