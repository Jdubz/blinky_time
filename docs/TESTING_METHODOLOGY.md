# Testing Methodology

## Development Workflow

Audio tests are loud and time-consuming. Use the minimum test tier needed to answer your current question. Never gather more data than the decision requires.

```
Code change
    │
    ▼
Tier 1: Smoke test (2 min)
    "Does it work at all? BPM plausible? Beats firing? No crashes?"
    │
    ├─ Broken → fix and re-smoke
    │
    ▼
Tier 2: Quick A/B (10 min)
    "Is it better than baseline? Any signal at all?"
    │
    ├─ Worse or neutral → abandon approach, try something else
    ├─ Shows signal → parameter sweep (Tier 2 on individual tracks)
    │
    ▼
Lock in feature set (may involve multiple Tier 1/2 iterations)
    │
    ▼
Tier 3: Reliable A/B (25 min)
    "Is the improvement real and statistically meaningful?"
    │
    ├─ Not significant → reconsider before committing
    │
    ▼
Tier 4: Full Validation (50 min)
    "Final regression check before merge to master"
    → Establishes new baseline for future comparisons
```

**Key rules:**
- Tier 1 after every compile+flash — always
- Tier 2 for proof-of-concept — prove/disprove quickly, then iterate or abandon
- Tier 3/4 only when features are finalized — never during iteration
- Parameter sweeps use Tier 2 on individual tracks, not the full suite
- Never run full validation to test a single code change

## Tiered Testing Approach

### Tier 1: Smoke Test (~2 min)
**When:** After every compile+flash. Verify firmware boots and feature activates.

```
run_music_test(track, ground_truth, port, duration_ms=30000, commands=["set hmm 1"])
```

- **1 track, 1 run, 1 device**
- Use `techno-minimal-01` (strong 4-on-floor kicks, 129 BPM, reliable baseline)
- Check: BPM output plausible? Beat events firing? No crashes?
- **NOT for performance measurement** — variance too high for 1 run

### Tier 2: Quick A/B (~10 min)
**When:** Proof-of-concept testing. Does a code change show any signal at all?

```
run_music_test_multi(ports, audio_file, ground_truth, port_commands, duration_ms=30000)
```

- **3 tracks, 1 run each, 2 devices** (A/B simultaneous)
- Same audio → eliminates acoustic variation between configs
- Catches catastrophic regressions (>0.10 F1 drop) and large improvements
- **Cannot detect improvements < ~0.10 F1** due to single-run variance

**Quick A/B Track Set** (chosen for diversity):

| Track | BPM | Why |
|-------|-----|-----|
| `techno-minimal-01` | 129 | Strong 4-on-floor, reliable baseline, "easy" |
| `trance-goa-mantra` | 136 | Moderate difficulty, good reference point |
| `breakbeat-background` | 86 | Slow tempo, tests 128 BPM gravity well escape |

### Tier 3: Reliable A/B (~25 min)
**When:** Feature validation before committing. Statistically meaningful comparison.

```
run_music_test_multi(ports, audio_file, ground_truth, port_commands, runs=3, duration_ms=45000)
```

- **4 tracks, 3 runs each, 2 devices**
- 45s duration to clear intros (many tracks have 10-15s beatless intros)
- 3 runs reduces per-track std from ~0.15 to ~0.09
- **Detects improvements > ~0.05 F1** with reasonable confidence

**Reliable A/B Track Set** (adds a hard syncopated track):

| Track | BPM | Why |
|-------|-----|-----|
| `techno-minimal-01` | 129 | Strong 4-on-floor, "easy" |
| `trance-goa-mantra` | 136 | Moderate difficulty |
| `breakbeat-background` | 86 | Slow tempo, gravity well test |
| `garage-uk-2step` | 129 | Syncopated, pure phase alignment test (99% BPM, low F1) |

### Tier 4: Full Validation (~50 min)
**When:** Before merging to master. Final regression check.

```
run_validation_suite(ports, runs=3, duration_ms=45000)
```

- **18 tracks, 3 runs, 1-2 devices**
- Gold standard for release decisions
- Run once per feature, not during iteration
- Save results file for historical comparison

## Duration Guidelines

- **30s** for smoke tests and quick A/B (sufficient for most tracks with strong intros)
- **45s** for reliable A/B and full validation (clears 10-15s beatless intros)
- **Full track** only when investigating specific track behavior

## When NOT to Run Full Validation

- Proof-of-concept testing (use Tier 2)
- Parameter sweeps (use Tier 2 on individual tracks)
- Debugging a specific track (use single `run_music_test` with `runs=3-5`)
- Checking if firmware compiles/boots (use Tier 1)

## Multi-Device A/B Best Practices

- Always use `run_music_test_multi` or `run_validation_suite` with `port_commands` for A/B
- Both devices hear identical audio → no acoustic confounding
- Set baseline on port 0, variant on port 1
- Use `duration_ms` parameter, not full track length, for consistent test windows
