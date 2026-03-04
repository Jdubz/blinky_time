# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 4, 2026 (v50 implemented: rhythmic pattern templates + subbeat alternation)

## Current Config (v50, SETTINGS_VERSION 50)

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

**v50 new features (all default OFF, awaiting A/B validation):**
- Rhythmic pattern templates: `templatecheck=0, templatescoreratio=1.3, templatecheckbeats=4`
- Subbeat alternation: `subbeatcheck=0, alternationthresh=1.2, subbeatcheckbeats=4`

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

### v45 18-Track Validation (Mar 3, 2026, 3 bare boards)

| Metric | 3-Device Avg | vs v43 |
|--------|:------------:|:------:|
| Beat F1 | **0.317** | +11.6% |
| BPM Accuracy | 0.815 | -7.1% |

Feature contributions (A/B isolation): PLL +0.031, Adaptive tightness +0.012, Percival +0.010.
All 3 features retained as defaults.

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

### Literature Review (March 4, 2026 — Updated)

Extended research into BTrack, madmom, BeatNet/BeatNet+, Essentia/Percival, librosa, and
recent papers identified the **key architectural gap**: all systems >60% F1 use the continuous
ODF as a probabilistic observation at every frame, not just for binary onset detection. Our
system uses the ODF only for threshold-based onset detection + CBSS accumulation. The
literature's consensus: observation model matters more than inference algorithm.

**Critical finding — v46 HMM failure was the observation model, not the architecture:**
The v46 Bernoulli model treated onset detection as binary (onset or not). madmom's observation
model uses the CONTINUOUS ODF value: high ODF at beat positions = evidence for beat, low ODF
between beats = evidence for correct phase. Missing a beat is just weaker evidence, not a
catastrophe. This fundamental difference was not tested in v46.

### Priority 1: Forward Filter with Continuous ODF Observation (Phase Alignment)

**Status: NOT STARTED — NEXT EXPERIMENT**

Replace CBSS countdown + onset snap + PLL with a 320-state forward filter (20 tempos × 16
phases) using BandFlux ODF as a continuous observation at every frame. This is the standard
architecture for all systems achieving >60% F1.

**madmom observation model (adapted for our ODF):**
```
P(ODF | beat position)     = ODF_value               // high ODF = beat evidence
P(ODF | non-beat position) = (1 - ODF_value) / (λ-1) // low ODF = non-beat evidence
```

**Why this differs from v46 (which failed):**

| | v46 Bernoulli (FAILED) | Continuous ODF (proposed) |
|---|---|---|
| At beat, ODF=0.1 | P → 0.1 (near-catastrophic) | P = 0.1 (low but survivable) |
| Between beats, ODF=0.0 | P = 1.0 (binary) | P = 1/λ ≈ 0.125 (proportional) |
| Missing an onset | Probability collapses | Probability reduced gracefully |

