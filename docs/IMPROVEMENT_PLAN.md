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

### Priority 1: CBSS Beat Detection Overhaul

Beat tracking produces F1 = 0.10–0.32 on real music despite excellent BPM estimation (97–99%). See test results in `blinky-test-player/PARAMETER_TUNING_HISTORY.md` (Feb 2026 session).

**What works:** BPM via autocorrelation (97–99%), transient detection on synthetic patterns (F1 0.80–0.94).

**What's broken:** Translating correct BPM into correct beat positions. Two root causes identified:

1. **No ODF pre-smoothing.** Production beat trackers universally low-pass filter the onset detection function before CBSS. Our raw spectral flux goes straight into CBSS with all its micro-fluctuations, creating many false local maxima. This is the most basic missing step.

2. **Wrong peak selection architecture.** Our `detectBeat()` triggers on the "first declining CBSS frame" within the beat window. BTrack (the reference real-time CBSS implementation by Adam Stark) does NOT do this. BTrack uses a **predict-then-countdown** mechanism that is fundamentally more robust.

#### Research Findings

Studied BTrack (Stark 2011), Ellis DP beat tracker (2007, used by librosa), Davies & Plumbley context-dependent tracker (2007), particle filtering (Hainsworth 2004, BeatNet 2021), OBTAIN (Mottaghi 2017), and multi-resolution ODF aggregation (McFee & Ellis 2014).

**Key insight from BTrack:** At the midpoint between beats, BTrack synthesizes future CBSS values by extrapolating with zero onset strength, multiplies by a Gaussian window centered at the expected next beat time, and takes the argmax. This argmax becomes a countdown timer — when it reaches zero, a beat is declared. The Gaussian window strongly suppresses early noise peaks (they get near-zero weight) while the countdown locks in the prediction so subsequent fluctuations can't change it.

**Key insight from Ellis DP:** Inter-beat intervals that deviate from the expected tempo are penalized with a log-Gaussian cost: `penalty = -(log2(interval / expected))^2`. This means an early noise peak at 417ms (when expecting 500ms at 120 BPM) must have significantly higher onset strength to outweigh the timing penalty.

**Key insight from ODF literature:** A causal 5-point moving average on the onset signal eliminates the micro-fluctuations our detector is triggering on. Every production beat tracker applies this. We don't.

#### Implementation Plan (Incremental, Build-Compile-Test Each Step)

**Step 1: ODF Pre-Smoothing** (~30 min, no architecture change)

Apply causal 5-point moving average to `onsetStrength` before `updateCBSS()`:
```
smoothed[n] = (oss[n] + oss[n-1] + oss[n-2] + oss[n-3] + oss[n-4]) / 5
```
Introduces ~33ms latency (2 frames at 60Hz). Cost: 20 bytes RAM, ~10 ops/frame. This directly removes the noise peaks that cause early triggering.

Files: `AudioController.h` (add 5-float ring buffer), `AudioController.cpp` (add smoothing before CBSS call)

**Step 2: Gaussian Beat Window Weighting** (~2 hours, small change to detectBeat)

Replace flat beat window with Gaussian-weighted scoring. Instead of accepting the first declining frame, weight each candidate peak by proximity to the expected beat time:
```
score = CBSS_value * exp(-(offset_from_center)^2 / (2 * sigma^2))
```
Track the highest-scoring peak within the window; declare beat at late bound or after clear decline from best.

Files: `AudioController.h` (precomputed Gaussian LUT, ~240 bytes), `AudioController.cpp` (rewrite inner loop of `detectBeat()`)

**Step 3: BTrack-Style Predict+Countdown** (~4-6 hours, rewrite detectBeat)

Replace the continuous peak scan with BTrack's two-phase approach:
1. At beat midpoint (`samplesSinceBeat >= T/2`), synthesize future CBSS for T/2 frames (feed zero onset into CBSS recursion)
2. Multiply future CBSS by Gaussian expectation window centered at T/2
3. `timeToNextBeat = argmax(weighted_future_CBSS)`
4. Countdown each frame; declare beat at zero

The prediction runs once per beat (~2Hz), not every frame. Future synthesis is O(T/2) ≈ 15 iterations at 120 BPM.

Cost: ~2.4 KB RAM (temporary stack buffer for future synthesis + Gaussian LUT), ~200 ops/frame + ~1000 ops per prediction. Easily feasible at 60Hz on Cortex-M4.

Files: `AudioController.h` (add countdown state, Gaussian LUT), `AudioController.cpp` (rewrite `detectBeat()`, keep `updateCBSS()` as-is)

