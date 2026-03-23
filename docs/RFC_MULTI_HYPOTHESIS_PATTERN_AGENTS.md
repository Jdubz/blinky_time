# RFC: Multi-Hypothesis Live Pattern Agents

**Date:** 2026-03-21
**Status:** Proposed (pending PLP validation)
**Author:** jdubz + Claude
**Prerequisites:** PLP epoch-fold pattern extraction validated and tuned

## Problem Statement

The current v77 pattern memory system uses an IOI histogram (128 bins, 512 bytes) for tempo discovery and a bar-position histogram (16 bins) for onset prediction, with cold-start template seeding. When a musical section changes (verse → chorus), the single bar histogram must:

1. Detect that the current pattern no longer matches (confidence drop)
2. Progressively overwrite the histogram via EMA learning

This takes 4-8 bars (~8-16 seconds at 120 BPM) to fully adapt. During the transition, the visual output is a noisy blend of old and new patterns. Drum fills contaminate the histogram because stability-gated learning only applies to PLP, not the bar histogram. Breakdowns cause confidence decay.

The PLP epoch-fold pattern extraction (Fourier tempogram) handles phase/pulse well, but the bar histogram provides onset prediction and anticipatory energy pre-ramp — features PLP doesn't replicate. The multi-agent architecture replaces the bar histogram while preserving these capabilities.

## Proposed Architecture

Replace the v77 pattern memory (single IOI histogram + bar histogram) with **4 live pattern agents** that continuously track competing rhythmic hypotheses. Each agent maintains its own bar histogram and independently interprets incoming onsets at its own tempo/phase. The agent with the highest confidence is the "active" agent driving PLP visual output. Switching agents is instant because all agents are always pre-warmed.

### Core Concept: Always-On Multi-Tracking

Instead of learning one pattern and caching snapshots, run 4 independent pattern trackers simultaneously. Each interprets the same onset stream differently:

```
Onset stream (from pulse detection)
    │
    ├──→ Agent 0: barBins at BPM=120, phase=0.0  → confidence 0.82 ← ACTIVE
    ├──→ Agent 1: barBins at BPM=120, phase=0.5  → confidence 0.15
    ├──→ Agent 2: barBins at BPM=60,  phase=0.0  → confidence 0.41
    └──→ Agent 3: barBins at BPM=240, phase=0.25 → confidence 0.08
```

When the chorus hits and Agent 0's confidence drops while Agent 2 (which has been quietly accumulating a half-time pattern in the background) rises, the switch is instant — no cache search, no progressive blend.

### Agent State

```cpp
struct PatternAgent {
    float barBins[16];        // Bar histogram (16th-note resolution)
    float bpm;                // Tempo at which this agent interprets onsets
    float phase;              // Current phase within the bar (0-1)
    float confidence;         // Peak-to-mean ratio of histogram
    float similarity;         // Cosine similarity to recent onsets
    uint16_t barsAccumulated; // How many bars this agent has observed
    bool seeded;              // Whether template-seeded for fast start
};
// 76 bytes per agent × 4 = 304 bytes total
```

### How Agents Differ

Each agent tracks the same onset stream but at different tempo/phase hypotheses. When the ACF detects a tempo change, agents are re-seeded:

- **Agent 0:** Primary — runs at the ACF-detected BPM
- **Agent 1:** Octave above — runs at 2× ACF BPM
- **Agent 2:** Octave below — runs at 0.5× ACF BPM
- **Agent 3:** Free — runs at the strongest non-primary ACF peak (may differ from Agent 0)

When an agent's confidence exceeds all others by > 0.1, it becomes the active agent. Its bar histogram feeds the PLP epoch-fold pattern. Other agents continue accumulating quietly.

### Agent Update (Per Onset)

When a pulse fires (onset detected):

```
for each agent:
    1. Map onset timestamp to agent's bar position using agent's BPM + phase
    2. barPosition = ((nowMs - agent.barStart) / agent.beatPeriod) mod 16
    3. barBins[barPosition] += onsetStrength × learningRate
    4. Update confidence (peak-to-mean ratio)
    5. Update similarity (cosine of last 4 onsets vs histogram)
```

