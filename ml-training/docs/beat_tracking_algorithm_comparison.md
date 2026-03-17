# Beat/Tempo/Phase Tracking Algorithm Comparison

**Target:** Cortex-M4F @ 64 MHz, 256 KB RAM, <1ms per frame at 62.5 Hz
**Input:** NN onset activation signal (0-1, kicks/snares only) at 62.5 Hz
**Outputs:** BPM (60-200), phase ramp (0->1 sawtooth), onset pulse (transient events)

---

## A. Predominant Local Pulse (PLP) — Grosche & Mueller 2011

### How It Works

PLP is fundamentally a Fourier tempogram approach. For each time position, it computes a
short-time Fourier transform (STFT) of the onset function to find which tempo (frequency)
best explains the local periodicity. The key steps:

1. **Fourier Tempogram**: STFT of the onset function with a long window (typically 6-8
   seconds). Each frequency bin corresponds to a tempo. The complex-valued output gives
   both magnitude (how strongly that tempo is present) and phase (where the beats fall).

2. **Peak selection**: At each frame, find the frequency bin with the maximum magnitude
   in the allowed tempo range (60-200 BPM). Zero out all other bins.

3. **Phase normalization**: Discard magnitude, keep only the phase of the winning bin.
   This gives a unit-magnitude complex sinusoid at the detected tempo.

4. **Inverse STFT (overlap-add)**: Accumulate these windowed sinusoids over time via
   overlap-add. The resulting waveform is the PLP curve -- a smooth, nearly-sinusoidal
   signal whose peaks align with beat positions.

5. **Peak picking**: Local maxima of the PLP curve are the detected beats.

### Causal (Real-Time) Variant (Meier, Krause, Mueller 2024)

The 2024 TISMIR paper adapts PLP to real-time by using only causal data (past frames).
The window covers [n-N : n] instead of [n-N : n+N], halving the effective context.

- F1 = 74.7% on GTZAN with RNN activation (standard PLP offline: ~80%)
- Zero-latency variant achieves F1 = 74.7% with 0ms latency
- With ground-truth activation function: F1 = 91.9%

Reference implementation: https://github.com/groupmm/real_time_plp

### Analysis for Embedded

**Computational cost**: The Fourier tempogram requires an FFT of the onset buffer at
every frame. For a 6-second window at 62.5 Hz = 375 samples, this is a 512-point FFT
per frame. At 62.5 Hz, that is 62.5 FFTs/second. A 512-point float FFT on Cortex-M4F
with CMSIS-DSP takes ~0.2-0.3ms. The overlap-add reconstruction adds trivial cost.
**Total: ~0.3-0.5ms per frame. Feasible.**

**Memory**: 375-sample onset buffer (1.5 KB) + 512-point complex FFT output (4 KB) +
PLP accumulation buffer (1.5 KB). **Total: ~7-8 KB. Feasible.**

**Phase stability**: Excellent. The overlap-add of windowed sinusoids inherently produces
a smooth output. Phase jumps are suppressed by the windowing. This is PLP's greatest
strength -- the phase ramp is smooth by construction.

**Tempo tracking speed**: Slow. The 6-second window means tempo changes take 3-6 seconds
to fully propagate. The causal variant (one-sided window) is even slower because it
lacks future context. Music with abrupt tempo changes will lag.

**Behavior during silence**: The tempogram magnitude drops to zero, so the PLP curve
decays to zero. No false beats. But there is no "memory" of the previous tempo -- when
music resumes, it takes a full window (6 seconds) to re-lock.

**Octave error susceptibility**: Moderate. The frequency-domain peak selection can lock
to harmonics (double-time) or sub-harmonics (half-time). The tempo range constraint
helps, but 120 BPM vs 60 BPM or 240 BPM are classic failure cases. No built-in
disambiguation beyond the range constraint.

**Published F1**: 74.7% (causal, RNN activation, GTZAN). 91.9% with perfect activation.

**Complexity**: Medium. FFT + peak selection + ISTFT. No iterative optimization or
complex state machines. The librosa `plp()` implementation is ~50 lines of core logic.

### Failure Modes

- Slow tempo adaptation (6-second latency)
- Octave errors when strong harmonics present
- Causal variant loses ~5% F1 vs offline (halved context)
- During breakdowns, phase ramp stops (no prediction/extrapolation)
- With noisy ODF, the tempogram peak may jump between adjacent bins, causing phase jitter

### Verdict for This Project

PLP's phase stability is its killer feature. The 0.3-0.5ms computational cost fits
the budget. The main weaknesses are slow tempo adaptation and loss of phase during
silence. For a visualizer that needs continuous smooth phase, the silence behavior is
problematic -- you need to keep the ramp going during short breakdowns.

---

## B. Phase-Locked Loop (PLL) for Beat Tracking

### How It Works

A digital PLL applied to beat tracking consists of three components:

