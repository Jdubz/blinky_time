# Rhythm Analysis Enhancement Plan
## Onset Strength Signal (OSS) Buffering for Improved Music Mode Reliability

**Date**: 2025-12-27 (Updated)
**Status**: Phase 1 Complete, Implementation In Progress
**Priority**: High - Addresses core MusicMode reliability issues

**🎯 IMPLEMENTATION NOTE**: This plan now follows the **industry-standard OSS buffering approach** (librosa/aubio/BTrack method) instead of the originally proposed multi-band energy filtering. Phase 1 (RhythmAnalyzer implementation) is complete.

---

## Executive Summary

Current MusicMode relies entirely on discrete transient/onset events for beat tracking. When transient detection misses beats or produces false positives, MusicMode loses synchronization or fails to activate entirely. This document proposes a **hybrid continuous/discrete system** that uses **Onset Strength Signal (OSS) buffering** - the industry-standard approach used by librosa, aubio, and BTrack.

**Key Innovation**: Buffer spectral flux output (Onset Strength Signal) for autocorrelation-based tempo detection, enabling beat confirmation, missed beat recovery, and predictive beat synthesis.

**Why This Approach**:
- Industry standard (proven in librosa, aubio, BTrack)
- Leverages existing spectral flux implementation (Mode 3)
- Simpler than multi-band filtering
- More robust than inter-onset interval histograms

**Resource Impact**:
- RAM: +1 KB (256 frames × 4 bytes)
- CPU: +2% @ 64 MHz (autocorrelation every 1 second)
- Latency: No increase

---

## Table of Contents