The key insight: the same onset at time T maps to different bar positions in different agents (because they have different BPMs). An onset that's "beat 1" in Agent 0 (120 BPM) might be "beat 2" in Agent 2 (60 BPM). The agent whose interpretation produces the most coherent histogram (highest confidence) is correct.

### Active Agent Selection

Every bar boundary (of the active agent):

```
1. Compute confidence for all agents
2. If any non-active agent has confidence > active agent + 0.1:
   → Switch active agent (instant — no blend needed)
   → The PLP epoch-fold now uses the new agent's tempo/pattern
3. If active agent confidence drops below 0.2 for > 2 bars:
   → Reset the weakest non-active agent with a new hypothesis
```

The +0.1 hysteresis prevents rapid switching between similarly-confident agents.

## Beat Stability Signal

Add a beat stability metric derived from PLP peak amplitude (Meier 2024):

```cpp
float plpPeakHistory;  // EMA of recent PLP peak amplitudes
float beatStability;   // current PLP peak / plpPeakHistory (0-1)
```

**Beat stability gates pattern learning:**

| Stability | Interpretation | Learning Rate | Behavior |
|-----------|---------------|---------------|----------|
| > 0.7 | Strong pattern | 0.10 (low) | Protect established pattern |
| 0.3 - 0.7 | Transitioning | 0.15 (normal) | Normal learning |
| < 0.3 | Disrupted (fill/breakdown) | 0.0 (frozen) | Hold all agent histograms |

This provides automatic fill/breakdown immunity without explicit detection logic.

## Fast Warm-Up via Template Seeding

During cold start (`barsAccumulated < 4`):

1. After 1 bar of onset data, compute cosine similarity against all 8 templates
2. If best match > 0.70, seed the agent's histogram: `barBins = 0.5 × template + 0.5 × observed`
3. Mark agent as `seeded = true` (prevents re-seeding)
4. Use elevated learning rate (0.40) during cold start

**Expected warm-up time:** ~2 bars (~4 seconds at 120 BPM) vs. current ~8 bars (~16 seconds).

The template seeding gives the system a "best guess" to work from. If the guess is wrong, the elevated learning rate quickly overwrites it with observed data. If it's right, the system locks on in half the time.

## Fill/Breakdown Immunity

### Fill Detection

A fill is a bar where onset positions differ significantly from the established pattern. Detection:

```
currentBarSimilarity = cosine(recentOnsets mapped to 16 bins, activeAgent.barBins)
if (currentBarSimilarity < 0.4 AND onsetDensity > 1.5 × recentAvgDensity):
    → Skip histogram update for all agents
    → Mark as "fill bar" (for diagnostics)
```

This catches the common pattern: drum fills have MORE onsets at DIFFERENT positions than the regular pattern. A fill at the end of an 8-bar phrase hits positions the regular pattern doesn't use.

### Breakdown Detection

A breakdown is sustained low energy with sparse or absent onsets:

```
if (smoothedEnergy < 0.3 × energyPeak AND onsetDensity < 1.0/s):
    → Freeze all agent histograms (learning rate = 0)
    → Maintain agent phases (free-running at each agent's BPM)
    → Hold confidence levels (prevent decay)

    When energy recovers to > 0.5 × energyPeak:
    → Resume normal learning
    → The agent whose phase best matches the returning pattern will win
```

The phase maintenance is critical: during a 4-bar breakdown, the agents continue advancing their phases. When the music returns, the agent whose phase happens to align with the first kick back gets an immediate confidence boost and wins the selection.

## AudioControl Changes

No new fields needed. The existing fields change meaning slightly:

| Field | Current Source | With Pattern Agents |
|-------|---------------|-------------------|
| `plpPulse` | Epoch-fold at ACF BPM | Epoch-fold at **active agent's** BPM |
| `rhythmStrength` | max(ACF periodicity, plpConfidence) | max(ACF periodicity, **active agent confidence**) |
| `phase` | PLP phase at ACF BPM | PLP phase at **active agent's** BPM |

