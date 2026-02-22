# Blinky Time - Improvement Plan

*Last Updated: February 21, 2026*

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

## Outstanding Issues

### Priority 1: CBSS Beat Tracking — Remaining Improvements

BTrack-style predict+countdown CBSS beat detection is implemented and working. Beat F1 avg **0.452** (best 0.704) on 9 real music tracks with BandFlux Solo + beatoffset=5. BPM estimation avg 94.2%.

**What's implemented (Feb 21):**
- ODF pre-smoothing (5-point causal moving average)
- Log-Gaussian transition weighting in CBSS backward search
- BTrack-style predict+countdown beat detection
- Beat timing offset compensation (`beatoffset=5`, recalibrated from 9 for BandFlux)
- Harmonic disambiguation (half-lag + 2/3-lag checks)
- 12 runtime-tunable parameters via serial console (7 now persisted to flash)

**Current performance (9 real music tracks, beatoffset=5):**

| Track | Beat F1 | BPM Acc | Beat Offset |
|-------|:-------:|:-------:|:-----------:|
| minimal-01 | 0.704 | 0.994 | -16ms |
| trance-party | 0.693 | 0.997 | -58ms |
| infected-vibes | 0.694 | 0.979 | -58ms |
| goa-mantra | 0.550 | 0.993 | +38ms |
| deep-ambience | 0.499 | 0.933 | -2ms |
| minimal-emotion | 0.382 | 0.996 | +82ms |
| machine-drum | 0.209 | 0.833 | +12ms |
| trap-electro | 0.176 | 0.927 | -6ms |
| dub-groove | 0.161 | 0.827 | +25ms |
| **Average** | **0.452** | **0.942** | |

**Remaining issues:**

#### 1a. Sub-Harmonic BPM Locking (techno-machine-drum) — INVESTIGATED
BPM locks to ~120 vs expected 143.6 (ratio 0.83x). Not a clean harmonic — half-lag/2/3-lag/double-lag checks don't catch it.

**Investigated Feb 21:** Disabling tempo prior moved BPM from 119.6→131.8 (better but still wrong). Shifting prior center to 128 barely helped (121.8) and hurt trance-party. **Conclusion: fundamental autocorrelation limitation** — the dominant autocorrelation peak genuinely corresponds to the sub-harmonic. No parameter tuning can fix this.

#### 1b. Ambient/Sparse Track (techno-deep-ambience) — IMPROVED
Was F1=0.134 at beatoffset=9. Now F1=0.499 at beatoffset=5. Still has -2ms offset (well-centered). Remaining gap is likely from weak onset signal in ambient sections.

#### 1c. Trap/Syncopated Failure (edm-trap-electro)
F1=0.176. Trap-style half-time feel with syncopated kicks fundamentally challenges 4-on-the-floor assumptions. Likely an inherent limitation for real-time causal beat tracking, or ground truth uses a different metrical level.

#### 1d. Per-Track Offset Variation
Beat offsets vary from -58ms (trance/infected) to +82ms (minimal-emotion) at beatoffset=5. A single fixed offset can't compensate for all tracks.

**Possible improvement:**
- Adaptive offset: track running median of transient-to-beat offsets at runtime
- Accept this as a ~70ms limitation of the causal approach

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

### Priority 4: Startup Latency

AudioController requires ~3s of OSS buffer before first beat detection (180 samples @ 60Hz). Progressive approach: start autocorrelation at 60 samples (1s), limit max lag to `ossCount_ / 3`.

### Priority 4: Environment Adaptability (Long-term)

System tuned for studio conditions. Real-world environments (club, festival, ambient) may benefit from different detector profiles. An environment classifier (based on signal level + spectral centroid + beat stability) could switch detector weights automatically every 2-5 seconds with hysteresis.

---

## Next Actions

1. **Pad rejection further improvement** — Still 16 FPs on pad-rejection at onsetDelta=0.3. Pad chord transitions create sharp spectral changes that pass the delta filter. Possible approaches: band-ratio gate (pads have significant midFlux, kicks are bass-only), sustain envelope check.
2. **Build diverse test music library** — hip hop (syncopated), DnB (broken beats, 170+ BPM), funk (swing), pop (sparse), rock (fills) with ground truth annotations.
3. **deep-ambience regression** — F1 dropped 0.499→0.404 with onset delta filter. Ambient tracks have soft onsets that get filtered. Investigate content-adaptive onset delta (auto-lower for ambient, auto-raise for dance).
4. **Environment adaptability** — Auto-switch detector profiles based on signal characteristics (club vs ambient vs festival).

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history + CBSS eval results |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
