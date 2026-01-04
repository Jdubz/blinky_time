# Quick Tuning Scenarios - 30 Minute Chunks

## Copy-Paste Ready Commands

### 🎯 Scenario 1: First-Time Setup (30 min)
**Goal**: Get basic transient detection working well
```bash
param-tuner sweep --port COM5 --params hitthresh,fluxthresh --gain 40
```
**Tunes**: 2 params × 10 min = **20 minutes**
**Impact**: Immediate improvement in hit detection accuracy

---

### 🎵 Scenario 2: Music Mode Quick Win (35 min)
**Goal**: Improve activation speed and reliability
```bash
param-tuner sweep --port COM5 --modes music --params musicthresh,musicbeats,confinc
```
**Tunes**: 3 params × 10 min = **30 minutes**
**Impact**: Faster lock-on, fewer false deactivations

---

### 🎶 Scenario 3: Beat Tracking Polish (45 min)
**Goal**: Tighten beat phase accuracy
```bash
param-tuner sweep --port COM5 --params pllkp,pllki,phasetol,missedtol
```
**Tunes**: 4 params × 10 min = **40 minutes**
**Impact**: More precise beat prediction, smoother LED sync

---

### 🥁 Scenario 4: Drummer Mode Optimization (40 min)
**Goal**: Optimize amplitude-based detection
```bash
param-tuner sweep --port COM5 --modes drummer
```
**Tunes**: 4 params × 10 min = **40 minutes**
**Impact**: Better sensitivity and false positive rejection

---

### 🔊 Scenario 5: Bass-Heavy Music (30 min)
**Goal**: Optimize for kicks and bass drops
```bash
param-tuner sweep --port COM5 --modes bass --params bassthresh,hitthresh
```
**Tunes**: 4 params total, but bassthresh + hitthresh prioritized = **30 minutes**
**Impact**: Better kick drum detection in EDM/dubstep

---

### 📊 Scenario 6: Rhythm Analysis (50 min)
**Goal**: Tune autocorrelation for tempo detection
```bash
param-tuner sweep --port COM5 --modes rhythm
```
**Tunes**: 5 params × 10 min = **50 minutes**
**Impact**: More accurate BPM detection and beat prediction

---

### 🎭 Scenario 7: Hybrid Mode Mastery (30 min)
**Goal**: Balance drummer + spectral flux combination
```bash
param-tuner sweep --port COM5 --modes hybrid
```
**Tunes**: 3 params × 10 min = **30 minutes**
**Impact**: Best of both worlds - balanced detection

---

### 🎸 Scenario 8: Rock/Live Drums (35 min)
**Goal**: Handle timing variance and ghost notes
```bash
param-tuner sweep --port COM5 --params hitthresh,attackmult,avgtau,cooldown
```
**Tunes**: 4 params × 10 min = **40 minutes**
**Impact**: Better handling of human timing variations

---

### 🔥 Scenario 9: Aggressive Tuning (25 min)
**Goal**: High sensitivity for quiet music
```bash
param-tuner sweep --port COM5 --params hitthresh,fluxthresh,musicthresh
```
**Tunes**: 3 params × 8 min (fewer values tested) = **25 minutes**
**Impact**: Detect softer hits without false positives

---

### 🎼 Scenario 10: Complete Musical Analysis (3 hrs)
**Goal**: Full optimization of music mode + rhythm
```bash
param-tuner sweep --port COM5 --modes music,rhythm
```
**Tunes**: 16 params × 10 min = **160 minutes (2.7 hrs)**
**Impact**: Comprehensive optimization of beat tracking pipeline

---

## Weekly Tuning Plan

### Week 1: Foundation
- **Monday**: Scenario 1 (basic detection) - 30 min
- **Wednesday**: Scenario 4 (drummer mode) - 40 min
- **Friday**: Scenario 7 (hybrid mode) - 30 min

### Week 2: Musical Intelligence
- **Monday**: Scenario 2 (music mode activation) - 35 min
- **Wednesday**: Scenario 6 (rhythm analysis) - 50 min
- **Friday**: Scenario 3 (beat tracking) - 45 min

### Week 3: Genre-Specific
- **Monday**: Scenario 5 (bass-heavy) - 30 min
- **Wednesday**: Scenario 8 (rock/live) - 35 min
- **Friday**: Scenario 9 (aggressive) - 25 min

**Total Time**: ~6 hours over 3 weeks

---

## Priority Rankings

### Must-Tune (High Impact, Low Time)
1. ⭐⭐⭐ Scenario 1: First-Time Setup (30 min)
2. ⭐⭐⭐ Scenario 2: Music Mode Quick Win (35 min)
3. ⭐⭐⭐ Scenario 7: Hybrid Mode (30 min)

### Should-Tune (Medium Impact, Medium Time)
4. ⭐⭐ Scenario 3: Beat Tracking Polish (45 min)
5. ⭐⭐ Scenario 6: Rhythm Analysis (50 min)
6. ⭐⭐ Scenario 4: Drummer Mode (40 min)

### Nice-to-Tune (Genre/Context Specific)
7. ⭐ Scenario 5: Bass-Heavy (30 min)
8. ⭐ Scenario 8: Rock/Live (35 min)
9. ⭐ Scenario 9: Aggressive (25 min)

