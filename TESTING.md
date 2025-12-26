# Onset Detection Testing Guide

This guide explains how to use the onset detection testing system to measure and improve algorithm accuracy.

## Overview

The testing system allows you to:
- Play test audio with known ground truth (labeled transient hits)
- Measure detection accuracy in real-time
- Compare parameter changes quantitatively
- Export results for analysis

**Key insight:** The Arduino device doesn't know it's being tested - it just sends transient events as usual. The blinky-console handles all the test intelligence.

---

## Quick Start

### 1. Prepare Test Audio

You need two files for each test:
- **Audio file**: WAV or MP3 with transient patterns
- **CSV file**: Ground truth annotations

#### Create a CSV Annotation File

```csv
time,type,strength
0.0,low,1.0
0.5,high,0.9
1.0,low,1.0
1.5,high,0.9
```

**Fields:**
- `time`: Seconds from audio start
- `type`: `low` (bass, 50-200 Hz) or `high` (brightness, 2-8 kHz)
- `strength`: 0.0-1.0 (how strong the hit is)

### 2. Run a Test

1. **Upload and flash** `blinky-things.ino` to your device
2. **Open** blinky-console in browser
3. **Connect** to device via USB
4. **Start** audio streaming on the Inputs tab
5. **Select** a test pattern from the dropdown
6. **Position** device 6-12 inches from speaker
7. **Click** "Run Test"
8. **Watch** metrics update live!

### 3. Interpret Results

**F1 Score** (most important):
- **>90%**: Excellent - algorithm works great
- **80-90%**: Good - minor improvements possible
- **70-80%**: Fair - needs tuning
- **<70%**: Poor - significant issues

