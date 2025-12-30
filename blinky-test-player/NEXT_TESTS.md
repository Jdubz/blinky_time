# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive 2-3 hour test plan.

Based on fast-tune analysis (2025-12-28), this document outlines the next testing priorities to address performance gaps and boundary conditions.

## Critical Findings from Fast-Tune

### ✅ Well-Performing Modes
- **Spectral Mode:** F1=0.670 (best overall, balanced)
- **Hybrid Mode:** F1=0.669 (nearly equal, robust)
- **Drummer Mode:** F1=0.664 (high precision, lower recall)

### ⚠️ Problem Patterns Identified

| Pattern | Mode | F1 | Issue |
|---------|------|-----|-------|
| simultaneous | Drummer | 0.400 | 75% missed - overlapping sounds |
| simultaneous | Spectral | 0.578 | 59% missed - overlapping sounds |
| fast-tempo | Drummer | 0.490 | 67% missed - rapid hits |
| sparse | Spectral | 0.526 | 54% precision - false positives |
| sparse | Hybrid | 0.500 | 35% precision - false positives |
| pad-rejection | Spectral | 0.516 | 35% precision - sustained tones |

### 🔴 Parameters at Boundaries

Three parameters hit or approached their minimum bounds during fast-tune:

1. **attackmult: 1.1** (AT minimum 1.1) ⚠️ CRITICAL
   - Optimal value is exactly at the boundary
   - May perform better below 1.1 (1.0, 1.05)

2. **hitthresh: 1.688** (near minimum 1.5) ⚠️ HIGH PRIORITY
   - Only 0.188 away from boundary
   - Binary search stopped here, may improve below

3. **fluxthresh: 1.4** (0.4 above minimum 1.0) ⚠️ MEDIUM PRIORITY
   - Reasonably far from boundary
   - But lower values might improve recall

**Action Taken:** Extended all three parameter ranges to allow testing below previous minimums.

## Priority 1: Extended Boundary Testing (15 min)

**Goal:** Confirm optimal values aren't below previous minimums

**Parameters to Test:**
```bash
npm run tuner -- fast --port COM41 --gain 40 \
  --params attackmult,hitthresh,fluxthresh \
  --patterns strong-beats,bass-line,synth-stabs
```

**New Ranges (already updated in types.ts):**
- attackmult: 1.0, 1.05, 1.1, 1.2 (was min: 1.1, now: 1.0)
- hitthresh: 1.0, 1.2, 1.4, 1.5, 1.688 (was min: 1.5, now: 1.0)
- fluxthresh: 0.5, 0.8, 1.0, 1.2, 1.4 (was min: 1.0, now: 0.5)

**Expected Outcome:**
- If current values are optimal: No change, ranges confirmed
- If lower values are better: Update firmware and retest
- If we hit new boundaries: Extend ranges further

**Success Criteria:**
- Optimal values are 0.2+ away from boundaries (not at edges)

## Priority 2: Fast-Tempo Optimization (20 min)

**Goal:** Improve drummer mode recall on fast-tempo patterns (currently 33%)

**Problem:** Drummer misses 67% of fast hits (24 FN out of 36 expected)

**Hypotheses:**
1. Cooldown too long (40ms = max 25 hits/sec, fast-tempo may exceed this)
2. hitthresh too high for quieter fast hits
3. Attack detection not fast enough

**Test 1: Cooldown Sweep**
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params cooldown \
  --modes drummer \
  --patterns fast-tempo
```

Test values: 20, 25, 30, 35, 40, 50ms

**Test 2: Threshold for Fast Patterns**

If cooldown doesn't help, test lower thresholds specifically for fast-tempo:
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params hitthresh \
  --modes drummer \
  --patterns fast-tempo,simultaneous
```

**Success Criteria:**
- Drummer mode fast-tempo: F1 > 0.6 (currently 0.490)
- Maintain precision > 80%

## Priority 3: Simultaneous Detection (30 min)

**Goal:** Improve all modes on simultaneous overlapping sounds

**Current Performance:**
- Drummer: F1=0.400 (75% missed)
- Spectral: F1=0.578 (59% missed)
- Hybrid: F1=0.640 (50% missed) - BEST

**Problem:** When kick + snare + hat hit simultaneously, system detects as single event

**Test Strategy:**

1. **Hybrid Mode Optimization** (best baseline):
```bash
npm run tuner -- fast --port COM41 --gain 40 \
  --modes hybrid \
  --patterns simultaneous
```

2. **Spectral Bins Test** - More bins = better frequency resolution:
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params fluxbins \
  --modes spectral \
  --patterns simultaneous
