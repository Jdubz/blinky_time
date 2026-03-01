# Parameter Tuning History

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive documentation.

This document tracks parameter optimization tests and findings. Raw test result files are excluded from git (see .gitignore) to avoid repo bloat.

---

## Architecture Change: December 2025

**Major Refactor:** Replaced PLL-based MusicMode + RhythmAnalyzer with unified AudioController using autocorrelation-based rhythm tracking.

**Impact on Tuning:**
- PLL parameters (pllkp, pllki, confinc, confdec, etc.) no longer exist
- Rhythm tracking now uses: `musicthresh`, `phaseadapt`, `bpmmin`, `bpmmax`
- Previous MusicMode tuning results are obsolete
- Transient detection parameters remain valid

---

## Test Session: 2025-12-27

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM41
- Hardware Gain: Locked during tests
- Test Patterns: strong-beats, simple-4-on-floor, and 6 additional representative patterns
- Tool: param-tuner sweep mode (exhaustive value testing)

**Parameters Tested:**

### hitthresh (Drummer Mode - Transient Detection)
- **Range Tested:** 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 7.0, 10.0
- **Previous Default:** 2.0 (tuned from original 3.0)
- **Optimal Value:** 3.5
- **Performance:**
  - @ 2.0: F1 = 0.438 (100% precision, 28.9% recall)
  - @ 3.5: F1 = 0.463 (100% precision, 31.3% recall)
  - **Improvement:** +5.7% F1
- **Applied to Firmware:** ✅ ConfigStorage.cpp, AdaptiveMic.h

### fluxthresh (Spectral Mode - FFT-based Detection)
- **Range Tested:** 1.0, 1.5, 2.0, 2.5, 2.8, 3.0, 4.0, 5.0, 7.0, 10.0
- **Previous Default:** 2.8 (tuned from 2.641, originally 3.0)
- **Optimal Value:** 2.0
- **Performance:**
  - @ 2.8: F1 ≈ 0.59 (estimated)
  - @ 2.0: F1 = 0.670 (97.2% precision, 52.1% recall)
  - **Improvement:** +13.6% F1
- **Applied to Firmware:** ✅ ConfigStorage.cpp, AdaptiveMic.h

**Key Insights:**

1. **Precision-Focused System:** Both optimal parameters show high precision (97-100%) but moderate recall (31-52%). The system prioritizes avoiding false positives over catching every hit.

2. **Spectral vs Drummer:** Spectral flux mode (52% recall) significantly outperforms drummer mode (31% recall) for the tested patterns, suggesting FFT-based detection is more robust for complex audio.

3. **Test Pattern Characteristics:**
   - strong-beats: Hard drum hits with clear transients
   - simple-4-on-floor: Regular kick pattern
   - Both patterns favor precision over recall in optimal settings

4. **Hardware Gain:** Tests conducted with locked hardware gain to eliminate AGC variability. Real-world performance may differ with adaptive gain.

## Test Session: 2025-12-28 (Fast Binary Search Tuning)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM41
- Hardware Gain: Locked at 40 during tests
- Test Patterns: strong-beats, simple-4-on-floor, bass-line, full-mix (4 core patterns for fast tuning)
- Tool: param-tuner fast mode (binary search + targeted validation, ~30 min)

**Parameters Tested:**

### hitthresh - Mode-Specific Optimization

**Drummer Mode:**
- **Binary Search Range:** 1.688 - 2.25
- **Optimal Value:** 1.688
- **Performance:** F1 = 0.664 (90.5% precision, 57.0% recall)
- **Previous Best (from exhaustive sweep):** 3.5 → F1 = 0.463
- **Improvement:** +43.3% F1 improvement over exhaustive sweep!

**Hybrid Mode:**
- **Binary Search Range:** 2.0 - 2.813
- **Optimal Value:** 2.813
- **Performance:** F1 = 0.669 (67.3% precision, 72.6% recall)

### fluxthresh (Spectral Mode)
- **Binary Search Range:** 1.2 - 2.8
- **Optimal Value:** 1.4
- **Performance:** F1 = 0.670 (72.3% precision, 69.8% recall)
- **Previous Best (from exhaustive sweep):** 2.0 → F1 = 0.670
- **Improvement:** Confirmed optimal, refined from 2.0 to 1.4

### Additional Parameters Optimized:

**Drummer Mode:**
- **attackmult:** 1.1 (down from 1.3 default)
- **cooldown:** 40ms (up from 30ms, down from original 80ms)

**Hybrid Mode:**
- **hydrumwt:** 0.3 (down from 0.5 default)
- **hyfluxwt:** 0.7 (up from 0.3 default)

**Key Findings:**

1. **Binary Search Vastly Superior:** Fast-tune found drummer-optimal hitthresh = 1.688 (F1: 0.664) vs exhaustive sweep's 3.5 (F1: 0.463). The exhaustive sweep tested too high and missed the optimal range entirely.

2. **Mode Performance Ranking:**
   - Spectral: F1 = 0.670 (BEST - balanced precision/recall)
   - Hybrid: F1 = 0.669 (nearly equal, more robust across patterns)
   - Drummer: F1 = 0.664 (good, but lower recall)

3. **Precision vs Recall Trade-off:**
   - Lower thresholds (1.4-1.7) significantly improve recall without sacrificing much precision
   - Previous high thresholds (2.0-3.5) were too conservative, missing 50-70% of hits

4. **Hybrid Mode Weighting:** Flux component should dominate (0.7 weight) with drummer providing supplemental detection (0.3 weight). This balances spectral's frequency analysis with drummer's amplitude tracking.

5. **Optimal Default Mode:** Hybrid (F1: 0.669) recommended as default despite spectral being marginally better (F1: 0.670) because hybrid is more robust across diverse audio patterns.

**Applied to Firmware:** ✅ All optimal values updated in ConfigStorage.cpp and AdaptiveMic.h

**Firmware Defaults (as of 2025-12-28):**
- Detection Mode: 4 (Hybrid)
- transientThreshold: 2.813 (hybrid-optimal)
- attackMultiplier: 1.1
- cooldownMs: 40
- fluxThresh: 1.4
- hybridFluxWeight: 0.7
- hybridDrumWeight: 0.3

### Boundary Analysis - CRITICAL FINDINGS

**Parameters at or near minimum bounds:**

1. **attackmult: 1.1** ⚠️ CRITICAL
   - Optimal value is EXACTLY at the old minimum (1.1)
   - Extended range: 1.0 - 2.0 (was 1.1 - 2.0)
   - **Action Required:** Test 1.0, 1.05 to confirm no better values below

2. **hitthresh: 1.688** ⚠️ HIGH PRIORITY
   - Optimal value only 0.188 above old minimum (1.5)
   - Extended range: 1.0 - 10.0 (was 1.5 - 10.0)
   - **Action Required:** Test 1.0, 1.2, 1.4 to explore lower range

3. **fluxthresh: 1.4** ⚠️ MEDIUM PRIORITY
   - Optimal value 0.4 above old minimum (1.0)
   - Extended range: 0.5 - 10.0 (was 1.0 - 10.0)
   - **Action Required:** Test 0.5, 0.8, 1.0, 1.2 to verify no improvement below

**Implication:** Binary search may have converged at artificial boundaries. True optimal values may exist below old minimums.

### Problem Patterns Identified

**Pattern performance analysis revealed 4 specific weaknesses:**

| Pattern | Best Mode | F1 | Issue | Hypothesis |
|---------|-----------|-----|-------|-----------|
| simultaneous | Hybrid | 0.640 | 50% missed hits | Overlapping sounds detected as single event |
| simultaneous | Drummer | 0.400 | 75% missed hits | Peak detection can't separate overlaps |
| simultaneous | Spectral | 0.578 | 59% missed hits | FFT resolution insufficient for overlaps |
| fast-tempo | Drummer | 0.490 | 67% missed hits | Cooldown 40ms too long OR threshold too high |
| sparse | Spectral | 0.526 | 54% precision | False positives on quiet sections |
| sparse | Hybrid | 0.500 | 65% precision | Spectral component over-sensitive |
| pad-rejection | Spectral | 0.516 | 65% precision | Sustained tones trigger spectral flux |

**Key Insights:**
- **simultaneous** pattern reveals algorithmic limitation - all modes struggle with overlapping transients
- **fast-tempo** may need cooldown < 40ms or lower threshold for rapid quiet hits
- **pad-rejection** shows spectral flux needs better sustained tone rejection
- All tested patterns with transient hits work well (F1 > 0.7)

---

## Next Priority Tests (Estimated 2 hours total)

### Priority 1: Extended Boundary Testing (15 min) ⚠️ CRITICAL

**Goal:** Confirm optimal values aren't below previous minimums

**Command:**
```bash
npm run tuner -- fast --port COM41 --gain 40 \
  --params attackmult,hitthresh,fluxthresh \
  --patterns strong-beats,bass-line,synth-stabs
```

**Expected Outcome:**
- If current values optimal: No change, ranges confirmed
- If lower values better: Update firmware, document +X% improvement
- If we hit new boundaries at 1.0/0.5: Extend ranges further

**Success Criteria:** Optimal values are 0.2+ away from boundaries

### Priority 2: Fast-Tempo Optimization (20 min)

**Goal:** Improve drummer mode on fast-tempo from F1=0.490 to >0.6

**Test cooldown below 40ms:**
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params cooldown --modes drummer \
  --patterns fast-tempo
```

Test values: 20, 25, 30, 35, 40ms

**If cooldown doesn't help, test lower thresholds:**
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params hitthresh --modes drummer \
  --patterns fast-tempo,simultaneous
```

**Success Criteria:** Drummer fast-tempo F1 > 0.6, maintain precision > 80%

### Priority 3: Simultaneous Detection (30 min)

**Goal:** Improve detection of overlapping sounds from F1=0.640 to >0.7

**Test FFT bin resolution:**
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params fluxbins --modes spectral \
  --patterns simultaneous
```

Test values: 32, 64, 96, 128 (hypothesis: more bins = better frequency separation)

**Optimize hybrid mode further:**
```bash
npm run tuner -- fast --port COM41 --gain 40 \
  --modes hybrid --patterns simultaneous
```

**Success Criteria:** Any mode achieves F1 > 0.7, or confirm as algorithmic limitation

### Priority 4: False Positive Reduction (15 min)

**Goal:** Reduce false positives on pad-rejection and sparse patterns

**Test higher fluxthresh for sustained tone rejection:**
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params fluxthresh --modes spectral,hybrid \
  --patterns sparse,pad-rejection,chord-rejection
```

Test values: 1.4, 1.6, 1.8, 2.0, 2.2, 2.5

**Success Criteria:**
- Spectral/Hybrid pad-rejection: Precision > 70% (currently 35-47%)
- Maintain recall > 80% on transient patterns

### Priority 5: Full Validation (30 min)

**Goal:** Validate optimized parameters across all 18 patterns

**Command:**
```bash
npm run tuner -- validate --port COM41 --gain 40
```

**Success Criteria:**
- No pattern F1 < 0.5 in its best mode
- Average F1 across all patterns > 0.65
- No regressions from fast-tune baseline

### Deferred Tests (Lower Priority)

**Untested parameters:**
- MusicMode (PLL beat tracking): musicthresh, confinc, pllkp, etc.
- RhythmAnalyzer (autocorrelation): beatthresh, minperiodicity, etc.
- Bass/HFC modes: bassfreq, bassq, hfcweight, etc.

