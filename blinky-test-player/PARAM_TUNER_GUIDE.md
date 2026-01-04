# Parameter Tuner Guide - Selective Tuning

## Overview

The param-tuner supports **30 parameters** across all musical analysis subsystems with **selective tuning** capabilities for efficient parameter optimization in ~30 minute chunks.

**Total Firmware Parameters**: 47 (Fire: 13, Audio: 2, AGC: 1, Musical Analysis: 30, Mode Selection: 1)
**Param-Tuner Focus**: 30 musical analysis parameters only

## Complete Parameter List

### Transient Detection Parameters (14 total)

**Drummer Mode (4 params)**:
- `hitthresh` - Main detection threshold (1.5-10.0)
- `attackmult` - Attack sensitivity multiplier (1.1-2.0)
- `avgtau` - Envelope smoothing time constant (0.1-5.0s)
- `cooldown` - Minimum ms between detections (20-500ms)

**Spectral Flux Mode (2 params)**:
- `fluxthresh` - Spectral flux threshold (1.0-10.0)
- `fluxbins` - Number of FFT bins to analyze (4-128)

**Hybrid Mode (3 params)**:
- `hyfluxwt` - Weight for spectral flux component (0.1-1.0)
- `hydrumwt` - Weight for drummer component (0.1-1.0)
- `hybothboost` - Boost when both algorithms agree (1.0-2.0)

**Bass Band Mode (3 params)**:
- `bassfreq` - Bass filter cutoff frequency Hz (40-200)
- `bassq` - Bass filter Q factor (0.5-3.0)
- `bassthresh` - Bass detection threshold (1.5-10.0)

**HFC Mode (2 params)**:
- `hfcweight` - HFC weighting factor (0.5-5.0)
- `hfcthresh` - HFC detection threshold (1.5-10.0)

### MusicMode Parameters (11 total)

**Activation Control**:
- `musicthresh` - Activation threshold (0.0-1.0)
- `musicbeats` - Stable beats required to activate (2-16)
- `musicmissed` - Missed beats before deactivation (4-16)

**Confidence Tracking**:
- `confinc` - Confidence gain per good beat (0.05-0.2)
- `confdec` - Confidence loss per bad/missed beat (0.05-0.2)
- `phasetol` - Phase error tolerance for good beat (0.1-0.5)
- `missedtol` - Missed beat tolerance multiplier (1.0-3.0)

**BPM Range**:
- `bpmmin` - Minimum BPM (40-120)
- `bpmmax` - Maximum BPM (120-240)

**PLL Control**:
- `pllkp` - PLL proportional gain (0.01-0.5)
- `pllki` - PLL integral gain (0.001-0.1)

### RhythmAnalyzer Parameters (5 total)

- `rhythmminbpm` - Minimum BPM for autocorrelation (60-120)
- `rhythmmaxbpm` - Maximum BPM for autocorrelation (120-240)
- `rhythminterval` - Autocorrelation update interval ms (500-2000)
- `beatthresh` - Beat likelihood threshold (0.5-0.9)
- `minperiodicity` - Minimum periodicity strength (0.3-0.8)

---

## Selective Tuning Commands

### 1. Tune Specific Parameters

Tune only selected parameters (fastest, ~5-10 min per param):

```bash
# Tune single parameter
param-tuner sweep --port COM5 --params hitthresh

# Tune multiple specific parameters
param-tuner sweep --port COM5 --params hitthresh,attackmult,musicthresh

# With hardware gain lock
param-tuner sweep --port COM5 --params hitthresh --gain 40
```

**Estimated Time**: ~8 sweep values × 8 patterns × 10s/pattern = **10 minutes per parameter**

### 2. Tune by Mode/Category

Tune all parameters for a specific subsystem (~30-60 min):

```bash
# Tune all MusicMode parameters (11 params)
param-tuner sweep --port COM5 --modes music

# Tune all RhythmAnalyzer parameters (5 params)
param-tuner sweep --port COM5 --modes rhythm

# Tune multiple modes
param-tuner sweep --port COM5 --modes music,rhythm

# Tune only detection modes
param-tuner sweep --port COM5 --modes drummer,spectral,hybrid
```

