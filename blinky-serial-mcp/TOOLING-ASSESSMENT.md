# MCP Server Tooling Assessment for Live Music Tracking

## Executive Summary

The blinky-serial MCP server has infrastructure for music mode tracking that is **underutilized**. The serial handler correctly parses music mode data and emits events, but the tool handlers don't expose this data to Claude. This document outlines the gaps and proposes specific fixes.

---

## Current Architecture Analysis

### What Works Well

| Component | Status | Notes |
|-----------|--------|-------|
| Serial JSON parsing | ✓ | `serial.ts:220-257` correctly parses `{"a":{...},"m":{...}}` |
| Music mode events | ✓ | Emits 'music' and 'beat' events on lines 234-246 |
| Event listeners | ✓ | `index.ts:104-130` stores music state in buffers |
| Test mode capture | ✓ | `run_test` captures full music metrics during pattern tests |
| Types defined | ✓ | `MusicModeState` interface exists with correct fields |

### What's Broken or Missing

#### Gap 1: `get_audio` Doesn't Return Music Mode Data

**Location**: `index.ts:433-457`

**Current behavior**:
```javascript
return {
  sample: lastAudioSample,
  led: lastLedSample,
  sampleCount: audioSampleCount,
};
```

**Problem**: `lastMusicState` is captured (line 68) and updated (lines 104-118) but never exposed.

**Impact**: Cannot see BPM, confidence, phase, or beat events during live monitoring.

---

#### Gap 2: `monitor_audio` Has No Statistics Collection

**Location**: `index.ts:577-611`

**Current behavior**:
```javascript
await new Promise(resolve => setTimeout(resolve, durationMs));
// Just returns final sample
return { currentSample: lastAudioSample };
```

**Missing statistics**:
- Transient count during monitoring period
- Min/max/avg audio levels
- Music mode summary (% active, avg BPM, avg confidence)
- Detection rate (transients/second)
- BPM stability (variance)

---

#### Gap 3: Multi-Line Command Responses Truncated

**Location**: `serial.ts:273-277`

**Current behavior**:
```javascript
if (this.pendingCommand) {
  this.pendingCommand.resolve(line);  // Only first line!
  this.pendingCommand = null;
  return;
}
```

**Impact**: Commands like `music` that return multi-line status get truncated to first line only:
```
=== Music Mode Status ===
Active: YES           <- LOST
BPM: 125.3            <- LOST
Phase: 0.45           <- LOST
Confidence: 0.82      <- LOST
```

---

#### Gap 4: Debug Stream Fields Not Typed

**Location**: `types.ts:13-31`, `types.ts:33-41`

**Current AudioSample type** has old dual-band debug fields:
```typescript
lob?: number;   // Low band baseline
hib?: number;   // High band baseline (these are obsolete)
```

**Missing fields** from simplified detection:
```typescript
avg?: number;   // Recent average level
prev?: number;  // Previous frame level
```

**Current MusicModeState type** is missing debug fields:
```typescript
// Missing:
sb?: number;    // Stable beats count
mb?: number;    // Missed beats count
pe?: number;    // Peak tempo energy
ei?: number;    // Error integral
```

---

#### Gap 5: No Dedicated Music Mode Monitoring Tool

There's no tool to specifically monitor music mode behavior over time:
- Track BPM stability
- Measure confidence trends
- Count beats detected
- Assess phase coherence

---

## Proposed Tooling Updates

### Update 1: Enhance `get_audio` Response

**File**: `index.ts`

**Change**: Include music mode state in response

```typescript
case 'get_audio': {
  // ... existing streaming check ...
  return {
    content: [{
      type: 'text',
      text: JSON.stringify({
        sample: lastAudioSample,
        music: lastMusicState,  // ADD THIS
        led: lastLedSample,
        sampleCount: audioSampleCount,
      }, null, 2),
    }],
  };
}
```

**Estimated effort**: 5 minutes

---

### Update 2: Enhanced `monitor_audio` with Statistics

**File**: `index.ts`

**Change**: Collect statistics during monitoring period