1. **Phase Detector (PD)**: Compares the expected beat phase (from the internal
   oscillator) with the detected onset position. Outputs a phase error signal.
   For beat tracking, this is typically: error = onset_time - predicted_beat_time,
   normalized to [-0.5, +0.5] of the beat period.

2. **Loop Filter (LF)**: A PI (proportional-integral) controller that smooths the
   error signal. The proportional term corrects immediate phase offset. The integral
   term corrects persistent tempo offset (frequency error).
   - Kp (proportional gain): controls how fast phase corrections are applied
   - Ki (integral gain): controls how fast tempo adapts

3. **Digital Controlled Oscillator (DCO)**: A counter/accumulator that produces the
   beat phase ramp. Its frequency (tempo) is adjusted by the loop filter output.
   The phase output IS the sawtooth ramp we want.

The PLL only updates at onset events. Between onsets, the DCO free-runs at the
current estimated tempo, producing a smooth phase ramp.

### Published Work

- Shiu & Kuo 2007: "On-Line Musical Beat Tracking with Phase-Locked-Loop Technique"
  (IEEE ICCE). Simple digital PLL, effective but tested on only one song. Implementation
  details are sparse.

- Kim 2007 (referenced in BTrack implementations): PLL-style proportional correction
  at each beat fire. This is essentially what the current Blinky firmware does (pllKp,
  pllKi parameters in AudioController).

### Analysis for Embedded

**Computational cost**: Trivially cheap. The DCO is a single addition per frame
(phase += increment). The PD+LF only run when an onset is detected (~2-4 times/second).
**Total: ~1-5 microseconds per frame. Negligible.**

**Memory**: ~20 bytes (phase accumulator, frequency estimate, integral state, Kp, Ki).
**Trivial.**

**Phase stability**: Very good when locked. The DCO free-runs between updates, producing
a perfectly smooth sawtooth. Phase corrections at onset events can cause small
discontinuities, but with proper Kp (<0.2), these are imperceptible.

**Tempo tracking speed**: Controlled by Ki. With Ki=0.005, tempo converges in ~20-40
beats (10-20 seconds at 120 BPM). This is slow for abrupt tempo changes. Increasing
Ki makes it faster but also noisier. A PLL is fundamentally a tradeoff between
tracking bandwidth and noise rejection.

**Behavior during silence**: The DCO free-runs at the last known tempo. Phase continues
smoothly. This is EXACTLY what a visualizer wants -- if music stops briefly, the
animation keeps its rhythm until music resumes.

**Octave error susceptibility**: HIGH. This is the PLL's Achilles heel. A PLL has no
inherent mechanism to distinguish 60 BPM from 120 BPM from 240 BPM. If the onset
function fires on every other beat, the PLL happily locks to half-time. If hi-hats
leak through, it locks to double-time. The PLL MUST be given a correct tempo estimate
from an external source (ACF, comb filter bank, etc.).

**Published F1**: No standalone PLL beat tracker has competitive F1 scores. PLLs are
always used as a phase refinement stage on top of a separate tempo estimator.

**Complexity**: Very low. ~20 lines of code for the full PLL.

### Failure Modes

- Cannot determine tempo on its own (needs external tempo estimation)
- Octave errors if tempo estimate is wrong
- Slow adaptation to tempo changes (limited by Ki)
- If many onsets are missed, the integral wind-up can cause large phase jumps on recovery
- With a noisy ODF (many false onsets), the PLL phase jitters

### Verdict for This Project

A PLL is not a complete beat tracker -- it is a phase tracking component. It excels
at producing a smooth phase ramp from a known tempo. The current firmware already
uses PLL-style phase correction (pllKp=0.15, pllKi=0.005). The PLL should be
COMBINED with a tempo estimator, not used alone.

The key insight: **PLL is the best approach for the phase ramp output**, but it needs
a tempo input from something else (ACF, comb filter, PLP, etc.).

---

## C. Adaptive Oscillator / Kuramoto Model

### How It Works

An adaptive Hopf oscillator is a nonlinear dynamical system that can entrain to
periodic input. The canonical equations (Righetti, Buchli, Ijspeert 2006):

```
dx/dt = (mu - r^2) * x - omega * y + epsilon * F(t)
dy/dt = (mu - r^2) * y + omega * x
d_omega/dt = -epsilon * F(t) * y / r
```

Where:
- (x, y) are the oscillator state (amplitude and phase)
- r = sqrt(x^2 + y^2) is the amplitude
- omega is the natural frequency (adapts via the learning rule)
- F(t) is the input forcing signal (onset function)
- epsilon controls coupling strength / learning rate
- mu controls the limit cycle amplitude (typically mu = 1)

The frequency learning rule (d_omega/dt) is Hebbian: it correlates the input with
the oscillator's quadrature component (y/r). When the input has a periodic component
near omega, the oscillator locks to it and the frequency stabilizes.

Phase is extracted as: phi = atan2(y, x)