### Complete Tuning (For Perfectionists)
10. 🏆 Scenario 10: Full Suite (3 hrs)

---

## Quick Decision Tree

**START HERE:**
```
Do you have music mode working?
├─ NO → Run Scenario 2 (Music Mode Quick Win)
└─ YES → Is transient detection accurate?
    ├─ NO → Run Scenario 1 (First-Time Setup)
    └─ YES → Are beats syncing well?
        ├─ NO → Run Scenario 3 (Beat Tracking Polish)
        └─ YES → What's your music genre?
            ├─ Electronic/EDM → Scenario 5 (Bass-Heavy)
            ├─ Rock/Live → Scenario 8 (Rock/Live)
            ├─ Mixed/All → Scenario 7 (Hybrid Mode)
            └─ Perfectionist → Scenario 10 (Complete)
```

---

## Estimating Total Sweep Time

**Formula**: `(# of params) × (# of sweep values) × (# of patterns) × (pattern duration)`

**Typical Values**:
- Params: 1-16 (depends on selection)
- Sweep values: 6-9 per param
- Patterns: 8 (representative set)
- Pattern duration: ~10s average

**Examples**:
- 1 param: 1 × 8 × 8 × 10s = **10 minutes**
- 3 params: 3 × 8 × 8 × 10s = **32 minutes**
- 11 params (all music): 11 × 8 × 8 × 10s = **117 minutes**

**Pro Tip**: Add 10-20% for serial communication overhead and test runner delays.

---

## Hardware Setup for Best Results

```bash
# Standard test setup
param-tuner sweep --port COM5 --gain 40 --params <your_params>
```

**Why --gain 40?**
- Locks hardware AGC for reproducible results
- 40 is middle-ground (0-80 range)
- Prevents gain drift during long sweeps

**Alternative Gains**:
- `--gain 30`: Quieter environments, softer music
- `--gain 50`: Louder environments, harder hits
- No gain flag: Let AGC adapt (less reproducible)

---

## Results Interpretation

After running a scenario, check results:

```bash
param-tuner status
```

Look for:
- ✅ **F1 > 0.85**: Excellent performance
- ⚠️ **F1 0.70-0.85**: Good, but room for improvement
- ❌ **F1 < 0.70**: Needs more tuning or different mode

**Optimal Value Changes**:
- Small change (±10%): Parameter already well-tuned
- Large change (±50%+): Found significant improvement
- No change: Default was already optimal

---

## Combining with Manual Testing

1. **Before Tuning**: Test manually with current settings
   ```bash
   # Via serial console
   stream on
   # Play test music, observe transient detection
   ```

2. **Run Scenario**: Let tuner optimize
   ```bash
   param-tuner sweep --port COM5 --params hitthresh,fluxthresh
   ```

3. **After Tuning**: Apply results and retest
   ```bash
   set hitthresh 2.8  # Use optimal value from tuner
   stream on
   # Verify improvement with same test music
   ```

4. **Iterate**: If not satisfied, tune related parameters

---

## Common Patterns

**Pattern 1: Too Sensitive** (too many false positives)
```bash
# Increase thresholds
param-tuner sweep --port COM5 --params hitthresh,fluxthresh,bassthresh
```

**Pattern 2: Too Insensitive** (missing hits)
```bash
# Decrease thresholds, adjust attack
param-tuner sweep --port COM5 --params hitthresh,attackmult,avgtau
```

**Pattern 3: Slow Activation** (music mode takes too long)
```bash
# Lower activation threshold, reduce beats required
param-tuner sweep --port COM5 --params musicthresh,musicbeats
```

**Pattern 4: Unstable Lock** (music mode keeps deactivating)
```bash
# Increase missed beat tolerance, adjust confidence
param-tuner sweep --port COM5 --params musicmissed,confdec,missedtol
```

**Pattern 5: Wrong Tempo** (BPM detection off)
```bash
# Adjust BPM range, autocorrelation interval
param-tuner sweep --port COM5 --params rhythmminbpm,rhythmmaxbpm,rhythminterval
```

---

## Emergency Quick Fix

**"Nothing works, help!"**

```bash
# Reset to defaults
defaults

# Run absolute minimum tuning (10 min)
param-tuner sweep --port COM5 --params hitthresh --gain 40

# Apply result
set hitthresh <optimal_value>
save

# Test immediately
stream on
```

If this works, gradually tune more parameters. If this doesn't work, check hardware (mic connection, power, etc.).

---

## Pro Tips

1. **Start Small**: Tune 1-3 params at a time
2. **Lock Gain**: Always use `--gain 40` for reproducibility
3. **Save Often**: Results auto-save, but manual `save` after applying
4. **Resume Always**: Ctrl+C to pause, `resume` to continue
5. **Status Check**: Run `status` before and after each session
6. **Test Music**: Use same test tracks for before/after comparison
7. **Serial Console**: Keep serial monitor open to watch real-time behavior
8. **Patience**: Some params have subtle effects - trust the F1 scores

---

**Happy Tuning!** 🎵🔥
