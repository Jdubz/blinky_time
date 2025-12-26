# Test Audio Files

This directory contains test audio files and their corresponding ground truth annotations for percussion detection testing.

## File Format

### Audio Files

- Format: WAV or MP3
- Recommended: 16kHz mono WAV (matches device sample rate)
- Higher quality works but will be heard by device at speaker/mic fidelity

### CSV Annotations

```csv
time,type,strength
0.0,kick,1.0
0.5,snare,0.9
1.0,hihat,0.6
```

**Fields:**

- `time`: Time in seconds from audio start
- `type`: One of: `kick`, `snare`, `hihat`
- `strength`: 0.0-1.0 (how strong the hit is)

## Creating Test Cases

### Option 1: Use Existing Datasets

Download from recommended datasets:

- **IDMT-SMT-Drums**: https://zenodo.org/records/7544164
- **WaivOps TR-808/909**: https://github.com/patchbanks/WaivOps-EDM-TR8

### Option 2: Record Your Own

1. Record drum pattern with phone/mic
2. Listen and note timestamps of each hit
3. Create CSV with annotations
4. Place both .wav and .csv files here

### Option 3: Synthetic (for quick testing)

Use online tools like:

- https://drumbit.app (create patterns, export)
- https://www.onlinesequencer.net (MIDI to audio)
- Add your own CSV annotations

## Sample Test Cases Included

### simple_beat.csv

Basic 4/4 pattern at 120 BPM (500ms per beat):

- Kick on beats 1 and 3
- Snare on beats 2 and 4
- 4 seconds total

Use any audio with this pattern or generate synthetic tones.

## Using Test Cases

1. Open blinky-console
2. Go to "Test" tab
3. Load audio file + corresponding CSV
4. Position device near speaker
5. Click "Play Test"
6. View metrics in real-time