### Kuramoto Coupling (Multiple Oscillators)

For beat tracking, you can use a bank of oscillators at different initial frequencies
(tempos). They couple to the input AND to each other:

```
d_theta_i/dt = omega_i + K/N * sum_j(sin(theta_j - theta_i)) + epsilon * F(t) * g(theta_i)
```

The mutual coupling (K term) encourages oscillators to synchronize, ideally to the
correct beat frequency. The oscillator bank can self-organize to track the dominant
periodicity.

### Analysis for Embedded

**Computational cost**: Each oscillator requires 3 ODE integrations per frame (dx, dy,
d_omega). With Euler integration, that is ~15 multiply-add operations per oscillator.
For a bank of 20 oscillators: ~300 operations per frame.
**Total: ~5-10 microseconds per frame. Very cheap.**

**Memory**: 3 floats per oscillator (x, y, omega). For 20 oscillators: 240 bytes.
**Trivial.**

**Phase stability**: Theoretically excellent once locked. The oscillator produces a
smooth limit cycle, and phase is derived from the continuous (x, y) trajectory. In
practice, the phase can exhibit small jitter because the Euler integration of a
nonlinear ODE is approximate, and the coupling to a noisy input signal causes
perturbations.

**Tempo tracking speed**: Controlled by epsilon. Higher epsilon = faster adaptation
but more sensitivity to noise. With a bank of oscillators, the one closest to the
true tempo locks fastest (within 2-5 beat cycles). However, there is a capture
range problem: if no oscillator is close to the true tempo, none of them may lock.

**Behavior during silence**: Oscillators continue at their learned frequencies (memory
property). This is good -- the phase ramp persists through breakdowns. However,
without input coupling, the oscillators may slowly drift apart if Kuramoto coupling
is weak.

**Octave error susceptibility**: HIGH. The frequency learning rule has no preference
for fundamental vs harmonic. An oscillator at 120 BPM and one at 60 BPM may both
lock to the input with equal strength. Selecting the "correct" one requires heuristics
(Rayleigh prior, onset density check) that are external to the oscillator model.

**Published F1**: No competitive F1 scores for beat tracking using pure adaptive
oscillators. The approach is more common in neuroscience (modeling human beat
perception) than in MIR. Kate Burgers' 2013 thesis ("Finding the Beat in Music:
Using Adaptive Oscillators") demonstrates the approach but does not report standard
MIR metrics.

**Complexity**: Medium. The ODE integration is simple, but tuning epsilon, mu, and
the coupling strength K is finicky. Too much coupling and all oscillators collapse
to the same frequency. Too little and they don't synchronize.

### Failure Modes

- Capture range: if true tempo is far from any oscillator's initial frequency, it may
  not lock at all
- Octave ambiguity (no inherent disambiguation)
- Tuning epsilon is critical: too high = noisy, too low = never locks
- Euler integration can cause drift in amplitude (r) unless mu is carefully set
- Non-stationary ODE: period of transient chaos during tempo changes before re-locking
- Bank of oscillators can mode-collapse (all lock to same harmonic)

### Verdict for This Project

Adaptive oscillators are elegant but under-proven for beat tracking. No published
system achieves competitive F1 with this approach alone. The frequency learning is
slow and susceptible to harmonics. For an MCU where simplicity and reliability matter
more than biological plausibility, this approach is risky. The same phase-tracking
benefit can be achieved with a simple PLL at lower complexity and better understood
failure modes.

Not recommended as a primary approach. Could be interesting as a future experiment.

---

## D. BTrack / CBSS (Adam Stark, 2014)

### How It Works

BTrack (Stark 2011 PhD thesis, Chapter 3) is the basis of the current Blinky firmware.
The algorithm has four stages:

1. **Onset Strength Signal (OSS)**: Buffer of onset detection values (6 seconds).

2. **Tempo Estimation (ACF + Comb Filter)**: Autocorrelate the OSS buffer to find
   periodic peaks. Apply a comb filter bank (Scheirer-style) to the ACF to enhance
   harmonically-related peaks. Select the lag with maximum energy as the tempo.
   A Rayleigh distribution prior biases toward moderate tempos (120-140 BPM).

3. **Cumulative Beat Strength Signal (CBSS)**: The core innovation. A recursive
   signal that accumulates onset evidence with temporal expectation:

   ```
   CBSS[n] = (1-alpha) * OSS[n] + alpha * max_{k in [T/2, 2T]} (w(k) * CBSS[n-k])
   ```

   Where w(k) is a log-Gaussian weighting centered at lag T (the estimated beat period).
   Tightness parameter controls how strictly the weighting penalizes off-beat lags.

   This means: the current CBSS value is a blend of the raw onset (new evidence) and
   the best CBSS value from approximately one beat ago (momentum/prediction). The
   log-Gaussian weighting makes it prefer values exactly one beat period back.

