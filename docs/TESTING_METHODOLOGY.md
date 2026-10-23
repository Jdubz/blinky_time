# Testing Methodology

## Development Workflow

Audio tests are loud and time-consuming. Use the minimum test tier needed to answer your current question. Never gather more data than the decision requires.

The task is **onset detection** (fire on every percussive hit), scored against `.onsets_consensus.json` ground truth on the `blinky-test-player/music/edm/` corpus (18 tracks). See CLAUDE.md.

```
Code change
    │
    ▼
Tier 0: Host harness (silent, minutes)
    "Does the real tracker code still score on edm/ offline?"
    │
    ▼
Tier 1: Smoke test (2 min)
    "Does it work at all? BPM plausible? Onsets firing? No crashes?"
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
- Tier 0 before any device test when the change touches `audio/` — it is silent and deterministic
- Tier 1 after every compile+flash — always
- Tier 2 for proof-of-concept — prove/disprove quickly, then iterate or abandon
- Tier 3/4 only when features are finalized — never during iteration
- Parameter sweeps use Tier 2 on individual tracks, not the full suite
- Never run full validation to test a single code change

## Tiered Testing Approach

### Tier 0: Host Harness (no device, no speaker)
**When:** Any change to `AudioTracker`, `SharedSpectralAnalysis` or `AdaptiveMic`.

`tests/tracker/` runs the firmware's real audio code on the host against the edm/ corpus, with a simulated main loop and a CPU cost model calibrated to hat_v1. It reports onset F1 and rhythm metrics, and can compare two builds. Build and run instructions: `tests/tracker/README.md`.

```bash
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness     --compare build/tracker_harness_old --tracks techno-minimal-01
```

CI runs it too (the "Tracker Harness" job in `.github/workflows/pr-validation.yml`).

### Tier 1: Smoke Test (~2 min)
**When:** After every compile+flash. Verify firmware boots and feature activates.

```
run_test(port="/dev/ttyACM0", tracks=["techno-minimal-01"], duration_ms=30000, commands=["set fluxdelta 14"])
# returns job_id — poll with check_test_result(job_id)
```

- **1 track, 1 run, 1 device**
- Use `techno-minimal-01` (strong 4-on-floor kicks, 129 BPM, reliable baseline)
- Check: BPM output plausible? Onset events firing? No crashes?
- **NOT for performance measurement** — variance too high for 1 run

### Tier 2: Quick A/B (~10 min)
**When:** Proof-of-concept testing. Does a code change show any signal at all?

```
# Multi-device A/B uses run_validation_suite (run_test is single-port).
run_validation_suite(
    ports=["/dev/ttyACM0", "/dev/ttyACM1"],
    tracks=["techno-minimal-01", "trance-goa-mantra", "breakbeat-background"],
    runs=1,
    duration_ms=30000,
    per_device_commands={"/dev/ttyACM0": ["set fluxdelta 7"], "/dev/ttyACM1": ["set fluxdelta 14"]},
)
# returns job_id — poll with check_test_result(job_id)
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
| `breakbeat-background` | 86 | Slow tempo, syncopated breaks |

### Tier 3: Reliable A/B (~25 min)
**When:** Feature validation before committing. Statistically meaningful comparison.

```
run_validation_suite(
    ports=["/dev/ttyACM0", "/dev/ttyACM1"],
    tracks=["techno-minimal-01", "trance-goa-mantra", "breakbeat-background", "garage-uk-2step"],
    runs=3,
    duration_ms=45000,
    per_device_commands={"/dev/ttyACM0": ["set fluxdelta 7"], "/dev/ttyACM1": ["set fluxdelta 14"]},
)
# returns job_id — poll with check_test_result(job_id)
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
| `breakbeat-background` | 86 | Slow tempo, syncopated breaks |
| `garage-uk-2step` | 129 | Syncopated 2-step: many off-beat hits |

### Tier 4: Full Validation (~50 min)
**When:** Before merging to master. Final regression check.

```
run_validation_suite(ports=[...], runs=3, duration_ms=45000)
# returns job_id — poll with check_test_result(job_id)
```

- **18 tracks (all of edm/), 3 runs, 1-2 devices**
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
- Debugging a specific track (use single-port `run_test(port=..., tracks=[name], runs=3-5)`)
- Checking if firmware compiles/boots (use Tier 1)

## Multi-Device A/B Best Practices

- Use bench test devices, never installed fleet devices. Setup commands are runtime `set` changes only; validation resets devices to defaults before each run. Never send `save`, `device upload` or `reboot` here (CLAUDE.md).
- `port` accepts a serial port or a device ID. `run_test` defaults to 35 s per track and 1 run; `run_validation_suite` defaults to 3 runs.
- `run_test` is **single-port only** (`port` is a required scalar). For A/B across multiple devices, use `run_validation_suite(ports=[...], per_device_commands={...})`. Poll results with `check_test_result(job_id)`.
- Both devices hear identical audio → no acoustic confounding
- Set baseline on port 0, variant on port 1 via `per_device_commands`
- Use `duration_ms` parameter, not full track length, for consistent test windows