**Recommendation:** Optimize core transient detection first, then tune higher-level subsystems.

---

## Test Session: 2025-12-30 (Fast-Tune Dual Run)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM11
- Hardware Gain: Unlocked (AGC active)
- Test Patterns: 8 representative patterns (strong-beats, sparse, bass-line, synth-stabs, pad-rejection, fast-tempo, simultaneous, full-mix)
- Tool: param-tuner fast mode (two consecutive runs for verification)

### Key Finding: Equal Hybrid Weights Outperform 0.7/0.3 Split

**Comparison across two fast-tune runs:**

| Mode | Run 1 F1 | Run 2 F1 | Best Params |
|------|----------|----------|-------------|
| HYBRID | 0.498 | **0.598** | hitthresh=2.341, hyfluxwt=0.5, hydrumwt=0.5 |
| DRUMMER | 0.517 | 0.545 | hitthresh=1.192, cooldown=80 |
| SPECTRAL | 0.316 | 0.43 | fluxthresh=1.7 |

**Run 2 - Best Results (HYBRID mode, F1=0.598):**
- **hitthresh:** 2.341 (vs current 2.813)
- **hyfluxwt:** 0.5 (vs current 0.7) ⚠️ KEY CHANGE
- **hydrumwt:** 0.5 (vs current 0.3) ⚠️ KEY CHANGE
- **hybothboost:** 1.2 (unchanged)

### Pattern-by-Pattern Performance (HYBRID, Run 2)

| Pattern | F1 | Precision | Recall | False Positives |
|---------|-----|-----------|--------|-----------------|
| simultaneous | **0.886** | 81.6% | 96.9% | 7 |
| fast-tempo | **0.774** | 63.2% | 100% | 21 |
| full-mix | **0.727** | 57.1% | 100% | 24 |
| strong-beats | 0.682 | 53.6% | 93.8% | 26 |
| bass-line | 0.658 | 49.0% | 100% | 25 |
| synth-stabs | 0.639 | 47.9% | 95.8% | 25 |
| sparse | 0.267 | 15.4% | 100% | 44 |
| **pad-rejection** | **0.148** | 8.2% | 75.0% | **67** |

### Critical Issues Identified

1. **Pad Rejection Broken:** All modes generate 50-229 false positives on sustained pad sounds
   - DRUMMER: 46 FPs
   - SPECTRAL: 143 FPs
   - HYBRID: 67 FPs
   - **Root cause:** Spectral flux triggers on gradual timbral changes in pads

2. **Sparse Pattern FPs:** All modes over-trigger during quiet sections
   - Need improved silence detection or adaptive thresholding

3. **SPECTRAL Mode Unusable:** 100% recall but only 20-30% precision means severe over-triggering

### Recommendations

1. **Change hybrid weights to 0.5/0.5** (from 0.7/0.3) - provides +20% F1 improvement
2. **Keep hitthresh at 2.813** for conservative false-positive control (vs tuned 2.341)
3. **Increase cooldown to 80ms** for DRUMMER mode
4. **Do NOT use SPECTRAL mode** as primary - severe over-triggering

### Firmware Update Status

**Changes applied (2025-12-30):**
```cpp
// In AdaptiveMic.h, ConfigStorage.cpp, SerialConsole.cpp, Presets.cpp
float hybridFluxWeight = 0.5f;   // Changed from 0.7f
float hybridDrumWeight = 0.5f;   // Changed from 0.3f
uint16_t cooldownMs = 80;        // Changed from 40 (reduces false positives)
```

**NOT changed (kept conservative):**
- hitthresh: Keep 2.813 (tuned value 2.341 has more false positives)
- fluxthresh: Keep 1.4 (SPECTRAL mode is broken regardless)

### ⚠️ Parameters Near Boundary - Requires Retesting

The following parameters converged near their configured limits, suggesting the true optimal may be outside the tested range:

| Parameter | Tuned Value | Old Min | New Min | Distance from Min |
|-----------|-------------|---------|---------|-------------------|
| hitthresh (DRUMMER) | 1.192 | 1.0 | **0.5** | Was only 0.192 from min |
| attackmult | 1.1 | 1.0 | **0.9** | Was only 0.1 from min |

**Ranges extended in `types.ts`:**
```typescript
hitthresh: min: 0.5,  // Extended from 1.0
           sweepValues: [0.5, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, ...]

attackmult: min: 0.9,  // Extended from 1.0
            sweepValues: [0.9, 0.95, 1.0, 1.05, 1.1, ...]
```

**TODO:** Run targeted sweep on hitthresh and attackmult with extended ranges:
```bash
npm run tuner -- sweep --port COM11 --params hitthresh,attackmult --modes drummer
```

---

## Test Infrastructure Notes

**Test Player:** blinky-test-player with MCP server integration
**Ground Truth:** Calibrated sample timing from analyze-samples.ts
**Metrics:** F1 score (harmonic mean of precision/recall), timing error in ms
**Result Storage:** `tuning-results/` (gitignored, local only)
**Result Viewer:** `view-results.cjs` for quick summary display

**Available Detection Modes:**
- drummer (mode 0): Amplitude-based transient detection
- bass (mode 1): Bass-band filtered detection
- hfc (mode 2): High-frequency content detection
- spectral (mode 3): FFT-based spectral flux
- hybrid (mode 4): Combined drummer + spectral

**Subsystems (December 2025 Architecture):**
- AudioController: Autocorrelation-based rhythm tracking (replaced MusicMode + RhythmAnalyzer)
- AdaptiveMic: Transient detection (5 algorithms: drummer, bass, hfc, spectral, hybrid)

---

## Test Session: 2026-01-04 (Tempo Prior Validation)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM11
- Hardware Gain: AGC enabled (hw gain 80 max)
- Test Patterns: steady-60bpm, steady-120bpm, steady-180bpm, strong-beats
- Tool: blinky-serial MCP server with run_test

### Critical Finding: Tempo Prior Was Disabled

**Root Cause:** The tempo prior (`priorenabled`) was disabled by default, causing severe half-time/double-time confusion in BPM tracking.

**Symptoms Before Fix:**
- steady-120bpm: Tracking at 72-88 BPM (half-time)
- strong-beats: Tracking at 145 BPM (incorrect)
- BPM values unstable and inconsistent

### Tempo Prior Settings (Category: `tempoprior`)

**Settings now exposed via serial:**
```bash
show tempoprior
# priorenabled = on/off
# priorcenter = 60-200 BPM
# priorwidth = 10-100 BPM (sigma)
# priorstrength = 0-1 (blend factor)
```

### Parameter Sweep Results

| Setting | 60 BPM | 120 BPM | 180 BPM | Notes |
|---------|--------|---------|---------|-------|
| Prior OFF | 82.5 | 82.5 | ~60 | Severe half-time bias |
| center=120, width=40, str=0.5 | 82.2 | **121.3** | 95.5 | Best for 120 BPM |
| center=120, width=50, str=0.5 | 83.2 | **123.3** | ~95 | Balanced setting |
| center=120, width=60, str=0.5 | **60.8** | ~120 | ~95 | Best for 60 BPM |
| center=120, width=80, str=0.3 | - | 130.4 | 98.9 | Too weak |

### Optimal Settings (Saved to Flash)

```
priorenabled = on
priorcenter = 120
priorwidth = 50
priorstrength = 0.5
```

**Trade-offs:**
- **Narrow prior (width=40-50):** Best for typical music (80-160 BPM), biases slow music toward double-time
- **Wide prior (width=60+):** Better for slow music (60 BPM), slightly worse 120 BPM accuracy
- **180 BPM:** Fundamentally problematic - autocorrelation strongly prefers 90 BPM subharmonic

### Algorithm Insight: Harmonic Disambiguation

The tempo prior cannot fully solve harmonic disambiguation because:

1. At 60 BPM, the autocorrelation has a STRONGER peak at 120 BPM (every beat aligns with every other 120 BPM beat)
2. At 180 BPM, the autocorrelation has a STRONGER peak at 90 BPM (subharmonic)
3. The prior can only WEIGHT peaks, not create new ones

**Calculation at 60 BPM (width=50, strength=0.5):**
- 60 BPM prior weight: 1.0 + 0.5 × (exp(-0.5×(60-120)²/50²) - 1) = 0.75
- 120 BPM prior weight: 1.0 (at center)
- Even with 25% penalty on 120 BPM, if autocorrelation is 2× stronger there, 120 BPM still wins

**Future Improvements Needed:**
1. Harmonic relationship detection (if peak at N BPM exists, check N/2 and 2N)
2. Dynamic prior adjustment based on detected tempo confidence
3. Prefer hypothesis closest to prior center when multiple are equally strong

### Test Pattern Improvements

**Fixed:** Music mode patterns (`steady-*bpm`) now use `deterministicHit()` with alternating samples:
- Old: Random samples every hit → poor autocorrelation periodicity
- New: Alternating kick_hard_2/1, snare_hard_1/2 → consistent phrase structure

### Remaining Work

**Phase 3 (Beat Stability):** Tested but results inconclusive - stability metric is coupled to BPM accuracy. When BPM tracks incorrectly, stability becomes meaningless.

**Phases 4-8:** Deferred - require correct BPM tracking first. With tempo prior enabled, these can now be revisited.

---

## Test Session: 2026-01-04 (Transient Detection Tuning)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Firmware: v1.0.1 with CONFIG_VERSION 25 (new rhythm parameter persistence)
- Serial Port: COM11
- Hardware Gain: 40 (locked during tests)
- Test Patterns: steady-60bpm, steady-90bpm, steady-120bpm, steady-180bpm

**Objective:** Reduce false positive transient detections while maintaining recall

### Problem Identified

**Baseline Issue:** 9.4 transients/second detected in silence (noise floor)
- Root cause: AGC at max gain (80), amplifying noise floor
- Spectral detector threshold too low (1.4)
- Single-detector agreement boost too high (0.6)
- Ensemble cooldown too short (80ms)

### Parameters Tuned

| Parameter | Before | After | Rationale |
|-----------|--------|-------|-----------|
| `ensemble_cooldown` | 80 ms | 180 ms | Reduce rapid-fire detections |
| `ensemble_minconf` | 0.30 | 0.50 | Require higher confidence |
| `agree_1` | 0.60 | 0.30 | Suppress single-detector FPs |
| `spectral_thresh` | 1.4 | 2.5 | Reduce spectral sensitivity |
| `complex_thresh` | 2.0 | 2.5 | Reduce complex sensitivity |
| `drummer_thresh` | 2.5 | 3.0 | Reduce drummer sensitivity |
| `mel_thresh` | 2.5 | 3.0 | Reduce mel sensitivity |

### Results: Transient Detection

| Pattern | Before F1 | After F1 | Improvement |
|---------|-----------|----------|-------------|
| steady-60bpm | 0.181 | 0.335 | +85% |
| steady-90bpm | 0.259 | 0.471 | +82% |
| steady-120bpm | 0.343 | 0.603 | +76% |
| steady-180bpm | 0.456 | 0.769 | +69% |

**Average F1 improvement: +78%**

### Results: BPM Tracking

