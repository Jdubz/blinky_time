# Quick Start: Get Samples in 5 Minutes

## Recommended: Freesound.org Free Samples

The fastest way to get started is using Freesound.org's free samples:

### Step-by-Step

1. **Go to Freesound.org** (no account needed for downloading CC0 samples)

2. **Download Kick Samples** (minimum 3-5):
   - Search: `kick drum one shot`
   - Filters: Duration: 0-2 seconds, License: CC0 (Public Domain)
   - Recommended packs:
     - "808 Kick" samples
     - "Acoustic Kick" samples
   - Save to: `blinky-test-player/samples/kick/`

3. **Download Snare Samples** (minimum 3-5):
   - Search: `snare drum one shot`
   - Filters: Duration: 0-2 seconds, License: CC0
   - Save to: `blinky-test-player/samples/snare/`

4. **Download Hi-Hat Samples** (minimum 3-5):
   - Search: `hi hat closed one shot`
   - Filters: Duration: 0-2 seconds, License: CC0
   - Save to: `blinky-test-player/samples/hat/`

5. **Download Tom Samples** (minimum 2-3):
   - Search: `tom drum one shot`
   - Filters: Duration: 0-2 seconds, License: CC0
   - Save to: `blinky-test-player/samples/tom/`

6. **Download Clap Samples** (minimum 2-3):
   - Search: `hand clap one shot`
   - Filters: Duration: 0-2 seconds, License: CC0
   - Save to: `blinky-test-player/samples/clap/`

### Verify Installation

```bash
cd blinky-test-player
npm run dev -- samples
```

You should see:
```
Sample folders:

  kick         5 sample(s)
  snare        5 sample(s)
  hat          5 sample(s)
  tom          3 sample(s)
  clap         3 sample(s)
```

### Test a Pattern

```bash
npm run dev -- play basic-drums
```

## Alternative: Use Hydrogen Drum Kit

If you have Hydrogen drum machine installed:

**Windows:**
```powershell
# Copy samples from Hydrogen installation
$hydrogenPath = "C:\Program Files\Hydrogen\data\drumkits\GMkit"
Copy-Item "$hydrogenPath\*Kick*.wav" samples\kick\
Copy-Item "$hydrogenPath\*Snare*.wav" samples\snare\
Copy-Item "$hydrogenPath\*HiHat*.wav" samples\hat\
Copy-Item "$hydrogenPath\*Tom*.wav" samples\tom\
```

**Linux/Mac:**
```bash
# Copy from Hydrogen user directory
cp ~/.hydrogen/data/drumkits/GMkit/*Kick*.wav samples/kick/
cp ~/.hydrogen/data/drumkits/GMkit/*Snare*.wav samples/snare/
cp ~/.hydrogen/data/drumkits/GMkit/*HiHat*.wav samples/hat/
cp ~/.hydrogen/data/drumkits/GMkit/*Tom*.wav samples/tom/
```

## What You Need Minimum

To run all test patterns, you need:

| Folder | Minimum Samples | Used By Patterns |
|--------|----------------|------------------|
| `kick/` | 5 | kick-focus, full-kit, basic-drums, simultaneous |
| `snare/` | 5 | snare-focus, full-kit, basic-drums, simultaneous |
| `hat/` | 5 | hat-patterns, full-kit, basic-drums, fast-tempo |
| `tom/` | 3 | full-kit, sparse |
| `clap/` | 3 | full-kit, simultaneous, sparse |
| `percussion/` | 0 (optional) | Future patterns |
| `bass/` | 0 (optional) | Future patterns |

## Sample Quality Tips

Good samples for testing:
- **Sharp attack** - Clear transient at start
- **Clean** - No reverb or background noise
- **Short** - 0.5-2 seconds
- **Consistent volume** - Normalized across samples

Avoid:
- Samples with long reverb tails
- Samples with multiple hits
- Samples with background music
- Very quiet or distorted samples

## Troubleshooting

**"No samples found"**
- Check file extensions: `.wav`, `.mp3`, `.ogg`, or `.flac`
- Make sure files are in the correct subfolder
- Run `npm run dev -- samples` to verify

**"Missing samples for required instruments"**
- The pattern needs specific instrument types
- Check which folders are empty
- Add at least 2-3 samples to each required folder

**"Pattern playback issues"**
- Ensure samples aren't corrupted
- Try .wav format if using .mp3
- Check sample rate (16kHz-48kHz recommended)
