# Blinky Time - Improvement Plan

*Last Updated: February 2026*

## Current Status

### Completed (February 2026)

**Rhythm Tracking:**
- CBSS beat tracking with counter-based beat detection (replaced multi-hypothesis v3)
- Deterministic phase derivation from beat counter
- Tempo prior with Gaussian weighting (120 BPM center, 50 width, 0.5 blend)
- Adaptive cooldown — tempo-aware, scales with beat period
- 2-detector ensemble: Drummer (0.50, thresh 4.5) + ComplexDomain (0.50, thresh 3.5)

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

BTrack-style predict+countdown CBSS beat detection is implemented and working. Beat F1 improved from 0.10–0.32 to **average 0.47, best 0.84** on real music. BPM estimation remains excellent (96–100%).

**What's implemented (Feb 20):**
- ODF pre-smoothing (5-point causal moving average)
- Log-Gaussian transition weighting in CBSS backward search
- BTrack-style predict+countdown beat detection (replaces "first declining frame")
- Beat timing offset compensation (`beatoffset=9`)
- Harmonic disambiguation (half-lag + 2/3-lag checks)
- 7 runtime-tunable parameters via serial console

**Current performance (9 real music tracks):**

| Category | Tracks | Avg F1 | Notes |
|----------|--------|--------|-------|
| Working well (F1 > 0.6) | 4 | 0.724 | Strong kick patterns |
| Moderate (F1 0.3-0.6) | 2 | 0.447 | Lighter kicks, syncopation |
| Failing (F1 < 0.2) | 3 | 0.150 | Wrong BPM or ambient |

**Remaining issues to address:**

#### 1a. Sub-Harmonic BPM Locking (techno-machine-drum)
BPM locks to 114.9 vs expected 143.6 (ratio 0.80x). Not a clean harmonic — current half-lag/2/3-lag/double-lag checks don't catch it. Tempo prior at 128 BPM center actively pulls toward the sub-harmonic.

**Possible fixes:**
- Widen tempo prior (increase `priorwidth` from 50)
- Raise prior center toward 130-135 BPM
- Add more harmonic ratio checks (4/5x, 5/4x)
- Dynamic prior that shifts based on detected content

#### 1b. Ambient/Sparse Track Failure (techno-deep-ambience)
+100ms systematic late offset, F1=0.134. Ambient sections with low onset energy delay CBSS accumulation. The `beatoffset=9` is tuned for tracks with strong transients.

**Possible fixes:**
- Adaptive beat offset based on onset strength or confidence
- Minimum CBSS threshold below which beats should not be declared
- Confidence-gated beat output (only emit beats when confidence > threshold)

#### 1c. Trap/Syncopated Failure (edm-trap-electro)
IQR=318ms (essentially random beat placement). Trap-style half-time feel with syncopated kicks fundamentally challenges our 4-on-the-floor assumptions.

**Possible fixes:**
- May be an inherent limitation for real-time causal beat tracking
- Ground truth may use different metrical level than device expects
- Consider if AMLt (allowed metrical levels) score is more relevant than F1

#### 1d. Per-Track Offset Variation
Beat offsets vary from -79ms (dub-groove) to +100ms (deep-ambience). A single `beatoffset` value can't compensate for all tracks.

**Possible fixes:**
- Adaptive offset: track running median of transient-to-beat offsets
- Content-adaptive: use onset strength variance to adjust offset
- Accept this as a ~50ms limitation of the causal approach

**What NOT to do (tested and rejected):**
- Phase correction (phasecorr): Destroys BPM on syncopated tracks
- ODF width > 5: Variable delay destroys beatoffset calibration
- Ensemble transient input to CBSS: Only works for 4-on-the-floor
- Multi-resolution ODF: Not needed given current ODF=5 stability

### Priority 2: BandWeightedFluxDetector — IMPLEMENTED & TESTED (Feb 20, 2026)

**Status:** Implemented, compiled, tested on 9 real music tracks. Available via serial but NOT yet default.

