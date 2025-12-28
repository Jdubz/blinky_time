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

## Test Session: 2025-12-28 (Different Machine)

**Status:** Exhaustive tests run, results pending documentation

**Parameters Tested:** TBD

**Findings:** TBD

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
