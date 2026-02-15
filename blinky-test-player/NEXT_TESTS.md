# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** February 14, 2026

## Current Performance (Feb 2026 Baseline)

**Config:** HFC (0.60) + Drummer (0.40), agree_1=0.2, cooldown=250ms, minconf=0.55

| Pattern | F1 | Precision | Recall | Issue |
|---------|-----|-----------|--------|-------|
| strong-beats | 1.000 | 1.0 | 1.0 | None |
| synth-stabs | 1.000 | 1.0 | 1.0 | None |
| full-mix | 0.901 | 0.82 | 1.0 | 7 FPs |
| sparse | 0.889 | 0.80 | 1.0 | 2 FPs, high variance |
| pad-rejection | 0.696 | 0.53 | 1.0 | 7 FPs (varies 0.64-0.80) |
| chord-rejection | 0.698 | 0.56 | 0.94 | 12 FPs |
| lead-melody | 0.286 | 0.17 | 1.0 | 38-40 FPs |

**Average F1: 0.781**

## Completed Work (Feb 2026)

- Algorithm improvements: spectral whitening, mel-band SuperFlux, cosine distance NoveltyDetector, ComplexDomain phase fix
- Drummer minRiseRate and HFC sustained-signal rejection
- Phase correction hardening (agreement gate, EMA alpha reduction)
- Comprehensive detector tuning: ComplexDomain, SpectralFlux, Novelty all tested and kept disabled
- Agreement boost optimization: agree_1=0.2 confirmed optimal

## Priority 1: lead-melody False Positive Reduction

**Problem:** HFC fires on every melody note (38-40 FPs, F1=0.286). This is the worst pattern by far.

**Root cause:** Melody notes have high-frequency harmonic content that HFC correctly identifies as sharp spectral changes. The system cannot distinguish "new note in melody" from "percussive transient."

**Potential approaches:**
1. **Pitch continuity gate** — Track if high-frequency content follows a pitched pattern (harmonically related bins across frames). Suppress detection when spectral content is pitched rather than noisy/broadband.
2. **Harmonic ratio check** — Percussive transients have flat/noisy spectra; pitched notes have peaked harmonic spectra. Compute harmonic-to-noise ratio and gate detection.
3. **Temporal envelope** — Melody notes sustain; percussive hits decay rapidly. Track post-onset energy decay and suppress if signal sustains >50ms.

**Estimated difficulty:** High — requires new algorithm development, not just parameter tuning.

## Priority 2: chord-rejection Improvement

**Problem:** 12 FPs on chord changes (F1=0.698). Chord transitions produce genuine amplitude spikes that trigger both HFC and Drummer.

**Potential approaches:**
1. **Spectral continuity** — Chord changes preserve harmonic structure while transients are broadband. Could add a harmonic preservation check.
2. **Rise time analysis** — Chord transitions have slower rise times than percussive attacks. The Drummer minRiseRate (0.02) already helps but could be more aggressive.
3. **Higher thresholds** — Raising HFC/Drummer thresholds would reduce chord FPs but risk missing real transients.

**Quick test:** Try `set detector_thresh drummer 4.0` and `set detector_thresh hfc 5.0` on chord-rejection pattern.

## Priority 3: Test Variance Reduction

**Problem:** Results vary significantly across runs (pad-rejection: 0.64-0.80, sparse: 0.64-0.94). This makes it hard to confidently evaluate parameter changes.

**Potential approaches:**
1. **Run each pattern 3x and average** — More reliable but 3x slower
2. **Higher gain lock** — Try gain=50 or 60 for stronger SNR
3. **Environment control** — Close doors, reduce ambient noise during tests
4. **Longer cooldown between tests** — Allow AGC to fully settle

## Priority 4: Rhythm Tracking at Extreme Tempos

**Problem:** BPM tracking biased toward prior center (120 BPM). 60 BPM and 180 BPM show 30-45% error.

**Root cause:** Autocorrelation naturally produces stronger peaks at subharmonics. The tempo prior helps but can't fully disambiguate.

**Potential approaches:**
1. **Harmonic relationship detection** — When candidate BPM is ~2x or 0.5x the primary, check which is more consistent with transient timing
2. **Double-time promotion** — If detected BPM is half of a hypothesis with stronger transient alignment, promote the faster tempo
3. **Wider prior** — Increase priorwidth to reduce center bias (trade-off: less precise at 120 BPM)

## Known Algorithmic Limitations

These issues are unlikely to be fixed by parameter tuning alone:

| Issue | Root Cause | Fix Requires |
|-------|-----------|-------------|
| Melody note false positives | Pitched harmonics trigger HFC | Pitch/harmonic analysis algorithm |
| Chord change false positives | Amplitude spikes on transitions | Rise-time or spectral continuity gate |
| Half-time BPM detection | Autocorrelation subharmonics | Harmonic tempo relationship logic |
| Simultaneous overlapping sounds | Single detection per cooldown window | Multi-band peak picking |
| Test variance | Room acoustics, ambient noise | Controlled test environment |