The active agent's BPM may differ from the ACF BPM (e.g., if Agent 2 at half-time is winning). This is correct — the visualizer should follow whichever tempo produces the most coherent pattern.

## Serial Stream / Diagnostics

Add to debug stream mode:

```
"ag": {                           // Active agent info
    "id": 0,                      // Which agent is active (0-3)
    "conf": [0.82, 0.15, 0.41, 0.08],  // All agent confidences
    "bpm": [120, 120, 60, 240],   // All agent BPMs
    "stab": 0.85                  // Beat stability
}
```

Add to `show beat` command:

```
=== Pattern Agents ===
Agent 0 (ACTIVE): BPM=120.0 conf=0.82 bars=42 seeded=yes
Agent 1:          BPM=120.0 conf=0.15 bars=42
Agent 2:          BPM=60.0  conf=0.41 bars=21
Agent 3:          BPM=240.0 conf=0.08 bars=84
Beat Stability: 0.85
```

## Tunable Parameters

| Parameter | Default | Range | Purpose |
|-----------|---------|-------|---------|
| `agentSwitchHysteresis` | 0.10 | 0.0-0.5 | Min confidence gap to switch active agent |
| `agentColdStartRate` | 0.40 | 0.1-0.8 | Learning rate during first 4 bars |
| `agentNormalRate` | 0.15 | 0.05-0.3 | Learning rate when pattern is established |
| `agentFillThreshold` | 0.40 | 0.2-0.8 | Min cosine similarity to accept bar as non-fill |
| `agentBreakdownEnergy` | 0.30 | 0.1-0.5 | Energy fraction below which to freeze agents |
| `stabilityAlpha` | 0.1 | 0.01-0.3 | EMA rate for beat stability tracking |

These replace the existing v77 pattern memory parameters: `patternLearnRate`, `patternDecayRate`, `ioiDecayRate`, `patternGain`, `anticipationGain`, `patternLookahead`, `confidenceRise`, `confidenceDecay`, `histogramMinStrength`, `patternEnabled`.

Net parameter change: remove 10 (pattern memory), add 6 (agents). Simpler tuning surface.

## Implementation Plan

### Phase 1: Beat Stability + Gated Learning — DONE (PLP only)

Beat stability signal exists in PLP (`plpPeakEma_`, `beatStability_`). PLP epoch-fold learning is gated by stability. However, the v77 bar histogram is NOT gated by beat stability — fills contaminate freely. This gap is resolved by replacing the v77 histogram with agents (Phase 3).

### Phase 2: Template-Accelerated Warm-Up — DONE (both PLP and v77)

Template seeding implemented for both PLP cold-start and v77 bar histogram. 8 canonical patterns, cosine similarity > 0.50, 50/50 blend. Activation time dropped from 8-22s to 0-2.4s on most tracks.

### Phase 3: Multi-Agent Pattern Tracking

Replace v77 pattern memory (IOI histogram, bar histogram, onset prediction) with 4 live agents. This is the core architectural change.

**Removes from AudioTracker:**
- `ioiBins_[128]`, `onsetTimes_[64]`, `barBins_[16]` and all associated state
- Methods: `recordOnsetForPattern()`, `updateIoiAnalysis()`, `updateBarHistogram()`, `computePatternStats()`, `predictOnsetStrength()`, `decayPatternBins()`
- Parameters: `patternLearnRate`, `patternDecayRate`, `ioiDecayRate`, `patternGain`, `anticipationGain`, `patternLookahead`, `confidenceRise`, `confidenceDecay`, `histogramMinStrength`, `patternEnabled`

**Adds:**
1. `PatternAgent` struct (76 bytes × 4 = 304 bytes)
2. All agents update on each onset
3. Active agent selection with hysteresis
4. Onset prediction via active agent's bar histogram (replaces `predictOnsetStrength()`)
5. Test: play music with section changes, measure switching latency

### Phase 4: PLP Integration

Wire the active agent's BPM/pattern into the PLP epoch-fold. PLP epoch-fold pattern extraction remains — it provides the `plpPulse` visual output. The agent system provides tempo/phase selection; PLP provides the pattern shape.

