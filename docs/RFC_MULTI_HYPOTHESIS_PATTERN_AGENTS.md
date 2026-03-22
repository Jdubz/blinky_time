# RFC: Multi-Hypothesis Live Pattern Agents

**Date:** 2026-03-21
**Status:** Proposed (pending PLP validation)
**Author:** jdubz + Claude
**Prerequisites:** PLP epoch-fold pattern extraction validated and tuned

## Problem Statement

The current pattern memory system (v77-78) uses a frozen LRU cache with save/restore semantics. When a musical section changes (verse → chorus), the system must:

1. Detect that the current pattern no longer matches (confidence drop)
2. Search the cache for a previously seen pattern (cosine similarity)
3. Progressively blend the cached pattern over 4 bar boundaries

This takes 4-8 bars (~8-16 seconds at 120 BPM) to fully switch patterns. During the transition, the visual output is a noisy blend of old and new patterns. Drum fills contaminate the histogram because there's no fill detection. Breakdowns cause confidence decay, which can trigger unnecessary cache searches.

Additionally, the system has a cold-start problem: ~12 seconds before any pattern is established, during which the PLP confidence is too low to drive visuals.

## Proposed Architecture

Replace the frozen LRU cache with **4 live pattern agents** that continuously track competing rhythmic hypotheses. Each agent maintains its own bar histogram and independently interprets incoming onsets at its own tempo/phase. The agent with the highest confidence is the "active" agent driving PLP visual output. Switching agents is instant because all agents are always pre-warmed.

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
- **Agent 3:** Free — runs at the comb bank BPM (may differ from ACF)

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
| `rhythmStrength` | max(ACF+comb blend, plpConfidence) | max(ACF+comb blend, **active agent confidence**) |
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

These replace the existing pattern memory parameters: `patternLearnRate`, `patternDecayRate`, `confidenceRise`, `confidenceDecay`, `histogramMinStrength`. The cache-specific parameters (`cacheRestoreBarsLeft`, blend rates) are eliminated entirely.

Net parameter change: remove 9 (pattern memory + cache), add 6 (agents). Simpler tuning surface.

## Implementation Plan

### Phase 1: Beat Stability + Gated Learning (no new agents)

Add beat stability signal to existing PLP. Use it to gate the existing bar histogram learning rate. This validates the stability signal and fill/breakdown immunity before adding multi-agent complexity.

1. Track PLP peak amplitude EMA
2. Compute beat stability = current peak / EMA
3. Gate learning rate: frozen when stability < 0.3, reduced when < 0.7
4. Test: play music with drum fills, verify histogram is not contaminated

### Phase 2: Template-Accelerated Warm-Up (no new agents)

Add 1-bar template seeding to existing pattern memory. This validates fast warm-up independently.

1. After 1 bar, match against 8 templates
2. If match > 0.70, seed histogram with 50/50 blend
3. Use elevated learning rate (0.40) for first 4 bars
4. Test: measure time-to-pattern-lock, compare to baseline

### Phase 3: Multi-Agent Pattern Tracking

Replace frozen LRU cache with 4 live agents. This is the core architectural change.

1. Create `PatternAgent` struct, 4 instances
2. All agents update on each onset
3. Active agent selection with hysteresis
4. Test: play music with section changes, measure switching latency

### Phase 4: PLP Integration

Wire the active agent's BPM/pattern into the PLP epoch-fold.

1. PLP epoch-folds at active agent's BPM (may differ from ACF BPM)
2. Phase correction uses active agent's phase
3. Test: verify PLP pulse tracks the dominant repeating pattern across section changes

## Resource Budget

| Component | Current | With Agents | Delta |
|-----------|---------|-------------|-------|
| Pattern memory state | ~400 bytes (bins + IOI + cache) | ~400 bytes (4 agents + stability) | ≈ 0 |
| Parameters | 9 floats (36 bytes) | 6 floats (24 bytes) | -12 bytes |
| Code size | ~350 lines (pattern + cache) | ~250 lines (agents, no cache logic) | -100 lines |
| CPU per onset | Update 1 histogram + IOI | Update 4 histograms | +3 histogram updates (~trivial) |

The multi-agent approach is actually simpler than the current cache system because it eliminates save/restore/blend logic.

## Success Metrics

1. **Section switch latency:** Time from first onset of new section to active agent switch. Target: < 2 bars (< 4 seconds at 120 BPM). Current: 4-8 bars.

2. **Fill immunity:** Play 18-track suite with tracks containing drum fills. Measure histogram contamination (cosine similarity to pre-fill histogram after fill passes). Target: > 0.9 (< 10% contamination). Current: no measurement (fills contaminate freely).

3. **Warm-up time:** Time from silence to pattern confidence > 0.5. Target: < 4 seconds. Current: ~12 seconds.

4. **Visual quality:** Subjective assessment of PLP-driven pulsing during section changes. The visualizer should not glitch, stutter, or go organic during verse→chorus transitions.

## Open Questions

1. **Agent initialization:** When ACF detects a new BPM, should Agent 0 be reset or allowed to adapt gradually? Reset gives faster lock but loses history. Adaptation is slower but preserves partial pattern information.

2. **Agent count:** 4 agents covers primary + octave above + octave below + free. Is this enough? Some music has more than 2 distinct sections. Could use 6-8 agents at 456-608 bytes total (still trivial).

3. **IOI histogram interaction:** The current IOI histogram provides an independent tempo estimate. With multi-agent, the agents implicitly discover tempo through pattern coherence. Should IOI be kept as a third tempo source, or folded into agent initialization?

4. **Cross-agent inhibition:** Should agents actively compete (high-confidence agent suppresses others' learning rates)? This would prevent the "winning" agent from being contaminated while other agents explore alternatives. Risk: if the winner is wrong, suppressed agents can't catch up.

5. **Onset density normalization:** Should agent histograms be normalized by onset count? Currently bars with more onsets (dense sections) dominate the histogram. Normalizing per-bar would give equal weight to sparse and dense sections.

## References

- Dixon (2007): BeatRoot multi-agent beat tracking — multi-hypothesis architecture with agent spawning/culling
- Krebs, Bock & Widmer (2013): Rhythmic pattern modeling for beat/downbeat tracking — bar-position onset distributions, pattern switching at bar boundaries
- Krebs & Korzeniowski (2014): Unsupervised pattern learning — online k-means for adaptive templates
- Meier, Chiu & Mueller (2024): Real-time PLP with beat stability signal — confidence-gated learning
- Heydari (2024): BeatNet+ information gate — threshold-gated filter updates during quiet passages
- van der Steen & Keller (2013): ADAM sensorimotor synchronization — dual-timescale adaptation + anticipation
- Whiteley, Cemgil & Godsill (2006): Bayesian temporal structure — probabilistic pattern transitions
- Scheirer (1998): Comb filter bank — resonant pattern accumulators as implicit multi-hypothesis trackers
