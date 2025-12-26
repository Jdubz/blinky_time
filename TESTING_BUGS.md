# Testing System Implementation - Bug Report

## Critical Bugs (System Non-Functional)

### BUG #1: TestPanel Cannot Receive Serial Messages ⚠️ CRITICAL
**Location:** `blinky-console/src/components/TestPanel.tsx:116-165`

**Problem:** TestPanel listens for `window.postMessage` events but percussion messages arrive via the serial service event system. The two are completely disconnected.

```typescript
// TestPanel.tsx - WRONG
window.addEventListener('message', handleMessage);
```

**Root Cause:**
- Arduino sends: `{"type":"PERCUSSION",...}` over serial
- Serial service emits it as a 'data' event (serial.ts:366)
- Goes to `consoleLines` array in useSerial hook
- TestPanel has no access to serial events

**Impact:** **Testing system is completely non-functional** - no detections will be recorded.

**Fix Required:**
1. Add new 'percussion' event type to SerialEvent in serial.ts
2. Parse PERCUSSION messages in serial.ts (like audio/battery messages)
3. Pass percussion events from useSerial to TestPanel via props
4. Update App.tsx to connect TestPanel to serial system

**Example Fix:**
```typescript
// serial.ts - Add percussion handling
if (trimmed.startsWith('{"type":"PERCUSSION"')) {
  try {
    const percMsg = JSON.parse(trimmed);
    this.emit({ type: 'percussion', percussion: percMsg });
    continue;
  } catch {}
}

// App.tsx - Pass to TestPanel
<TestPanel
  onPercussionEvent={(msg) => {...}}
  connectionState={connectionState}
/>
```

---

## High Priority Bugs (Incorrect Results)

### BUG #2: Incorrect Overall Metrics Calculation
**Location:** `blinky-console/src/lib/testMetrics.ts:100-114`

**Problem:** Overall metrics use simple averaging of per-type metrics, which gives equal weight to types regardless of sample count.

```typescript
// WRONG - gives equal weight to type with 2 hits vs 100 hits
const overall: TestMetrics = {
  precision: (kick.precision + snare.precision + hihat.precision) / 3,
  recall: (kick.recall + snare.recall + hihat.recall) / 3,
  f1Score: (kick.f1Score + snare.f1Score + hihat.f1Score) / 3,
```

**Example:**
- Kick: 100 hits, 95% F1 score
- Snare: 2 hits, 50% F1 score (missed 1)
- Hihat: 2 hits, 50% F1 score (missed 1)
- **Current overall F1:** (95 + 50 + 50) / 3 = **65%** ❌
- **Correct overall F1:** Based on total TP/FP/FN = **~90%** ✓

**Fix:**
```typescript
const totalTP = kick.truePositives + snare.truePositives + hihat.truePositives;
const totalFP = kick.falsePositives + snare.falsePositives + hihat.falsePositives;
const totalFN = kick.falseNegatives + snare.falseNegatives + hihat.falseNegatives;

const precision = totalTP / (totalTP + totalFP) || 0;
const recall = totalTP / (totalTP + totalFN) || 0;

const overall: TestMetrics = {
  precision,
  recall,
  f1Score: 2 * precision * recall / (precision + recall) || 0,
  truePositives: totalTP,
  falsePositives: totalFP,
  falseNegatives: totalFN,
  avgTimingErrorMs: /* combine all timing errors before averaging */
};
```

### BUG #3: Incorrect Timing Error Calculation
**Location:** `testMetrics.ts:108-113`

**Problem:** The reduce function divides by array length multiple times instead of once.

```typescript
// WRONG - divides by 3 on each iteration!
.reduce((a, b, _, arr) => a + b / arr.length, 0)

// If values are [10, 20, 30]:
// Iteration 1: 0 + 10/3 = 3.33
// Iteration 2: 3.33 + 20/3 = 10
// Iteration 3: 10 + 30/3 = 20
// Result: 20 (should be 20, but only works by coincidence!)
```

**Fix:**
```typescript
const timingErrors = [
  kick.avgTimingErrorMs,
  snare.avgTimingErrorMs,
  hihat.avgTimingErrorMs
].filter((x): x is number => x !== undefined);

const avgTimingErrorMs = timingErrors.length > 0
  ? timingErrors.reduce((a, b) => a + b) / timingErrors.length
  : undefined;
```

