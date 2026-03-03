# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 3, 2026

## Current Config (v45, SETTINGS_VERSION 45)

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.3, bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0, beatoffset=5, onsetSnapWindow=8
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 — raw ODF preserves ACF structure)
- phasecheck=0, warmup=0, cbssContrast=1.0
- bisnap=1 (v44 bidirectional onset snap)

**v45 new features (all enabled by default, all togglable):**
- Percival ACF harmonic pre-enhancement: `percival=1, percivalw2=0.5, percivalw4=0.25`
- PLL proportional phase correction: `pll=1, pllkp=0.15, pllki=0.005`
- Adaptive CBSS tightness: `adaptight=1, tightlowmult=0.7, tighthighmult=1.3, tightconfhi=3.0, tightconflo=1.5`

**v43 algorithmic fixes (no new settings):**
- Removed double inverse-lag normalization (balanced ACF is sufficient)
- Full-resolution comb-on-ACF (47 lags, not just 20 bin centers)
- Octave folding: each bin sums evidence from lag L + lag L/2
- Lag-space Gaussian transition matrix (fixed sigma, symmetric bandwidth)

## Current Baselines

### v43 18-Track Validation (Mar 3, 2026, 3 bare boards)

| Metric | 3-Device Avg | Best-Device Avg |
|--------|:------------:|:---------------:|
| Beat F1 | 0.284 | 0.355 |
| BPM Accuracy | **0.877** | — |

### v45 — NOT YET TESTED

v45 adds 3 new features (Percival + PLL + adaptive tightness). All enabled by default.
Baselines below are from v43/v44. Run 18-track validation to establish v45 baselines.

### Historical Baselines

| Version | Change | N-Dev F1 | Best-Dev F1 | BPM Acc |
|---------|--------|:--------:|:-----------:|:-------:|
| v36 | Frame rate fix, onset snap, downward correction | 0.275 | 0.351 | 0.825 |
| v40 | cbssTightness 5→8 | 0.285 | 0.348 | — |
| v43-baseline | Pre-fix bare boards | 0.313 | — | **0.33** |
| v43 | 4 Bayesian bug fixes | 0.284 | 0.355 | **0.877** |
| **v44** | **bisnap=1** (only keeper from v44) | **0.271** | — | **0.877** |

### Known Bottlenecks

1. **~128 BPM gravity well** — system gravitates to 125-135 BPM regardless of actual tempo.
   Tracks in-range (garage, minimal, trance) get 97-100% accuracy. Slow tracks (86-96 BPM)
   lock to 3:2 harmonic (~128 BPM). Rayleigh prior peaked at ~120 BPM likely contributes.

2. **Phase alignment** — even with 88% BPM accuracy, Beat F1 averages only 0.28.
   CBSS derives phase indirectly from beat counter. Onset snap helps but doesn't fully solve it.
   Garage-uk-2step: 99% BPM accuracy but F1 = 0.05-0.32 (pure phase problem).

3. **Run-to-run variance** (std 0.04-0.23) — single-run evaluations cannot detect < ~0.15 F1 changes.

## Next Steps Toward 70% Beat F1

> **Design philosophy:** See [VISUALIZER_GOALS.md](../docs/VISUALIZER_GOALS.md) — visual quality over metric perfection. Low Beat F1 on ambient/trap tracks is acceptable (organic mode fallback is correct).

### Analysis: Why Are We Stuck at ~0.28?

v43 testing on 3 identical bare boards revealed:
1. **Double-time lock SOLVED** (v43): Was caused by 4 compounding bugs in Bayesian tempo
   estimation, not acoustic/enclosure effects. BPM accuracy 33%→88%.
2. **~128 BPM gravity well**: Slow tracks (86-96 BPM) lock to 3:2 harmonic. Root cause is
   the **additive 4-harmonic comb-on-ACF** — it boosts harmonics just as much as fundamentals.
   Three forces compound: Rayleigh prior peaked at 120 BPM, additive comb, IIR comb bank
   resonance. All three reinforce each other multiplicatively in `acfObs`.