| Pattern | Expected | Before | After | Error Δ |
|---------|----------|--------|-------|---------|
| 60 BPM | 60 | 88.4 | 83.3 | +6% better |
| 90 BPM | 90 | 126.7 | 108.3 | +50% better |
| 120 BPM | 120 | 107.1 | 119.2 | **+92% better** |
| 180 BPM | 180 | 105.2 | 101.4 | +8% better |

**Key Finding:** 120 BPM now tracks with <1% error (119.2 detected).

### Remaining Limitations

**Extreme tempo bias:** 60 BPM and 180 BPM still show 30-45% error.
- Root cause: Autocorrelation naturally produces stronger peaks at subharmonics
- Tempo prior helps but cannot fully disambiguate
- Future work: Implement harmonic relationship detection algorithm

### Settings Saved

All tuned parameters saved to device flash. Optimal configuration:
```
ensemble_cooldown = 180
ensemble_minconf = 0.5
agree_1 = 0.3
spectral_thresh = 2.5
complex_thresh = 2.5
drummer_thresh = 3.0
mel_thresh = 3.0
priorstrength = 0.5
priorwidth = 50
priorcenter = 120
```

---

## Test Session: 2026-02-14

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense (hat_v1, 89 LEDs)
- Serial Port: COM14
- Hardware Gain: Locked at 40 during tests
- Tool: MCP `run_test` (blinky-serial-mcp)
- Firmware: 214,624 bytes flash (26%), 51,368 bytes RAM (21%)

### Algorithm Improvements Implemented

Four algorithm improvements were implemented and tested:

1. **Adaptive spectral whitening on mel bands** — Normalizes each mel band by its running maximum (decay=0.97, floor=0.01). Applied only to mel bands (not raw magnitudes) because HFC/ComplexDomain need raw magnitudes for absolute energy metrics. Change-based detectors (SpectralFlux, Novelty) benefit from normalization against sustained spectral content.

2. **SpectralFlux rewritten as mel-band SuperFlux** — Operates on 26 whitened mel bands instead of 128 raw FFT bins. Uses lag-2 comparison with 3-band max-filter for vibrato suppression. RAM reduced from ~600 to ~260 bytes.

3. **NoveltyDetector (cosine distance)** — Replaces MelFluxDetector. Measures spectral shape change independent of volume: `novelty = 1 - dot(prev,curr)/(|prev|*|curr|)`. Detects chord changes, instrument entries, timbral shifts.

4. **ComplexDomain phase wrapping fix** — Fixed circular phase prediction using `wrapPhase(prev - prevPrev)` instead of linear `2*prev - prevPrev` which broke at ±pi boundaries.

Additional changes:
- **Drummer minRiseRate (0.02)** — Rejects gradual swells that cross threshold slowly
- **HFC sustained-signal rejection (10 frames)** — Suppresses detections after 10+ consecutive elevated frames
- **Disabled detector CPU optimization** — `EnsembleDetector::detect()` skips disabled detectors entirely
- **Phase correction hardening** — transientCorrectionMin raised to 0.45, EMA alpha reduced to 0.15, requires detectorAgreement >= 2

### Phase 1: Baseline (HFC + Drummer, agree_1=0.3)

Initial baseline with all algorithm improvements but only HFC+Drummer enabled:

| Pattern | F1 | Precision | Recall | FP | FN |
|---------|-----|-----------|--------|-----|-----|
| strong-beats | 0.985 | 0.97 | 1.0 | 1 | 0 |
| pad-rejection | 0.640 | 0.47 | 1.0 | 9 | 0 |
| chord-rejection | 0.698 | 0.56 | 0.94 | 12 | 1 |
| synth-stabs | 1.000 | 1.0 | 1.0 | 0 | 0 |
| full-mix | 0.886 | 0.82 | 0.97 | 7 | 1 |
| sparse | 0.889 | 0.80 | 1.0 | 2 | 0 |
| lead-melody | 0.296 | 0.17 | 1.0 | 38 | 0 |

**Average F1: 0.771**

### Phase 2: ComplexDomain Threshold Sweep

Enabled ComplexDomain (weight=0.13) alongside HFC+Drummer:

| Threshold | strong-beats | pad-rejection | Notes |
|-----------|-------------|---------------|-------|
| 2.0 | 1.000 | 0.552 (13 FP) | Too many FPs on pads |
| 3.0 | 0.984 | 0.593 (11 FP) | Still too many |
| **5.0** | **1.000** | **0.762** (5 FP) | Best balance |

Full suite at threshold=5.0 (avg 0.769): Helps strong-beats and pad-rejection but hurts sparse (-0.047) and synth-stabs (-0.053).

**Verdict:** Marginal net benefit. Phase deviation detects pad chord changes, adding FPs on rejection patterns at lower thresholds.

### Phase 3: SpectralFlux (mel-band SuperFlux)

Enabled SpectralFlux (weight=0.20) alongside HFC+Drummer:

| Threshold | pad-rejection FPs | Notes |
|-----------|------------------|-------|
| 1.4 | 37 | Massive over-detection |
| 3.0 | 41 | Even worse |
| 5.0 | 37 (+ 2 FN) | Unusable at any threshold |

**Verdict: KEEP DISABLED.** SpectralFlux fires on chord changes in pads, which IS spectral flux. The mel-band whitening amplifies this by normalizing sustained content and making transitions appear as large relative spikes. Fundamentally incompatible with pad rejection.

### Phase 4: Novelty (Cosine Distance)

Enabled Novelty (weight=0.12) alongside HFC+Drummer:

| Threshold | strong-beats | pad-rejection | sparse |
|-----------|-------------|---------------|--------|
| 2.5 | 1.000 | 0.583 (9 FP) | — |
| 4.0 | 1.000 | 0.696 (7 FP) | — |
| **5.0** | 0.984 | **0.762** (5 FP) | 0.762 |

Full suite at threshold=5.0 (avg 0.748): Helps pad-rejection (+0.122) but hurts sparse (-0.127) and full-mix (-0.048).

**Verdict: KEEP DISABLED.** Net negative average F1. Same pad-rejection improvement as ComplexDomain but more collateral damage.

### Phase 5: Agreement Boost Tuning

Tested `agree_1` (single-detector suppression factor) with 2-detector config:

| agree_1 | pad-rejection | sparse | synth-stabs | Avg F1 |
|---------|--------------|--------|-------------|--------|
| 0.30 (baseline) | 0.640 | 0.889 | 1.000 | 0.771 |
| **0.20** | **0.727** | **0.941** | 0.974 | **0.785** |
| 0.15 | 0.667 | 0.889 | — | ~0.76 |

**Winner: agree_1=0.2** — Consistent improvement across the board. More aggressive single-detector suppression filters weak false positives without hurting 2-detector consensus hits.

Also tested minConfidence:
- 0.55 (current): avg 0.785
- 0.60: pad-rejection=0.800 but sparse=0.842 and full-mix=0.831 regressed
- **0.55 confirmed optimal** — better balance

### Phase 6: Final Validation

Config: HFC + Drummer only, agree_1=0.2, minconf=0.55, cooldown=250ms

| Pattern | Before (Jan 2026) | After (Feb 2026) | Delta |
|---------|-------------------|------------------|-------|
| strong-beats | 0.985 | **1.000** | +0.015 |
| pad-rejection | 0.640 | **0.696** | +0.056 |
| chord-rejection | 0.698 | 0.698 | 0 |
| synth-stabs | 1.000 | **1.000** | 0 |
| full-mix | 0.886 | **0.901** | +0.015 |
| sparse | 0.889 | 0.889 | 0 |
| lead-melody | 0.296 | 0.286 | -0.010 |
| **Average** | **0.771** | **0.781** | **+0.010** |

Note: pad-rejection varied 0.696–0.800 across runs due to ambient noise. The improvement is consistent but magnitude varies with environment.

### Key Findings

1. **agree_1=0.2 is the most impactful single change** — Improves average F1 from 0.771 to 0.785 with no code changes needed (parameter only)
2. **Additional detectors don't help the 2-detector config** — ComplexDomain, SpectralFlux, and Novelty all tested negative or neutral when added to HFC+Drummer
3. **SpectralFlux is fundamentally incompatible with pad rejection** — Fires on chord changes at all thresholds
4. **Spectral whitening on mel bands works correctly** — Initial implementation on raw magnitudes hurt HFC (whitening compressed dynamic range). Moving to mel bands only preserved HFC while enabling future change-based detectors
5. **lead-melody remains unsolvable** — HFC fires on every melody note (38-40 FPs). Would require a pitch-tracking gate or harmonic analysis to distinguish melody from transients
6. **Test variance is significant** — sparse and pad-rejection F1 vary ±0.1 across runs due to ambient noise, speaker positioning, and room reflections

### Settings Baked into Firmware Defaults

```
agree_1 = 0.2          (was 0.3)
ensemble_cooldown = 250 (was 180)
ensemble_minconf = 0.55 (was 0.50)
drummer_thresh = 3.5    (was 3.0)
hfc_thresh = 4.0        (was 3.0)
drummer_minRiseRate = 0.02  (new)
hfc_sustainRejectFrames = 10 (new)
```

All other detectors remain disabled with updated comments explaining why:
- SpectralFlux: fires on pad chord changes at all thresholds
- BassBand: susceptible to room rumble/HVAC
- ComplexDomain: adds FPs on sparse patterns
- Novelty: net negative avg F1, hurts sparse/full-mix

---

## Test Session: 2026-02-19 (CBSS Beat Tracking Evaluation)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM3
- Hardware Gain: AGC active (unlocked)
- Detector config: Drummer (0.50, thresh 4.5) + ComplexDomain (0.50, thresh 3.5)
- Tool: MCP `run_test` and `run_music_test`

### Architecture: CBSS Beat Tracking

Replaced multi-hypothesis phase accumulator (4 concurrent TempoHypotheses + PLL + phase fusion) with CBSS (Cumulative Beat Strength Signal) and counter-based beat detection. See `docs/AUDIO_ARCHITECTURE.md`.

Key properties:
- `CBSS[n] = (1-alpha)*OSS[n] + alpha*max(CBSS[n-2T : n-T/2])` — combines current onset with predicted beat
- Beat detection: search for local max in CBSS within prediction window `[T*(1-scale), T*(1+scale)]`
- Phase derived deterministically: `phase = (sampleCounter - lastBeatSample) / beatPeriodSamples`
- Default params: `cbssAlpha=0.9`, `beatWindowScale=0.5`, threshold multiplier `1.05x`

### Synthetic Pattern Results (Transient Detection)

| Pattern | Trans F1 | Prec | Recall | BPM | BPM Err | Conf | Beats |
|---------|----------|------|--------|-----|---------|------|-------|
| steady-120bpm | 0.864 | 0.792 | 0.950 | 121.6 | 1.3% | 0.18 | 42 |
| steady-90bpm | 0.940 | 0.907 | 0.975 | 90.5 | 0.6% | 0.11 | 35 |
| basic-drums | 0.800 | 0.737 | 0.875 | 121.7 | 1.4% | 0.21 | 12 |
| full-mix | 0.817 | 0.980 | 0.700 | 121.6 | 1.3% | 0.25 | 5 |

**Transient detection is solid.** BPM estimation is excellent (< 1.5% error).

### Real Music Results (Beat Tracking — Full-Length Tracks)

