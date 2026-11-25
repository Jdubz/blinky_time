# Testing Methodology

## Development Workflow

Audio tests are loud and time-consuming. Use the minimum test tier needed to answer your current question. Never gather more data than the decision requires.

The task is **drum-hit detection**: kicks and snares, wherever they fall in the bar (CLAUDE.md, "Task is Drum-Hit Detection"). Runs are scored on the `corpus/edm/` corpus (18 tracks) by `blinky_scoring` in the `corpus/` submodule (blinky-corpus). The primary numbers are the drum metrics: kick and snare recall at ±30/±50 ms (`kickR30`, `kickR50`, `snareR50`), the share of detections on a kick or snare (`onDrum`), and drum F (`drumF`). `snareR50` is provisional until the drum labels are validated (blinky-corpus#4). All-onset F1 against `.onsets_consensus.json` is a secondary number.

Device tests run on the test box's one chip (`TEST_BOX.md`). Every chip on the box is a test chip, so setup commands go straight to it; validation still resets the chip to defaults before each run.

```
Code change
    │
    ▼
Tier 0: Host harness (silent, minutes)
    "Does the real tracker code still score on edm/ offline?"
    │
    ▼
Tier 1: Smoke test (2 min)
    "Does it work at all? Detections firing? No crashes?"
    │
    ├─ Broken → fix and re-smoke
    │
    ▼
Tier 2: Quick A/B (~20 min)
    "Is it better than baseline? Any signal at all?"
    │
    ├─ Worse or neutral → abandon approach, try something else
    ├─ Shows signal → parameter sweep (Tier 2 on individual tracks)
    │
    ▼
Lock in feature set (may involve multiple Tier 1/2 iterations)
    │
    ▼
Tier 3: Reliable A/B (~50 min)
    "Is the improvement real and statistically meaningful?"
    │
    ├─ Not significant → reconsider before committing
    │
    ▼
Tier 4: Full Validation (~50 min)
    "Final regression check before merge"
    → Establishes new baseline for future comparisons
```

**Key rules:**
- Tier 0 before any device test when the change touches `audio/`: it is silent and deterministic
- Tier 1 after every flash, always
- Tier 2 for proof of concept: prove or disprove quickly, then iterate or abandon
- Tier 3/4 only when features are finalized, never during iteration
- Parameter sweeps use Tier 0, or Tier 2 on individual tracks, not the full suite
- Never run full validation to test a single code change

## Tiered Testing Approach

### Tier 0: Host Harness (no device, no speaker)
**When:** Any change to `AudioTracker`, `SharedSpectralAnalysis` or `AdaptiveMic`.

blinky-firmware's `tests/tracker/` runs the firmware's real audio code on the host against the edm/ corpus, with a simulated main loop and a CPU cost model calibrated to hat_v1. It reports the drum metrics, onset F1 and rhythm metrics, and can compare two builds. Build and run instructions: blinky-firmware's `tests/tracker/README.md`.

```bash
# in a blinky-firmware checkout
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness \
    --compare build/tracker_harness_old --tracks techno-minimal-01
```

blinky-firmware's CI runs it too (the "Tracker Harness" job in its `pr-validation.yml`), and its `tracker-ab.yml` workflow runs a host A/B between two refs. Host A/B is the cheapest A/B there is: use it before a device A/B.

### Tier 1: Smoke Test (~2 min)
**When:** After every flash. Verify firmware boots and the feature activates.

```
run_test(port="<chip id or port>", tracks=["techno-minimal-01"], duration_ms=30000, commands=["set fluxdelta 8"])
# returns job_id — poll with check_test_result(job_id)
```

- **1 track, 1 run**
- Use `techno-minimal-01` (strong 4-on-floor kicks, 129 BPM, reliable baseline)
- Check: detections firing on the kicks? No crashes, no `[ASSERT]`/`[FALLBACK]`?
- **Not for performance measurement:** variance is too high for one run

### Tier 2: Quick A/B (~20 min)
**When:** Proof-of-concept testing. Does a code change show any signal at all?

With one chip, an A/B runs the two configurations one after the other on the same chip, through the same speaker. Alternate them (A, B, A, B) rather than all-A then all-B, so drift in the room or the chip doesn't land on one side.

```
# A, then B; repeat for each round
run_validation_suite(ports=["<chip>"], tracks=["techno-minimal-01", "trance-goa-mantra", "breakbeat-background"],
                     runs=1, duration_ms=30000, commands=["set fluxdelta 7"])
run_validation_suite(ports=["<chip>"], tracks=["techno-minimal-01", "trance-goa-mantra", "breakbeat-background"],
                     runs=1, duration_ms=30000, commands=["set fluxdelta 8"])
# each returns job_id — poll with check_test_result(job_id)
```

- **3 tracks, 1 run each, per configuration**
- Catches catastrophic regressions and large improvements only: single runs vary a lot (per-track onset-F1 std was ~0.15 in earlier measurements)

**Quick A/B Track Set** (chosen for diversity):

| Track | BPM | Why |
|-------|-----|-----|
| `techno-minimal-01` | 129 | Strong 4-on-floor, reliable baseline, "easy" |
| `trance-goa-mantra` | 136 | Moderate difficulty, good reference point |
| `breakbeat-background` | 86 | Slow tempo, syncopated breaks |

### Tier 3: Reliable A/B (~50 min)
**When:** Feature validation before committing. Statistically meaningful comparison.

Same shape as Tier 2, with more tracks, more runs and longer windows:

```
# three rounds; in each round run A, wait for it to finish, then run B
run_validation_suite(ports=["<chip>"],
                     tracks=["techno-minimal-01", "trance-goa-mantra", "breakbeat-background", "garage-uk-2step"],
                     runs=1, duration_ms=45000, commands=[...A...])
run_validation_suite(ports=["<chip>"], tracks=[...same...], runs=1, duration_ms=45000, commands=[...B...])
```

- **4 tracks, 3 runs each, per configuration, interleaved A/B by round.** One suite call holds a single session with fixed setup commands for all its runs (`testing/workflows.py`). So `runs=3` once for A and once for B gives all-A then all-B, and room or chip drift would land on one side.
- 45 s duration to clear intros (many tracks have 10–15 s beatless intros)
- 3 runs cut per-track std from ~0.15 to ~0.09 (onset F1, earlier measurements)

**Reliable A/B Track Set** (adds a hard syncopated track):

| Track | BPM | Why |
|-------|-----|-----|
| `techno-minimal-01` | 129 | Strong 4-on-floor, "easy" |
| `trance-goa-mantra` | 136 | Moderate difficulty |
| `breakbeat-background` | 86 | Slow tempo, syncopated breaks |
| `garage-uk-2step` | 129 | Syncopated 2-step: many off-beat hits |

### Tier 4: Full Validation (~50 min)
**When:** Before merging. Final regression check.

```
run_validation_suite(ports=["<chip>"], runs=3, duration_ms=45000)
# returns job_id — poll with check_test_result(job_id)
```

- **18 tracks (all of edm/), 3 runs**
- Gold standard for release decisions
- Run once per feature, not during iteration
- Save the results file for later comparison

The staging suite on the box (`TEST_BOX.md`, "Staging runner") plays every edm/ track once for each staging build and reports the numbers; it doesn't gate on them yet.

## Duration Guidelines

- **30 s** for smoke tests and quick A/B (enough for most tracks with strong intros)
- **45 s** for reliable A/B and full validation (clears 10–15 s beatless intros)
- **Full track** only when investigating specific track behavior

## When NOT to Run Full Validation

- Proof-of-concept testing (use Tier 0 or Tier 2)
- Parameter sweeps (use Tier 0, or Tier 2 on individual tracks)
- Debugging a specific track (use `run_test(port=..., tracks=[name], runs=3-5)`)
- Checking if firmware compiles or boots (use Tier 1)

## A/B practice

- Use the box's test chip, never an installed fleet device. Setup commands are runtime `set` changes; validation resets the chip to defaults before each run. Don't send `save` in a test.
- `port` accepts a serial port or a device ID. `run_test` defaults to 35 s per track and 1 run; `run_validation_suite` defaults to 3 runs.
- One chip means A and B never hear the same playback. Alternate the configurations and use enough runs that the difference is larger than the run-to-run spread.
- Use the `duration_ms` parameter, not full track length, for consistent test windows.

## Removed metrics (do not reintroduce)

These were dropped from scoring because they measure beat tracking, not drum hits:

| Metric | Why |
|--------|-----|
| beatF1 | Confounds music structure with model quality |
| CMLt / CMLc / AMLt | Beat continuity is meaningless for a visualizer |
| detectedBpm | Just a number; half and double time are equally valid |
| phaseStability | Coupled to the BPM estimate, misleading |
| downbeatF1 | Meter position is irrelevant |
| bpmError | Never a server metric |