4. **Beat Prediction (Predict + Countdown)**: At the midpoint between beats, synthesize
   future CBSS values by propagating the recursion with zero onset input. Find the
   peak of this projected CBSS, weighted by a Gaussian expectation centered at the
   predicted beat time. Set the countdown timer. When the countdown reaches zero,
   fire a beat.

Phase is derived deterministically: `phase = (now - lastBeat) / beatPeriod`.

### The Current Blinky Implementation

The current firmware (AudioController.cpp, ~2162 lines) implements a heavily extended
version of BTrack:

- CBSS with log-Gaussian transition weights (recomputed when T changes)
- Bayesian tempo fusion: 20-bin posterior over ~60-198 BPM
  - ACF observation (weight 0.8) + comb filter bank observation (weight 0.7)
  - Gaussian transition matrix with harmonic shortcuts
  - Rayleigh prior peaked at 140 BPM
- Percival harmonic enhancement (fold 2nd/4th harmonics before comb-on-ACF)
- Onset-density octave discriminator (Gaussian penalty for implausible tempos)
- Shadow CBSS octave checker (compare T vs T/2 every 2 beats)
- PLL phase correction (Kp=0.15, Ki=0.005) on top of onset snap
- Onset snap with hysteresis (anchor beat to strongest nearby onset)
- Adaptive CBSS tightness (looser when onsets are strong, tighter in noise)
- ODF information gate (suppress weak NN output before CBSS)
- Beat-boundary tempo updates (defer period changes to next beat fire)
- CBSS warmup (lower alpha for first 8 beats for faster initial lock)
- CBSS adaptive threshold (suppress beats during silence)
- ODF pre-smoothing (5-point causal moving average)
- ~56 tunable parameters

### Analysis for Embedded

**Computational cost**: The expensive parts are:
- Autocorrelation of 360-sample OSS buffer: O(N*N) = 129,600 multiply-adds, every
  150ms. At 64 MHz, ~2ms per autocorrelation run.
- CBSS update: scan T/2 to 2T (worst case ~130 iterations) per frame, but with
  precomputed log-Gaussian weights. ~130 multiply-compares per frame. ~2-5 microseconds.
- Beat prediction: simulates ~T frames of future CBSS, each scanning ~130 lags.
  Worst case ~T*130 = ~12,000 operations. Runs once per beat (~2 Hz). ~0.2ms.
- Comb filter bank: 20 filters, 1 multiply-add each per frame. ~20 microseconds.
**Total per frame: ~5-20 microseconds (CBSS + comb). Autocorrelation amortized: ~0.3ms/frame.**

**Memory**: OSS buffer 360*4 = 1.4 KB, CBSS buffer 360*4 = 1.4 KB, comb filter bank
20*66*4 = 5.3 KB, Bayesian state 20*20*4 + 20*7*4 = 2.2 KB, misc state ~2 KB.
**Total: ~12-13 KB. Feasible.**

**Phase stability**: Good but not perfect. Phase is derived from the counter
(sampleCounter - lastBeat) / T, which produces a smooth ramp. But whenever a beat
fires, the lastBeat value jumps, which can cause small phase discontinuities if the
beat timing doesn't exactly match the previous period. The onset snap + PLL correction
help, but they also introduce their own small perturbations.

**Tempo tracking speed**: Moderate. The ACF runs every 150ms with 6 seconds of history.
New tempo evidence takes 2-4 seconds to dominate the autocorrelation. The Bayesian
posterior provides some smoothing/inertia. Beat-boundary tempo updates add further
latency (tempo change deferred to next beat). Total: 2-6 seconds for tempo change.

**Behavior during silence**: CBSS decays toward zero. The adaptive threshold suppresses
beat firing. Phase continues via the counter (free-running sawtooth). This is
reasonable behavior. When music resumes, the CBSS warmup phase gives fast re-lock.

**Octave error susceptibility**: Moderate, with extensive mitigations:
- Percival harmonic enhancement (fold ACF harmonics)
- Rayleigh prior (bias toward 140 BPM)
- Onset-density octave discriminator
- Shadow CBSS octave checker (compare T vs T/2 every 2 beats)
- Despite all this, octave errors still occur in practice

**Published F1**: BTrack's original paper does not report standard F1 on public datasets.
The OBTAIN paper (Mottaghi 2017, which uses similar CBSS) reports outperforming
"state-of-art results" but specific numbers are behind PDF walls. The causal PLP paper
reports BTrack-derived methods achieving ~75% F1 range on GTZAN.

**Complexity**: HIGH. The current implementation is 2162 lines of C++ with 56 tunable
parameters. The Bayesian fusion, multiple octave checks, adaptive tightness, PLL,
onset snap with hysteresis, and beat-boundary tempo updates create a deeply
interconnected system that is difficult to reason about and tune.

### Known Failure Modes (from A/B testing history)