| Track | Duration | Trans F1 | Beat F1 | BPM Acc | Conf | Beat Rate | Expected |
|-------|----------|----------|---------|---------|------|-----------|----------|
| techno-minimal-emotion | 120s | 0.274 | 0.103 | 98.9% | 0.18 | 1.3/s | 1.9/s |
| trance-party | 108s | 0.675 | 0.323 | 99.3% | 0.23 | 0.8/s | 2.0/s |
| trance-infected-vibes | 91s | 0.445 | 0.103 | 97.9% | 0.28 | 0.4/s | 1.9/s |

**Beat tracking is fundamentally broken on real music** — F1 ranges 0.10–0.32 despite excellent BPM accuracy.

### Approaches Tested and Results

#### 1. Threshold Fix (1.5x → 1.05x) — Partial Improvement

Original threshold multiplier 1.5x was above the CBSS peak-to-mean ratio (~1.46x due to alpha blending), so no real beats were ever detected — all beats were forced at the late bound.

Lowering to 1.05x produced the best single-track result (techno-minimal-emotion Beat F1=0.757 on a ~30s segment) but full-length tracks still showed poor results.

#### 2. Ensemble Transient Input — Mixed Results

Fed ensemble-filtered `transientStrength` into CBSS instead of raw spectral flux. Rationale: cleaner signal at confirmed kick/snare positions.

| Track | Flux Input F1 | Ensemble Input F1 | Change |
|-------|--------------|-------------------|--------|
| trance-party | 0.292 | **0.457** | +56% |
| trance-infected-vibes | 0.099 | **0.527** | +432% |
| techno-dub-groove | 0.053 | 0.153 | +189% |
| techno-minimal-01 | 0.212 | 0.228 | +8% |
| techno-minimal-emotion | **0.757** | 0.236 | -69% |

Improved trance tracks but regressed techno-minimal-emotion. Sparse ensemble input creates sharp peaks but loses the continuous signal CBSS needs for propagation. Also, only works when transients land on every beat — fails for genres with syncopated or sparse beat patterns.

#### 3. Blend Input (flux + transient boost) — No Improvement

Combined `onsetStrength + 3.0 * transientStrength` to get continuous flux plus dominant peaks at ensemble detections. Also changed to "window peak" detection requiring 20% decline before declaring a beat.

Result: Beat F1=0.162 on techno-minimal-emotion. The 20% decline requirement added ~81ms latency, placing beats consistently late.

#### 4. Transient-Gated Detection — Wrong Approach

Abandoned CBSS peak detection entirely; accepted first ensemble transient within beat window as a beat. Simpler, zero latency.

**Rejected:** Only works for 4-on-the-floor EDM where every beat has a kick/snare. Fails for hip hop, DnB, funk, and any genre with syncopated beats.

### Root Cause Analysis

**BPM estimation works. Beat placement doesn't.** The disconnect is in the CBSS peak detection within the beat window:

1. **First-declining-frame detector is fragile** — Spectral flux has many micro-fluctuations, so the first local max in the beat window is often a noise peak early in the window. The real beat peak later in the window is missed.

2. **Consistent early placement** — 83ms median phase offset across tracks confirms beats are triggered by early noise peaks, not actual beat positions.

3. **Low beat event rate** — 0.4–1.3 beats/s detected vs 1.9–2.0/s expected. Many beat windows produce no detection above threshold, resulting in forced beats at wrong positions.

4. **CBSS peak-to-mean ratio is inherently small** — With alpha=0.9 blending, peaks are only ~5–15% above the running mean, making threshold-based detection unreliable.

### Confidence Never Builds

Across all tests, CBSS confidence stays 0.04–0.28 (target: >0.6). The +0.15 boost per beat is constantly eroded by 0.98x per-frame decay and 0.9x forced-beat penalty. With only ~50% of beats correctly detected, confidence can't accumulate.

### Conclusions

1. **CBSS with "first declining frame" detection is not viable** for real music beat tracking
2. **BPM estimation via autocorrelation is excellent** and should be preserved
3. **The peak selection algorithm needs a fundamentally different approach** — finding the window maximum, particle filtering, or a different architecture entirely
4. **Testing must include diverse genres** (hip hop, DnB, funk) not just 4-on-the-floor EDM
5. **Full-length tracks are essential** — short segments can give misleadingly good results

### Parameters Unchanged

CBSS defaults remain as-is pending architecture rework:
```
cbssalpha = 0.9
beatwindow = 0.5
beatconfdecay = 0.98
temposnap = 0.15
```

---

## Test Session: 2026-02-20 (BTrack-Style Predict+Countdown + Runtime Tuning)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense (bucket_v3, 128 LEDs)
- Serial Port: COM3
- Hardware Gain: AGC active (unlocked)
- Detector config: Drummer (0.50, thresh 4.5) + ComplexDomain (0.50, thresh 3.5)
- Tool: MCP `run_music_test`

### Architecture Changes Implemented

1. **ODF Pre-Smoothing** — Causal 5-point moving average on `onsetStrength` before CBSS/OSS buffer. Eliminates micro-fluctuations that caused early false triggers.

2. **Log-Gaussian Transition Weighting in CBSS** — Replaced flat `max()` backward search with BTrack-style log-Gaussian weighting centered at one beat period back. Tightness parameter `cbssTightness=5.0`.

3. **BTrack-Style Predict+Countdown Beat Detection** — Replaced "first declining CBSS frame" with:
   - At beat midpoint: synthesize future CBSS with zero onset, weight by Gaussian expectation
   - `timeToNextBeat = argmax(weighted_future_CBSS)` becomes countdown timer
   - Beat declared when countdown reaches zero (no threshold needed)

4. **Beat Timing Offset** — `beatTimingOffset=9` compensates for ODF smoothing delay + CBSS propagation delay by subtracting frames from prediction result.

5. **Harmonic Disambiguation** — Added half-lag (2x BPM) and 2/3-lag (3/2x BPM) checks to escape sub-harmonic locking.

6. **Runtime-Tunable Parameters** — 7 new parameters exposed via serial for faster tuning without reflash:
   - `temposmooth` (default 0.75) — BPM EMA blend factor
   - `odfsmooth` (default 5) — ODF window width (3-11)
   - `harmup2x` (default 0.5) — Half-lag harmonic fix threshold
   - `harmup32` (default 0.6) — 2/3-lag harmonic fix threshold
   - `peakmincorr` (default 0.3) — Min autocorrelation for peak acceptance
   - `beatoffset` (default 9.0) — Beat prediction advance (frames)
   - `phasecorr` (default 0.0) — Phase correction strength (DISABLED)

7. **Streaming Observability** — Added `cb` (CBSS value), `oss` (smoothed onset), `ttb` (time to next beat), `bp` (predicted flag) to music mode JSON.

### Phase Correction: Tested and Disabled

Phase correction (`phasecorr=0.3`) nudges `lastBeatSample_` toward nearby transients to correct BPM drift. **Net negative on real music:**

| Track | F1 without | F1 with phasecorr=0.3 | Delta |
|-------|-----------|----------------------|-------|
| trance-infected-vibes | 0.878 | 0.075 | -0.803 |
| techno-dub-groove | 0.352 | 0.191 | -0.161 |
| Average (6 tracks) | 0.496 | 0.396 | -0.100 |

**Root cause:** On syncopated tracks, transients near beat boundaries (but not on beats) nudge phase in wrong direction, causing BPM to drift to sub-harmonics. Default set to 0.0 (disabled).

### ODF Width: 5 Confirmed, 7 Rejected

ODF smooth width 7 caused major regression:

| Track | F1@ODF=5 | F1@ODF=7 | Delta |
|-------|----------|----------|-------|
| trance-infected-vibes | 0.878 | 0.021 | -0.857 |
| Average (6 tracks) | 0.496 | 0.372 | -0.124 |

**Root cause:** ODF=7 adds variable delay per track (17-150ms extra), destroying the calibrated `beatoffset=9`. Width 5 is the proven optimum.

### Tempo Smooth Factor: 0.75 Optimal

Tested `temposmooth` at 0.7, 0.75, and 0.8 via runtime serial tuning:

| Track | F1@0.7 | F1@0.75 | F1@0.8 |
|-------|--------|---------|--------|
| trance-party | **0.870** | 0.842 | 0.717 |
| trance-infected-vibes | 0.219 | **0.681** | 0.685 |
| Net delta vs 0.75 | -0.434 | baseline | -0.121 |

**Winner: temposmooth=0.75** — Only -0.028 loss on trance-party vs 0.7, but +0.462 gain on infected-vibes. Best compromise across all tracks.

### Harmonic Disambiguation: Confirmed Helpful

Tested with harmonic thresholds at 0.95 (effectively disabled) vs 0.5/0.6:

| Track | F1 (harmonics ON) | F1 (harmonics OFF) | Delta |
|-------|-------------------|-------------------|-------|
| trance-infected-vibes | 0.685 | 0.499 | -0.186 |

Harmonic checks prevent sub-harmonic locking on this track. Keep enabled.

### Full Suite Results (temposmooth=0.75, all other defaults)

| Track | Beat F1 | BPM Acc | Offset | CMLt | AMLt | Notes |
|-------|---------|---------|--------|------|------|-------|
| trance-party | **0.842** | 97.4% | +32ms | 0.836 | 0.882 | Excellent |
| techno-minimal-01 | **0.726** | 98.6% | -15ms | 0.726 | 0.730 | Good |
| trance-infected-vibes | **0.681** | 97.0% | +50ms | 0.700 | 0.733 | Good (was 0.103) |
| trance-goa-mantra | 0.647 | 99.5% | +31ms | — | — | Decent |
| techno-minimal-emotion | 0.526 | 98.5% | +42ms | — | — | Improved (was 0.103) |
| techno-dub-groove | 0.368 | 99.5% | -79ms | 0.367 | 0.612 | Early beats |
| edm-trap-electro | 0.189 | 95.9% | +12ms | 0.168 | 0.453 | Scattered |
| techno-deep-ambience | 0.134 | 99.9% | +100ms | — | — | Late, low conf |
| techno-machine-drum | 0.126 | 83.9% | +25ms | — | — | Wrong BPM |
| **Average** | **0.471** | **96.7%** | | | | **Was 0.10-0.32** |

### Improvement vs Previous CBSS (Feb 19)

| Metric | Before (Feb 19) | After (Feb 20) | Change |
|--------|-----------------|----------------|--------|
| Best Beat F1 | 0.323 | **0.842** | +161% |
| Avg Beat F1 (3 common tracks) | 0.176 | **0.683** | +288% |
| Worst Beat F1 | 0.103 | 0.126 | +22% |
| BPM Accuracy | 97-99% | 96-100% | Maintained |

### Track Categories

**Working well (F1 > 0.6):**
- trance-party, techno-minimal-01, trance-infected-vibes, trance-goa-mantra
- Common trait: strong kick drums, clear 4-on-the-floor or regular beat patterns

**Moderate (F1 0.3-0.6):**
- techno-minimal-emotion, techno-dub-groove
- Common trait: lighter kicks, more syncopation, beat offsets suggest timing calibration issue

**Failing (F1 < 0.2):**
- edm-trap-electro: scattered beat histogram (high IQR=318ms), BPM 4% off
- techno-deep-ambience: very late (+100ms), ambient sections confuse detection
- techno-machine-drum: BPM locked to 114.9 vs 143.6 expected (sub-harmonic)