```typescript
case 'monitor_audio': {
  const durationMs = args.duration_ms || 1000;

  // Initialize statistics collectors
  let transientCount = 0;
  let levelSum = 0;
  let levelMin = Infinity;
  let levelMax = -Infinity;
  let musicActiveCount = 0;
  let bpmSum = 0;
  let confSum = 0;
  let beatCount = 0;

  const startCount = audioSampleCount;
  const startTime = Date.now();

  // Set up temporary listeners
  const onAudio = (sample: AudioSample) => {
    if (sample.t > 0) transientCount++;
    levelSum += sample.l;
    levelMin = Math.min(levelMin, sample.l);
    levelMax = Math.max(levelMax, sample.l);
  };

  const onMusic = (state: MusicModeState) => {
    if (state.a === 1) {
      musicActiveCount++;
      bpmSum += state.bpm;
      confSum += state.conf;
    }
    if (state.q === 1) beatCount++;
  };

  serial.on('audio', onAudio);
  serial.on('music', onMusic);

  await new Promise(r => setTimeout(r, durationMs));

  serial.off('audio', onAudio);
  serial.off('music', onMusic);

  const samplesReceived = audioSampleCount - startCount;

  return {
    content: [{
      type: 'text',
      text: JSON.stringify({
        durationMs,
        samplesReceived,
        sampleRate: (samplesReceived / (durationMs / 1000)).toFixed(1) + ' Hz',
        statistics: {
          transientCount,
          transientRate: (transientCount / (durationMs / 1000)).toFixed(2) + ' /sec',
          level: {
            min: levelMin === Infinity ? null : levelMin.toFixed(3),
            max: levelMax === -Infinity ? null : levelMax.toFixed(3),
            avg: samplesReceived > 0 ? (levelSum / samplesReceived).toFixed(3) : null,
          },
        },
        musicMode: musicActiveCount > 0 ? {
          activePercent: ((musicActiveCount / samplesReceived) * 100).toFixed(1),
          avgBpm: (bpmSum / musicActiveCount).toFixed(1),
          avgConfidence: (confSum / musicActiveCount).toFixed(2),
          beatCount,
          beatRate: (beatCount / (durationMs / 1000)).toFixed(2) + ' /sec',
        } : null,
        currentSample: lastAudioSample,
        currentMusic: lastMusicState,
      }, null, 2),
    }],
  };
}
```

**Estimated effort**: 30 minutes

---

### Update 3: New `get_music_status` Tool

**File**: `index.ts`

**Change**: Add dedicated music mode status tool that handles multi-line responses

```typescript
// Add to tool list
{
  name: 'get_music_status',
  description: 'Get detailed music mode status (BPM, confidence, phase, beat counts)',
  inputSchema: {
    type: 'object',
    properties: {},
  },
}

// Add handler
case 'get_music_status': {
  // If streaming, get latest state directly
  if (serial.getState().streaming && lastMusicState) {
    return {
      content: [{
        type: 'text',
        text: JSON.stringify({
          active: lastMusicState.a === 1,
          bpm: lastMusicState.bpm,
          phase: lastMusicState.ph,
          confidence: lastMusicState.conf,
          quarterNote: lastMusicState.q === 1,
          halfNote: lastMusicState.h === 1,
          wholeNote: lastMusicState.w === 1,
          // Debug fields if available
          stableBeats: lastMusicState.sb,
          missedBeats: lastMusicState.mb,
          peakEnergy: lastMusicState.pe,
          errorIntegral: lastMusicState.ei,
        }, null, 2),
      }],
    };
  }

  // Not streaming - need to query device
  // This requires fixing multi-line response handling first
  return {
    content: [{
      type: 'text',
      text: JSON.stringify({
        error: 'Start streaming first to get music status',
        hint: 'Call stream_start, then get_music_status',
      }, null, 2),
    }],
  };
}
```

**Estimated effort**: 20 minutes

---

### Update 4: Fix Multi-Line Response Handling

**File**: `serial.ts`

**Change**: Accumulate lines until complete response

```typescript
// Add response accumulator
private responseBuffer: string[] = [];
private responseTimeout: NodeJS.Timeout | null = null;

// Modify handleLine
private handleLine(line: string): void {
  // ... existing JSON handling ...

  // Handle pending command responses
  if (this.pendingCommand) {
    // Clear existing timeout
    if (this.responseTimeout) {
      clearTimeout(this.responseTimeout);
    }

    this.responseBuffer.push(line);

    // Set timeout to finalize response (50ms after last line)
    this.responseTimeout = setTimeout(() => {
      const response = this.responseBuffer.join('\n');
      this.responseBuffer = [];
      this.responseTimeout = null;

      clearTimeout(this.pendingCommand!.timeout);
      this.pendingCommand!.resolve(response);
      this.pendingCommand = null;
    }, 50);
  }
}
```

**Estimated effort**: 30 minutes

---

### Update 5: Update Types for Debug Fields

**File**: `types.ts`

**Change**: Add missing debug fields

```typescript
export interface AudioSample {
  // ... existing fields ...

  // Debug fields (present in debug stream mode)
  avg?: number;   // Recent average level
  prev?: number;  // Previous frame level
}

export interface MusicModeState {
  // ... existing fields ...

  // Debug fields (present in debug stream mode)
  sb?: number;    // Stable beats count
  mb?: number;    // Missed beats count
  pe?: number;    // Peak tempo energy (comb filter)
  ei?: number;    // PLL error integral
}
```

**Estimated effort**: 10 minutes

---

### Update 6: New `monitor_music` Tool

**File**: `index.ts`

**Change**: Add dedicated music mode monitoring with BPM tracking metrics

