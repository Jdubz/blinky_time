# Music Mode Testing Plan (Simplified)

## Goal
Rapid iteration on transient + beat detection for **electronic music** (house, D&B, hip-hop).

---

## Phase 1: Music Mode Telemetry (Priority)

**Arduino changes** - Add to serial stream:
```json
{"m":{"a":1,"bpm":125,"ph":0.45,"conf":0.82}}
```

**Beat events** - Emit when beat occurs:
```json
{"type":"BEAT","ts":12345,"bpm":125}
```

**Files:** `SerialConsole.cpp`, `blinky-things.ino`

---

## Phase 2: Compact Test Results

**Problem:** Test results are too verbose, wasting tokens.

**Solution:** Write details to file, return summary only.

**MCP `run_test` returns:**
```json
{
  "pattern": "house-4x4",
  "transient": { "f1": 0.91, "precision": 0.88, "recall": 0.94 },
  "music": { "bpmError": 1.2, "beatF1": 0.89, "lockTimeMs": 2100 },
  "detailsFile": "test-results/house-4x4-1703712000.json"
}
```

**Full details written to file:**
- Raw detection events
- Per-hit timing errors
- Audio samples captured
- Debug diagnostics

---

## Phase 3: Genre Test Patterns

**3 core patterns targeting electronic music:**

| Pattern | BPM | Focus |
|---------|-----|-------|
| `house-4x4` | 124 | Kick on every beat, offbeat hats |
| `dnb-break` | 174 | Fast breaks, snare on 2&4 |
| `hiphop-lazy` | 90 | Swing timing, ghost notes |

**Sample randomization:**
- Import ~50 samples from `D:\Ableton\Drums`
- Random selection each run
- Velocity variation (0.7-1.0)

---

## Phase 4: Suite Runner

New tool: `run_suite`

```
run_suite --patterns house-4x4,dnb-break,hiphop-lazy --iterations 2
```

**Returns:**
```json
{
  "summary": {
    "avgF1": 0.88,
    "avgBpmError": 1.5,
    "weakest": "hiphop-lazy"
  },
  "detailsDir": "test-results/suite-1703712000/"
}
```

---

## Implementation Order

1. **Compact results** (1 hr) - Reduce token usage immediately
2. **Music telemetry** (2 hrs) - Enable music mode testing
3. **Import samples** (1 hr) - Copy 50 samples from Ableton
4. **Genre patterns** (2 hrs) - Create 3 core patterns
5. **Suite runner** (1 hr) - Batch testing

---

## Key Metrics (What We're Optimizing)

| Metric | Target |
|--------|--------|
| Transient F1 | >90% |
| BPM Error | <2% |
| Beat Lock Time | <3s |
| Beat F1 | >85% |

---

## File Structure

```
test-results/
├── latest.json              # Most recent summary
├── house-4x4-{timestamp}.json
├── dnb-break-{timestamp}.json
└── suite-{timestamp}/
    ├── summary.json
    └── {pattern}-{n}.json
```