### Remaining Issues

1. **techno-machine-drum sub-harmonic lock** — BPM 114.9 vs 143.6 (ratio 0.80x, not a clean harmonic). Tempo prior at 128 BPM center pulls toward the sub-harmonic. May need wider prior or specialized handling for >140 BPM tracks.

2. **techno-deep-ambience systematic late offset** — +100ms median suggests ambient/sparse sections with low onset energy delay the CBSS accumulation. The current `beatoffset=9` is tuned for tracks with strong transients.

3. **edm-trap-electro scattered histogram** — IQR=318ms means beats are essentially random placement. BPM slightly off (116.9 vs 112.3). Trap-style syncopation may fundamentally challenge 4-on-the-floor assumptions.

4. **techno-dub-groove early beats** — -79ms median offset suggests this track's beat character causes CBSS to peak early. Opposite problem from deep-ambience.

### Optimal Defaults (to be committed)

```
temposmooth = 0.75    (was 0.8, new parameter)
odfsmooth = 5         (new parameter, confirmed optimal)
harmup2x = 0.5        (new parameter)
harmup32 = 0.6        (new parameter)
peakmincorr = 0.3     (new parameter)
beatoffset = 9.0      (was 5.0, retuned)
phasecorr = 0.0       (disabled — hurts syncopated tracks)
cbsstight = 5.0       (new parameter)
cbssalpha = 0.9       (unchanged)
beatconfdecay = 0.98  (unchanged)
temposnap = 0.15      (unchanged)
```

---

## Test Session: 2026-02-20 (BandWeightedFluxDetector Evaluation)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense (hat_v1, 89 LEDs)
- Serial Port: COM14
- Hardware Gain: AGC active (unlocked)
- Tool: MCP `run_music_test`
- Firmware: 220,128 bytes flash, 51,128 bytes RAM (with new detector compiled in)

### New Detector: BandWeightedFluxDetector

Added as 7th detector in the ensemble (`BAND_FLUX = 6`, `DetectorType::COUNT = 7`). Disabled by default to preserve existing behavior.

**Algorithm:**
1. Log-compress FFT magnitudes: `log(1 + 20 * mag[k])` — equalizes quiet/loud events
2. 3-bin max-filter on reference frame (SuperFlux vibrato suppression)
3. Band-weighted half-wave rectified flux:
   - Bass bins 1-6 (62-375 Hz): weight 2.0 (kicks)
   - Mid bins 7-32 (437-2000 Hz): weight 1.5 (snares)
   - High bins 33-63 (2-4 kHz): weight 0.1 (suppress hi-hats)
4. **Additive threshold**: `mean + delta` (not `median * factor`) — works at low signal levels
5. Asymmetric threshold update: skip buffer update on detection frames
6. Hi-hat rejection gate: suppress when ONLY high band has flux

**Key design difference from Drummer:** Drummer uses multiplicative thresholds (`median * 4.5`), which at low signal levels (level=0.04) means threshold=0.18 — kicks at 0.10-0.15 never cross. BandFlux uses additive thresholds that work regardless of signal level.

### Configurations Tested

4 detector configurations tested across 9 real-music tracks (full-length, 90-132s each):

| Config | Detectors Enabled | Weights |
|--------|------------------|---------|
| **Baseline** | Drummer (0.50) + Complex (0.50) | agree_1=0.7 |
| **BandFlux Solo** | BandFlux only (thresh 0.5) | Single detector |
| **BandFlux+Drummer** | BandFlux + Drummer | Both 0.50, agree_1=0.7 |
| **All Three** | BandFlux + Drummer + Complex | All 0.50 |

### Full Results: Beat F1 Scores

| Track | Baseline (D+C) | BandFlux Solo | BandFlux+Drummer | All Three | Best |
|-------|:-:|:-:|:-:|:-:|:--|
| trance-party | 0.668 | **0.836** | 0.789 | 0.795 | BF Solo (+0.168) |
| trance-infected-vibes | 0.393 | **0.764** | 0.745 | 0.695 | BF Solo (+0.371) |
| techno-minimal-emotion | **0.700** | 0.451 | 0.512 | 0.522 | Baseline |
| trance-goa-mantra | 0.590 | 0.573 | 0.637 | **0.649** | All Three (+0.059) |
| techno-deep-ambience | 0.171 | **0.571** | 0.330 | 0.355 | BF Solo (+0.400) |
| techno-minimal-01 | 0.611 | 0.549 | **0.627** | 0.546 | BF+Drummer (+0.016) |
| techno-dub-groove | 0.170 | **0.188** | 0.180 | 0.111 | BF Solo (+0.018) |
| techno-machine-drum | **0.245** | 0.136 | 0.101 | 0.132 | Baseline |
| edm-trap-electro | 0.151 | 0.140 | **0.198** | *missing* | BF+Drummer (+0.047) |
| **Average** | **0.411** | **0.468** | **0.458** | **0.476*** | |

*All Three average is over 8 tracks (edm-trap-electro missing due to serial timeout). For fair comparison, 8-track averages: Baseline=0.444, BF Solo=0.509, BF+Drummer=0.490, All Three=0.476.

### Full Results: Transient F1 Scores

| Track | Baseline (D+C) | BandFlux Solo | BandFlux+Drummer | All Three |
|-------|:-:|:-:|:-:|:-:|
| trance-party | **0.569** | 0.352 | 0.546 | 0.467 |
| trance-infected-vibes | **0.552** | 0.327 | 0.276 | 0.311 |
| techno-minimal-emotion | **0.583** | 0.240 | 0.263 | 0.269 |
| trance-goa-mantra | 0.287 | **0.525** | 0.338 | 0.318 |
| techno-deep-ambience | **0.506** | 0.226 | 0.431 | 0.417 |
| techno-minimal-01 | 0.529 | 0.545 | **0.557** | 0.558 |
| techno-dub-groove | **0.330** | 0.289 | 0.338 | 0.333 |
| techno-machine-drum | 0.315 | 0.263 | **0.341** | 0.317 |
| edm-trap-electro | 0.231 | 0.249 | **0.270** | *missing* |

**Note:** Transient F1 is lower for BandFlux Solo because it generates more detections (higher recall, lower precision). However, Beat F1 improves because the CBSS beat tracker benefits from more onset signal — even imprecise detections feed the autocorrelation.

### BandFlux Threshold Tuning (trance-party only)

| Threshold | Beat F1 | Trans F1 | Notes |
|-----------|---------|----------|-------|
| **0.5** | **0.836** | 0.352 | Optimal for beat tracking |
| 0.7 | 0.405 | 0.670 | Higher precision but kills beat tracker input |
| 1.0 | 0.467 | 0.577 | Compromise, worse at both |

**Finding:** Low threshold (0.5) is critical. The CBSS beat tracker needs continuous onset signal for accumulation — precision matters less than having detections near beat positions. Higher thresholds reduce recall too much for beat tracking.

### BandFlux + Complex (3 tracks, from initial testing)

| Track | Beat F1 | Notes |
|-------|---------|-------|
| trance-party | 0.775 | Worse than BF Solo (0.836) |
| techno-minimal-01 | 0.412 | Worse than BF Solo (0.549) |
| techno-deep-ambience | 0.422 | Worse than BF Solo (0.571) |

**Verdict:** BandFlux+Complex is consistently worse than BandFlux Solo. Complex adds noise that degrades beat tracking.

### Key Findings

1. **BandFlux Solo is the best overall config** — Highest 8-track average Beat F1 (0.509 vs Baseline 0.444), with dramatic improvements on the tracks that were most broken (+0.400 deep-ambience, +0.371 infected-vibes, +0.168 trance-party).

2. **BandFlux's additive threshold solves the low-signal problem** — At speaker volumes where Drummer's multiplicative threshold (`median * 4.5`) produces thresholds above the signal level, BandFlux's `mean + delta` still detects kicks reliably.

3. **Log compression is the key innovation** — `log(1 + 20 * mag)` compresses the dynamic range so that quiet kicks in ambient tracks produce similar onset magnitudes to loud kicks in heavy techno. This is why deep-ambience improved by +0.400.

4. **Multi-detector configs don't help BandFlux** — Adding Drummer or Complex alongside BandFlux consistently degrades Beat F1. The ensemble fusion dilutes BandFlux's cleaner signal with noisier multiplicative-threshold detections.

5. **BandFlux hurts techno-minimal-emotion** (-0.249) — This track has strong, clear transients where Drummer already works well. BandFlux's higher sensitivity generates more false positives that confuse the CBSS phase prediction. The Baseline's 0.700 is the best F1 across all configs for this track.

6. **machine-drum remains broken across all configs** — Beat F1 0.10-0.25 regardless of detector. Root cause: BPM locked to ~114-121 vs 143.6 expected (sub-harmonic). This is a tempo estimation problem, not a detection problem.

7. **Transient F1 and Beat F1 are inversely correlated for BandFlux** — BandFlux Solo has the lowest transient F1 (more false positives) but the highest beat F1. This confirms that CBSS beat tracking benefits from high-recall onset signals even at the cost of precision.

### Track Category Analysis

**BandFlux dramatically improves (Beat F1 gain > 0.15):**
- trance-party: +0.168 (0.668 → 0.836)
- trance-infected-vibes: +0.371 (0.393 → 0.764)
- techno-deep-ambience: +0.400 (0.171 → 0.571)
- Common trait: moderate dynamics, complex timbres, Drummer's multiplicative threshold too high

**BandFlux roughly equivalent (|delta| < 0.1):**
- trance-goa-mantra: -0.017 (0.590 → 0.573)
- techno-minimal-01: -0.062 (0.611 → 0.549)
- techno-dub-groove: +0.018 (0.170 → 0.188)
- edm-trap-electro: -0.011 (0.151 → 0.140)

**BandFlux regresses (Beat F1 loss > 0.1):**
- techno-minimal-emotion: -0.249 (0.700 → 0.451)
- techno-machine-drum: -0.109 (0.245 → 0.136)
- Common trait: louder dynamics where Drummer already provides good onset signal; BandFlux's extra detections add noise

### Recommendations

1. **Enable BandFlux as default, disable Drummer** — Net +14% average Beat F1 improvement. The tracks that improve are the ones that were most broken before (ambient, trance). The regression on techno-minimal-emotion is real but it was already the best-performing track.

2. **Keep Complex disabled** — Adding Complex to BandFlux hurts in all tested configurations.

3. **BandFlux threshold 0.5 is optimal** — Do not raise above 0.5; higher thresholds kill recall needed for CBSS.

4. **Default gamma=20 is untested at other values** — Future work should sweep gamma (log compression strength) to see if the default is optimal.

5. **Band weights are untested** — Default bass=2.0, mid=1.5, high=0.1 are theoretical values from the design. Sweeping these (especially increasing bass weight for machine-drum-style tracks) may improve results.

### Parameters (Not Yet Changed in Firmware Defaults)

The BandFlux detector is compiled in and available via serial, but defaults remain unchanged (BandFlux disabled, Drummer+Complex enabled). To test BandFlux Solo:

```
set detector_enable drummer 0
set detector_enable complex 0
set detector_enable bandflux 1
```