```

Test values: 32, 64, 96, 128 (hypothesis: more bins helps separate overlapping frequencies)

**Success Criteria:**
- Any mode: F1 > 0.7 on simultaneous pattern
- Or: Identify as algorithmic limitation requiring code changes

**If Parameter Tuning Doesn't Help:**
- May need algorithm enhancement (multi-band detection, peak picking in FFT)
- Document as known limitation

## Priority 4: False Positive Reduction (15 min)

**Goal:** Reduce false positives on sparse and pad-rejection patterns

**Problem:**
- Spectral on sparse: 54% precision (6 FP on 5 TP)
- Hybrid on sparse: 35% precision (13 FP on 7 TP)
- Spectral on pad-rejection: 35% precision (15 FP on 8 TP)

**Hypothesis:** Sustained pad sounds create spectral flux that triggers detection

**Test:** Increase fluxthresh to reduce sensitivity:
```bash
npm run tuner -- sweep --port COM41 --gain 40 \
  --params fluxthresh \
  --modes spectral,hybrid \
  --patterns sparse,pad-rejection,chord-rejection
```

Test values: 1.4, 1.6, 1.8, 2.0, 2.2, 2.5

**Success Criteria:**
- Spectral/Hybrid on pad-rejection: Precision > 70% (currently 35-47%)
- Maintain recall > 80% on transient-heavy patterns

**Trade-off Analysis:**
- Higher threshold = fewer false positives on sustained tones
- But may reduce recall on weak transients
- Need to find balance

## Priority 5: Rhythm Tracking Parameters (Background)

> **Note:** Architecture changed in December 2025. MusicMode PLL and RhythmAnalyzer
> were replaced by AudioController with autocorrelation-based rhythm tracking.

**AudioController Parameters** (new architecture) - Not yet tuned:
- `musicthresh` (0.0-1.0): Rhythm activation threshold
- `phaseadapt` (0.01-1.0): Phase adaptation rate
- `bpmmin` (40-120): Minimum BPM to detect
- `bpmmax` (80-240): Maximum BPM to detect
- `pulseboost` (1.0-2.0): On-beat pulse enhancement
- `pulsesuppress` (0.3-1.0): Off-beat pulse suppression
- `energyboost` (0.0-1.0): On-beat energy enhancement

**REMOVED Parameters** (no longer exist):
- ~~pllkp, pllki~~ (PLL removed)
- ~~confinc, confdec, misspenalty~~ (PLL removed)
- ~~combdecay, combfb, combconf~~ (merged into autocorrelation)

**Bass/HFC Modes** - Not included in fast-tune:
- bassfreq, bassq, bassthresh
- hfcweight, hfcthresh

**Recommendation:** See [AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) Phase 3 for rhythm tracking tests

## Priority 6: Cross-Pattern Validation

**Goal:** Ensure optimized parameters work across ALL patterns, not just test subset

**Full Test Suite:**
```bash
npm run tuner -- validate --port COM41 --gain 40
```

This will run all modes on all 18 patterns and generate comprehensive report.

**Success Criteria:**
- No pattern has F1 < 0.5 in its best-suited mode
- Average F1 across all patterns > 0.65
- No significant regressions from fast-tune results

## Testing Schedule Recommendation

**Session 1: Boundary + Fast-Tempo (35 min)**
1. Extended boundary testing (15 min)
2. Fast-tempo optimization (20 min)

**Session 2: Simultaneous + False Positives (45 min)**
3. Simultaneous detection (30 min)
4. False positive reduction (15 min)

**Session 3: Full Validation (30 min)**
5. Run complete validation suite
6. Update firmware with final optimal values
7. Document final results in PARAMETER_TUNING_HISTORY.md

**Total Time: ~2 hours**

## Known Limitations (Algorithmic, Not Parameter-Tunable)

Some issues may not be fixable via parameter tuning:

1. **Simultaneous overlapping sounds** - May need multi-band or peak-picking algorithms
2. **Sustained tones vs transients** - May need temporal envelope analysis
3. **Very fast patterns** - Limited by cooldown minimum and ADC sampling rate

If parameter tuning doesn't resolve these, document as architectural limitations
and consider algorithm enhancements in future development.

## Updates Applied (2025-12-28)

✅ Updated test parameter defaults to match fast-tune optimal values
✅ Extended boundaries: attackmult (1.0), hitthresh (1.0), fluxthresh (0.5)
✅ Added boundary values to sweepValues arrays
✅ Created analyze-fast-tune.cjs for detailed performance analysis