**Implementation:** `BandWeightedFluxDetector` added as 7th detector (`BAND_FLUX = 6`, `COUNT = 7`). Files: `audio/detectors/BandWeightedFluxDetector.h/.cpp`. Disabled by default to preserve existing behavior.

**Algorithm:** Log-compress FFT magnitudes (`log(1 + 20 * mag[k])`), 3-bin max-filter (SuperFlux vibrato suppression), band-weighted half-wave rectified flux (bass 2.0x, mid 1.5x, high 0.1x), additive threshold (`mean + delta`), asymmetric threshold update, hi-hat rejection gate.

**Test Results (4 configs, 9 tracks):**

| Config | Avg Beat F1 | Best For |
|--------|:-:|:--|
| Baseline (Drummer+Complex) | 0.411 | techno-minimal-emotion (0.700), machine-drum (0.245) |
| **BandFlux Solo** | **0.468** | trance-party (0.836), infected-vibes (0.764), deep-ambience (0.571) |
| BandFlux+Drummer | 0.458 | goa-mantra (0.637), minimal-01 (0.627) |
| All Three (B+D+C) | 0.476* | goa-mantra (0.649) |

*8 tracks only (1 missing due to serial timeout)

**Key findings:**
1. BandFlux Solo is the best overall config (+14% avg Beat F1 vs baseline)
2. Additive threshold solves the low-signal problem where Drummer's multiplicative threshold fails
3. Multi-detector combos are worse than BandFlux Solo — ensemble fusion dilutes the cleaner signal
4. Low threshold (0.5) is critical — CBSS needs high-recall onset signal
5. BandFlux hurts techno-minimal-emotion (-0.249) where Drummer already works well

**Remaining tuning work:**
- Gamma sweep (10, 15, 20, 30, 50)
- Bass weight sweep (1.5, 2.0, 2.5, 3.0)
- Threshold fine-tuning (0.3, 0.4, 0.5, 0.6)
- Update firmware defaults after optimization (flip BandFlux on, Drummer off)

See `PARAMETER_TUNING_HISTORY.md` for full per-track results.

### Priority 2b: False Positive Reduction

With BandFlux Solo config, transient F1 is lower (more false positives, higher recall) but Beat F1 improves. The CBSS beat tracker benefits from high-recall onset signal. False positive reduction on synthetic patterns (pad-rejection, lead-melody) needs re-evaluation with BandFlux enabled.

| Pattern | Baseline F1 | Notes |
|---------|:-:|:--|
| lead-melody | 0.286 | Untested with BandFlux — hi-hat gate may help |
| chord-rejection | 0.698 | Untested with BandFlux — max-filter should help |
| pad-rejection | 0.696 | Untested with BandFlux — asymmetric threshold may help |

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

1. **Tune BandFlux parameters** — Sweep gamma (10-50), bass weight (1.5-3.0), and threshold (0.3-0.6) on full 9-track suite. Current defaults are theoretical, not optimized.
2. **Update firmware defaults** — After BandFlux parameter optimization, flip defaults: BandFlux enabled, Drummer disabled. Requires incrementing SETTINGS_VERSION.
3. **Re-evaluate false positives with BandFlux** — Test pad-rejection, chord-rejection, lead-melody patterns with BandFlux Solo to assess hi-hat gate and max-filter effectiveness.
4. **Persist runtime-tunable params** — `temposmooth`, `odfsmooth`, `harmup2x`, `harmup32`, `peakmincorr` are serial-only; add to `StoredMusicParams` and `ConfigStorage` for flash persistence
5. **Sub-harmonic disambiguation for non-standard ratios** — techno-machine-drum locks at 0.80x (not half/2/3). Investigate broader harmonic search or adaptive prior
6. **Adaptive beat timing offset** — Per-track offsets vary -79ms to +100ms. Explore runtime adaptation based on onset strength variance
7. **Build diverse test music library** — hip hop (syncopated), DnB (broken beats, 170+ BPM), funk (swing), pop (sparse), rock (fills) with ground truth annotations
8. **Confidence-gated beat output** — Only output beats when confidence exceeds threshold, to suppress random placement on ambient tracks

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history + CBSS eval results |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
