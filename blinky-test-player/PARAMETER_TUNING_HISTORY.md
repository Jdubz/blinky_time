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
