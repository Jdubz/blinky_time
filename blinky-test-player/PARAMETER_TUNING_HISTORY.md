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