BandFlux-specific parameters (all at design defaults):
```
bfgamma = 20.0      (log compression strength)
bfbassweight = 2.0  (bass band weight, bins 1-6)
bfmidweight = 1.5   (mid band weight, bins 7-32)
bfhighweight = 0.1  (high band weight, bins 33-63)
bfmaxbin = 64       (max FFT bin to analyze)
bandflux threshold = 0.5   (set via: set detector_thresh bandflux 0.5)
```

### Next Steps

1. ~~**Gamma sweep**~~ Done (Feb 21) — 20 confirmed optimal
2. ~~**Bass weight sweep**~~ Done (Feb 21) — 2.0 confirmed optimal
3. ~~**Threshold fine-tuning**~~ Done (Feb 21) — 0.5 confirmed optimal
4. ~~**Update firmware defaults**~~ Done (Feb 21) — BandFlux Solo enabled, all others disabled
5. **Agreement boost tuning** — If BandFlux + another detector proves useful, tune agree_1/agree_2 for that combo

---

## Test Session: 2026-02-21 (BandFlux Default + beatoffset Recalibration + Onset Delta Filter)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense (hat_v1, 89 LEDs)
- Serial Port: COM14
- Hardware Gain: AGC active (unlocked)
- Tool: MCP `run_music_test`

### Changes Made

1. **BandFlux set as firmware default** — All EnsembleFusion defaults updated: BandFlux enabled, all others disabled, agree_1=1.0
2. **beatoffset recalibrated 9 → 5** — With BandFlux (lower ODF latency than Drummer), the optimal offset shifted
3. **Parameter persistence** — 5 new params persisted to flash: tempoSmoothFactor, odfSmoothWidth, harmonicUp2xThresh, harmonicUp32Thresh, peakMinCorrelation (SETTINGS_VERSION 12)
4. **Onset delta filter** — New `minOnsetDelta` parameter (default 0.3) rejects slow-rising signals

### beatoffset Recalibration

With BandFlux Solo as default, the original beatoffset=9 (tuned for Drummer+Complex) was too large. BandFlux's log-compressed spectral flux has less processing latency than Drummer's broadband energy derivative.

| beatoffset | Avg Beat F1 | Notes |
|:----------:|:-----------:|-------|
| 9 (old) | 0.216 | Tuned for Drummer, way too large for BandFlux |
| **5** | **0.452** | +109% improvement, new default |

### BandFlux Parameter Sweep

Confirmed all defaults are optimal. Each parameter tested at 3 values on 9 real music tracks:

| Parameter | Tested Values | Optimal | Delta from Optimal |
|-----------|:------------:|:-------:|:------------------:|
| gamma | 10, **20**, 30 | 20 | 10: -0.06, 30: -0.01 |
| bassWeight | 1.5, **2.0**, 3.0 | 2.0 | 1.5: worse, 3.0: -0.02 |
| threshold | 0.3, **0.5**, 0.7 | 0.5 | 0.3: -0.03, 0.7: -0.15 |

### Onset Delta Filter (minOnsetDelta)

New post-detection filter requiring minimum frame-to-frame flux jump. Pads/swells rise slowly (0.01-0.1/frame), kicks spike instantly (1-3+/frame).

**Sweep results:**

| onsetDelta | pad-rejection FPs | medium-beats recall | trance-party Beat F1 | deep-ambience Beat F1 |
|:----------:|:-----------------:|:-------------------:|:--------------------:|:---------------------:|
| 0.0 (off) | 35 | 1.00 | 0.677 | 0.499 |
| 0.2 | ~22 | 1.00 | 0.637 | 0.618 |
| **0.3** | **16** | **1.00** | **0.775** | **0.404** |
| 0.5 | 16 | 1.00 | — | — |
| 0.7 | 15 | 0.97 | — | — |
| 1.0 | 5 | 0.88 | — | — |

**Decision:** 0.3 chosen as default — best overall average, preserves all medium-strength kicks, fixes synth-stabs regression (0.600→1.000).

### Final Results (BandFlux Solo, beatoffset=5, onsetDelta=0.3)

**Synthetic patterns:**

| Pattern | F1 | Notes |
|---------|:---:|-------|
| strong-beats | 1.000 | Perfect |
| hat-rejection | 1.000 | Perfect |
| synth-stabs | 1.000 | Fixed (was 0.600 before onset delta) |
| medium-beats | 0.985 | Near-perfect |
| pad-rejection | 0.421 | Improved (was 0.314, 35→16 FPs) |

**Real music (9 tracks):**

| Track | Beat F1 | BPM Acc | Beat Offset |
|-------|:-------:|:-------:|:-----------:|
| trance-party | 0.775 | 0.993 | -54ms |
| minimal-01 | 0.695 | 0.959 | -47ms |
| infected-vibes | 0.691 | 0.973 | -58ms |
| goa-mantra | 0.605 | 0.993 | +45ms |
| minimal-emotion | 0.486 | 0.998 | +57ms |
| deep-ambience | 0.404 | 0.949 | -18ms |
| machine-drum | 0.224 | 0.825 | -42ms |
| trap-electro | 0.190 | 0.924 | -23ms |
| dub-groove | 0.176 | 0.830 | +56ms |
| **Average** | **0.472** | **0.938** | |

### Key Findings

1. **beatoffset=5 is correct for BandFlux** — Different from the 9 tuned for Drummer. BandFlux's log-compressed spectral flux has ~4 fewer frames of latency.
2. **Onset delta filter at 0.3 is net positive** — Avg Beat F1 improved 0.452→0.472. Best improvements: trance-party +0.098, minimal-emotion +0.104. One regression: deep-ambience (0.499→0.404) from soft ambient onsets being filtered.
3. **Per-track beat offsets vary -58ms to +57ms** — A single fixed offset can't compensate for all tracks. Adaptive offset investigated but risks oscillation for marginal benefit. Accepted as ~70ms limitation.
4. **Sub-harmonic lock on machine-drum is unfixable by tuning** — Tested disabling tempo prior (BPM: 119.6→131.8, still wrong), shifting prior center to 128 (barely helped, hurt trance). Fundamental autocorrelation limitation.

---

## Test Session: 2026-02-23 (Feature Isolation Testing — IOI, FT, ODF Mean Sub)

**Environment:**
- Hardware: Seeeduino XIAO nRF52840 Sense
- Serial Port: COM3
- Hardware Gain: AGC active (unlocked)
- Detector config: BandFlux Solo (thresh 0.5, onsetDelta 0.3)
- Firmware: SETTINGS_VERSION 17 (IOI + Fourier Tempogram + ODF Mean Sub)
- Tool: MCP `run_music_test`

### Context

Three new tempo-estimation features implemented (SETTINGS_VERSION 17):
1. **IOI Histogram** (`ioi=1`) — onset inter-onset interval cross-validation, upward-only guard
2. **Fourier Tempogram** (`ft=1`) — Goertzel DFT of OSS buffer, sub-harmonic suppression
3. **ODF Mean Subtraction** (`odfmeansub=1`) — BTrack-style DC removal before autocorrelation

All enabled by default. Previous baseline (Feb 21, BandFlux Solo): avg Beat F1 = 0.472.

### Phase 1: All Features ON — 9 Tracks

**Result: REGRESSION — avg Beat F1 dropped 0.472 → 0.381 (hit abort condition < 0.40)**

| Track | Prev F1 | All ON F1 | Delta | Prev BPM | All ON BPM |
|-------|:-------:|:---------:|:-----:|:--------:|:----------:|
| trance-party | 0.775 | 0.432 | **-0.343** | 138 | 138.2 |
| minimal-01 | 0.695 | 0.658 | -0.037 | 125 | 125 |
| infected-vibes | 0.691 | 0.758 | +0.067 | 141 | 141 |
| goa-mantra | 0.571 | 0.577 | +0.006 | 138 | 137 |
| minimal-emotion | 0.550 | 0.472 | -0.078 | 125 | 125 |
| deep-ambience | 0.267 | 0.216 | -0.051 | 118 | 118 |
| machine-drum | 0.224 | 0.066 | **-0.158** | 118 | 161.7 |
| trap-electro | 0.190 | 0.034 | **-0.156** | 130 | 177.3 |
| dub-groove | 0.176 | 0.214 | +0.038 | 121 | 121 |
| **Average** | **0.472** | **0.381** | **-0.091** | | |

### Phase 2: Additive Isolation — 5 Diagnostic Tracks

Methodology: All features OFF as baseline, then enable ONE feature at a time.

**All OFF Baseline (ioi=0, ft=0, odfmeansub=0):**

| Track | Beat F1 | BPM | Notes |
|-------|:-------:|:---:|-------|
| trance-party | 0.753 | ~136 | Strong baseline |
| infected-vibes | 0.800 | ~143 | Best track |
| goa-mantra | 0.338 | ~136 | Phase drift |
| machine-drum | 0.178 | ~118 | Sub-harmonic lock |
| trap-electro | 0.238 | ~112 | Syncopated |

**Isolation Matrix (each feature tested alone):**

| Track | All OFF | IOI Only | FT Only | MeanSub Only | All ON |
|-------|:-------:|:--------:|:-------:|:------------:|:------:|
| trance-party | 0.753 | 0.794 (+) | 0.624 (**-**) | 0.741 (=) | 0.432 |
| machine-drum | 0.178 | 0.181 (=) | 0.343 (**+**) | 0.020 (**-**) | 0.066 |
| infected-vibes | 0.800 | 0.805 (=) | 0.876 (+) | 0.633 (**-**) | 0.758 |
| goa-mantra | 0.338 | 0.380 (+) | 0.181 (**-**) | 0.513 (**+**) | 0.577 |
| trap-electro | 0.238 | 0.172 (-) | 0.269 (+) | 0.009 (**-**) | 0.034 |

**Feature scorecard:**
- **IOI:** +4 tracks, -1 track. Improvements within variance (~±0.04). Safe but marginal.
- **FT:** +3 tracks, -2 tracks. Strong on machine-drum (+0.165) but destroys trance-party (-0.129) and goa-mantra (-0.157).
- **ODF Mean Sub:** +1 track, -3 tracks. Catastrophic on sparse tracks (machine-drum -0.158, trap-electro -0.229).

### Phase 3: FT Threshold Calibration Attempts

Tested 4 threshold combinations (all features OFF except FT):

| ftmagratio | ftcorr | machine-drum F1 | trance-party F1 | goa-mantra F1 |
|:----------:|:------:|:---------------:|:---------------:|:-------------:|
| 1.5 (default) | 0.15 (default) | 0.343 | 0.624 | 0.181 |
| 2.5 | 0.30 | — (too conservative, no FT corrections) | — | — |
| 2.0 | 0.20 | — (still too conservative) | — | — |
| 1.7 | 0.15 | ~0.30 (BPM 150) | — | worse |
| 1.5 | 0.30 | — | — | still bad |

**Conclusion:** No threshold combination fixes both machine-drum AND preserves trance-party/goa-mantra. FT corrects when autocorrelation is wrong AND when it's right — threshold can't distinguish these cases.

### Phase 4: FT Upward-Only Architectural Fix

Changed AudioController.cpp line 787: FT now only pushes BPM **upward** (>10% higher than autocorrelation). Rationale: sub-harmonic lock = BPM too low is the primary failure mode.

**Code change:**
```cpp
// Before: if (fabsf(ftPeakBpm - autoBpm) / autoBpm > 0.1f)
// After:  if (ftPeakBpm > autoBpm * 1.1f)
```