```typescript
{
  name: 'monitor_music',
  description: 'Monitor music mode for BPM tracking assessment',
  inputSchema: {
    type: 'object',
    properties: {
      duration_ms: {
        type: 'number',
        description: 'Duration to monitor (default: 5000)',
      },
      expected_bpm: {
        type: 'number',
        description: 'Expected BPM for accuracy calculation (optional)',
      },
    },
  },
}

case 'monitor_music': {
  const durationMs = args.duration_ms || 5000;
  const expectedBpm = args.expected_bpm;

  // Ensure streaming with debug mode
  if (!serial.getState().streaming) {
    await serial.startStream();
  }
  await serial.sendCommand('stream debug');

  // Collect music mode samples
  const samples: MusicModeState[] = [];
  const beats: { type: string; timestampMs: number }[] = [];
  const startTime = Date.now();

  const onMusic = (state: MusicModeState) => {
    samples.push({ ...state, timestampMs: Date.now() - startTime });
    if (state.q === 1) beats.push({ type: 'quarter', timestampMs: Date.now() - startTime });
  };

  serial.on('music', onMusic);
  await new Promise(r => setTimeout(r, durationMs));
  serial.off('music', onMusic);

  // Calculate metrics
  const activeStates = samples.filter(s => s.a === 1);
  const firstActive = activeStates[0];
  const bpmValues = activeStates.map(s => s.bpm);

  const avgBpm = bpmValues.length > 0
    ? bpmValues.reduce((a, b) => a + b, 0) / bpmValues.length : 0;
  const bpmVariance = bpmValues.length > 1
    ? bpmValues.reduce((sum, b) => sum + Math.pow(b - avgBpm, 2), 0) / bpmValues.length : 0;

  return {
    content: [{
      type: 'text',
      text: JSON.stringify({
        durationMs,
        totalSamples: samples.length,
        activeSamples: activeStates.length,
        activePercent: samples.length > 0 ? ((activeStates.length / samples.length) * 100).toFixed(1) : 0,
        activationMs: firstActive?.timestampMs || null,
        bpm: {
          average: avgBpm.toFixed(1),
          variance: bpmVariance.toFixed(2),
          stability: bpmVariance < 1 ? 'excellent' : bpmVariance < 5 ? 'good' : bpmVariance < 20 ? 'fair' : 'poor',
          expected: expectedBpm || null,
          error: expectedBpm ? ((Math.abs(avgBpm - expectedBpm) / expectedBpm) * 100).toFixed(1) + '%' : null,
        },
        confidence: {
          average: activeStates.length > 0
            ? (activeStates.reduce((s, x) => s + x.conf, 0) / activeStates.length).toFixed(2) : 0,
          final: samples[samples.length - 1]?.conf?.toFixed(2) || null,
        },
        beats: {
          count: beats.length,
          rate: (beats.length / (durationMs / 1000)).toFixed(2) + ' /sec',
        },
        debug: samples.length > 0 ? {
          stableBeats: samples[samples.length - 1]?.sb,
          missedBeats: samples[samples.length - 1]?.mb,
          peakEnergy: samples[samples.length - 1]?.pe?.toFixed(4),
          errorIntegral: samples[samples.length - 1]?.ei?.toFixed(3),
        } : null,
      }, null, 2),
    }],
  };
}
```

**Estimated effort**: 45 minutes

---

## Implementation Priority

| Priority | Update | Effort | Impact |
|----------|--------|--------|--------|
| P0 | Update 1: Add music to get_audio | 5 min | High - immediate visibility |
| P0 | Update 5: Fix types | 10 min | Required for other updates |
| P1 | Update 2: Enhanced monitor_audio | 30 min | High - statistics during monitoring |
| P1 | Update 3: get_music_status tool | 20 min | Medium - dedicated music access |
| P2 | Update 6: monitor_music tool | 45 min | High - comprehensive BPM assessment |
| P3 | Update 4: Multi-line responses | 30 min | Low - workaround exists |

**Total estimated effort**: ~2.5 hours

---

## Quick Wins (< 15 minutes)

### 1. Add `lastMusicState` to `get_audio` response
Single line change in `index.ts:450`:
```javascript
music: lastMusicState,
```

### 2. Update `MusicModeState` type
Add debug fields to `types.ts`:
```typescript
sb?: number;
mb?: number;
pe?: number;
ei?: number;
```

These two changes would immediately enable Claude to see music mode data during live monitoring.

---

## Testing the Fixes

After implementing updates, verify with:

1. **Basic music mode visibility**:
   ```
   stream_start → get_audio
   // Should now show "music" object with bpm, conf, etc.
   ```

2. **Statistics collection**:
   ```
   monitor_audio(duration_ms: 10000)
   // Should show transientCount, level stats, music mode summary
   ```

3. **BPM tracking assessment**:
   ```
   monitor_music(duration_ms: 30000, expected_bpm: 120)
   // Should show BPM accuracy, stability, confidence trend
   ```

---

## Conclusion

The MCP server already has most of the infrastructure for music mode tracking. The main issue is that the tool handlers don't expose the data that's being captured. Two quick fixes (adding music to get_audio, updating types) would provide immediate value. The enhanced monitoring tools would enable comprehensive live music assessment without requiring test patterns.