- Octave errors persist despite extensive mitigation (~5-15% of tracks)
- ~135 BPM gravity well (Rayleigh prior + harmonic structure creates attractor)
- Slow convergence on tracks with weak periodicity
- Phase can "stutter" when onset snap shifts the beat anchor by several frames
- Beat-boundary tempo deferral can delay correction by up to one full beat period
- 56 parameters create a vast configuration space that's hard to globally optimize
- Each parameter was tuned in isolation; interactions between parameters are unknown

### Verdict for This Project

CBSS is the current system and has been extensively tested. It works but is complex.
The question is whether a simpler system could achieve similar or better results with
fewer moving parts. The CBSS recursion itself is elegant and effective -- the complexity
comes from the many bolt-on corrections (octave checks, PLL, onset snap, adaptive
tightness, etc.) that each fix a specific failure mode but add system complexity.

The phase ramp derivation is simple and effective: `(now - lastBeat) / T`.

---

## E. Comb Filter Resonator Bank

### How It Works (Scheirer 1998)

Scheirer's approach uses a bank of IIR comb filter resonators at different tempos:

```
y_k[n] = (1-alpha) * x[n] + alpha * y_k[n - L_k]
```

Where:
- y_k is the output of the k-th resonator
- x[n] is the input onset signal
- L_k is the delay (in samples) for the k-th tempo hypothesis
- alpha controls resonance strength (0.85-0.98)

Each resonator accumulates energy when the input has periodicity matching its delay.
The resonator with maximum energy indicates the dominant tempo.

**Phase extraction**: The key advantage over ACF is that comb filter resonators preserve
phase information. The output y_k[n] is a filtered version of the input at period L_k.
Phase can be extracted by:
1. Finding the peak position in the resonator's recent output history
2. Computing a DFT at the resonator's frequency on its output history
3. Using the complex exponential: correlate y_k with exp(-j*2*pi*n/L_k) over a window

The current Blinky firmware already implements this (CombFilterBank class, extractPhase()
using Fourier method every 4 frames).

### Analysis for Embedded

**Computational cost**: Per frame, each of N resonators needs 1 multiply-add
(the IIR update) plus 1 delay-line read. For 20 resonators: 20 multiply-adds +
20 memory reads. Phase extraction (Fourier method every 4 frames): ~66 complex
multiply-adds per extraction. **Total: ~2-5 microseconds per frame. Very cheap.**

**Memory**: Each resonator needs a delay line of length L_max. For 60 BPM at 62.5 Hz,
L_max = 62.5 * 60/60 = 62.5 -> 63 samples. For 20 resonators: 20 * 63 * 4 = 5.0 KB.
Plus output energy tracking: 20 * 4 = 80 bytes.
**Total: ~5.1 KB. Same as current implementation.**

**Phase stability**: The resonator output is inherently smooth (IIR filter). Phase
extracted via Fourier method is also smooth (averaged over a window). However, the
phase is only meaningful when the resonator has significant energy (tempo match).
If the wrong resonator wins (octave error), the phase is locked to the wrong tempo.

**Tempo tracking speed**: The IIR resonator has an exponential time constant controlled
by alpha. At alpha=0.92 and 120 BPM (L=31 frames), the resonator's effective memory
is about L / (1-alpha) = 31/0.08 = ~387 frames = ~6 seconds. This means tempo changes
take several seconds to propagate. Lowering alpha speeds adaptation but reduces
selectivity (wider bandwidth per resonator).

**Behavior during silence**: Resonator outputs decay exponentially. No false beats.
The resonator "remembers" the last tempo for several seconds (good for short breakdowns).

**Octave error susceptibility**: HIGH. A resonator at period L resonates equally well
to input with period L, 2L, 3L, etc. (sub-harmonics). And a resonator at period 2L
will also resonate to input at period L (because every other input matches). The bank
provides no inherent disambiguation. You need external heuristics (Rayleigh prior,
onset density) -- same as all other approaches.

**Published F1**: Scheirer's 1998 paper reports performance "similar to human listeners"
in a small validation experiment but does not use modern F1/continuity metrics.

**Complexity**: Low for the basic bank. Medium if you add phase extraction and tempo
selection heuristics.

### The Comb Filter + PLL Hybrid

The most promising approach may be: use the comb filter bank for tempo estimation,
and feed the winning tempo into a PLL for phase tracking. The comb filter provides
continuous tempo monitoring (every frame), while the PLL provides smooth phase.

This is essentially what the current firmware does (comb bank feeds Bayesian fusion,
which feeds CBSS, which derives phase). But the indirection through CBSS adds
complexity. A direct comb-to-PLL path might be simpler.

### Verdict for This Project

The comb filter bank is already implemented and useful for tempo estimation. Its
phase output is decent but not as smooth as a PLL's free-running sawtooth. Best
used as a tempo estimator feeding into a simpler phase tracking system.

---

## F. Autocorrelation + Simple Counter

### How It Works

The simplest possible beat tracker:

1. **Tempo estimation**: Autocorrelate the OSS buffer. The lag with the highest peak
   (in the allowed BPM range) is the tempo.

2. **Beat counting**: Maintain a phase counter that increments by `1/T` each frame
   (where T = beat period in frames). When the counter exceeds 1.0, a beat fires
   and the counter wraps to 0.

3. **Phase correction**: When an onset is detected near the predicted beat time,
   shift the counter slightly toward the onset (soft correction). This is essentially
   a very simple PLL.

### Analysis for Embedded

**Computational cost**: ACF is O(N*K) where N is the buffer size and K is the
search range. For N=360, K=200 (all lags): ~72,000 multiply-adds every 150ms.
Counter increment: 1 add per frame. **Total: 0.2ms amortized per frame. Very cheap.**

**Memory**: 360-sample buffer (1.4 KB) + counter state (12 bytes).
**Total: ~1.4 KB. Minimal.**

**Phase stability**: Very good. The counter produces a perfectly smooth sawtooth.
Phase corrections at onset events introduce small jumps, but these can be smoothed
with a low-pass or simply kept small (Kp < 0.1).

**Tempo tracking speed**: Same as CBSS -- depends on ACF window length (6 seconds).
New tempo shows up in ACF in 2-4 seconds.

**Behavior during silence**: Counter free-runs at last known tempo. Perfect for
visualizer. When music resumes, ACF re-estimates tempo; phase correction re-aligns.

**Octave error susceptibility**: HIGH. ACF has strong harmonics -- the peak at lag L
always has a secondary peak at 2L (half-time). The standard mitigations apply:
Rayleigh prior, harmonic enhancement (Percival), onset density check.

**Published F1**: No standalone ACF+counter system has competitive published F1.
The approach is too simple for academic publishing but is common in practical
implementations (many LED reactive systems use exactly this).

**Complexity**: Very low. ~50 lines of code for the core system.

### Robustness

ACF+counter is robust for music with clear, consistent beats (EDM, pop, rock). It
breaks down with:
- Music with ambiguous tempo (jazz, classical, ambient)
- Polyrhythmic music (competing periodicities in the ACF)
- Tempo changes (requires ACF window to "forget" old tempo)
- Noisy ODF (false onsets corrupt the ACF)

### Verdict for This Project

ACF+counter is the simplest viable approach. Its main weakness is octave ambiguity
in the ACF, which requires the same heuristics that CBSS uses. The question is
whether CBSS's recursive structure provides meaningful benefit over plain ACF
for tempo estimation. For phase tracking specifically, the counter approach is
essentially the same as what CBSS already does (phase = (now - lastBeat) / T).

---

## G. Recent Approaches (2020-2025)

### BeatNet (Heydari et al., ISMIR 2021)

- CRNN activation + particle filtering for causal beat/downbeat/meter tracking
- F1: ~80% on GTZAN (state-of-art for online methods)
- Uses "information gate" to reduce particle filter computational cost (only resample
  when onset is detected)
- **Not feasible on MCU**: CRNN requires ~10-50ms inference on GPU. Particle filter
  with 1000 particles is too expensive for Cortex-M4F. However, the information gate
  concept is already used in Blinky firmware (suppress weak NN output before CBSS).

### Real-Time PLP (Meier, Krause, Mueller, TISMIR 2024)

- Causal PLP with zero latency option
- F1 = 74.7% on GTZAN, 0ms latency variant
- **Feasible on MCU** (see Section A above)
- Provides beat stability metric and local tempo
- Available Python implementation at https://github.com/groupmm/real_time_plp

### Beat-and-Tempo-Tracking (Krzyzaniak, ongoing)

- ANSI C library, designed for embedded Linux in musical robots
- Runs in 20% of real-time on a laptop (7% for tempo scoring)
- Uses generalized autocorrelation (GAC) with spectral compression:
  `GAC(oss) = IFFT(FFT(oss^0.5))`
- Scores tempo candidates by cross-correlating OSS with synthetic pulse trains
- Maintains tempo via decaying Gaussian histogram
- **Feasible on MCU** with reduced buffer sizes

### "Don't Look Back" (Heydari & Duan, 2020)

- RNN-based online beat tracking without dynamic programming
- Uses causal convolutional layers + recurrent network
- Not feasible on MCU (requires NN inference for tracking, not just ODF)

### Key Trend

The academic trend is toward NN-based activation + lightweight tracking. The ODF
quality is more important than the tracking algorithm. With a good NN ODF (which
Blinky already has), even simple tracking methods achieve good results. The causal
PLP paper demonstrates this: F1 jumps from 74.7% to 91.9% when using ground-truth
activation instead of a learned one.

---

## Comparative Summary