**Results (new firmware, FT upward-only, all other features OFF):**

| Track | All OFF Baseline | FT Upward-Only | Delta |
|-------|:----------------:|:--------------:|:-----:|
| trance-party | 0.652* | 0.611 | -0.041 |
| machine-drum | 0.178 | **0.300** | +0.122 |
| infected-vibes | 0.800 | 0.814 | +0.014 |
| goa-mantra | 0.338 | 0.151** | -0.187 |
| trap-electro | 0.238 | **0.300** | +0.062 |

*All-OFF baseline shifted with new firmware (0.753→0.652 for trance-party — run-to-run variance).
**goa-mantra: BPM correct at 136.0 but severe phase drift causing low F1. Needs investigation.

### Key Findings

1. **Features interact negatively** — Combined effect (0.381 avg) is worse than any individual feature or the baseline (0.472). This is not additive — features compete for BPM control.

2. **ODF Mean Subtraction is net destructive** — Helps only goa-mantra (+0.175), catastrophic on machine-drum (-0.158) and trap-electro (-0.229). Should be disabled or gated by onset density.

3. **FT upward-only is the best single change** — Preserves machine-drum improvement (+0.122) while eliminating most downward-correction regressions. goa-mantra phase drift remains an issue.

4. **IOI is noise-level** — All improvements within ±0.04, which is below run-to-run variance. Safe but contributes essentially nothing.

5. **Run-to-run variance is ~±0.1 F1** — Room acoustics in speaker-based tests create significant measurement noise. Small improvements (<0.05) cannot be reliably measured.

6. **No feature has been properly calibrated** — All tested at implementation defaults. No parameter sweeps performed for any feature.

7. **The system lacks a coordination mechanism** — Each feature independently modifies BPM. When they disagree, the result is worse than having none of them. Need a framework for feature cooperation rather than independent stacking.

---

## Multi-Device Bayesian Weight Sweep: 2026-02-25

**Environment:**
- Hardware: 3× Seeeduino XIAO nRF52840 Sense (Long Tube config)
- Serial Ports: /dev/ttyACM0, /dev/ttyACM1, /dev/ttyACM2
- Audio: USB speakers (JBL Pebbles) via ffplay, headless Raspberry Pi
- Tool: `param-tuner multi-sweep` (parallel sweep across 3 devices)
- Duration: 30 seconds per track
- Tracks: 9 real music files with ground truth beat annotations
- Scoring: Beat events (music.q=1), 200ms tolerance, ground truth filtered to played duration

### Methodology

Multi-device parallel sweep: 3 devices hear the same audio simultaneously, each configured with a different parameter value. This provides 3× speedup over single-device sweeps. Each parameter is swept independently while others remain at their defaults.

Scoring uses `scoreBeatEvents()` (not transient scoring): ground truth beat positions matched against firmware beat events (q=1) with 200ms tolerance. Audio latency estimated via median offset and corrected before matching.

### Parameters Tested

#### bayesacf (Autocorrelation observation weight)
- **Range Tested:** 0, 0.3, 0.5, 0.7, 1.0, 1.3, 1.5, 2.0
- **Previous Default:** 1.0
- **Independent Sweep Optimal:** 0 (disabled), F1 0.265
- **Final Validated Value:** 0.3 — independent sweep found 0 optimal, but combined 4-device validation showed bayesacf=0 causes half-time lock (F1 0.410). bayesacf=0.3 is optimal when combined with cbssthresh=1.0 (F1 0.519).
- **Applied to Firmware:** ✅ SETTINGS_VERSION 22

#### bayesft (Fourier tempogram observation weight)
- **Range Tested:** 0, 0.3, 0.5, 0.8, 1.0, 1.3, 1.5, 2.0
- **Previous Default:** 0.8
- **Optimal Value:** 0 (disabled)
- **Best F1:** 0.196
- **Applied to Firmware:** ✅ SETTINGS_VERSION 21

#### bayescomb (Comb filter bank observation weight)
- **Range Tested:** 0, 0.3, 0.5, 0.7, 1.0, 1.3, 1.5, 2.0
- **Previous Default:** 0.7
- **Optimal Value:** 0.7 (unchanged)
- **Best F1:** 0.203
- **Applied to Firmware:** ✅ (default unchanged)

#### bayesioi (IOI histogram observation weight)
- **Range Tested:** 0, 0.2, 0.5, 0.7, 1.0, 1.3, 1.5, 2.0
- **Previous Default:** 0.5
- **Optimal Value:** 0 (disabled)
- **Best F1:** 0.166
- **Applied to Firmware:** ✅ SETTINGS_VERSION 21

#### bayeslambda (Transition tightness)
- **Range Tested:** 0.02, 0.05, 0.1, 0.15, 0.2, 0.3, 0.5
- **Previous Default:** 0.1
- **Optimal Value:** 0.15
- **Best F1:** 0.186
- **Applied to Firmware:** ✅ SETTINGS_VERSION 21

#### cbssthresh (CBSS adaptive threshold factor)
- **Range Tested:** 0, 0.2, 0.3, 0.4, 0.5, 0.7, 1.0
- **Previous Default:** 0.4
- **Optimal Value:** 1.0
- **Best F1:** 0.590
- **Applied to Firmware:** ✅ SETTINGS_VERSION 21

### Summary of Default Changes (SETTINGS_VERSION 20 → 22)

| Parameter | Old Default | New Default | Change |
|-----------|:-----------:|:-----------:|--------|
| bayesacf | 1.0 | **0.3** | Reduced — independent sweep found 0 optimal but combined validation showed half-time lock; 0.3 optimal with cbssthresh=1.0 (v22) |
| bayesft | 0.8 | **0** | Disabled — mean-norm kills discriminability |
| bayesioi | 0.5 | **0** | Disabled — unnormalized counts dominate posterior |
| bayescomb | 0.7 | 0.7 | Unchanged |
| bayeslambda | 0.1 | **0.15** | Slightly wider tempo transitions |
| cbssthresh | 0.4 | **1.0** | Higher threshold = fewer false beats = more stable BPM |

### Key Findings

1. **CBSS threshold is the biggest single improvement** — F1 jumps from 0.209 (at default 0.4) to 0.590 (at 1.0). Higher threshold prevents phantom beats during low-energy sections, keeping BPM stable, which paradoxically improves recall (0.509 vs ~0.08).

2. **Three of four observation signals hurt beat tracking** — ACF, FT, and IOI all score optimal at 0 (disabled). Only the comb filter bank contributes positively.

3. **Root cause analysis (compared to BTrack, madmom, librosa):**
   - **ACF:** Missing inverse-lag normalization (BTrack divides `acf[i]/lag`). Sub-harmonic peaks dominate. BTrack processes ACF through a 4-harmonic comb filter bank before Bayesian inference — our raw ACF is too noisy.
   - **FT:** Mean normalization across bins makes all observations ≈1.0 (near-flat). Uses magnitude-squared (amplifies noise). No reference implementation uses Fourier tempogram for real-time beat tracking.
   - **IOI:** Raw integer counts (1-10+ range) dominate the multiplicative posterior. 2x folding biases toward fast tempos. No reference implementation uses IOI histograms for polyphonic beat tracking.
   - **Comb:** Works because it's closest to BTrack/Scheirer — continuous resonance, phase-sensitive, self-normalizing.

4. **Architectural mismatch:** Our multiplicative fusion (`posterior = prediction × ACF × FT × comb × IOI`) assumes independent, comparably-scaled signals. They are neither — they share the same input (OSS buffer) and have different scales and biases. BTrack uses a pipeline (ACF → comb → Rayleigh → Viterbi), not multiplicative fusion.

5. **These are independent sweeps** — interaction effects between parameters were not tested. The combined effect of all new defaults together should be validated.

### Raw Results

Full per-track, per-value results saved to: `tuning-results/multi-sweep-results.json`

---

## Test Session: 2026-02-26 (Post-Spectral Bayesian Re-Tuning)

**Environment:**
- Hardware: 4x Seeeduino XIAO nRF52840 Sense (ACM0-3)
- Firmware: v23 (spectral processing: adaptive whitening + soft-knee compressor)
- Test Tracks: 9 EDM tracks (full duration, ~100-130s each)
- Tool: multi-sweep (independent parameter sweeps) + run_music_test (combined validation)

**Context:** Spectral processing (v23) normalizes FFT magnitudes, potentially fixing previously-broken Bayesian observation signals (FT and IOI were disabled in v21 due to scale/normalization issues).

### Independent Sweep Results (4-device, 30s/track)

| Parameter | Optimal | Previous | F1 at Optimal |
|-----------|:-------:|:--------:|:-------------:|
| bayesacf | 0.3 | 0.3 | 0.349 |
| bayesft | **2.0** | 0 (disabled) | **0.544** |
| bayescomb | 0.7 | 0.7 | 0.366 |
| bayesioi | **2.0** | 0 (disabled) | **0.597** |
| bayeslambda | 0.15 | 0.15 | 0.350 |
| cbssthresh | 0.7 | 1.0 | 0.412 |

**Key finding:** Spectral processing FIXED the FT and IOI signals. Both now score optimal at 2.0 (strong weight) instead of 0 (disabled).

### Combined Validation (4-device, full duration, 2 samples per config)

Tested 4 configurations to isolate interaction effects:

| Config | bayesft | bayesioi | cbssthresh | Avg Beat F1 |
|--------|:-------:|:--------:|:----------:|:-----------:|
| A (all proposed) | 2.0 | 2.0 | 0.7 | 0.049 |
| **C (FT+IOI only)** | **2.0** | **2.0** | **1.0** | **0.158** |
| D (thresh only) | 0.0 | 0.0 | 0.7 | 0.140 |
| B (control) | 0.0 | 0.0 | 1.0 | 0.106 |

**Critical interaction effect:** cbssthresh=0.7 + FT/IOI = disaster (F1=0.049). The lower threshold allows too many phantom beats that overwhelm the improved tempo estimation. cbssthresh must stay at 1.0.

### Per-Track Results (Config C vs Control, full duration, averaged over 2 devices)

| Track | Config C (F1) | Control (F1) | Delta |
|-------|:-------------:|:------------:|:-----:|
| edm-trap-electro | 0.005 | 0.005 | 0.000 |
| techno-deep-ambience | 0.141 | 0.059 | +0.082 |
| techno-dub-groove | 0.051 | 0.005 | +0.046 |
| techno-machine-drum | 0.000 | 0.025 | -0.025 |
| techno-minimal-01 | 0.225 | 0.209 | +0.016 |
| techno-minimal-emotion | 0.185 | 0.117 | +0.068 |
| trance-goa-mantra | 0.286 | 0.220 | +0.066 |
| trance-infected-vibes | 0.387 | 0.310 | +0.077 |
| trance-party | 0.142 | 0.007 | +0.135 |
| **Average** | **0.158** | **0.106** | **+0.052 (+49%)** |

### Summary of Default Changes (SETTINGS_VERSION 23 → 24)

| Parameter | Old Default | New Default | Change |
|-----------|:-----------:|:-----------:|--------|
| bayesft | 0 | **2.0** | Re-enabled — spectral whitening fixes discriminability |
| bayesioi | 0 | **2.0** | Re-enabled — compressor normalizes count scale |
| cbssthresh | 1.0 | 1.0 | Unchanged — lower values cause phantom beats |

