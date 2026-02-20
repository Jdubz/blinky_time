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

### Priority 1: False Positive Reduction

Current detector config: **Drummer (0.40) + HFC (0.60) + BassBand (0.18)**, cooldown=250ms, minconf=0.55

| Pattern | F1 | Issue |
|---------|-----|-------|
| lead-melody | 0.286 | 38-40 FPs — HFC fires on every melody note |
| chord-rejection | 0.698 | 12 FPs — amplitude spikes on chord changes |
| pad-rejection | 0.696 | 7 FPs, high variance (0.64–0.80) |

**lead-melody** (hardest, algorithmic): HFC correctly detects high-frequency spectral change; can't distinguish melody note from percussive transient. Potential approaches:
- Temporal envelope gate: melody notes sustain >50ms, percussive hits decay rapidly
- Harmonic-to-noise ratio: percussive = broadband, pitched = harmonic peaks

**chord-rejection**: Chord transitions produce genuine amplitude spikes.
- Quick test: raise `drummer` and `hfc` thresholds, check trade-off against strong-beats recall
- Rise-time analysis: chord transitions have slower attack than percussive hits

**pad-rejection**: High variance suggests AGC-related instability.
- Run 3x and average for stable baseline before tuning

### Priority 2: CBSS Tuning

New CBSS beat tracker needs real-music validation. Key params: `cbssalpha`, `beatwindow`, `beatconfdecay`, `temposnap`.

### Priority 3: Startup Latency

AudioController requires ~3s of OSS buffer before first beat detection (180 samples @ 60Hz). Progressive approach: start autocorrelation at 60 samples (1s), limit max lag to `ossCount_ / 3`.

### Priority 4: Environment Adaptability (Long-term)

System tuned for studio conditions. Real-world environments (club, festival, ambient) may benefit from different detector profiles. An environment classifier (based on signal level + spectral centroid + beat stability) could switch detector weights automatically every 2-5 seconds with hysteresis.

---

## Next Actions

1. Re-run full pattern suite with current 3-detector config to establish fresh F1 baseline
2. Tune chord-rejection threshold trade-off (threshold vs. recall on strong-beats)
3. Tune CBSS beat tracking params on real music (`cbssalpha`, `beatwindow`, `beatconfdecay`)
4. Investigate temporal envelope gate for lead-melody false positives

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/AUDIO_ARCHITECTURE.md` | AudioController architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `docs/AUDIO_IMPROVEMENT_ANALYSIS.md` | Detailed improvement approaches |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