**Precision** = TP / (TP + FP) = "How many detections were correct?"
- Low precision = too many false positives (detecting hits that aren't real)

**Recall** = TP / (TP + FN) = "How many real hits did we catch?"
- Low recall = missing too many hits (false negatives)

---

## Improving Detection Accuracy

### Typical Workflow

1. **Baseline Test**
   ```
   Run test → F1: 75%
   Note: Low recall (65%), missing high-band transients
   ```

2. **Adjust Parameters**
   ```
   Go to Settings → Inputs tab → Onset Detection
   Lower "Onset Threshold" from 2.5 to 2.0
   Save settings
   ```

3. **Re-test**
   ```
   Run same test → F1: 82% ✓ Improvement!
   Recall improved to 78%
   ```

4. **Iterate**
   ```
   Continue adjusting, testing, comparing
   Keep track of what works
   ```

### Key Parameters to Tune

**Detection Thresholds** (lower = more sensitive):
- `onsetthresh` - Energy must exceed baseline × threshold (default: 2.5x)
- `risethresh` - Energy must rise by this factor from previous frame (default: 1.5x)

**Timing**:
- Onset cooldown is fixed at 80ms to prevent retriggering (max 12.5 hits/sec)

**Via Serial Console:**
```
set onsetthresh 2.0
set risethresh 1.3
save
```

---

## Creating Test Cases

### Option 1: Use Built-in Patterns

The blinky-console includes several built-in test patterns that synthesize audio directly in the browser:
- **Alternating Low/High** - Basic pattern alternating between bass and brightness
- **Low Band Barrage** - Rapid bass hits to test cooldown
- **High Band Barrage** - Rapid high-frequency hits
- **Mixed Intensity** - Varying strength levels
- **Stress Test** - Fast alternating hits at detection limits

### Option 2: Use Existing Datasets

Download pre-labeled datasets:

**IDMT-SMT-Drums** (Recommended):
- 608 files, isolated percussion samples
- https://zenodo.org/records/7544164
- Map kick → low, snare/hihat → high

**WaivOps TR-808/909**:
- Synthetic drum machine sounds
- MIDI provides perfect labels
- https://github.com/patchbanks/WaivOps-EDM-TR8

### Option 3: Create Your Own

1. **Record/find** transient-rich audio
2. **Listen** and note each hit timestamp
3. **Create CSV** with annotations:
   ```csv
   time,type,strength
   0.125,low,1.0
   0.625,high,0.9
   1.125,low,1.0
   ```
4. **Save** both files to `blinky-console/public/test_audio/`

---

## Understanding Metrics

### Confusion Matrix Example

```
Ground Truth: L H L H (L=low, H=high)
Detected:     L H L .

True Positives:  3 (L, H, L matched)
False Positives: 0 (no extra detections)
False Negatives: 1 (missed final H)

Precision: 3/(3+0) = 100% (all detections correct)
Recall:    3/(3+1) = 75%  (missed 1 out of 4)
F1 Score:  2*(1.0*0.75)/(1.0+0.75) = 85.7%
```

### Timing Tolerance

Detections within **±50ms** of ground truth count as correct.

Example:
```
Ground truth: Low at 1.000s
Detected:     Low at 1.012s (+12ms)
Result:       TRUE POSITIVE ✓
```

---

## Troubleshooting

### "False Positives Too High"
**Symptoms:** Detecting hits that aren't real
**Causes:**
- Onset threshold too low (too sensitive)
- Rise threshold too low
- Background noise triggering detection

**Solutions:**
- Increase onset threshold (e.g., `set onsetthresh 3.0`)
- Increase rise threshold (e.g., `set risethresh 1.8`)
- Test in quieter environment

### "Missing Lots of Hits" (Low Recall)
**Symptoms:** Many false negatives
**Causes:**
- Thresholds too high (not sensitive enough)
- Audio too quiet
- Transients not sharp enough

**Solutions:**
- Lower onset threshold (e.g., `set onsetthresh 2.0`)
- Lower rise threshold (e.g., `set risethresh 1.3`)
- Increase speaker volume
- Ensure transients have sharp attack

### "Timing Errors Large"
**Symptoms:** High average timing error (>30ms)
**Causes:**
- Baseline tracking too slow
- Processing lag

**Solutions:**
- Generally not a problem unless >50ms
- Check that device isn't overloaded

---

## Advanced: Exporting and Analysis

### Export Results

After running a test, click **"Export CSV"** to save:

```csv
Test: alternating_low_high
Date: 2025-12-25T12:34:56.789Z

Overall Metrics:
F1 Score,87.5%
Precision,90.2%
Recall,85.0%
True Positives,17
False Positives,2
False Negatives,3

Per-Type Metrics:
Type,F1,Precision,Recall,TP,FP,FN
Low,92.3%,95.0%,90.0%,9,1,1
High,85.7%,88.9%,83.3%,8,1,2
```

### Compare Runs

Keep a log of tests:

| Test | Params | F1 Score | Notes |
|------|--------|----------|-------|
| Run 1 | Default | 75% | Baseline |
| Run 2 | onsetthresh 2.0 | 78% | ↗ Better recall |
| Run 3 | risethresh 1.2 | 68% | ↘ Too many FPs, reverted |
| Run 4 | onsetthresh 2.0, risethresh 1.4 | 82% | ✓ Best so far |

---

## Example Session

```bash
# 1. Load test
Select: "Alternating Low/High" pattern
Ground truth: 20 hits (10 low, 10 high)

# 2. First run (baseline)
Run test...
Results: F1: 75%, Precision: 80%, Recall: 71%
Analysis: Missing 2 low, 3 high (low recall)

# 3. Adjust parameters
set onsetthresh 2.0  (was 2.5)
set risethresh 1.4   (was 1.5)
save

# 4. Second run
Run test...
Results: F1: 85%, Precision: 85%, Recall: 85%
Analysis: Much better! Caught more transients
          Added 1 FP but worth it

# 5. Export results
Save CSV for records
Document parameter changes
```

---

## Best Practices

1. **Start with simple tests** - Basic patterns before complex
2. **Test at different volumes** - Algorithm should work across range
3. **Use diverse audio** - Don't overtune to one test case
4. **Document changes** - Keep notes on what works
5. **Compare before/after** - Quantitative improvement only
6. **Test in target environment** - Fire camp, loud music, etc.

---

## Recommended Test Suite

Build a collection of tests:

1. **Alternating Low/High** - Basic pattern (sanity check)
2. **Low Band Barrage** - Rapid bass hits (tests cooldown)
3. **High Band Barrage** - Rapid high hits (tests selectivity)
4. **Mixed Intensity** - Varying strengths (tests sensitivity)
5. **Stress Test** - Fast alternating (tests limits)

Aim for:
- Simple patterns: >90% F1
- Barrage tests: >85% F1
- Mixed intensity: >80% F1
- Stress test: >75% F1

---

## Frequency Bands

The two-band system detects transients in these frequency ranges:

| Band | Frequency Range | Typical Sources |
|------|-----------------|-----------------|
| **Low** | 50-200 Hz | Bass drums, bass notes, sub-bass |
| **High** | 2-8 kHz | Hi-hats, cymbals, snare crack, consonants |

The system uses biquad IIR bandpass filters with Q factors optimized for transient detection:
- Low band: Q=0.7 (wide for bass punch)
- High band: Q=0.5 (wide for brightness)

---

## Next Steps

1. Run a baseline test with default parameters
2. Note which band has lower recall
3. Tune thresholds for that band
4. Re-test and compare
5. Iterate until F1 >85%
6. Test in real environment
7. Enjoy data-driven improvements! 🎉
