# Percussion Detection Testing Guide

This guide explains how to use the percussion detection testing system to measure and improve algorithm accuracy.

## Overview

The testing system allows you to:
- Play test audio with known ground truth (labeled percussion hits)
- Measure detection accuracy in real-time
- Compare parameter changes quantitatively
- Export results for analysis

**Key insight:** The Arduino device doesn't know it's being tested - it just sends percussion events as usual. The blinky-console handles all the test intelligence.

---

## Quick Start

### 1. Prepare Test Audio

You need two files for each test:
- **Audio file**: WAV or MP3 with drum patterns
- **CSV file**: Ground truth annotations

#### Create a CSV Annotation File

```csv
time,type,strength
0.0,kick,1.0
0.5,snare,0.9
1.0,kick,1.0
1.5,snare,0.9
```

**Fields:**
- `time`: Seconds from audio start
- `type`: `kick`, `snare`, or `hihat`
- `strength`: 0.0-1.0 (how strong the hit is)

### 2. Run a Test

1. **Upload and flash** `blinky-things.ino` to your device
2. **Open** blinky-console in browser
3. **Connect** to device via USB
4. **Click** "Test" tab
5. **Load** your audio file (WAV/MP3)
6. **Load** corresponding CSV annotations
7. **Position** device 6-12 inches from speaker
8. **Click** "Play Test"
9. **Watch** metrics update live!

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
   Note: Low recall (65%), missing snares
   ```

2. **Adjust Parameters**
   ```
   Go to Settings → Inputs tab
   Lower "Snare Threshold" from 1.20 to 1.15
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
- `kick.threshold` - How much bass energy triggers a kick (default: 1.15)
- `snare.threshold` - How much mid energy triggers a snare (default: 1.20)
- `hihat.threshold` - How much high energy triggers a hihat (default: 1.25)

**Timing**:
- `cooldown` - Minimum time between detections in ms (default: 60ms)

**Via Serial Console:**
```
kick.threshold 1.10
snare.threshold 1.15
save
```

---

## Creating Test Cases

### Option 1: Use Existing Datasets

Download pre-labeled datasets:

**IDMT-SMT-Drums** (Recommended):
- 608 files, isolated kick/snare/hihat
- https://zenodo.org/records/7544164
- Perfect for testing

**WaivOps TR-808/909**:
- Synthetic drum machine sounds
- MIDI provides perfect labels
- https://github.com/patchbanks/WaivOps-EDM-TR8

### Option 2: Create Your Own

1. **Record/find** drum audio
2. **Listen** and note each hit timestamp
3. **Create CSV** with annotations:
   ```csv
   time,type,strength
   0.125,kick,1.0
   0.625,snare,0.9
   1.125,kick,1.0
   ```
4. **Save** both files to `blinky-console/public/test_audio/`

### Option 3: Generate Synthetic

Use online tools:
- https://drumbit.app - Create patterns, export
- https://www.onlinesequencer.net - MIDI to audio
- Add CSV annotations manually

---

## Understanding Metrics

### Confusion Matrix Example

```
Ground Truth: K S K S (K=kick, S=snare)
Detected:     K S K .

True Positives:  3 (K, S, K matched)
False Positives: 0 (no extra detections)
False Negatives: 1 (missed final S)

Precision: 3/(3+0) = 100% (all detections correct)
Recall:    3/(3+1) = 75%  (missed 1 out of 4)
F1 Score:  2*(1.0*0.75)/(1.0+0.75) = 85.7%
```

### Timing Tolerance

Detections within **±50ms** of ground truth count as correct.

Example:
```
Ground truth: Kick at 1.000s
Detected:     Kick at 1.012s (+12ms)
Result:       TRUE POSITIVE ✓
```

---

## Troubleshooting

### "False Positives Too High"
**Symptoms:** Detecting hits that aren't real
**Causes:**
- Thresholds too low (too sensitive)
- Background noise triggering detection
- Electrical interference

**Solutions:**
- Increase thresholds (e.g., `kick.threshold 1.20`)
- Test in quieter environment
- Increase `noiseGate` to filter out background

### "Missing Lots of Hits" (Low Recall)
**Symptoms:** Many false negatives
**Causes:**
- Thresholds too high (not sensitive enough)
- Audio too quiet
- Wrong frequency band

**Solutions:**
- Lower thresholds (e.g., `snare.threshold 1.10`)
- Increase speaker volume
- Check that hit type matches frequency

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

After running a test, click **"Export Results CSV"** to save:

```csv
Test: simple_beat.wav
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
Kick,92.3%,95.0%,90.0%,9,1,1
Snare,85.7%,88.9%,83.3%,5,1,1
Hihat,80.0%,80.0%,80.0%,3,0,1
```

### Compare Runs

Keep a log of tests:

| Test | Params | F1 Score | Notes |
|------|--------|----------|-------|
| Run 1 | Default | 75% | Baseline |
| Run 2 | Kick -0.05 | 78% | ↗ Better kick recall |
| Run 3 | All -0.05 | 68% | ↘ Too many FPs, reverted |
| Run 4 | Kick -0.05, Cooldown +10ms | 82% | ✓ Best so far |

---

## Example Session

```bash
# 1. Load test
Files: simple_beat.wav + simple_beat.csv
Ground truth: 20 hits (8 kick, 8 snare, 4 hihat)

# 2. First run (baseline)
Play test...
Results: F1: 75%, Precision: 80%, Recall: 71%
Analysis: Missing 3 kicks, 2 snares (low recall)

# 3. Adjust parameters
kick.threshold 1.10  (was 1.15)
snare.threshold 1.15 (was 1.20)
save

# 4. Second run
Play test...
Results: F1: 85%, Precision: 85%, Recall: 85%
Analysis: Much better! Caught 2 more kicks, 1 more snare
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

1. **simple_beat.wav** - Basic 4/4, clean (sanity check)
2. **fast_kicks.wav** - Double bass drums (tests cooldown)
3. **quiet_snares.wav** - Low volume (tests sensitivity)
4. **full_mix.wav** - Drums + bass + guitar (tests selectivity)
5. **compressed.wav** - MP3 quality (tests real-world)

Aim for:
- Simple: >90% F1
- Fast kicks: >85% F1
- Quiet: >80% F1
- Full mix: >75% F1
- Compressed: >70% F1

---

## Next Steps

1. Create 2-3 simple test cases
2. Run baseline tests
3. Tune parameters
4. Re-test and compare
5. Iterate until F1 >85%
6. Test in real environment
7. Enjoy data-driven improvements! 🎉