| Criterion | PLP | PLL | Adaptive Osc | CBSS (current) | Comb Filter | ACF+Counter |
|-----------|-----|-----|-------------|----------------|-------------|-------------|
| **Ops/frame** | ~10K (FFT) | ~5 | ~300 | ~200 | ~50 | ~5 (counter) |
| **ACF cost** | none | none | none | 130K/150ms | none | 72K/150ms |
| **Memory** | ~8 KB | ~20 B | ~240 B | ~13 KB | ~5 KB | ~1.4 KB |
| **Phase smoothness** | Excellent | Excellent | Good | Good | Good | Excellent |
| **Tempo track speed** | 3-6 sec | N/A* | 2-5 beats | 2-6 sec | 4-8 sec | 2-4 sec |
| **Silence behavior** | Stops | Free-runs | Free-runs | Decays+freerun | Decays | Free-runs |
| **Octave errors** | Moderate | High* | High | Moderate | High | High |
| **Published F1** | 74.7% | N/A | N/A | ~75% | N/A | N/A |
| **Implementation** | Medium | Trivial | Medium | High (2162 loc) | Low-Medium | Trivial |
| **# Parameters** | ~5 | 2 (Kp, Ki) | 3 (eps, mu, K) | ~56 | ~3 | ~3 |

*PLL requires external tempo input; does not estimate tempo on its own.

---

## Recommendations

### Option 1: Simplify Current System (Lowest Risk)

Keep CBSS but strip unnecessary complexity:

1. Remove Bayesian posterior (use ACF peak directly with Rayleigh prior weighting)
2. Remove comb filter bank observation fusion (keep bank for monitoring only)
3. Remove adaptive tightness (use fixed tightness)
4. Remove onset snap hysteresis (use simple snap)
5. Remove beat-boundary tempo deferral (update immediately)
6. Keep: CBSS core, log-Gaussian weighting, PLL correction, ODF gate, octave checker

This could reduce the system from ~56 parameters to ~15-20 and from ~2162 lines to
~800-1000 lines, while retaining the proven CBSS beat detection.

**Risk**: Low. Each simplification can be A/B tested.
**Benefit**: Easier to reason about, tune, and maintain.

### Option 2: PLP + PLL Hybrid (Medium Risk, Potentially Smoother Phase)

Replace CBSS with causal PLP for beat detection, keep PLL for phase smoothing:

1. Buffer 6 seconds of ODF (375 samples at 62.5 Hz)
2. Every frame, compute 512-point FFT of the ODF buffer (Fourier tempogram)
3. Select peak frequency in allowed tempo range -> tempo estimate
4. Keep only phase of peak frequency -> beat phase signal
5. Apply inverse FFT overlap-add to get PLP curve
6. Detect beats as PLP peaks above adaptive threshold
7. Feed beat times into PLL for smooth phase ramp
8. Onset pulse detection remains independent (floor-tracking baseline)

**Advantages**:
- PLP's overlap-add produces inherently smooth phase (no onset snap discontinuities)
- Fewer parameters (~8-10)
- Well-documented algorithm with reference implementation
- FFT is well-optimized on Cortex-M4F (CMSIS-DSP)

**Disadvantages**:
- Requires 512-point FFT per frame (~0.3ms, 30% of budget)
- Tempo tracking is slower than CBSS (6-second window, no prediction)
- No built-in octave disambiguation (need to add Rayleigh prior to tempogram)
- PLP curve stops during silence (need PLL to maintain phase)
- Less tested on this hardware/input

**Memory**: ~8 KB (FFT buffers) + ~1.4 KB (ODF) + ~20 B (PLL) = ~10 KB

### Option 3: ACF + Comb Validation + PLL (Simplest Viable)

The simplest system that could work well:

1. Buffer 6 seconds of ODF
2. ACF every 150ms -> tempo candidates (with Rayleigh prior + Percival enhancement)
3. Comb filter bank (20 resonators) for continuous tempo validation
4. Simple tempo selector: prefer ACF peak when strong, comb peak when ACF is ambiguous
5. PLL for phase tracking (free-running sawtooth, corrected at onset events)
6. Beat = when PLL phase wraps from 1 -> 0
7. Onset pulse from ODF floor-tracking baseline (already implemented)

**Advantages**:
- Very simple: ~200-300 lines of core code
- ~8-10 parameters
- Phase is perfectly smooth (PLL free-running)
- ACF + comb provide two independent tempo estimates
- Comb bank is already implemented

**Disadvantages**:
- No CBSS-style beat prediction (beats declared by PLL phase wrap, not CBSS peak)
- Octave disambiguation still needed (same heuristics)
- PLL phase correction at onsets could cause small discontinuities
- Less sophisticated beat timing than CBSS (no onset snap, no predict+countdown)

**Memory**: ~1.4 KB (OSS) + ~5 KB (comb bank) + ~20 B (PLL) = ~6.5 KB

### Option 4: Comb Filter Bank as Primary (Novel)

Make the comb filter bank the primary beat tracker, not just a tempo validator:

1. 20-40 IIR comb resonators covering 60-200 BPM
2. Each resonator: y[n] = (1-a)*x[n] + a*y[n-L]
3. Find resonator with maximum energy -> tempo
4. Extract phase from winning resonator (Fourier method)
5. Use extracted phase as the beat phase ramp directly
6. No ACF needed (comb bank IS the tempo estimator)
7. Beat = phase zero crossing

**Advantages**:
- Simplest possible system: just a filter bank
- Continuous operation (no periodic ACF recomputation)
- Phase and tempo from the same source (inherently consistent)
- Very cheap: ~50 ops/frame + phase extraction every 4 frames

**Disadvantages**:
- Phase from comb filter is noisier than PLL free-running
- Comb bank has poor octave disambiguation (harmonics resonate too)
- Fewer proven results than CBSS-based approaches
- Tempo resolution limited by number of resonators
- 20 resonators gives ~7 BPM resolution at 120 BPM (coarse)

**Memory**: ~5 KB (resonator delay lines) + ~200 B (state) = ~5.2 KB

---

## Overall Assessment

### What Matters Most for a Visualizer

1. **Phase smoothness** (no jitter in the breathing/pulsing animation)
2. **Correct tempo** (not half-time or double-time)
3. **Graceful degradation** (keep running during breakdowns)
4. **Fast initial lock** (start pulsing in rhythm within 2-3 seconds)

### The Core Insight

**The phase ramp quality is primarily determined by the phase tracking mechanism, not
the tempo estimator.** CBSS, ACF, PLP, and comb filters all need roughly the same
amount of time to determine tempo (2-6 seconds). What differs is how they produce
the phase ramp.

- **PLP**: Phase is inherently smooth (overlap-add of sinusoids)
- **PLL/Counter**: Phase is inherently smooth (free-running accumulator)
- **CBSS-derived**: Phase is derived from beat counter + onset snap, which can stutter
- **Comb filter**: Phase is from Fourier extraction, moderately smooth

### My Recommendation

**Option 3 (ACF + Comb + PLL)** is the best balance of simplicity and quality for this
application. The PLL produces the smoothest possible phase ramp. The ACF + comb provide
redundant tempo estimation. The system has very few parameters to tune.

If the 512-point FFT per frame is acceptable, **Option 2 (PLP + PLL)** may produce
better results because PLP's frequency-domain approach is more robust to noisy ODF
than time-domain ACF.

**Option 1 (simplified CBSS)** is the safest path since it preserves the proven system
while reducing complexity.

The adaptive oscillator approach (Option C) is not recommended -- it offers no concrete
advantage over a PLL while being harder to tune and reason about.

---

## Sources

- [Grosche & Mueller PLP (AudioLabs)](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S3_PredominantLocalPulse.html)
- [Real-Time PLP 2024 (TISMIR)](https://transactions.ismir.net/articles/10.5334/tismir.189)
- [Real-Time PLP Implementation (GitHub)](https://github.com/groupmm/real_time_plp)
- [Librosa PLP Documentation](https://librosa.org/doc/main/generated/librosa.beat.plp.html)
- [Shiu & Kuo 2007 PLL Beat Tracking (IEEE)](https://ieeexplore.ieee.org/document/4145989)
- [BTrack GitHub (Adam Stark)](https://github.com/adamstark/BTrack)
- [OBTAIN Beat Tracking (arXiv)](https://arxiv.org/abs/1704.02216)
- [Scheirer 1998 Comb Filter Beat Tracking (JASA)](https://pubmed.ncbi.nlm.nih.gov/9440344/)
- [Comb Filter Matrix Beat Tracking (Stark 2011)](https://www.adamstark.co.uk/pdf/papers/comb-filter-matrix-ICMC-2011.pdf)
- [BeatNet CRNN + Particle Filtering (ISMIR 2021)](https://github.com/mjhydri/BeatNet)
- [Adaptive Hopf Oscillator Arduino (GitHub)](https://github.com/MartinStokroos/adaptiveFreqOsc)
- [Adaptive Frequency Oscillators (EPFL)](https://www.epfl.ch/labs/biorob/research/neuromechanical/page-36365-en-html/)
- [Hopf Oscillator Equations (Tichko Tutorial)](https://ptichko.github.io/2022/03/14/Adaptive-Frequency-Oscillators.html)
- [Beat-and-Tempo-Tracking ANSI C Library (GitHub)](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking)
- [Davies & Plumbley Causal Tempo Tracking (ISMIR 2004)](https://archives.ismir.net/ismir2004/paper/000226.pdf)
- [Beat Tracking Octave Errors (ISMIR 2010)](https://archives.ismir.net/ismir2010/paper/000019.pdf)
- [Kuramoto Model (Wikipedia)](https://en.wikipedia.org/wiki/Kuramoto_model)
- [Finding the Beat Using Adaptive Oscillators (Burgers 2013)](https://scholarship.claremont.edu/hmc_theses/3/)