1. PLP epoch-folds at active agent's BPM (may differ from ACF BPM)
2. Phase correction uses active agent's phase
3. Test: verify PLP pulse tracks the dominant repeating pattern across section changes

## Resource Budget

| Component | Current (v77 pattern memory) | With Agents | Delta |
|-----------|---------|-------------|-------|
| Pattern memory state | ~832 bytes (ioiBins\_[128] 512B + barBins\_[16] 64B + onsetTimes\_[64] 256B) | ~304 bytes (4 agents × 76B) | -528 bytes |
| Parameters | 10 floats (patternLearnRate, patternDecayRate, ioiDecayRate, patternGain, anticipationGain, patternLookahead, confidenceRise, confidenceDecay, histogramMinStrength, patternEnabled) | 6 floats (24 bytes) | -4 params |
| Code size | ~200 lines (pattern memory methods) | ~250 lines (agents, onset prediction built-in) | +50 lines |
| CPU per onset | Update 1 histogram + IOI analysis | Update 4 histograms | +3 histogram updates (~trivial) |

The multi-agent approach eliminates the separate IOI histogram (agents implicitly discover tempo through pattern coherence) and replaces single-histogram adaptation with instant agent switching.

## Success Metrics

1. **Section switch latency:** Time from first onset of new section to active agent switch. Target: < 2 bars (< 4 seconds at 120 BPM). Current: 4-8 bars.

2. **Fill immunity:** Play 18-track suite with tracks containing drum fills. Measure histogram contamination (cosine similarity to pre-fill histogram after fill passes). Target: > 0.9 (< 10% contamination). Current: no measurement (fills contaminate freely).

3. **Warm-up time:** Time from silence to pattern confidence > 0.5. Target: < 4 seconds. Current: ~12 seconds.

4. **Visual quality:** Subjective assessment of PLP-driven pulsing during section changes. The visualizer should not glitch, stutter, or go organic during verse→chorus transitions.

## Open Questions

1. **Agent initialization:** When ACF detects a new BPM, should Agent 0 be reset or allowed to adapt gradually? Reset gives faster lock but loses history. Adaptation is slower but preserves partial pattern information.

2. **Agent count:** 4 agents covers primary + octave above + octave below + free. Is this enough? Some music has more than 2 distinct sections. Could use 6-8 agents at 456-608 bytes total (still trivial).

3. **Cross-agent inhibition:** Should agents actively compete (high-confidence agent suppresses others' learning rates)? This would prevent the "winning" agent from being contaminated while other agents explore alternatives. Risk: if the winner is wrong, suppressed agents can't catch up.

4. **Onset density normalization:** Should agent histograms be normalized by onset count? Currently bars with more onsets (dense sections) dominate the histogram. Normalizing per-bar would give equal weight to sparse and dense sections.

5. **v77 pattern memory A/B test:** Before implementing agents, test with `patternEnabled=false` to determine whether the v77 onset prediction and anticipatory energy pre-ramp provide visible benefit over PLP alone. If not, removing the v77 system without replacement may be sufficient.

## References

- Dixon (2007): BeatRoot multi-agent beat tracking — multi-hypothesis architecture with agent spawning/culling
- Krebs, Bock & Widmer (2013): Rhythmic pattern modeling for beat/downbeat tracking — bar-position onset distributions, pattern switching at bar boundaries
- Krebs & Korzeniowski (2014): Unsupervised pattern learning — online k-means for adaptive templates
- Meier, Chiu & Mueller (2024): Real-time PLP with beat stability signal — confidence-gated learning
- Heydari (2024): BeatNet+ information gate — threshold-gated filter updates during quiet passages
- van der Steen & Keller (2013): ADAM sensorimotor synchronization — dual-timescale adaptation + anticipation
- Whiteley, Cemgil & Godsill (2006): Bayesian temporal structure — probabilistic pattern transitions
- Scheirer (1998): Comb filter bank — resonant pattern accumulators as implicit multi-hypothesis trackers