**Estimated Time**:
- `--modes music`: 11 params × 10 min = **~110 minutes (1.8 hrs)**
- `--modes rhythm`: 5 params × 10 min = **~50 minutes**
- `--modes drummer`: 4 params × 10 min = **~40 minutes**

### 3. Combined Filtering

Combine mode and parameter filters for maximum control:

```bash
# Tune specific music mode params only
param-tuner sweep --port COM5 --modes music --params musicthresh,confinc,confdec

# Tune activation params across modes
param-tuner sweep --port COM5 --params musicthresh,beatthresh
```

---

## Common Tuning Scenarios

### Scenario 1: Quick Transient Detection Tuning (~30 min)

Optimize the most impactful transient detection parameters:

```bash
param-tuner sweep --port COM5 --params hitthresh,fluxthresh,hyfluxwt,hydrumwt
```

### Scenario 2: Music Mode Activation Tuning (~30 min)

Improve music mode lock-on speed and reliability:

```bash
param-tuner sweep --port COM5 --modes music --params musicthresh,musicbeats,confinc,confdec
```

### Scenario 3: Beat Tracking Accuracy (~45 min)

Optimize phase-locked loop and confidence tracking:

```bash
param-tuner sweep --port COM5 --modes music --params pllkp,pllki,phasetol,missedtol,confinc,confdec
```

### Scenario 4: Rhythm Analyzer Tuning (~50 min)

Tune autocorrelation parameters:

```bash
param-tuner sweep --port COM5 --modes rhythm
```

### Scenario 5: Complete Musical Analysis (~3 hrs)

Tune all music and rhythm parameters (comprehensive):

```bash
param-tuner sweep --port COM5 --modes music,rhythm
```

### Scenario 6: Bass-Heavy Music Optimization (~25 min)

Optimize for kick drums and bass drops:

```bash
param-tuner sweep --port COM5 --modes bass --params bassfreq,bassthresh,hitthresh
```

---

## Workflow Recommendations

### Day 1: Transient Detection (30 min)
```bash
# Morning: Optimize core detection
param-tuner sweep --port COM5 --params hitthresh,attackmult,fluxthresh

# Save results
param-tuner status
```

### Day 2: Music Mode Activation (45 min)
```bash
# Optimize activation speed and reliability
param-tuner sweep --port COM5 --modes music --params musicthresh,musicbeats,musicmissed,confinc
```

### Day 3: Beat Tracking (45 min)
```bash
# Fine-tune PLL and confidence tracking
param-tuner sweep --port COM5 --modes music --params pllkp,pllki,phasetol,missedtol,confdec
```

### Day 4: Rhythm Analysis (50 min)
```bash
# Tune autocorrelation
param-tuner sweep --port COM5 --modes rhythm
```

### Day 5: BPM Range Tuning (30 min)
```bash
# Optimize BPM detection range
param-tuner sweep --port COM5 --params bpmmin,bpmmax,rhythmminbpm,rhythmmaxbpm
```

### Day 6: Hybrid Mode (30 min)
```bash
# Optimize hybrid detection weights
param-tuner sweep --port COM5 --modes hybrid
```

**Total Time**: ~4 hours spread across 6 days (~30-50 min sessions)

---

## Advanced Usage

### Resume After Interruption

All tuning runs support resume - Ctrl+C to pause, then:

```bash
param-tuner resume --port COM5
```

The tuner will continue from the exact parameter value it was testing.

### Check Progress

```bash
param-tuner status
```

Shows:
- Current phase and progress
- Parameters completed
- Optimal values found
- Next parameter to tune

### Reset and Start Fresh

```bash
param-tuner reset
```

Clears all saved progress and results.

---

## Output and Results

Results are saved to `tuning-results/`:

