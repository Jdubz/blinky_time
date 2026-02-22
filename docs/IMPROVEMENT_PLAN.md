# Blinky Time - Improvement Plan

*Last Updated: February 22, 2026 (pad FP investigation closed)*

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
- `bandflux_bassratio` — band-dominance gate (0.0=disabled)
- `bandflux_decayratio` + `bandflux_decayframes` — post-onset decay gate (0.0=disabled)
- `bandflux_crestgate` — spectral crest factor gate (0.0=disabled)

### Priority 2: CBSS Beat Tracking — Stable

BTrack-style predict+countdown CBSS beat detection is working. Beat F1 avg **0.472** on 9 real music tracks. BPM estimation avg 93.8%.

**Current performance (9 real music tracks, beatoffset=5, onsetDelta=0.3):**

| Track | Beat F1 | BPM Acc | Visual Behavior |
|-------|:-------:|:-------:|-----------------|
| trance-party | 0.775 | 0.993 | Beat-sync, sparks on kicks |
| minimal-01 | 0.695 | 0.959 | Beat-sync, clean pulsing |
| infected-vibes | 0.691 | 0.973 | Beat-sync, strong phase lock |
| goa-mantra | 0.605 | 0.993 | Beat-sync, occasional drift |
| minimal-emotion | 0.486 | 0.998 | Partial music mode |
| deep-ambience | 0.404 | 0.949 | Energy-reactive (correct) |
| machine-drum | 0.224 | 0.825 | Half-time lock (acceptable) |
| trap-electro | 0.190 | 0.924 | Energy-reactive (correct) |
| dub-groove | 0.176 | 0.830 | Energy-reactive (correct) |
| **Average** | **0.472** | **0.938** | |

**Known limitations (not fixable, visually acceptable):**
- machine-drum sub-harmonic BPM lock — fundamental autocorrelation limitation, half-time still looks rhythmic
- trap/syncopated low F1 — energy-reactive mode is the correct visual response
- deep-ambience low F1 — organic mode fallback is correct for ambient content
- Per-track offset variation (-58 to +57ms) — invisible at LED update rates

**What NOT to do (tested and rejected):**
- Phase correction (phasecorr): Destroys BPM on syncopated tracks
- ODF width > 5: Variable delay destroys beatoffset calibration
- Ensemble transient input to CBSS: Only works for 4-on-the-floor
- Disabling tempo prior to fix machine-drum: Doesn't fix it, destabilizes other tracks
- Shifting tempo prior center to 128+: Marginal effect on machine-drum, hurts trance

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
- At 1s (60 samples): can detect ~120-200 BPM
- At 2s (120 samples): full 60-200 BPM range available
- `periodicityStrength_` smoothing (0.7/0.3 EMA) handles early estimate noise

Band autocorrelation (adaptive weighting) also lowered from 120 to 60 minimum samples.

### Priority 4: Music Content Classification (Long-term)

The existing `rhythmStrength` blend works well but could be enhanced with additional content descriptors. Research (Feb 22) identified three cheap features that would improve organic/music mode transitions:

1. **Onset density** — onsets/second EMA. Dance=2-6/s, ambient=0-1/s, complex=4-10/s. Trivial to add.
2. **Spectral centroid variability** — variance of spectral centroid over 2-4s window. High variance=dynamic/percussive, low=sustained/ambient. Already have centroid computation.
3. **Energy crest factor** — peak/mean energy ratio over 2-4s. High=percussive with quiet periods, low=continuous drone.

These would modulate the existing `rhythmStrength` for smoother, more appropriate visual responses without hard mode switching.

### Priority 5: Multi-Agent Beat Tracking (Future)

For syncopated music (trap, dub, machine-drum), maintaining 2-3 competing beat hypotheses at different metrical levels (T, T/2, 2T) could improve tracking. Each agent runs a lightweight CBSS and is scored by onset alignment over a rolling 4-beat window. The winning agent drives output.

Cost: ~1.5KB RAM, +3% CPU. References: IBT (INESC Beat Tracker), BeatRoot, Klapuri (2006) metrical level tracking.

Per VISUALIZER_GOALS.md this is low priority — energy-reactive organic mode is already the correct visual response for syncopated content.

---

## Next Actions

1. **Add onset density tracking** — Cheap EMA of onsets/second for content classification.
2. **Build diverse test music library** — hip hop, DnB, funk, pop, rock with ground truth annotations.

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