**Better approach:** Combine all individual timing errors before averaging (don't average averages).

### BUG #4: Race Condition in Detection State Updates
**Location:** `TestPanel.tsx:149-154`

**Problem:** State updates are async, so rapid detections may be lost or metrics miscalculated.

```typescript
// WRONG - detections state may be stale
setDetections(prev => [...prev, ...newDetections]);

// Later - uses stale detections!
const liveMetrics = calculateAllMetrics(groundTruth, [...detections, ...newDetections], testStartTime);
```

**Impact:** If two PERCUSSION messages arrive within a few milliseconds, the second may not see the first's detections.

**Fix:**
```typescript
setDetections(prev => {
  const updated = [...prev, ...newDetections];
  const liveMetrics = calculateAllMetrics(groundTruth, updated, testStartTime);
  setMetrics(liveMetrics);
  return updated;
});
```

### BUG #5: Final Metrics May Be Incomplete
**Location:** `TestPanel.tsx:97-101`

**Problem:** Metrics are calculated immediately when audio ends, but detections may still be in flight.

```typescript
audio.onended = () => {
  // Serial messages may still be arriving!
  const finalMetrics = calculateAllMetrics(groundTruth, detections, testStartTime);
```

**Impact:** Last few detections near the end of audio may not be counted.

**Fix:**
```typescript
audio.onended = () => {
  clearInterval(progressInterval);
  setIsPlaying(false);
  setProgress(100);

  // Wait 500ms for final serial messages to arrive
  setTimeout(() => {
    setDetections(current => {
      const finalMetrics = calculateAllMetrics(groundTruth, current, testStartTime);
      setMetrics(finalMetrics);
      return current;
    });
  }, 500);
};
```

---

## Medium Priority Bugs (Data Quality)

### BUG #6: No Validation of Percussion Types in CSV
**Location:** `testMetrics.ts:137`

**Problem:** Invalid percussion types are silently cast without validation.

```typescript
type: type.toLowerCase() as PercussionType,  // No validation!
```

**Impact:** Typos in CSV (e.g., "snar", "hat", "kik") create invalid data that won't match.

**Fix:**
```typescript
const validTypes = ['kick', 'snare', 'hihat'];
const percType = type.toLowerCase();
if (!validTypes.includes(percType)) {
  console.warn(`Skipping invalid percussion type "${type}" at time ${time}s`);
  return null;
}
```

### BUG #7: Poor CSV Header Detection
**Location:** `testMetrics.ts:127-129`

**Problem:** Checks if first line contains "time" anywhere, which could match filenames or data.

```typescript
const dataLines = lines[0].toLowerCase().includes('time')
  ? lines.slice(1)
  : lines;
```

**Issues:**
- False positive if first data line has "time" in filename comment
- Won't detect proper CSV header "time,type,strength"
- Case-sensitive to field order

**Fix:**
```typescript
const hasHeader = /^time\s*,\s*type\s*,\s*strength/i.test(lines[0]);
const dataLines = hasHeader ? lines.slice(1) : lines;
```

### BUG #8: Silent Failure on Malformed CSV Lines
**Location:** `testMetrics.ts:141`

**Problem:** Malformed lines are silently filtered without user notification.

```typescript
.filter(hit => !isNaN(hit.time) && !isNaN(hit.strength));
```

**Impact:** User doesn't know their CSV has errors. A CSV with 100 lines might silently load only 80 annotations.

**Fix:**
```typescript
.map((hit, idx) => {
  if (isNaN(hit.time) || isNaN(hit.strength)) {
    console.warn(`Skipping malformed CSV line ${idx + 2}: ${dataLines[idx]}`);
    return null;
  }
  return hit;
})
.filter((hit): hit is GroundTruthHit => hit !== null);

// After parsing, show notification:
if (parsed.length < dataLines.length) {
  alert(`Loaded ${parsed.length} annotations (${dataLines.length - parsed.length} lines skipped due to errors)`);
}
```

---

## Low Priority Bugs (Memory & UX)

### BUG #9: Memory Leak - Object URLs Not Revoked
**Location:** `TestPanel.tsx:54-62, 79`

**Problem:** `URL.createObjectURL()` creates blob URLs that are never revoked.

```typescript
const audio = new Audio(URL.createObjectURL(file));
// URL is never revoked - memory leak!
```

**Impact:** Every test run leaks memory. After many tests, browser performance degrades.

**Fix:**
```typescript
const handleAudioUpload = (file: File) => {
  setAudioFile(file);
  const url = URL.createObjectURL(file);
  const audio = new Audio(url);
  audio.addEventListener('loadedmetadata', () => {
    setDuration(audio.duration);
    URL.revokeObjectURL(url);  // Clean up
  });
};

// In playTest:
useEffect(() => {
  return () => {
    // Cleanup on unmount
    if (audioRef.current?.src) {
      URL.revokeObjectURL(audioRef.current.src);
    }
  };
}, []);
```

### BUG #10: Division by Zero in Progress Calculation
**Location:** `TestPanel.tsx:86-89`

**Problem:** No check for zero duration before division.

```typescript
setProgress((audio.currentTime / audio.duration) * 100);
```

**Fix:**
```typescript
if (audio.currentTime && audio.duration && audio.duration > 0) {
  setProgress((audio.currentTime / audio.duration) * 100);
}
```

### BUG #11: No Check for Device Connection State
**Location:** `TestPanel.tsx:65-69`

**Problem:** Can start test even when device is disconnected.

```typescript
const playTest = () => {
  if (!audioFile || groundTruth.length === 0) {
    alert('Please load both audio file and CSV annotations');
    return;
  }
  // No check if device is connected!
```

**Impact:** Test runs but no detections arrive, confusing user.

**Fix:**
```typescript
interface TestPanelProps {
  connectionState: ConnectionState;
}

const playTest = () => {
  if (connectionState !== 'connected') {
    alert('Device not connected. Please connect device first.');
    return;
  }
  // ...
};
```

---

## Gaps (Missing Features)

### GAP #1: No Integration with useSerial Hook
**Critical:** TestPanel is not connected to the serial system at all.

**Required:**
- Accept serial event callbacks as props
- Receive connectionState, consoleLines, or percussion events
- Update App.tsx to wire up the connection

### GAP #2: No Visual Timeline
Users can't see which hits were matched, which were missed (FN), or which were false (FP).

**Suggestion:** Add a simple timeline showing:
- Ground truth marks (vertical lines)
- Detection marks (dots)
- Color coding: green (TP), red (FP), yellow (FN)

### GAP #3: No Test Comparison
Can't compare multiple test runs to see if parameter changes improved accuracy.

**Suggestion:** Store test history in state or localStorage.

### GAP #4: No Download/Upload of Test Configurations
Can't save test runs for later comparison or sharing.

### GAP #5: No Loading States
No feedback while CSV is parsing or audio metadata is loading.

**Fix:** Add loading spinner:
```typescript
const [isLoading, setIsLoading] = useState(false);

const handleCsvUpload = async (file: File) => {
  setIsLoading(true);
  try {
    const gt = await parseGroundTruthCSV(file);
    setGroundTruth(gt);
  } finally {
    setIsLoading(false);
  }
};
```

### GAP #6: No Pause/Resume
Can only stop test, not pause and resume.

### GAP #7: No Delay for Serial Latency
Measurements include serial transmission latency (~1-10ms). Could be more accurate by using device timestamps.

---

## Summary

**Critical (Blocking):**
1. ⚠️ Serial integration completely missing - **system doesn't work**

**High Priority (Wrong Results):**
2. Overall metrics calculation incorrect (macro vs micro averaging)
3. Timing error calculation has logic bug
4. Race condition in state updates
5. Final metrics may miss last detections

**Medium Priority (Data Quality):**
6. No CSV type validation
7. Poor header detection
8. Silent failure on malformed CSV

**Low Priority (Polish):**
9. Memory leaks from blob URLs
10. Division by zero possibility
11. No connection state check

**Gaps:**
- No serial integration (critical)
- No visual feedback
- No test history/comparison
- No loading states
- No pause/resume

## Recommended Fix Order

1. **BUG #1** - Add serial integration (required for any functionality)
2. **BUG #2** - Fix overall metrics calculation
3. **BUG #4** - Fix race condition
4. **BUG #5** - Fix final metrics timing
5. **BUG #6-8** - CSV validation improvements
6. **BUG #9-11** - Memory and UX polish
7. **Gaps** - Enhanced features (timeline, comparison, etc.)
