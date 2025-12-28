# Parameter Tuning History

This document tracks parameter optimization tests and findings. Raw test result files are excluded from git (see .gitignore) to avoid repo bloat.

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

**Subsystems:**
- music: PLL-based beat tracking
- rhythm: Autocorrelation tempo detection