- **Applied to Firmware:** ✅ SETTINGS_VERSION 24

### Key Findings

1. **Spectral processing fixed two broken signals** — FT and IOI were disabled in v21 because raw FFT magnitudes had scale/normalization problems. Adaptive whitening (per-bin peak normalization) and soft-knee compression (frame-level gain normalization) fixed both issues.

2. **FT was broken by mean normalization** — mean-normalizing across Fourier tempogram bins made all values ≈1.0. After spectral whitening, the magnitudes entering the tempogram are already normalized, so the existing mean-norm produces meaningful variation.

3. **IOI was broken by raw counts** — IOI histogram counts (1-10+) had wildly different scales from other observations (0-1 range). The compressor normalizes the ODF that feeds IOI, producing more consistent count distributions.

4. **cbssthresh=1.0 is essential** — even with better tempo estimation, lowering the beat threshold causes phantom beats during quiet sections. The interaction between threshold and FT/IOI is strongly negative.

5. **Independent sweeps miss interactions** — cbssthresh swept to 0.7 optimal independently, but combined with FT/IOI it's catastrophic (F1 drops from 0.158 to 0.049). Always validate combined defaults.

### Raw Results

Full sweep results: `tuning-results/post-spectral/multi-sweep-results.json`

---

## Test Session: 2026-02-28 — v31→v32 Octave Disambiguation Experiments

**Environment:**
- Hardware: 4× Seeeduino XIAO nRF52840 Sense (3 Long Tube, 1 Tube Light)
- Ports: /dev/ttyACM0-3 (Linux, Raspberry Pi)
- Audio: USB speakers (JBL Pebbles), 30s clips, `run_music_test_multi`
- Starting firmware: SETTINGS_VERSION 31 (all new features OFF by default)
- 9 core tracks: trance-party, minimal-01, infected-vibes, goa-mantra, minimal-emotion, deep-ambience, machine-drum, trap-electro, dub-groove

**Goal:** Improve Beat F1 from ~0.148 baseline via BTrack-inspired algorithms.

### Phase 0: Baseline (all defaults)
- Avg best-device Beat F1: **0.148** across 9 tracks
- Consistent with known acoustic degradation vs historical v22 baseline (0.519)

### Phase 1: ODF Mean Subtraction (A/B)

| Config | Avg Beat F1 | vs Baseline |
|--------|:-----------:|:-----------:|
| ACM0: defaults (adaptodf=0, odfmeansub=1) | 0.111 | — |
| ACM1: adaptodf=1, odfmeansub=1 | 0.144 | +30% |
| ACM2: adaptodf=1, odfmeansub=0 | 0.154 | +39% |
| **ACM3: adaptodf=0, odfmeansub=0 (raw ODF)** | **0.252** | **+127%** |

**Key finding:** Disabling global ODF mean subtraction was the major win (+70% vs odfmeansub=1). The adaptive local threshold (adaptodf) showed marginal benefit. Raw ODF preserves natural ACF peak structure.

**Winner:** `odfmeansub=0`

### Phase 2: Onset-Density Octave Discriminator (A/B)

Locked: `odfmeansub=0`

| Config | Avg Beat F1 | vs Control |
|--------|:-----------:|:----------:|
| ACM0: control (odfmeansub=0 only) | 0.239 | — |
| **ACM1: + densityoctave=1, min=0.5, max=5.0** | **0.270** | **+13%** |
| ACM2: + densityoctave=1, min=0.3, max=3.0 | 0.250 | +5% |
| ACM3: + densityoctave=1, min=1.0, max=8.0 | 0.239 | +0% |

Notable per-track: infected-vibes 0.276→0.389 (+41%), trance-party 0.142→0.266 (+87%), dub-groove 0.269→0.365 (+36%).

**Winner:** `densityoctave=1, densityminpb=0.5, densitymaxpb=5.0`

### Phase 3: Shadow CBSS Octave Checker (A/B)

Locked: `odfmeansub=0`, `densityoctave=1` (0.5-5.0)

| Config | Avg Beat F1 | vs Control |
|--------|:-----------:|:----------:|
| ACM0: control (Phase 1+2 winners) | 0.233 | — |
| ACM1: + octavecheck=1, beats=4, ratio=1.5 | 0.250 | +7% |
| **ACM2: + octavecheck=1, beats=2, ratio=1.3** | **0.263** | **+13%** |
| ACM3: + octavecheck=1, beats=8, ratio=2.0 | 0.257 | +10% |

ACM2 consistently achieved best BPM accuracy: deep-ambience 0.968, trap-electro 0.981, dub-groove 0.844. The aggressive octave checker breaks the double-time lock.

**Winner:** `octavecheck=1, octavecheckbeats=2, octavescoreratio=1.3`

### Phase 4: Hi-Res Bass Toggle (A/B)

Locked: all Phase 1-3 winners

| Config | Avg Beat F1 |
|--------|:-----------:|
| **bfhiresbass=0 (off)** | **0.255** |
| bfhiresbass=1 (on) | 0.233 |

Same-device paired comparison (ACM0 vs ACM1): 0.253 vs 0.231. No hiresbass wins 6/9 tracks.

**Winner:** `bfhiresbass=0` (no change needed)

### Compound Validation (all 4 devices, best combined config)

Config: `odfmeansub=0, densityoctave=1, densityminpb=0.5, densitymaxpb=5.0, octavecheck=1, octavecheckbeats=2, octavescoreratio=1.3`

| Track | ACM0 | ACM1 | ACM2 | ACM3 | Avg | Best |
|-------|:----:|:----:|:----:|:----:|:---:|:----:|
| trance-party | 0.222 | 0.210 | 0.263 | 0.315 | 0.253 | 0.315 |
| minimal-01 | 0.220 | 0.206 | 0.224 | 0.157 | 0.202 | 0.224 |
| infected-vibes | 0.281 | 0.325 | 0.235 | 0.283 | 0.281 | 0.325 |
| goa-mantra | 0.183 | 0.212 | 0.143 | 0.218 | 0.189 | 0.218 |
| minimal-emotion | 0.283 | 0.243 | 0.330 | 0.317 | 0.293 | 0.330 |
| deep-ambience | 0.241 | 0.213 | 0.212 | 0.235 | 0.225 | 0.241 |
| machine-drum | 0.291 | 0.180 | 0.244 | 0.288 | 0.251 | 0.291 |
| trap-electro | 0.208 | 0.280 | 0.336 | 0.254 | 0.270 | 0.336 |
| dub-groove | 0.283 | 0.367 | 0.189 | 0.270 | 0.277 | 0.367 |
| **Overall** | **0.246** | **0.248** | **0.242** | **0.260** | **0.249** | **0.294** |

### Summary

| Metric | Baseline (v31 defaults) | Best Combined (v32) | Improvement |
|--------|:-----------------------:|:-------------------:|:-----------:|
| 4-device avg Beat F1 | 0.148 | 0.249 | +68% |
| Best-device avg Beat F1 | 0.148 | 0.294 | +99% |

**New SETTINGS_VERSION 32 defaults:**
- `odfmeansub=0` (was true) — raw ODF preserves natural ACF structure
- `densityoctave=1` (was false) — onset-density octave penalty
- `octavecheck=1` (was false) — shadow CBSS octave checker
- `octavecheckbeats=2` (was 4) — aggressive checking
- `octavescoreratio=1.3` (was 1.5) — lower switch threshold
- `adaptodf=0` (unchanged) — local threshold not needed
- `bfhiresbass=0` (unchanged) — hi-res bass hurts

**Remaining bottleneck:** Double-time lock at ~182 BPM persists on many tracks/devices. The density penalty and octave checker mitigate but don't eliminate it. Fundamental octave disambiguation remains the primary gap vs BTrack.

### Full 18-Track Validation (v32 firmware, full-length tracks, all devices at defaults)

| # | Track | BPM | ACM0 | ACM1 | ACM2 | ACM3 | Avg | Best |
|:-:|-------|:---:|:----:|:----:|:----:|:----:|:---:|:----:|
| 1 | trance-party | 136 | 0.271 | 0.279 | 0.275 | 0.272 | 0.274 | 0.279 |
| 2 | minimal-01 | 129 | 0.213 | 0.243 | 0.343 | 0.185 | 0.246 | 0.343 |
| 3 | infected-vibes | 144 | 0.338 | 0.298 | 0.270 | 0.247 | 0.288 | 0.338 |
| 4 | goa-mantra | 136 | 0.312 | 0.381 | 0.230 | 0.370 | 0.323 | 0.381 |
| 5 | minimal-emotion | 129 | 0.237 | 0.293 | 0.220 | 0.225 | 0.244 | 0.293 |
| 6 | deep-ambience | 123 | 0.247 | 0.258 | 0.305 | 0.282 | 0.273 | 0.305 |
| 7 | machine-drum | 144 | 0.328 | 0.310 | 0.248 | 0.355 | 0.310 | 0.355 |
| 8 | trap-electro | 112 | 0.234 | 0.255 | 0.239 | 0.260 | 0.247 | 0.260 |
| 9 | dub-groove | 123 | 0.299 | 0.319 | 0.251 | 0.311 | 0.295 | 0.319 |
| 10 | afrobeat-feelgood | 118 | 0.288 | 0.272 | 0.276 | 0.268 | 0.276 | 0.288 |
| 11 | amapiano-vibez | 112 | 0.250 | 0.255 | 0.242 | 0.245 | 0.248 | 0.255 |
| 12 | breakbeat-bg | 86 | 0.313 | 0.284 | 0.174 | 0.366 | 0.284 | 0.366 |
| 13 | breakbeat-drive | 96 | 0.209 | 0.206 | 0.194 | 0.197 | 0.202 | 0.209 |
| 14 | dnb-energetic | 118 | 0.298 | 0.286 | 0.308 | 0.256 | 0.287 | 0.308 |
| 15 | dnb-liquid-jungle | 112 | 0.255 | 0.240 | 0.232 | 0.284 | 0.253 | 0.284 |
| 16 | dubstep-halftime | 118 | 0.278 | 0.306 | 0.366 | 0.317 | 0.317 | 0.366 |
| 17 | garage-uk-2step | 129 | 0.301 | 0.305 | 0.253 | 0.306 | 0.291 | 0.306 |
| 18 | reggaeton | 92 | 0.057 | 0.123 | 0.178 | 0.123 | 0.120 | 0.178 |
| | **Overall** | | **0.265** | **0.273** | **0.256** | **0.271** | **0.265** | **0.302** |

| Metric | Core 9 | Extended 9 | All 18 |
|--------|:------:|:----------:|:------:|
| 4-device avg | 0.278 | 0.253 | 0.265 |
| Best-device avg | 0.319 | 0.284 | 0.302 |

**Observations:**
- ACM2 (Tube Light, different acoustic position) consistently gets best BPM accuracy but not always best Beat F1
- Double-time lock at ~182 BPM remains dominant failure mode on ACM0/ACM1/ACM3
- Worst: reggaeton (92 BPM, below density discriminator range) — F1 0.120
- Best: goa-mantra (0.381), breakbeat-bg (0.366), dubstep-halftime (0.366), machine-drum (0.355)
- Extended tracks perform ~10% worse than core 9, mainly due to breakbeat/reggaeton genres with BPMs outside 110-150 range