**Implementation:**
- State space: 20 tempo bins × 16 phase positions = 320 states
- Per frame: advance phase counters, compute log-likelihood for each state
- Transition: deterministic advance within-tempo; exponential penalty at beat boundaries
- Beat detection: highest-probability state at phase=0 fires beat
- `observation_lambda = 8` (wider beat zone than madmom's 16 for noisy mic ODF)
- RAM: ~1.3 KB | CPU: negligible | Replaces: CBSS + onset snap + PLL + octave check

**Test plan:**
1. Implement forward filter alongside existing CBSS (togglable: `fwdfilter=0/1`)
2. Run 18-track validation with 3+ runs per track (need statistical power)
3. Compare vs CBSS baseline. If F1 > 0.35, retain and tune observation_lambda.
4. If F1 < CBSS: try lower observation_lambda (4, 6) for wider beat zone.

### Priority 2: Rhythmic Pattern Templates (Octave Ambiguity)

**Status: IMPLEMENTED v50, default OFF, awaiting A/B validation**

Pre-compute 3 EDM bar templates (16 time slots per bar). Correlate observed CBSS pattern
against templates at each candidate tempo via Pearson correlation. Tempo where pattern best
matches a template is preferred. Compares T vs T/2 and T vs 2T; calls `switchTempo()` if
alternative wins by `templateScoreRatio` (default 1.3).

**Templates (precomputed zero-mean):**
- Standard 4/4 kick (emphasis on beats 1,3)
- Four-on-the-floor (equal kicks all 4 beats)
- Sparse/breakbeat (kick on 1, snare on 3)

**Settings:** `templatecheck=0/1`, `templatescoreratio=1.0-3.0`, `templatecheckbeats=2-8`

Source: Krebs, Böck, Widmer - "Rhythmic Pattern Modeling for Beat/Downbeat Tracking" (ISMIR 2013).

- RAM: ~512 bytes | CPU: negligible (runs every 4 beats)

### Priority 3: Beat Critic Subbeat Alternation (Octave Ambiguity)

**Status: IMPLEMENTED v50, default OFF, awaiting A/B validation**

Divides each beat period into 8 subbeat bins, measures CBSS energy in each. Computes
alternation measure: odd-bin vs even-bin mean ratio. Strong alternation at period T
indicates double-time tracking → switches to T/2. Only downward switching (T→T/2);
upward branch removed as weak discriminative signal.

Different from existing `checkOctaveAlternative` (CBSS score comparison) and
`metricalcheck` (beat/midpoint ratio, tested v48 — negative).

**Settings:** `subbeatcheck=0/1`, `alternationthresh=0.3-3.0`, `subbeatcheckbeats=2-8`

Source: Davies (ISMIR 2010) — Beat Critic: Beat Tracking Octave Error Identification.

- RAM: ~256 bytes | CPU: negligible (runs every 4 beats)

### v50 Validation Test Plan

**Goal:** Determine if template check and/or subbeat alternation improve octave disambiguation.

**Phase 1: Smoke test (single track, 1 run)**
```bash
# Verify no crashes, features toggle correctly
run_music_test_multi ports=["/dev/ttyACM0"] audio_file="breakbeat-amen.mp3"
  port_commands={"/dev/ttyACM0": ["set templatecheck 1", "set subbeatcheck 1"]}
```
- Verify device doesn't crash, beat tracking produces output
- Check `show rhythm` confirms new params are active

**Phase 2: Combined feature test (3 tracks, 3 runs)**
Use `run_music_test_multi` with 2-3 devices: one baseline (both OFF), one templates-only, one subbeat-only.
Tracks: breakbeat-amen, reggaeton-perreo, dub-groove (slow tracks where 128 BPM lock occurs).

**Phase 3: Targeted slow tracks (6 tracks, 3 runs)**
Focus on tracks in 86-96 BPM range where 3:2 harmonic lock is the known problem.
Compare: baseline vs templatecheck=1 vs subbeatcheck=1 vs both enabled.

**Phase 4: Full 18-track validation suite (3 runs)**
```bash
run_validation_suite ports=["/dev/ttyACM0", "/dev/ttyACM1"]
  port_commands={
    "/dev/ttyACM0": [],  # baseline
    "/dev/ttyACM1": ["set templatecheck 1", "set subbeatcheck 1"]
  }
  runs=3
```
- Compare avg Beat F1, BPM accuracy, per-track wins/losses
- Accept if: avg F1 improvement > 0.01 AND no track regresses > 0.05

### Completed / Deprioritized

**Phase alignment (completed, retained as defaults):**
- v45 PLL phase correction: +0.031 F1 (strongest individual feature)
- v45 Adaptive tightness: +0.012 F1
- v45 Percival ACF pre-enhancement: +0.010 F1
- v44 Bidirectional onset snap: +0.005 F1

**Octave ambiguity (tested, all negative or marginal):**
- v48 Multi-agent beat tracking: -4% on full 18-track (-0.013 avg F1)
- v48 Anti-harmonic comb (percivalw3=0.3): marginal BPM improvement, no F1 improvement
- v48 Metrical contrast check: negative on full 18-track validation
- v44 3:2 octave folding (fold32): -0.009 avg F1
- v44 3:2 shadow check (sesquicheck): no benefit
- v44 Harmonic transition shortcuts: catastrophic on fast tracks

**Signal chain (tested v47, NOT the bottleneck):**
- Raw FFT path (bfprewhiten=1): no measurable improvement
- Bass whitening bypass (whitenbassbypass): no measurable improvement
- BandFlux self-normalizes via log(1+20*mag) + adaptive threshold

**Phase tracking architectures (tested, all regress vs CBSS):**
- v46 Bernoulli HMM (3 approaches): all regress. Root cause: binary obs model, NOT the
  HMM architecture itself. Continuous ODF observation model (Priority 1) is fundamentally
  different and has NOT been tested.
- v42 PLP phase extraction: no effect (OSS too noisy for Fourier angle)
- v37 Phase check / CBSS warmup / cbssContrast: all negative

**Deprioritized:**
- Off-beat suppression (Davies & Plumbley 2007): minor refinement per literature review
- Complex Spectral Difference ODF: eliminated — phase too noisy via mic (Dixon 2006)
- Signal chain experiments 5a-5f: v47 proved signal chain is not the bottleneck

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

## Completed (v28-v50, Feb-Mar 2026)

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
- v45: Percival +0.010, PLL +0.031, adaptive tightness +0.012 (+11.6% combined)
- v46: HMM phase tracker (3 approaches tested, all regress vs CBSS). Signal chain audit completed
- v48: Multi-agent (-4%), percivalw3 (marginal), metricalcheck (negative). All default OFF
- v49: Continuous ODF observation model in CBSS phase tracker (replaces Bernoulli)
- v50: Rhythmic pattern templates + subbeat alternation (implemented, default OFF, awaiting validation)

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| Phase alignment limits F1 | CBSS derives phase indirectly from beat counter | **High** — correct BPM but misplaced beats | Forward filter with continuous ODF obs (Priority 1) |
| ~128 BPM gravity well | Rayleigh prior + comb harmonic resonance | **Medium** — slow tracks lock to 3:2 harmonic | Rhythmic pattern templates (Priority 2) |
| ~~Signal chain attenuation~~ | ~~Compressor/whitening/AGC~~ | **CLOSED** | v47 proved BandFlux self-normalizes |
| Run-to-run variance (std 0.04-0.23) | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation | — |
| Per-track beat offset (-58 to +57ms) | ODF latency varies by content | **None** — invisible at LED update rates | — |
| deep-ambience low Beat F1 | Soft ambient onsets below threshold | **None** — organic mode is correct | — |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive acceptable | — |
