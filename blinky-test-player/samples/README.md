# Drum Samples for Onset Detection Testing

This directory contains drum samples used by the blinky-test-player for testing transient detection algorithms.

## Folder Structure

Each instrument type has its own folder:
- `kick/` - Bass drum/kick samples (low band: 50-200 Hz)
- `snare/` - Snare drum samples (high band: 2-8 kHz)
- `hat/` - Hi-hat samples (high band: 2-8 kHz)
- `tom/` - Tom samples (low band: 50-200 Hz)
- `clap/` - Clap samples (high band: 2-8 kHz)
- `percussion/` - General percussion samples (high band: 2-8 kHz)
- `bass/` - Bass hit samples (low band: 50-200 Hz)

## Supported Formats

- `.wav` (recommended for best quality)
- `.mp3`
- `.ogg`
- `.flac`

## Sample Requirements

For best testing results, samples should:
- Be **isolated** (single hit, no background music)
- Have **sharp transients** (quick attack)
- Be **clean** (minimal noise/reverb)
- Be **normalized** (consistent volume across samples)
- Duration: 0.5-2 seconds (short is fine)

## Free/Open Source Sample Options

### Option 1: Freesound.org (Recommended)
High-quality, isolated drum samples with Creative Commons licenses:

1. Visit https://freesound.org
2. Search for each instrument type:
   - "kick drum isolated"
   - "snare drum one shot"
   - "hi hat closed"
   - "tom drum"
   - "hand clap"
3. Filter by: Duration (0-2 sec), License (CC0 or CC-BY)
4. Download 5-10 samples per instrument type

### Option 2: Hydrogen Drum Kits
Open source drum machine samples:

```bash
# Download Hydrogen (open source drum machine)
# Samples are in ~/.hydrogen/data/drumkits/ (Linux/Mac)
# or C:\Program Files\Hydrogen\data\drumkits\ (Windows)

# Extract .wav files from kits like:
# - GMkit (General MIDI drum kit)
# - TR808 (Roland TR-808 sounds)
# - Techno (Electronic drum sounds)
```

### Option 3: IDMT-SMT-Drums Dataset
Professional research dataset (mentioned in TESTING.md):
- 608 isolated percussion samples
- https://zenodo.org/records/7544164
- Free for research use

### Option 4: Sample Your Own
Record samples from:
- Real drum kit with microphone
- Electronic drum pads/controllers
- Drum machine/sampler outputs
- Virtual instruments (with proper licensing)

## Quick Start: Minimal Sample Set

To get started quickly, you need at least:
- **3-5 kick samples** (required for kick-focus, full-kit patterns)
- **3-5 snare samples** (required for snare-focus, full-kit patterns)
- **3-5 hat samples** (required for hat-patterns, full-kit patterns)
- **2-3 tom samples** (required for full-kit pattern)
- **2-3 clap samples** (required for simultaneous, full-kit patterns)

Patterns will randomly select from available samples in each folder.

## Naming Convention

Use descriptive names for easy identification:
```
kick/
  ├── kick_808_01.wav
  ├── kick_acoustic_01.wav
  └── kick_heavy_01.wav
snare/
  ├── snare_acoustic_01.wav
  ├── snare_rim_01.wav
  └── snare_tight_01.wav
```

## Testing Your Samples

After adding samples, verify they're detected:

```bash
# List available samples
npm run dev -- samples

# Test a pattern
npm run dev -- play basic-drums
```

## Sample Diversity for Algorithm Testing

For comprehensive testing, include variety:
- **Different timbres**: Acoustic, electronic, synthetic
- **Different pitch**: Low kicks, high snares, etc.
- **Different attack**: Sharp vs soft transients
- **Different duration**: Short vs sustained

This ensures the onset detection algorithm generalizes well across different sound sources.

## Legal Notes

- Ensure you have proper licenses for any samples used
- Freesound samples often require attribution (check license)
- Personal/research use is typically fine
- This project is for educational/testing purposes

## Example Download Script

Here's a helper script to download some free samples from Freesound:

```powershell
# See download_samples.ps1 in this folder for a template script
# Customize it with your preferred sample sources:
# - Uses Freesound API or direct URLs
# - Downloads CC0 samples automatically
# - Organizes into correct folders

.\download_samples.ps1
```

Note: Currently PowerShell only. Linux/Mac users can adapt the logic to bash.
