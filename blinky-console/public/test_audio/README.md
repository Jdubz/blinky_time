# Test Audio Files

> **Not used by the console today (checked 2026-10-21).** No code in `blinky-console/src` reads this folder. It holds one sample annotation file, `simple_beat.csv`, left over from an older test tab that loaded audio + CSV pairs.
>
> - **Console test mode:** the **Audio Debug** page (button in the top bar, dev builds only: `npm run dev`) has a **Run Test** control. It plays built-in synthesized patterns from `src/lib/testPatterns.ts` and scores the device's transient events against them. It does not load files from here.
> - **Real validation:** onset detection is validated on the 18-track corpus in `blinky-test-player/music/edm/`, scored against `.onsets_consensus.json` by the blinky-server test harness. Don't add test material here expecting it to count.

## CSV Annotation Format

```csv
time,type,strength
0.0,kick,1.0
0.5,snare,0.9
1.0,hihat,0.6
```

**Fields:**

- `time`: Time in seconds from audio start
- `type`: `kick`, `snare` or `hihat`
- `strength`: 0.0-1.0 (how strong the hit is)

The console's built-in patterns no longer use per-instrument types; every hit is a single `transient` type (`src/types/testTypes.ts`).

## Sample File

### simple_beat.csv

Basic 4/4 pattern at 120 BPM (500 ms per beat):

- Kick on beats 1 and 3
- Snare on beats 2 and 4
- 4 seconds total

No audio file goes with it.

## Datasets

If you need labeled drum audio for offline work:

- **IDMT-SMT-Drums**: https://zenodo.org/records/7544164
- **WaivOps TR-808/909**: https://github.com/patchbanks/WaivOps-EDM-TR8
