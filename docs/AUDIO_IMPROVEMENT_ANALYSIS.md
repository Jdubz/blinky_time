# Audio Analysis Improvement Analysis

## Current System Weaknesses

Based on test results (Feb 2026) with corrected onset-event grouping:

| Pattern | F1 | Recall | Issue |
|---------|-----|--------|-------|
| strong-beats | 0.92 | 91% | ✅ Good baseline |
| bass-line | 0.70 | 55% | ⚠️ **37% miss rate on bass** |
| full-mix | 0.69-0.80 | 53-67% | ⚠️ Variable |
| fast-tempo (150 BPM) | 0.39 | 24% | ❌ **76% miss rate** |

### Root Causes

#### 1. Bass Detection Gap (37% miss rate)
- **BassBand detector disabled** due to false positives from room rumble/HVAC noise
- Only HFC (0.60) + Drummer (0.40) enabled - both optimized for percussive attacks
- Bass notes have slower attacks (10-50ms rise time vs 1-5ms for drums)
- Low frequency content (60-130Hz) not well captured by HFC

#### 2. Fast Tempo / Dense Content (76% miss rate)
- 80ms cooldown at 150 BPM = 400ms beat interval
- Pattern has ~5 layered instruments per beat position
- Cooldown correctly limits to 1 detection per 80ms
- But 96 onset events in 10s = 104ms average spacing
- **Miss rate primarily due to soft instruments (hats at 0.3 strength)**

#### 3. Disabled Detectors
Four of six ensemble detectors disabled:
- **SpectralFlux**: Fires on pad chord changes
- **BassBand**: Room noise false positives
- **ComplexDomain**: Adds FPs on sparse patterns
- **Novelty**: Net negative avg F1

---

## State-of-the-Art Techniques (2024-2025)

### Onset Detection

| Method | F1 Score | Latency | Compute | Notes |
|--------|----------|---------|---------|-------|
| CNN (Schlüter 2014) | 0.88+ | ~50ms | High | Requires GPU/NPU |
| RNN/LSTM | 0.85+ | ~30ms | High | State-of-the-art offline |
| SuperFlux | 0.82 | ~10ms | Low | Max-filter vibrato suppression |
| ComplexFlux | 0.83 | ~10ms | Low | + tremolo suppression |
| NINOS2 (2021) | 0.88 | ~20ms | Low | Spectral sparsity, good on strings |
| Spectral Flux | 0.78 | ~5ms | Very Low | Simple baseline |

**Key insight**: CNNs achieve best accuracy but are impractical for nRF52840 (64MHz Cortex-M4, no FPU for inference). SuperFlux/ComplexFlux offer best accuracy-per-compute for embedded.

### Beat Tracking

| Method | Accuracy | Real-time | Embedded-Ready |
|--------|----------|-----------|----------------|
| BeatNet (2021) | State-of-art | Yes | No (Python/GPU) |
| BEAST-1 (2024) | State-of-art | Yes | No (Transformer) |
| IBT | Good | Yes | Partial |
| Krzyzaniak C lib | Good | Yes | **Yes (ANSI C)** |
| Autocorrelation | Moderate | Yes | **Yes** |

**Key insight**: Our multi-hypothesis autocorrelation approach is appropriate for embedded. Main improvements should focus on OSS quality, not tracking algorithm.

---

## Reasonable Accuracy Expectations

### Onset Detection (Embedded Real-time)

| Scenario | Target F1 | Notes |
|----------|-----------|-------|
| Clean drums | 0.95+ | Already achieving 0.92 |
| Mixed drums + bass | 0.85 | Need bass-aware detection |
| Full mix (all instruments) | 0.75 | Complex, layered audio |
| Fast tempo (>140 BPM) | 0.65 | Physical cooldown limits |
| Soft dynamics | 0.80 | Below 0.5 strength |

### Beat Tracking (Embedded Real-time)

| Metric | Target | Notes |
|--------|--------|-------|
| BPM accuracy | ±2% | Already achieving |
| Phase accuracy | ±50ms | Currently ~15ms avg |
| Lock-on time | <3s | Currently ~1.5s |
| Stability (std dev) | <5 BPM | Pattern-dependent |

---

## Proposed Improvements

### Priority 1: Bass-Aware Onset Detection

**Problem**: 37% bass miss rate with current HFC+Drummer only

**Approach**: Adaptive band-weighted spectral flux