```
tuning-results/
├── state.json                  # Resume state
├── sweeps/
│   ├── hitthresh.json          # Full sweep results
│   ├── musicthresh.json
│   └── ...
└── reports/
    └── summary.json            # Optimal values summary
```

### Applying Results

After tuning, optimal parameters are shown in the summary. Apply them via serial console:

```bash
# Connect to device via Arduino IDE Serial Monitor
set hitthresh 2.8
set musicthresh 0.65
set confinc 0.12
save  # Persist to flash
```

Or use the MCP server:
```typescript
await mcpServer.invoke('mcp__blinky-serial__set_setting', {
  name: 'hitthresh',
  value: 2.8
});
```

---

## Performance Notes

**Sweep Time per Parameter**:
- Fast parameters (4-6 values): ~5 minutes
- Normal parameters (7-9 values): ~10 minutes
- Exhaustive parameters (10+ values): ~15 minutes

**Pattern Testing**:
- Representative patterns (8): Used in sweeps for speed
- All patterns (20): Used in validation for thoroughness

**Test Pattern Duration**:
- Short patterns (sparse, strong-beats): ~5-8 seconds
- Medium patterns (bass-line, synth-stabs): ~10-12 seconds
- Long patterns (full-mix, tempo-sweep): ~15-20 seconds

**Hardware Recommendations**:
- Lock hardware gain (--gain 40) for reproducible results
- Use USB power (not battery) to ensure stable power
- Minimize environmental noise during testing

---

## Parameter Dependencies

Some parameters work together and should be tuned in sequence:

1. **Transient Detection**: `hitthresh` → `attackmult` → `avgtau` → `cooldown`
2. **Hybrid Mode**: `hyfluxwt` ↔ `hydrumwt` (tune together via interact phase)
3. **MusicMode Confidence**: `confinc` ↔ `confdec` (should be similar values)
4. **BPM Ranges**: `bpmmin`/`bpmmax` must align with `rhythmminbpm`/`rhythmmaxbpm`
5. **PLL Gains**: `pllkp` → `pllki` (tune proportional first, then integral)

**Interaction Testing** (future feature):
```bash
# Test parameter interactions (2D grid)
param-tuner interact --port COM5 --params hyfluxwt,hydrumwt
```

---

## Troubleshooting

**"No data collected for parameter"**:
- Check serial connection
- Verify hardware gain isn't too low (try --gain 40)
- Ensure test patterns are playing correctly

**"Tests timing out"**:
- Increase timeout in runner.ts (default: 30s)
- Check device isn't frozen
- Restart device and reconnect

**"F1 scores all zero"**:
- Hardware gain too low - increase with --gain flag
- Mic not working - check PDM connection
- Threshold too high - manually lower and retry

---

## Next Steps

After tuning with selective parameters:

1. **Validate Results**: Run full validation to ensure improvements generalize
   ```bash
   param-tuner validate --port COM5
   ```

2. **Generate Report**: Create comprehensive tuning report
   ```bash
   param-tuner report
   ```

3. **Compare to Baseline**: Check improvements vs defaults
   ```bash
   param-tuner status
   ```

4. **Apply to Device**: Use serial console or MCP server to apply optimal values

5. **Real-World Testing**: Test with actual music across genres

---

## Summary

The extended param-tuner provides:
- ✅ **30 parameters** across all musical analysis subsystems (47 total firmware params)
- ✅ **Selective tuning** via --params and --modes flags
- ✅ **~30 minute chunks** for efficient parameter optimization
- ✅ **Full resume support** for interrupted sessions
- ✅ **Comprehensive coverage** of transient detection, MusicMode, and RhythmAnalyzer
- ✅ **5 detection modes**: Drummer, Bass Band, HFC, Spectral Flux, Hybrid

Param-tuner focuses on musical analysis parameters only. Fire generator aesthetics (cooling, sparks, heat) and basic audio processing (peaktau, releasetau, hwtarget) are configured separately via the UI or serial console.

Start with the recommended scenarios above, tune in focused 30-50 minute sessions, and gradually optimize all aspects of musical analysis!
