# Blinky Time - Improvement Plan

*Last Updated: February 2026*

## Current Status

### Completed (February 2026)

**Rhythm Tracking (greatly improved):**
- Tempo prior with Gaussian weighting (120 BPM center, 50 width, 0.5 blend) — persisted to flash
- Pulse train Fourier phase (PLP-inspired, Phase 3) — 10-11% F1 improvement
- Comb filter phase tracker (Phase 4) — multi-system phase fusion framework
- All rhythm params now persisted to flash (`StoredMusicParams`, 68 bytes)
- Adaptive cooldown — 250ms base, scales with tempo via `beatPeriod / 6`
- BassBand detector re-enabled with noise rejection (3-detector config: Drummer 0.40 + HFC 0.60 + BassBand 0.18)

**Particle System & Visuals:**
- Frame-rate independent physics (centiseconds, not frames)
- Continuous mode blending (replaced binary `hasRhythm()` threshold)
- Particle variety system (FAST_SPARK, SLOW_EMBER, BURST_SPARK types)
- Smooth 6-stop color gradient for fire (eliminated banding)
- Hardware AGC full range in loud mode (0-80, was 10-80)
- Multi-octave SimplexNoise turbulence wind (replaced sine wave)
- Runtime device configuration (safe mode, JSON registry, serial upload)

### Completed (January 2026)

**Beat Tracking:**
- CBSS beat tracking with counter-based beat detection (replaced multi-hypothesis v3)
- Deterministic phase derivation from beat counter
- Tempo prior for half-time/double-time disambiguation

### Completed (December 2025)

**Architecture:** Generator → Effect → Renderer, AudioController v3, ensemble detection (6 algorithms), agreement-based fusion, comprehensive testing infrastructure (MCP + param-tuner), calibration completed.

---

## Outstanding Issues

### Priority 1: CBSS Beat Detection Overhaul

CBSS beat tracking is fundamentally broken on real music (Beat F1 = 0.10–0.32 across EDM tracks) despite excellent BPM estimation (97–99% accuracy). See detailed findings in `blinky-test-player/PARAMETER_TUNING_HISTORY.md` (Feb 2026 session).

**Root cause:** The "first declining CBSS frame" peak detector within the beat window is too fragile — spectral flux has many micro-fluctuations, so the first local max is often a noise peak early in the window. This places beats consistently early (~83ms offset) and misses the real beat peak later.

**What works:** BPM estimation via autocorrelation is solid. Transient detection on synthetic patterns is strong (F1 0.80–0.94).

**What doesn't work:** Translating correct BPM into correct beat positions on real music. The CBSS signal processing layer between autocorrelation and beat detection adds noise rather than clarity.

**Approaches to investigate:**
- Better peak selection in beat window (find maximum, not first decline)
- Adaptive window sizing based on confidence
- Alternative beat tracking architectures (BTrack, particle filtering)
- Must test on diverse genres (hip hop, DnB, funk) not just 4-on-the-floor EDM

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

1. Fix CBSS beat detection — the peak selection algorithm within the beat window needs a fundamentally better approach
2. Build diverse test music library (hip hop, DnB, funk, pop) with ground truth annotations
3. Tune chord-rejection threshold trade-off (threshold vs. recall on strong-beats)
4. Investigate temporal envelope gate for lead-melody false positives

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/AUDIO_ARCHITECTURE.md` | AudioController architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `docs/AUDIO_ARCHITECTURE.md` | CBSS architecture details |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