```
Instead of disabled BassBand detector:
1. Compute spectral flux in 3 bands:
   - Low (60-250Hz): bass, kick
   - Mid (250-2000Hz): snare, vocals
   - High (2000-8000Hz): hats, cymbals

2. Weight bands by recent activity:
   - If low band has strong periodicity → boost bass detection
   - If high band is noisy → suppress hat false positives

3. Adaptive noise gate per band:
   - Low band: higher gate (room rumble rejection)
   - High band: lower gate (capture soft hats)
```

**Expected improvement**: +15-20% recall on bass-line pattern

### Priority 2: Soft Transient Recovery

**Problem**: Soft instruments (strength 0.3) often missed

**Approach**: Context-aware threshold adaptation

```
1. Track rolling average of detected transient strengths
2. If average is low (quiet passage):
   - Temporarily lower detection threshold
   - Increase agreement boost for weak detections
3. If average is high (loud passage):
   - Raise threshold to prevent over-triggering
```

**Expected improvement**: +10% recall on soft-beats, medium-beats

### Priority 3: Velocity-Aware Cooldown

**Problem**: Fixed 80ms cooldown limits fast tempo detection

**Approach**: Adaptive cooldown based on detected tempo

```
Current: cooldown = 80ms (fixed)

Proposed:
  cooldown = max(40ms, min(80ms, beatPeriod / 6))

  At 120 BPM: beatPeriod = 500ms → cooldown = 80ms (unchanged)
  At 150 BPM: beatPeriod = 400ms → cooldown = 67ms
  At 180 BPM: beatPeriod = 333ms → cooldown = 55ms
```

**Expected improvement**: +10-15% recall on fast-tempo

### Priority 4: Re-enable BassBand with Noise Rejection

**Problem**: BassBand disabled due to room rumble false positives

**Approach**: Highpass the BassBand detector

```
1. Apply 40Hz highpass before BassBand analysis
   - Room rumble is typically < 40Hz
   - Bass notes are typically > 40Hz

2. Add periodicity check:
   - Only fire if low-band energy has been periodic
   - Single bass hits in silence are suspicious (noise)

3. Require HFC or Drummer agreement for bass detection:
   - Bass + attack = real bass note
   - Bass alone = possible HVAC/room noise
```

**Expected improvement**: +20% recall on bass-line without FP increase

### Priority 5: SuperFlux Max-Filter Implementation

**Problem**: SpectralFlux fires on sustained content (pads, vibrato)

**Approach**: Implement SuperFlux vibrato suppression

```
Current SpectralFlux:
  flux[t] = sum(max(0, mag[t] - mag[t-1]))

SuperFlux (Böck & Widmer 2013):
  flux[t] = sum(max(0, mag[t] - max_filter(mag[t-w:t-1])))

Where max_filter suppresses slow energy changes (vibrato, swells)
```

**Implementation cost**: Minimal - replace subtraction with max-filtered subtraction

**Expected improvement**: Enables re-enabling SpectralFlux (+5-10% recall)

---

## Implementation Plan

### Phase 1: Quick Wins (1-2 hours each)

1. **Adaptive cooldown** - Straightforward parameter change
2. **SuperFlux max-filter** - Simple algorithm modification
3. **BassBand highpass** - Add IIR filter before analysis

### Phase 2: Moderate Effort (3-4 hours each)

4. **Context-aware thresholds** - Track rolling statistics
5. **Band-weighted spectral flux** - New detection approach

### Phase 3: Validation

6. Run full test suite with each change
7. Measure F1 improvement vs. compute increase
8. Document optimal parameter values

---

## References

- [madmom onset detection](https://madmom.readthedocs.io/en/v0.16/modules/features/onsets.html) - Reference implementations
- [CNN onset detection (Schlüter 2014)](https://www.ofai.at/~jan.schlueter/pubs/2014_icassp.pdf) - State-of-the-art accuracy
- [BeatNet](https://github.com/mjhydri/BeatNet) - State-of-the-art beat tracking
- [Beat-and-Tempo-Tracking C library](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking) - Embedded-ready reference
- [NINOS2 spectral sparsity](https://asmp-eurasipjournals.springeropen.com/articles/10.1186/s13636-021-00214-7) - Low-compute onset detection
- [Zero-latency beat tracking (2024)](https://transactions.ismir.net/articles/10.5334/tismir.189) - Real-time techniques
- [Cycfi onset detection](https://www.cycfi.com/2021/01/onset-detection/) - Practical implementation guide