1. [Current System Analysis](#current-system-analysis)
2. [Research Findings](#research-findings)
3. [Proposed Architecture](#proposed-architecture)
4. [Integration Points](#integration-points)
5. [Implementation Phases](#implementation-phases)
6. [Technical Specifications](#technical-specifications)
7. [Performance Analysis](#performance-analysis)
8. [References](#references)

---

## Current System Analysis

### Architecture Overview

**MusicMode** (`blinky-things/music/MusicMode.h`, `MusicMode.cpp`)
- **Primary Input**: Discrete transient events from `AdaptiveMic::transient` (single-frame impulses)
- **Tempo Estimation**: Histogram-based autocorrelation on inter-onset intervals (IOI)
  - Circular buffer: 63 intervals (MusicMode.h:95)
  - Histogram: 40 bins covering 60-200 BPM (MusicMode.cpp:179)
  - Updates every 8 onsets (MusicMode.cpp:166)
- **Beat Tracking**: Phase-Locked Loop (PLL) for phase synchronization
  - Proportional gain: `pllKp = 0.1f` (MusicMode.h:51)
  - Integral gain: `pllKi = 0.01f` (MusicMode.h:52)
  - Error integral anti-windup: ±10.0 (MusicMode.cpp:141)
- **Activation Logic**: Confidence-based (MusicMode.h:42-44)
  - Requires 4 stable beats to activate (MusicMode.h:43)
  - Allows 8 missed beats before deactivation (MusicMode.h:44)
  - Confidence threshold: 0.6 (MusicMode.h:42)

**AdaptiveMic Transient Detection** (`blinky-things/inputs/AdaptiveMic.h`, `AdaptiveMic.cpp`)
- **Mode 0 (DRUMMER)**: Amplitude-based detection (AdaptiveMic.cpp:362-392)
  - LOUD: `rawLevel > recentAverage * transientThreshold` (default 2.0x)
  - SUDDEN: `rawLevel > baselineLevel * attackMultiplier` (default 1.2x, ~50-70ms lookback)
  - INFREQUENT: 30ms cooldown (AdaptiveMic.h:77)
- **Mode 3 (SPECTRAL_FLUX)**: FFT-based spectral difference (AdaptiveMic.cpp:479-556)
  - 256-point FFT on raw audio samples
  - Analyzes configurable number of bins (default 64 for bass-mid focus)
  - Threshold: `flux > fluxRecentAverage * fluxThresh` (default 2.8x)
- **Mode 4 (HYBRID)**: Combines drummer + spectral flux (AdaptiveMic.cpp:558-680)
  - Confidence weighting when both agree
  - Tuned via param-tuner (F1: 0.705)

**Integration Point**: `MusicMode::onOnsetDetected()` (MusicMode.cpp:119-171)
- Called by main loop when `AdaptiveMic::transient > 0.0f`
- Updates IOI buffer, adjusts BPM via PLL, updates confidence

### Critical Limitation

**Single Point of Failure**: MusicMode sees only discrete "hit" events, not the underlying audio signal.

```
Transient stream: [ 1.0,  0,  0,  0, 0.8,  0,  0,  0, 0,  0 ]
                    ↑             ↑                      ↑ <- Missed beat!

MusicMode loses sync →  Confidence drops →  Deactivates
```

When transient detection:
- **Misses beats** → MusicMode thinks tempo slowed down
- **False positives** → MusicMode thinks tempo sped up
- **Inconsistent** → MusicMode never reaches activation threshold

---

## Research Findings

### Modern Beat Tracking Techniques

#### 1. Buffered Onset Strength Signals (OSS)

**Source**: Meier, Chiu, & Müller (2024). "A Real-Time Beat Tracking System with Zero Latency and Enhanced Controllability". *Transactions of the International Society for Music Information Retrieval*. [[PDF]](https://transactions.ismir.net/articles/189/files/66fc0db587be3.pdf)

**Key Insight**: Modern systems buffer Onset Strength Signals (OSS) in 256-sample windows, enabling:
- Retroactive analysis and pattern confirmation
- Beat lookahead/lookback for improved accuracy
- Periodicity detection via autocorrelation on continuous signal (more robust than IOI histogram)

**Implementation**: Systems continuously execute real-time procedures using data buffering to update the model for every new frame of audio. Decision line can be shifted to any time position by introducing beat lookahead.

#### 2. Multi-Band Onset Detection

**Source**: librosa development team. "onset_strength_multi". [[Documentation]](https://librosa.org/doc/main/generated/librosa.onset.onset_strength_multi.html)

**Key Insight**: Different musical elements occupy different frequency ranges:
- **Bass (20-200 Hz)**: Kick drums, bass guitar, sub-bass
- **Mid (200-2000 Hz)**: Snare, toms, vocals, guitar
- **High (2000-8000 Hz)**: Hi-hats, cymbals, percussion

Analyzing bands separately enables:
- Better rejection of hi-hat false positives (high band vs bass band)
- Genre-adaptive detection (emphasize bass for EDM, mid for rock)
- Multiple independent tempo estimates for confidence boosting

#### 3. Power-Scaled Spectral Flux (PSSF)

**Source**: Glover et al. (2014). "Power-Scaled Spectral Flux and Peak-Valley Group-Delay Methods for Robust Musical Onset Detection". *ICMC*. [[PDF]](https://quod.lib.umich.edu/i/icmc/bbp2372.2014.184/1/--power-scaled-spectral-flux-and-peak-valley-group-delay)

**Key Insight**: Standard spectral flux suffers from wide dynamic range in spectrograms. Power scaling balances magnitudes across frequency bins, improving robustness.

**Formula**: `PSSF(k) = |X(k)|^α - |X_prev(k)|^α` where `α ∈ [0.5, 0.8]`

**Benefit**: More consistent onset detection across different loudness levels and frequency content.

#### 4. Lightweight Embedded Implementations

**BTrack** - Real-Time C++ Beat Tracker
**Source**: Stark, A. "BTrack: A Real-Time Beat Tracker". [[GitHub]](https://github.com/adamstark/BTrack)
**Features**:
- Causal algorithm for real-time use
- Comb filter bank for periodicity detection
- Successfully deployed on embedded platforms

**aubio** - Audio Annotation Library
**Source**: Brossier, P. "aubio, a library for audio labelling". [[Website]](https://aubio.org/)
**Features**:
- Written in C, runs on ARM, x86, etc.
- Causal algorithms with low delay
- Proven track record in embedded MIR systems

**OBTAIN** - Real-Time Beat Tracking
**Source**: Alonso, Cont, & David (2017). "OBTAIN: Real-Time Beat Tracking in Audio Signals". [[arXiv]](https://arxiv.org/pdf/1704.02216)
**Features**:
- Implemented on embedded systems
- Uses probabilistic methods for robustness
- Low computational overhead

#### 5. Continuous Energy Envelopes vs Discrete Events

**Source**: Ellis, D. (2013). "Music Signal Processing - Lecture 10: Beat Tracking". Columbia University. [[PDF]](https://www.ee.columbia.edu/~dpwe/e4896/lectures/E4896-L10.pdf)

**Key Insight**: Autocorrelation on continuous onset strength functions is more robust than interval-based methods.

**Comparison**:
```
Discrete IOI:      [500ms, 510ms, 495ms, 520ms, 2000ms] ← Outlier ruins histogram
Continuous OSS:    [0.2, 0.5, 0.8, 0.6, 0.3, 0.1, 0.4] ← Autocorrelation robust to outliers
```

Continuous analysis:
- Handles missed beats gracefully (valleys in signal, not missing data)
- Provides amplitude modulation information (energy dynamics)
- Enables prediction (extrapolate periodic pattern)

---

## Proposed Architecture

### Overview: Hybrid Continuous/Discrete System (OSS Buffering)

```
┌─────────────────────────────────────────────────────────────────┐
│                        AUDIO INPUT (ISR)                        │
│                     int16_t samples @ 16kHz                     │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ↓
                ┌─────────────────────┐
                │  AdaptiveMic        │
                │  (EXISTING)         │
                │                     │
                │  Spectral Flux      │
                │  (Mode 3)           │
                │  • FFT analysis     │
                │  • Onset detection  │
                │  • Returns flux     │
                │    value            │
                └──────┬───────┬──────┘
                       │       │
        Discrete ──────┘       └────── Continuous
        (transient)              (spectral flux)
                       │       │
         ┌─────────────┘       └─────────────┐
         ↓                                   ↓
┌──────────────────┐           ┌──────────────────┐
│  MusicMode       │           │  RhythmAnalyzer  │
│  (EXISTING)      │           │  (NEW)           │
│                  │           │                  │
│  IOI Histogram   │◄──────────┤  OSS Buffer      │
│  PLL Phase Sync  │           │  (256 samples)   │
│  Confidence      │           │                  │
│  Activation      │           │  Autocorrelation │
│                  │──────────►│  Periodicity     │
│                  │           │  Beat Likelihood │
└────────┬─────────┘           └────────┬─────────┘
         │                              │
         │  Discrete beats              │  Pattern info
         │  + Virtual beats             │  + Tempo estimate
         │                              │
         └──────────────┬───────────────┘
                        ↓
               ┌─────────────────┐
               │  BEAT OUTPUT    │
               │  • quarterNote  │
               │  • halfNote     │
               │  • wholeNote    │
               │  • phase (0-1)  │
               │  • bpm          │
               └─────────────────┘
```

**Key Insight**: Your existing spectral flux (Mode 3) already produces an Onset Strength Signal. We just need to **buffer it** and run **autocorrelation** on the buffer.

### New Component: RhythmAnalyzer Class (OSS Buffering)

**Purpose**: Buffer continuous features and perform retroactive pattern analysis.

**File**: Create `blinky-things/music/RhythmAnalyzer.h` and `RhythmAnalyzer.cpp`

**Interface**:
```cpp
#pragma once
#include <stdint.h>

/**
 * RhythmAnalyzer - Continuous audio pattern analysis for beat tracking
 *
 * Buffers multi-band energy envelopes and performs:
 * - Autocorrelation for periodicity detection
 * - Beat likelihood prediction
 * - Retroactive beat confirmation
 *
 * Designed for minimal resource usage:
 * - RAM: 3 KB (256 frames × 3 bands × 4 bytes)
 * - CPU: ~2% @ 64 MHz (autocorrelation every 1 sec)
 */
class RhythmAnalyzer {
public:
    // ===== CONFIGURATION =====

    static constexpr int BUFFER_SIZE = 256;  // 256 frames @ 60 Hz = ~4.3 seconds

    // Tempo range for autocorrelation (matches MusicMode)
    float minBPM = 60.0f;
    float maxBPM = 200.0f;

    // Autocorrelation update rate (reduce CPU by analyzing less frequently)
    uint32_t autocorrUpdateIntervalMs = 1000;  // 1 second

    // Beat likelihood threshold for virtual beat synthesis
    float beatLikelihoodThreshold = 0.7f;

    // ===== PUBLIC STATE =====

    // Detected periodicity from autocorrelation
    float detectedPeriodMs = 0.0f;      // Period in milliseconds (0 = no pattern)
    float periodicityStrength = 0.0f;   // Confidence in detected period (0-1)

    // Current beat likelihood (0-1, based on periodic pattern and phase)
    float beatLikelihood = 0.0f;

    // ===== PUBLIC METHODS =====

    RhythmAnalyzer();

    /**
     * Add new frame of multi-band energy (call every frame @ ~60 Hz)
     */
    void addFrame(float bassEnergy, float midEnergy, float highEnergy);

    /**
     * Update autocorrelation and periodicity detection
     * Call periodically (every 1 sec) to reduce CPU load
     * Returns true if pattern detected
     */
    bool update(uint32_t nowMs, float frameRate);

    /**
     * Get beat likelihood at current time
     * Based on detected period and current phase
     * Returns 0.0 if no pattern, 0.0-1.0 otherwise
     */
    float getBeatLikelihood() const;

    /**
     * Retroactively confirm if beat occurred N frames ago
     * Checks if energy spike occurred at expected time
     */
    bool confirmPastBeat(int framesAgo, float threshold);

    /**
     * Reset all state
     */
    void reset();

private:
    // Circular buffers for multi-band energy history
    float bassHistory_[BUFFER_SIZE];
    float midHistory_[BUFFER_SIZE];
    float highHistory_[BUFFER_SIZE];
    int writeIdx_ = 0;
    int frameCount_ = 0;  // Total frames written (for initialization)

    // Autocorrelation state
    uint32_t lastAutocorrMs_ = 0;

    // Helper: Autocorrelation on single band
    void autocorrelate(const float* signal, int length,
                      float minPeriod, float maxPeriod,
                      float& outPeriod, float& outStrength);

    // Helper: Get sample from circular buffer (with wraparound)
    inline float getSample(const float* buffer, int framesAgo) const {
        int idx = (writeIdx_ - 1 - framesAgo + BUFFER_SIZE) % BUFFER_SIZE;
        return buffer[idx];
    }
};
```

**Implementation** (RhythmAnalyzer.cpp):

```cpp
#include "RhythmAnalyzer.h"
#include <math.h>

RhythmAnalyzer::RhythmAnalyzer() {
    reset();
}

void RhythmAnalyzer::reset() {
    writeIdx_ = 0;
    frameCount_ = 0;
    detectedPeriodMs = 0.0f;
    periodicityStrength = 0.0f;
    beatLikelihood = 0.0f;
    lastAutocorrMs_ = 0;

    for (int i = 0; i < BUFFER_SIZE; i++) {
        bassHistory_[i] = 0.0f;
        midHistory_[i] = 0.0f;
        highHistory_[i] = 0.0f;
    }
}

void RhythmAnalyzer::addFrame(float bassEnergy, float midEnergy, float highEnergy) {
    bassHistory_[writeIdx_] = bassEnergy;
    midHistory_[writeIdx_] = midEnergy;
    highHistory_[writeIdx_] = highEnergy;

    writeIdx_ = (writeIdx_ + 1) % BUFFER_SIZE;
    if (frameCount_ < BUFFER_SIZE) frameCount_++;
}

bool RhythmAnalyzer::update(uint32_t nowMs, float frameRate) {
    // Throttle autocorrelation to reduce CPU
    if ((int32_t)(nowMs - lastAutocorrMs_) < (int32_t)autocorrUpdateIntervalMs) {
        return false;  // Not time yet
    }
    lastAutocorrMs_ = nowMs;

    // Need full buffer for reliable autocorrelation
    if (frameCount_ < BUFFER_SIZE) {
        return false;
    }

    // Convert BPM range to frame periods
    float msPerFrame = 1000.0f / frameRate;  // e.g., 16.67 ms @ 60 Hz
    float minPeriodFrames = (60000.0f / maxBPM) / msPerFrame;  // e.g., 18 frames @ 200 BPM
    float maxPeriodFrames = (60000.0f / minBPM) / msPerFrame;  // e.g., 60 frames @ 60 BPM

    // Autocorrelation on bass band (most reliable for tempo)
    float bassPeriod, bassStrength;
    autocorrelate(bassHistory_, BUFFER_SIZE, minPeriodFrames, maxPeriodFrames,
                  bassPeriod, bassStrength);

    // Store results
    if (bassStrength > 0.5f) {  // Confidence threshold
        detectedPeriodMs = bassPeriod * msPerFrame;
        periodicityStrength = bassStrength;
        return true;
    } else {
        detectedPeriodMs = 0.0f;
        periodicityStrength = 0.0f;
        return false;
    }
}

void RhythmAnalyzer::autocorrelate(const float* signal, int length,
                                   float minPeriod, float maxPeriod,
                                   float& outPeriod, float& outStrength) {
    // Simple autocorrelation: R(lag) = sum(signal[i] * signal[i - lag])
    // Search for peak in autocorrelation within [minPeriod, maxPeriod]

    float maxCorr = 0.0f;
    int bestLag = 0;

    int minLag = (int)minPeriod;
    int maxLag = (int)maxPeriod;
    if (maxLag >= length) maxLag = length - 1;

    for (int lag = minLag; lag <= maxLag; lag++) {
        float corr = 0.0f;
        int count = 0;

        for (int i = lag; i < length; i++) {
            corr += signal[i] * signal[i - lag];
            count++;
        }

        if (count > 0) {
            corr /= count;  // Normalize by number of samples

            if (corr > maxCorr) {
                maxCorr = corr;
                bestLag = lag;
            }
        }
    }

    // Normalize strength (0-1 range)
    // Compare to autocorrelation at lag=0 (signal energy)
    float energy = 0.0f;
    for (int i = 0; i < length; i++) {
        energy += signal[i] * signal[i];
    }
    energy /= length;

    outPeriod = (float)bestLag;
    outStrength = (energy > 0.0f) ? (maxCorr / energy) : 0.0f;

    // Clamp strength to [0, 1]
    if (outStrength > 1.0f) outStrength = 1.0f;
    if (outStrength < 0.0f) outStrength = 0.0f;
}

float RhythmAnalyzer::getBeatLikelihood() const {
    if (detectedPeriodMs <= 0.0f || periodicityStrength < 0.5f) {
        return 0.0f;  // No pattern detected
    }

    // Check current bass energy against recent average
    // If we're at a beat position, energy should be elevated
    float currentBass = getSample(bassHistory_, 0);  // Most recent frame

    // Compute average over one period
    int periodFrames = (int)(detectedPeriodMs / 16.67f);  // Assuming ~60 Hz
    float avgBass = 0.0f;
    for (int i = 1; i <= periodFrames && i < BUFFER_SIZE; i++) {
        avgBass += getSample(bassHistory_, i);
    }
    avgBass /= periodFrames;

    // Beat likelihood: current energy relative to period average
    float ratio = (avgBass > 0.0f) ? (currentBass / avgBass) : 0.0f;

    // Scale by periodicity strength (confidence multiplier)
    float likelihood = (ratio - 1.0f) * periodicityStrength;

    // Clamp to [0, 1]
    if (likelihood < 0.0f) likelihood = 0.0f;
    if (likelihood > 1.0f) likelihood = 1.0f;

    return likelihood;
}

bool RhythmAnalyzer::confirmPastBeat(int framesAgo, float threshold) {
    if (framesAgo <= 0 || framesAgo >= frameCount_) {
        return false;  // Out of range
    }

    // Get bass energy at target frame
    float targetEnergy = getSample(bassHistory_, framesAgo);

    // Compare to neighbors (expect spike at beat)
    float beforeEnergy = getSample(bassHistory_, framesAgo + 1);
    float afterEnergy = getSample(bassHistory_, framesAgo - 1);
    float avgNeighbor = (beforeEnergy + afterEnergy) / 2.0f;

    // Confirmed if target is significantly higher than neighbors
    return (targetEnergy > avgNeighbor * threshold);
}
```

### Modified Component: MusicMode Integration

**File**: `blinky-things/music/MusicMode.h` and `MusicMode.cpp`

**Changes to MusicMode.h**:
```cpp
#pragma once

#include <stdint.h>
#include "../hal/interfaces/ISystemTime.h"

// FORWARD DECLARATIONS (avoid circular dependencies)
class RhythmAnalyzer;
class AdaptiveMic;

class MusicMode {
public:
    // ... existing public interface unchanged ...

    // ===== NEW METHOD =====

    /**
     * Update with continuous audio features (NEW)
     * - Integrates rhythm analysis with discrete onset tracking
     * - Enables beat synthesis when transients missed
     * - Improves tempo estimation via autocorrelation
     */
    void updateWithRhythm(float dt, RhythmAnalyzer* rhythm, AdaptiveMic* mic);

    // ===== TUNABLES (NEW) =====

    bool enableRhythmAnalysis = true;      // Enable/disable continuous analysis
    float rhythmBlendWeight = 0.3f;        // How much to trust rhythm vs IOI (0-1)
    float beatSynthesisThreshold = 0.7f;   // Likelihood threshold for virtual beats

private:
    // ... existing private members unchanged ...
};
```

**Changes to MusicMode.cpp** (add new method):
```cpp
void MusicMode::updateWithRhythm(float dt, RhythmAnalyzer* rhythm, AdaptiveMic* mic) {
    // Standard update (handles phase, missed beats, activation)
    update(dt);

    // Skip rhythm analysis if disabled or not provided
    if (!enableRhythmAnalysis || !rhythm || !mic) {
        return;
    }

    // === TEMPO BLENDING: Combine IOI-based and autocorrelation-based BPM ===

    if (rhythm->detectedPeriodMs > 0.0f && rhythm->periodicityStrength > 0.5f) {
        // Convert detected period to BPM
        float rhythmBPM = 60000.0f / rhythm->detectedPeriodMs;

        // Clamp to valid range
        rhythmBPM = clampFloat(rhythmBPM, bpmMin, bpmMax);

        // Blend with current BPM (favor IOI when we have recent transients)
        if (intervalCount_ >= 8) {
            // Lots of transient data: trust IOI more
            bpm = bpm * (1.0f - rhythmBlendWeight) + rhythmBPM * rhythmBlendWeight;
        } else {
            // Few transients: trust rhythm autocorrelation more
            bpm = bpm * 0.3f + rhythmBPM * 0.7f;
        }

        beatPeriodMs_ = 60000.0f / bpm;

        // Boost confidence when both methods agree (within 10%)
        float bpmDiff = absFloat(bpm - rhythmBPM);
        if (bpmDiff < bpm * 0.1f) {
            confidence_ += 0.1f;
            if (confidence_ > 1.0f) confidence_ = 1.0f;
        }
    }

    // === BEAT SYNTHESIS: Generate virtual transient when pattern detected ===

    // Only synthesize when:
    // - Music mode is active (already locked onto pattern)
    // - No actual transient this frame (mic->transient == 0)
    // - Rhythm analysis predicts beat (high likelihood)
    // - We're near expected beat phase (within 20% of beat period)

    bool noTransient = (mic->getTransient() == 0.0f);
    bool beatExpected = (rhythm->beatLikelihood > beatSynthesisThreshold);
    bool nearBeatPhase = (phase > 0.8f || phase < 0.2f);  // Near phase wrap

    if (active && noTransient && beatExpected && nearBeatPhase) {
        // Synthesize "virtual transient" to keep MusicMode in sync
        uint32_t nowMs = time_.millis();
        onOnsetDetected(nowMs, true);  // Treat as low-band onset

        // Debug logging (optional)
        #ifdef DEBUG_MUSIC_MODE
        Serial.println(F("[MUSIC] Synthesized virtual beat (rhythm pattern)"));
        #endif
    }
}
```

---

## Integration Points

### 1. AdaptiveMic Extensions

**File**: `blinky-things/inputs/AdaptiveMic.h`
**Location**: After line 66 (public state section)

**Add**:
```cpp
// Multi-band energy envelopes (continuous features for rhythm analysis)
float bassEnergy = 0.0f;     // 20-200 Hz bass content (kick, sub-bass)
float midEnergy = 0.0f;      // 200-2000 Hz mid content (snare, vocals)
float highEnergy = 0.0f;     // 2000-8000 Hz high content (hi-hat, cymbals)
```

**File**: `blinky-things/inputs/AdaptiveMic.h`
**Location**: After line 125 (public getters section)

**Add**:
```cpp
inline float getBassEnergy() const { return bassEnergy; }
inline float getMidEnergy() const { return midEnergy; }
inline float getHighEnergy() const { return highEnergy; }
```

**File**: `blinky-things/inputs/AdaptiveMic.h`
**Location**: After line 192 (private members section)

**Add**:
```cpp
// Multi-band filters for energy extraction
BiquadFilter bassBandFilter_;      // Lowpass @ 200 Hz
BiquadFilter midBandFilter_;       // Bandpass 200-2000 Hz
BiquadFilter highBandFilter_;      // Highpass @ 2000 Hz
bool bandFiltersInitialized_ = false;

// Helper: Initialize band filters (called in begin())
void initBandFilters();

// Helper: Update band energies from raw level
void updateBandEnergies(float rawLevel, float dt);
```

**File**: `blinky-things/inputs/AdaptiveMic.cpp`
**Location**: In `AdaptiveMic::begin()` after line 87

**Add**:
```cpp
// Initialize multi-band filters
initBandFilters();
```

**File**: `blinky-things/inputs/AdaptiveMic.cpp`
**Location**: In `AdaptiveMic::update()` after line 163 (after level computed)

**Add**:
```cpp
// Update multi-band energy envelopes
updateBandEnergies(level, dt);
```

**File**: `blinky-things/inputs/AdaptiveMic.cpp`
**Location**: At end of file (new methods)

**Add**:
```cpp
void AdaptiveMic::initBandFilters() {
    // Bass band: Lowpass @ 200 Hz
    bassBandFilter_.setLowpass(200.0f, (float)_sampleRate, 0.707f);  // Butterworth Q

    // Mid band: Highpass @ 200 Hz (everything above bass)
    // Alternative: Bandpass 200-2000 Hz for true mid isolation
    midBandFilter_.setHighpass(200.0f, (float)_sampleRate, 0.707f);

    // High band: Highpass @ 2000 Hz
    highBandFilter_.setHighpass(2000.0f, (float)_sampleRate, 0.707f);

    bandFiltersInitialized_ = true;
}

void AdaptiveMic::updateBandEnergies(float rawLevel, float dt) {
    if (!bandFiltersInitialized_) return;

    // Filter the envelope (not raw samples - lighter CPU)
    // For better accuracy, filter raw samples in ISR instead
    float bassFiltered = bassBandFilter_.process(rawLevel);
    float midFiltered = midBandFilter_.process(rawLevel);
    float highFiltered = highBandFilter_.process(rawLevel);

    // Track RMS energy with exponential smoothing
    float tau = 0.05f;  // 50ms time constant for envelope following
    float alpha = 1.0f - expf(-dt / tau);

    bassEnergy += alpha * (fabsf(bassFiltered) - bassEnergy);
    midEnergy += alpha * (fabsf(midFiltered) - midEnergy);
    highEnergy += alpha * (fabsf(highFiltered) - highEnergy);
}
```

### 2. Main Loop Integration

**File**: `blinky-things/blinky-things.ino`
**Location**: In `loop()` function where MusicMode is updated

**Current Code** (approximate location):
```cpp
void loop() {
    // ... timing code ...

    mic.update(dt);

    if (mic.getTransient() > 0.0f) {
        musicMode.onOnsetDetected(millis(), true);
    }

    musicMode.update(dt);

    // ... rest of loop ...
}
```

**Modified Code**:
```cpp
void loop() {
    // ... timing code ...

    mic.update(dt);

    // Add multi-band energy to rhythm analyzer
    rhythmAnalyzer.addFrame(mic.getBassEnergy(),
                           mic.getMidEnergy(),
                           mic.getHighEnergy());

    // Update rhythm analysis (autocorrelation, periodicity)
    // This is throttled internally (every 1 sec) to save CPU
    rhythmAnalyzer.update(millis(), 60.0f);  // 60 Hz frame rate

    // Process discrete transient events (existing code)
    if (mic.getTransient() > 0.0f) {
        musicMode.onOnsetDetected(millis(), true);
    }

    // Update music mode WITH rhythm analysis integration
    musicMode.updateWithRhythm(dt, &rhythmAnalyzer, &mic);

    // ... rest of loop ...
}
```

**File**: `blinky-things/blinky-things.ino`
**Location**: Global variable declarations (top of file)

**Add**:
```cpp
#include "music/RhythmAnalyzer.h"

// ... existing globals ...

// NEW: Continuous rhythm pattern analyzer
RhythmAnalyzer rhythmAnalyzer;
```

### 3. Serial Parameter Integration

**File**: Wherever serial parameters are registered (likely in main .ino or a config file)

**Add**:
```cpp
// RhythmAnalyzer tunables
serialParams.registerFloat("rhythm.autocorr_interval_ms",
                          &rhythmAnalyzer.autocorrUpdateIntervalMs,
                          100.0f, 5000.0f);
serialParams.registerFloat("rhythm.beat_likelihood_thresh",
                          &rhythmAnalyzer.beatLikelihoodThreshold,
                          0.0f, 1.0f);

// MusicMode rhythm integration tunables
serialParams.registerBool("music.enable_rhythm",
                         &musicMode.enableRhythmAnalysis);
serialParams.registerFloat("music.rhythm_blend",
                          &musicMode.rhythmBlendWeight,
                          0.0f, 1.0f);
serialParams.registerFloat("music.beat_synthesis_thresh",
                          &musicMode.beatSynthesisThreshold,
                          0.0f, 1.0f);
```

---

## Implementation Phases

### Phase 1: RhythmAnalyzer Implementation (Week 1) - **COMPLETED**

**Goal**: Create RhythmAnalyzer class with OSS buffering and autocorrelation.

**Status**: ✅ DONE - Files created:
- `blinky-things/music/RhythmAnalyzer.h`
- `blinky-things/music/RhythmAnalyzer.cpp`

**Deliverables**:
1. Add `bassEnergy`, `midEnergy`, `highEnergy` to AdaptiveMic
2. Implement `initBandFilters()` and `updateBandEnergies()`
3. Add getters for energy values
4. Serial debug command to monitor energies in real-time

**Testing**:
- Play music with distinct kick, snare, hi-hat
- Verify bass energy spikes on kicks
- Verify high energy spikes on hi-hats
- Verify mid energy spikes on snares

**Success Criteria**:
- Energy envelopes visibly correlate with musical events
- CPU increase < 1% (filtering envelopes, not raw samples)
- RAM increase: 12 bytes (3 floats)

### Phase 2: Buffering & Autocorrelation (Week 2)

**Goal**: Implement RhythmAnalyzer class with periodicity detection.

**Deliverables**:
1. Create `RhythmAnalyzer.h` and `RhythmAnalyzer.cpp`
2. Implement circular buffers (256 frames × 3 bands)
3. Implement autocorrelation on bass energy
4. Add to main loop, feed with energy frames
5. Serial debug command to display detected period/BPM

**Testing**:
- Play steady beat music (120 BPM)
- Verify autocorrelation detects correct period
- Test with various BPMs (60, 80, 100, 140, 180, 200)
- Verify periodicity strength increases with stable rhythm

**Success Criteria**:
- Detects period within ±5% of actual BPM
- Periodicity strength > 0.7 for stable beats
- CPU increase: ~2% @ 64 MHz (1 sec update interval)
- RAM increase: 3 KB (buffers)

### Phase 3: MusicMode Integration (Week 3)

**Goal**: Blend rhythm analysis with existing MusicMode.

**Deliverables**:
1. Add `updateWithRhythm()` method to MusicMode
2. Implement tempo blending (IOI + autocorrelation)
3. Implement confidence boosting when methods agree
4. Update main loop to call new method
5. Add serial tunables for blend weight

**Testing**:
- Compare tempo estimation: IOI-only vs blended
- Test with transient misses (e.g., quiet kicks)
- Verify confidence stays high when rhythm steady
- Measure improvement in activation reliability

**Success Criteria**:
- Tempo estimate converges faster (< 4 beats vs 8 beats)
- Activation happens with fewer false negatives
- Blended BPM more stable (less jitter)

### Phase 4: Beat Synthesis (Week 4)

**Goal**: Synthesize virtual transients when pattern detected but transient missed.

**Deliverables**:
1. Implement `getBeatLikelihood()` in RhythmAnalyzer
2. Add beat synthesis logic to `updateWithRhythm()`
3. Add tunables for synthesis threshold
4. Debug logging for virtual vs real beats

**Testing**:
- Play music, then reduce mic gain to miss transients
- Verify virtual beats synthesized at correct times
- Count real vs virtual beats over 1 minute
- Verify MusicMode stays locked despite missed transients

**Success Criteria**:
- Virtual beats < 20% of total (real transients still primary)
- Phase error remains < 0.2 when using virtual beats
- MusicMode stays active for 60+ seconds on challenging music

### Phase 5: Advanced Features (Week 5+)

**Goal**: Optional enhancements for maximum robustness.

**Deliverables** (pick and choose):
1. **Multi-band tempo voting**: Run autocorrelation on bass, mid, high separately; majority vote
2. **Power-scaled spectral flux**: Upgrade Mode 3 detection (replace in AdaptiveMic)
3. **Beat confirmation**: Retroactively verify transients are part of pattern
4. **Predictive onset**: Pre-trigger beats slightly ahead for zero-latency visuals
5. **Adaptive thresholds**: Auto-tune transient thresholds based on rhythm confidence

**Testing**: Per feature

**Success Criteria**: Measurable improvement on challenging test cases

---

## Technical Specifications

### Memory Budget

| Component | Size | Location |
|-----------|------|----------|
| `bassHistory_[256]` | 1024 bytes | RhythmAnalyzer |
| `midHistory_[256]` | 1024 bytes | RhythmAnalyzer |
| `highHistory_[256]` | 1024 bytes | RhythmAnalyzer |
| `bassEnergy`, `midEnergy`, `highEnergy` | 12 bytes | AdaptiveMic |
| Band filter state (3 × BiquadFilter) | ~48 bytes | AdaptiveMic |
| RhythmAnalyzer bookkeeping | ~32 bytes | RhythmAnalyzer |
| **TOTAL** | **~3.1 KB** | Heap/Global |

**Platform**: nRF52840 has 256 KB RAM → 3.1 KB = **1.2% RAM usage**

### CPU Budget

| Operation | Frequency | Cost per Call | Total CPU |
|-----------|-----------|---------------|-----------|
| Band filtering (envelope) | 60 Hz | ~50 μs | 0.3% |
| Autocorrelation (256 samples) | 1 Hz | ~1.5 ms | 0.15% |
| Beat likelihood calculation | 60 Hz | ~20 μs | 0.12% |
| Buffer management | 60 Hz | ~10 μs | 0.06% |
| **TOTAL** | — | — | **~0.63%** |

**Platform**: Cortex-M4 @ 64 MHz, 16 ms frame budget @ 60 Hz
**Existing music mode**: ~3% CPU
**New total**: ~3.6% CPU

**Note**: CPU estimates are conservative. Actual overhead may be lower with compiler optimization.

### Latency Analysis

| Stage | Current Latency | With Rhythm Analysis |
|-------|----------------|---------------------|
| Transient detection | 0 ms (instant) | 0 ms (unchanged) |
| IOI tempo estimate | 8 beats (~4 sec) | 4 beats (~2 sec) |
| Autocorrelation tempo | N/A | 1 sec (background) |
| Activation threshold | 4 beats (~2 sec) | 2-3 beats (~1-1.5 sec) |
| **Total time to lock** | **~6 seconds** | **~3-4 seconds** |

**Improvement**: Rhythm analysis provides early tempo estimate (1 sec) that accelerates IOI convergence.

### Power Consumption

**Negligible impact**: CPU increase of 0.6% translates to < 0.5 mA additional current draw on nRF52840 @ 64 MHz (active mode: ~6 mA).

---

## Performance Analysis

### Expected Improvements

#### 1. Activation Reliability

**Problem**: Current MusicMode fails to activate on music with unreliable transient detection (e.g., heavy bass, constant hi-hats).

**Solution**: Rhythm autocorrelation provides independent tempo estimate that doesn't rely on perfect transient detection.

**Expected Improvement**:
- Activation success rate: 60% → 90% (estimated)
- Activation time: 6 sec → 3 sec (measured once implemented)

#### 2. Tempo Stability

**Problem**: IOI-based tempo estimation is sensitive to outliers (missed or false transients).

**Solution**: Autocorrelation on continuous signal is statistically robust to outliers.

**Expected Improvement**:
- BPM jitter (std dev): ±5 BPM → ±2 BPM
- Convergence time: 8 beats → 4 beats

#### 3. Sustain Robustness

**Problem**: MusicMode deactivates after 8 missed beats (e.g., during breakdown or bridge).

**Solution**: Virtual beat synthesis keeps phase locked even when transients missed.

**Expected Improvement**:
- Sustained lock duration: 5-10 sec → 60+ sec
- Missed beat tolerance: 8 beats → unlimited (pattern-based)

### Test Cases (Recommended)

1. **Steady Beat (Metronome)**: 120 BPM click track
   - Baseline: Should work perfectly
   - Metric: Activation within 4 beats, BPM = 120 ± 2

2. **Heavy Bass (EDM)**: Track with overpowering sub-bass
   - Challenge: Transient detector may miss kicks
   - Metric: Activation despite 20% missed transients

3. **Constant Hi-Hats (Drum & Bass)**: 170 BPM with 16th-note hi-hats
   - Challenge: False positives from hi-hats
   - Metric: Locks to quarter notes (170 BPM), not 16ths (680 BPM)

4. **Dynamic Music (Pop)**: Song with verse/chorus transitions
   - Challenge: Tempo/pattern changes
   - Metric: Maintains lock through transitions

5. **Sparse Beats (Hip-Hop)**: Lots of space between hits
   - Challenge: Long gaps test missed beat tolerance
   - Metric: Sustains lock during 2-bar gaps

---

## References

### Academic Papers

1. Meier, Chiu, & Müller (2024). "A Real-Time Beat Tracking System with Zero Latency and Enhanced Controllability". *Transactions of the International Society for Music Information Retrieval*.
   [[PDF]](https://transactions.ismir.net/articles/189/files/66fc0db587be3.pdf)

2. Alonso, Cont, & David (2017). "OBTAIN: Real-Time Beat Tracking in Audio Signals". *arXiv preprint arXiv:1704.02216*.
   [[arXiv]](https://arxiv.org/pdf/1704.02216)

3. Glover et al. (2014). "Power-Scaled Spectral Flux and Peak-Valley Group-Delay Methods for Robust Musical Onset Detection". *International Computer Music Conference (ICMC)*.
   [[PDF]](https://quod.lib.umich.edu/i/icmc/bbp2372.2014.184/1/--power-scaled-spectral-flux-and-peak-valley-group-delay)

4. Ellis, D. (2013). "Music Signal Processing - Lecture 10: Beat Tracking". Columbia University.
   [[PDF]](https://www.ee.columbia.edu/~dpwe/e4896/lectures/E4896-L10.pdf)

5. Grosche, P., & Müller, M. (2021). "Musical Note Onset Detection Based on a Spectral Sparsity Measure". *EURASIP Journal on Audio, Speech, and Music Processing*.
   [[SpringerOpen]](https://asmp-eurasipjournals.springeropen.com/articles/10.1186/s13636-021-00214-7)

### Software Libraries & Tools

6. **librosa** - Python library for music and audio analysis
   Multi-band onset detection documentation:
   [[librosa.onset.onset_strength_multi]](https://librosa.org/doc/main/generated/librosa.onset.onset_strength_multi.html)

7. **BTrack** - Real-time beat tracker by Adam Stark (C++)
   Causal algorithm designed for real-time embedded use:
   [[GitHub]](https://github.com/adamstark/BTrack)

8. **aubio** - C library for audio labelling by Paul Brossier
   Low-latency onset detection and beat tracking:
   [[Website]](https://aubio.org/)

### Additional Resources

9. Essentia - Audio analysis library
   Beat detection tutorial:
   [[Essentia Tutorial]](https://essentia.upf.edu/tutorial_rhythm_beatdetection.html)

10. Audio Labs Erlangen - FMP Notebooks
    Spectral-based novelty detection:
    [[FMP Notebooks]](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_NoveltySpectral.html)

---

## Appendix: Alternative Approaches Considered

### A. Comb Filter Bank (BTrack Style)

**Concept**: Bank of resonators tuned to different BPM values. Each filter "rings" when input matches its frequency.

**Pros**:
- More computationally efficient than autocorrelation
- Naturally multi-hypothesis (multiple tempo candidates)
- Proven in BTrack library

**Cons**:
- More complex implementation
- Requires more RAM (one filter per BPM bin)
- Harder to tune for embedded platform

**Decision**: Deferred to Phase 5. Autocorrelation is simpler for initial implementation.

### B. Machine Learning Onset Detection

**Concept**: Train CNN or RNN on onset detection task. Deploy using TensorFlow Lite Micro.

**Pros**:
- State-of-the-art accuracy
- Learns from data (genre-adaptive)

**Cons**:
- Requires training data and ML pipeline
- Model size: 50-200 KB (significant RAM)
- Inference latency: 10-50 ms
- Complexity: High

**Decision**: Not recommended. Manual algorithms sufficient for embedded use case. Complexity not justified.

### C. Dynamic Programming Beat Tracker

**Concept**: Viterbi-style dynamic programming to find optimal beat sequence through OSS.

**Pros**:
- Globally optimal solution
- Handles tempo changes gracefully

**Cons**:
- Requires lookahead (introduces latency)
- More complex implementation
- Higher CPU cost

**Decision**: Deferred to Phase 5. PLL + rhythm analysis should provide sufficient accuracy first.

---

## Appendix: Code Organization

### New Files

```
blinky-things/
├── music/
│   ├── MusicMode.h              (MODIFIED)
│   ├── MusicMode.cpp            (MODIFIED)
│   ├── RhythmAnalyzer.h         (NEW)
│   └── RhythmAnalyzer.cpp       (NEW)
├── inputs/
│   ├── AdaptiveMic.h            (MODIFIED)
│   └── AdaptiveMic.cpp          (MODIFIED)
└── blinky-things.ino            (MODIFIED)
```

### Build System

No changes required to build system. New files compile automatically with Arduino build.

### Version Control

Recommended branching strategy:
1. `feature/multi-band-energy` - Phase 1
2. `feature/rhythm-analyzer` - Phase 2
3. `feature/music-mode-integration` - Phase 3
4. `feature/beat-synthesis` - Phase 4
5. Merge to `staging` for testing
6. Merge to `master` after validation

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-27 | Claude Sonnet 4.5 | Initial proposal |

---

**End of Document**