**Step 4: Multi-Resolution ODF** (~1 hour, complementary)

Compute onset strength at 3 smoothing levels (raw, 3-frame avg, 7-frame avg) and take the geometric mean. Noise peaks at one scale that don't appear at others get suppressed. Inspired by McFee & Ellis 2014.

Cost: 40 bytes RAM, ~20 ops/frame.

**Step 5: Ellis-Style Temporal Penalty** (~2 hours, complementary)

When scoring candidate peaks in the beat window, add a log-Gaussian penalty for inter-beat intervals deviating from expected tempo:
```
penalty = tightness * (log2(candidate_interval / expected_interval))^2
```
Precompute as lookup table (~480 bytes). This creates a strong basin of attraction around the correct beat time.

#### What NOT To Do

- **Particle filters**: Too complex for marginal benefit. Our autocorrelation already handles tempo uncertainty well.
- **Neural networks / TCN**: Not feasible on Cortex-M4 without FPU.
- **Full offline DP**: Requires backtracking, not suitable for real-time. The causal adaptation is essentially BTrack.
- **OBTAIN in full**: Requires DNN inference.

#### Resource Budget

| Component | RAM | CPU (per frame) | Notes |
|-----------|-----|-----------------|-------|
| ODF smoothing | 20 B | ~10 ops | 5-point ring buffer |
| Gaussian LUT | 240 B | 0 (precomputed) | Beat expectation window |
| Future CBSS synthesis | 1.68 KB | ~1000 ops (per beat only) | Stack-allocated, temporary |
| Multi-res ODF | 40 B | ~20 ops | 2 additional moving averages |
| Penalty LUT | 480 B | ~100 ops (per candidate) | Log-Gaussian precomputed |
| **Total** | **~2.5 KB** | **~230 ops/frame** | Well within headroom |

#### Evaluation Criteria

Must test on diverse genres, not just 4-on-the-floor EDM:
- Build test library: hip hop (syncopated), DnB (broken beats, 170+ BPM), funk (swing), pop (sparse), rock (fills)
- Use `annotate-beats.py` to generate ground truth from librosa
- Target: Beat F1 > 0.5 on diverse genres, Beat F1 > 0.7 on steady-tempo tracks

#### References

- Stark 2011: [BTrack — Real-time beat tracking](https://github.com/adamstark/BTrack)
- Ellis 2007: [Beat Tracking by Dynamic Programming](https://www.ee.columbia.edu/~dpwe/pubs/Ellis07-beattrack.pdf)
- Davies & Plumbley 2007: [Context-Dependent Beat Tracking](https://ieeexplore.ieee.org/document/4317507)
- McFee & Ellis 2014: [Better Beat Tracking Through Robust Onset Aggregation](https://www.ee.columbia.edu/~dpwe/pubs/McFeeE14-beats.pdf)

### Priority 2: False Positive Reduction

Current detector config: **Drummer (0.50) + ComplexDomain (0.50)**, cooldown=tempo-adaptive, minconf=0.55

| Pattern | F1 | Issue |
|---------|-----|-------|
| lead-melody | 0.286 | 38-40 FPs — fires on every melody note |
| chord-rejection | 0.698 | 12 FPs — amplitude spikes on chord changes |
| pad-rejection | 0.696 | 7 FPs, high variance (0.64–0.80) |

### Priority 3: Startup Latency

AudioController requires ~3s of OSS buffer before first beat detection (180 samples @ 60Hz). Progressive approach: start autocorrelation at 60 samples (1s), limit max lag to `ossCount_ / 3`.

### Priority 4: Environment Adaptability (Long-term)

System tuned for studio conditions. Real-world environments (club, festival, ambient) may benefit from different detector profiles. An environment classifier (based on signal level + spectral centroid + beat stability) could switch detector weights automatically every 2-5 seconds with hysteresis.

---

## Next Actions

1. **ODF pre-smoothing** — 5-point causal moving average on onset strength (quick win, directly addresses noise peaks)
2. **BTrack-style predict+countdown** — Replace `detectBeat()` with Gaussian-weighted future synthesis (the real fix)
3. **Build diverse test music library** — hip hop, DnB, funk, pop with ground truth annotations
4. **Multi-resolution ODF + Ellis temporal penalty** — complementary improvements after core fix
5. Tune chord-rejection threshold trade-off
6. Investigate temporal envelope gate for lead-melody false positives

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history + CBSS eval results |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