3. **Phase alignment is the F1 bottleneck**: Correct BPM doesn't translate to correct beat
   placement. CBSS phase derivation is inherently indirect — phase is emergent from a counter,
   not an explicit state variable. Systems achieving >60% F1 all track phase explicitly
   (HMM state, Fourier angle, or particle cloud).

### Literature Review (March 2026)

Research into BTrack, madmom, Essentia/Percival, librosa, and recent papers (Krebs/Böck
ISMIR 2015, Heydari ICASSP 2022, Meier/Chiu/Müller TISMIR 2024, Beat Critic ISMIR 2010)
identified techniques we haven't tried that address our specific bottlenecks:

**For 128 BPM gravity well — root cause is additive comb:**
- **Percival ACF harmonic enhancement** (Essentia): Fold ACF[2L]+ACF[4L] into ACF[L] *before*
  comb evaluation. Gives fundamental a unique advantage harmonics don't get. ~50 ops, 0 memory.
- **Anti-harmonic comb** (speech F0 lit): Replace additive `+ACF[3L]+ACF[4L]` with subtractive
  `-0.3*ACF[3L]-0.2*ACF[4L]`. Aggressive but may produce negatives with noisy ACF.
- **Metrical contrast check** (Beat Critic ISMIR 2010): Compare onset strength at beat positions
  vs. midpoints. Low contrast = possible double-time. Different signal from density discriminator.

**For phase alignment — CBSS is inherently limited:**
- **PLL-style proportional correction**: Overlay on CBSS. Measure phase error at each beat,
  apply Kp*error correction. ~20 bytes, negligible CPU. Addresses slow phase drift.
- **Adaptive tightness**: Lower tightness when onset confidence is high (allow phase correction),
  higher when low (resist noise). Resolves tightness 5 vs 8 dilemma correctly.
- **1D joint tempo-phase HMM** (Krebs/Böck 2015, Heydari 2022): The standard architecture for
  >60% F1. ~1,800 states, sparse Viterbi, ~14 KB RAM, ~2% CPU. Also inherently solves 128 BPM
  gravity well (transition_lambda=100 makes octave jumps impossible).

**Eliminated by research:**
- ~~Widen Rayleigh prior~~ — weak lever, doesn't overcome comb filter harmonics
- ~~Stronger density penalty for 3:2~~ — already tested as fold32/sesquicheck in v44, no net benefit
- ~~Lower cbssTightness (8→5)~~ — tested in v40, wrong for noisy mic audio (noise pulls phase)
- ~~Bidirectional onset snap~~ — already implemented as bisnap in v44
- ~~PLP phase extraction~~ — tested in v42, OSS too noisy for analytical phase

### Priority 1: Fix ~128 BPM Gravity Well

Slow tracks (breakbeat 86 BPM, reggaeton 92 BPM) lock to ~128 BPM (3:2 harmonic).
Root cause: additive comb-on-ACF boosts harmonics equally with fundamentals.

**1a. Percival ACF harmonic pre-enhancement — IMPLEMENTED (v45)**
Folds ACF[2L] and ACF[4L] into ACF[L] before comb-on-ACF evaluation. Gives fundamental
a unique advantage: at lag L, both the pre-enhanced ACF values and the comb-on-ACF see
boosted energy. The double-time at L/2 doesn't get the same treatment. Forward iteration
is safe (always reading from higher lags that haven't been modified yet).
Toggle: `percival=0/1`, weights: `percivalw2=0.5`, `percivalw4=0.25`.

**1b. Anti-harmonic comb (LOW effort, alternative to 1a)**
Replace additive 4-harmonic comb with discriminative version:
```cpp
combAcf = ACF[L] + 0.5*ACF[2L] - 0.3*ACF[3L] - 0.2*ACF[4L]
```
Keep 2nd harmonic (confirms periodicity), subtract higher harmonics (shared with harmonic
candidate). Risk: may produce negative values with noisy ACF. Test with floor at 0.

**1c. Metrical contrast check (LOW effort, new disambiguation signal)**
From Beat Critic (ISMIR 2010): compare mean onset strength at beat positions vs. half-beat
positions. If midpoints are equally strong, suspect double-time. Apply as correction factor
in density discriminator or shadow CBSS checker.

### Priority 2: Phase Alignment

Even with 88% BPM accuracy, Beat F1 averages only 0.28. CBSS derives phase indirectly
from a beat counter. Alpha=0.9 means 90% of CBSS comes from history, self-reinforcing
any phase lock-in errors.

**2a. PLL-style proportional phase correction — IMPLEMENTED (v45)**
After each beat + onset snap, measures IBI (inter-beat interval) error against expected
period T. Proportional correction (Kp=0.15) nudges `lastBeatSample_`. Leaky integral
(Ki=0.005, decay=0.95) corrects persistent drift. Max shift capped at T/4.
Toggle: `pll=0/1`, gains: `pllkp=0.15`, `pllki=0.005`.

**2b. Adaptive tightness — IMPLEMENTED (v45)**
Modulates CBSS log-Gaussian tightness based on onset confidence (OSS/cbssMean ratio).
Strong onsets (ratio > 3.0) → tightness × 0.7 (looser, allow phase correction).
Weak onsets (ratio < 1.5) → tightness × 1.3 (tighter, resist noise drift).
Toggle: `adaptight=0/1`, multipliers: `tightlowmult=0.7`, `tighthighmult=1.3`,
thresholds: `tightconfhi=3.0`, `tightconflo=1.5`.

**2c. Off-beat suppression in CBSS (LOW effort)**
From Davies & Plumbley (2007). After establishing phase, multiply CBSS by a windowing
function centered on expected beat positions. Attenuates off-beat onsets that pull phase
incorrectly. ~100 bytes, ~0.1% CPU.

### Priority 3: Evaluate Visual Quality

Before investing in the HMM (Priority 4), evaluate whether Priorities 1-2 produce
acceptable visual results:
- Run `render_preview` with real music patterns
- Compare animation smoothness and beat-reactivity
- If visual quality is acceptable at the improved F1, focus on reliability over accuracy

### Priority 4: 1D Joint Tempo-Phase HMM (HIGH effort, highest impact)

The madmom-style 1D state space (Krebs/Böck ISMIR 2015, Heydari ICASSP 2022) is the
**architectural change** needed for >60% F1. All reference systems achieving this level
track phase explicitly as a state variable.

**Architecture:**
- ~1,800 states (sum of integer lags 20-66, each lag L contributes L phase positions)
- Phase advances deterministically within a beat: state p → p+1
- Tempo changes only at beat boundaries (phase wraps 0): jump-back transitions
- `transition_lambda=100`: tempo changes >10% are essentially zero probability
- Observation model: high likelihood at position 0 when onset detected, low otherwise
- Sparse Viterbi forward pass: each state has 1-2 transitions

**Resources:** ~14 KB RAM, ~2% CPU (1.2M ops/sec at 66 Hz, trivial for 64 MHz Cortex-M4)

**Why this also solves 128 BPM gravity well:** `transition_lambda=100` makes octave jumps
(ratio 1.5 or 2.0) mathematically impossible within the HMM. Tempo can only change
gradually at beat boundaries.

**Previous v37 HMM failure analysis:** Failed due to too-few bins (20) and wrong observation
model (flat across all states). Corrected version needs: (1) full integer-lag resolution
(46 distinct periods), (2) position-0-specific observation model (beat states get
`lambda * activation`, non-beat states get `1 - activation`).

## v45 Test Plan

### Step 1: Establish v45 Baseline (all 3 features ON)
Run full 18-track validation on 3 devices with all defaults. This is the combined baseline.
```
# All features enabled (default):
# percival=1, pll=1, adaptight=1
```

### Step 2: A/B Each Feature (3 devices, one feature OFF per device)
For each 18-track run, configure 3 devices differently to test each feature's contribution:
```
# ACM0: percival=0 (Percival OFF, others ON)
# ACM1: pll=0 (PLL OFF, others ON)
# ACM2: adaptight=0 (adaptive tightness OFF, others ON)
```
Compare each device's F1 against the Step 1 baseline. A feature is a keeper if disabling it
hurts F1 (i.e., baseline > feature-off).

### Step 3: Slow Track Focus (Percival validation)
Percival specifically targets the 128 BPM gravity well on slow tracks. Run a focused test
on the slow subset (breakbeat-amen 86 BPM, reggaeton 92 BPM, dub-groove 96 BPM):
```
# ACM0: percival=1 (default)
# ACM1: percival=0
# ACM2: percival=1, percivalw2=0.8, percivalw4=0.5 (stronger weights)
```
Check BPM accuracy specifically — does Percival prevent 3:2 harmonic lock?

### Step 4: Phase-Sensitive Tracks (PLL validation)
PLL targets phase drift. Focus on tracks with good BPM but poor F1 (garage-uk-2step,
trance-goa-mantra):
```
# ACM0: pll=1 (default)
# ACM1: pll=0
# ACM2: pll=1, pllkp=0.25 (stronger correction)
```

### Step 5: Full Validation (if features look promising)
If individual features show improvement, run a 5-run repeated 18-track validation
to confirm results exceed run-to-run variance (std 0.04-0.23).

## PF Evaluation — Completed (v40)

### Phase 1: Smoke Test — PASSED
- PF runs stably on 3 tracks, no crashes, produces beats
- BPM accuracy: PF 0.848 vs CBSS 0.560 (+52%)
- Beat F1: PF 0.274 vs CBSS 0.295 (within noise)
- **Conclusion:** PF solves tempo, not phase

### Phase 2-5: Deferred
- Full A/B and parameter sweeps deferred pending architectural work on phase alignment
- PF's value is in tempo estimation; without phase improvement, parameter tuning won't help F1

### CBSS Parameter Sweeps — Completed (v40)

| Parameter | Values Tested | Optimal | Effect |
|-----------|:------------:|:-------:|--------|
| beatTimingOffset | 3, 5, 7, 9 | 5 (default) | Inconclusive — device BPM variation dominates |
| cbssTightness | 2, 5, 8, 12 | **8** | +24% on same-BPM devices, +3.6% on 18-track avg |

## Completed (v28-v45, Feb-Mar 2026)

- v28: FT+IOI disabled, beat-boundary tempo, peak picking, unified ODF, 40→20 bins
- v29: Transition matrix drift investigation (reverted 40 bins)
- v32: ODF mean sub disabled (+70%), density octave penalty (+13%), shadow octave checker (+13%)
- v33: BTrack-style Viterbi max-product on comb-ACF (experimental, not default)
- v35-v37: Phase alignment experiments (onset snap +20%, phase check/warmup/HMM negative)
- v38-v39: Particle filter implementation and bar-pointer model
- v40: cbssTightness 5→8, PF smoke test, parameter sweeps
- v42: PLP phase extraction (tested, no effect — disabled by default)
- v43: 4 Bayesian tempo bug fixes — BPM accuracy 33%→88%, double-time lock eliminated
- v44: bisnap=1 (+0.005 F1); fold32/sesquicheck/harmonicsesqui all negative
- v45: Percival harmonic pre-enhancement + PLL phase correction + adaptive tightness (all implemented, untested)

## Known Limitations

| Issue | Root Cause | Visual Impact |
|-------|-----------|---------------|
| ~128 BPM gravity well | Rayleigh prior + comb harmonic resonance | **Medium** — slow tracks lock to 3:2 harmonic |
| Phase alignment limits F1 | CBSS derives phase indirectly from beat counter | **Medium** — correct BPM but misplaced beats |
| Run-to-run variance (std 0.04-0.23) | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation |
| Per-track beat offset (-58 to +57ms) | ODF latency varies by content | **None** — invisible at LED update rates |
| deep-ambience low Beat F1 | Soft ambient onsets below threshold | **None** — organic mode is correct |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive acceptable |
