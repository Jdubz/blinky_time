# Hybrid Feature Analysis Plan

**Status:** In progress (Apr 2026)
**Owner:** Audio/ML
**Supersedes:** ad-hoc `flat`/`rflux` streaming (b132)

## Goal

Evaluate a catalog of **deterministic audio features** (spectral shape, transient, phase) against three questions:

1. **Does the signal exist?** Does the feature actually separate GT onset frames from non-onset frames on clean audio?
2. **Does the signal survive?** Does the offline value match the on-device value on the same track (speaker → room → mic → INT8)?
3. **Is the implementation correct?** Does the firmware computation match its Python reference bit-for-bit?

A feature passes only if all three answer yes. Passing features feed two downstream paths:

- **Deterministic FP gates** on-device — cheap, fast to iterate, extends the existing bass-gate pattern.
- **New NN input channels** — retrain with the winners to raise the F1 ceiling.

Features that discriminate offline but drift on-device are either dropped or flagged as "close the gap first" work (e.g., a normalization or calibration missing from the firmware path).

## Design decisions

### D1: Selective feature computation + streaming (not ring buffer)

Every candidate feature has an independent runtime flag that gates **both computation and streaming**:

```
stream signal flatness on    # compute AND stream
stream signal centroid off   # compute skipped, field absent from the stream
```

- **Deployed firmware cost:** zero (all flags default off, branch-predicted out).
- **Test-time cost:** proportional to what's enabled.
- **Bandwidth:** only enabled features consume stream bytes.

Rejected alternative: fixed ring buffer of all candidates dumped post-run. That requires always-computing every feature during tests, which changes the detection CPU budget relative to deployment and poisons the baseline we're measuring against.

### D2: Labels come from ground truth, not the NN

The NN's own opinion is explicitly **not** a label source. We want to know whether a feature separates real onsets from real non-onsets, independent of any model. Labeling runs off `onsets_consensus.json` / `.beats.json` only:

- **Onset frame:** within ±50 ms of any GT onset.
- **Non-onset frame:** everything else.

No "true positive" vs "false positive" language in Phase 1 — that framing bakes in NN bias and answers a different question ("which features explain the NN's mistakes"). That question is interesting but downstream: if a feature passes Phase 1–3, *then* we can ask whether adding it to the NN reduces FPs (Phase 4 Path B).

### D3: Firmware-to-Python parity is a first-class acceptance criterion

Every signal the firmware computes must match its Python reference within a documented MAE. The existing mel-pipeline parity (MAE = 0.44 INT8 levels, see `AUDIO_ARCHITECTURE.md`) is the template. Without parity, any offline-vs-device discrepancy is ambiguous: it could be acoustic-chain drift OR an implementation bug. Parity tests rule out the implementation side before we invest in on-device analysis.

## Candidate signal catalog

Ten candidates grouped by family.

| Signal | Family | Rationale | Cost estimate (nRF52840 frame) |
|--------|--------|-----------|---------------------------------|
| Spectral flatness (Wiener entropy) | Shape | Tonal↔noise discriminator; drums peaky, sustained tones flat | ~0.3 ms (deployed) |
| Raw spectral flux (SuperFlux) | Transient | Magnitude attack rate; peaks at broadband onsets | ~0.2 ms (deployed) |
| Complex spectral difference | Transient | Phase-aware flux; suppresses steady-state tones even at loud levels | ~0.8 ms (phase history needed) |
| High-frequency content (HFC) | Shape | Percussion has broadband high-freq energy; tonal impulses are low-freq-dominant | ~0.2 ms |
| Weighted phase deviation (WPD) | Phase | Measures how far phase drifts from linear prediction; tonal tones are phase-stable | ~0.7 ms |
| Kick-band / total ratio | Energy | Explicit bass-dominance test (generalizes current bass-gate) | ~0.1 ms |
| Crest factor (peak / RMS) | Shape | Transients have high crest; sustained tones have low crest | ~0.3 ms |
| Rényi entropy (α=2) | Shape | Alternative noisiness measure, more robust to quantization than flatness | ~0.4 ms |
| Spectral centroid | Shape | Where the spectral "mass" is; drums shift centroid per hit | ~0.2 ms |
| Spectral rolloff (85th percentile) | Shape | Bandwidth proxy; tonal impulses are narrow-band | ~0.3 ms |

Total if all enabled: ~3.5 ms/frame. At 62.5 Hz that's 22 % CPU — unacceptable for deployment but fine when measuring a subset during tests.

## Phase plan

### Phase 1 — Does the signal exist? (Python, no NN)

**Input:** GT corpus (EDM tracks + consensus onsets) and, optionally, synthetic tonal clips (pure chords, synth pads, vocals).
**Compute:** all 10 candidate features per frame.
**Label:** onset (within ±50 ms of any GT onset) or non-onset.
**Report per feature:** mean / std / distribution per class, Cohen's d (onset vs non-onset), ROC-AUC as a standalone classifier, KS statistic.
**Aggregate across corpus:** per-track-weighted mean of |d|, AUC, KS; rank by |d|.
**Keep:** top 6 for Phase 2. Drop anything with |d| < 0.1 or inconsistent signed d across tracks.

**Deliverable:** ranked shortlist appended to this doc. Code in `ml-training/analysis/`.

### Phase 2 — Make it measurable on-device

Three chunks of work, in this order:

**2a. Firmware-to-Python parity tests.** For each shortlisted signal: a Python reference matching the firmware implementation line-by-line, a short audio fixture, and an assertion on MAE (target ≤ 0.005 in normalized units, ≈ INT8 granularity). Reuse the pattern from `ml-training/verify_mel_pipeline.py`. A feature fails this stage if the firmware version disagrees with its Python reference — that's an implementation bug, not an acoustic-chain issue, and it must be fixed before any on-device measurement.

**2b. Selective streaming infrastructure.**

- Firmware: `SignalRegistry` with one entry per candidate (name, compute_fn, flag). `SerialConsole` gets `stream signal <name> on|off`. Stream tick iterates the registry and emits `"signals":{"flat":0.34, "hfc":0.81, ...}` with only enabled signals. Compute gated by flag — zero cost when disabled.
- Server: `test_session.py` ingests arbitrary signal fields under `signals` into a generic `SignalFrame` list (replacing the hardcoded `NNFrame`). `scoring.py` generalizes `HybridMetrics` so every streamed signal gets its own onset-vs-non-onset stats. `test_runner.py` accepts `signals: [str]` in validate/sweep requests and issues `stream signal X on` before the run.

**2c. Acceptance check.** A validate request with `signals=["flatness","hfc"]` produces a capture with both fields populated; `signals=[]` produces a capture with no signal fields (confirms zero-cost baseline when no investigation is active).

### Phase 3 — Does the signal survive the acoustic chain?

Play the same GT tracks through two paths:

- **Offline:** Python (Phase 1 code).
- **On-device:** validation harness with the signal flags enabled on all 4 devices.

Align frames by timestamp (within 1 hop = 16 ms). Per signal, compute:

- **Pearson correlation** between offline and on-device values on matched frames — primary "does the signal survive?" metric.
- **KS statistic** on the two distributions — catches distribution-level drift that correlation misses.
- **On-device Cohen's d** (onset vs non-onset, same labels as Phase 1) — does the offline discrimination carry over?
- **Cross-device std** across the 4 devices — are the devices seeing the same signal?

**Deployability score:**

```
score = |d_ondevice| × corr_offline_device × (1 / max(ε, cross_device_std_normalized))
```

with `ε = 0.01` to prevent a perfectly consistent signal from scoring infinitely. High score = real signal, survives the chain, consistent across devices.

**Diagnostic for low correlation:** if a feature has good offline |d| but poor offline-vs-device correlation, investigate before dropping. Typical suspects are (a) a firmware normalization path the offline code doesn't replicate, (b) noise-subtraction that kills the signal differently on device, (c) INT8 quantization clipping the dynamic range. Fixing any of these closes the on-device gap for this feature and often for others too. Log findings under "gap-closing candidates" in the Phase 3 writeup.

**Deliverable:** decision matrix appended to this doc listing per-signal offline |d|, on-device |d|, offline↔device correlation, cross-device std, final score, and any gap-closing notes.

### Phase 4 — Act on the shortlist

Two paths run in parallel; whichever lands first wins.

**Path A: deterministic FP gates.** For each top-ranked signal, add a threshold-based gate in the onset detection code (extending the current bass-gate pattern). Sweep thresholds via `run_param_sweep`. Cheap to try, cheap to revert.

**Path B: retrain NN with new features.** Add the top 2–3 signals as additional NN input channels (bumps 32 → 34–35). Retrain v28, evaluate, deploy if F1 improves by ≥ 0.03.

Expected outcome: one new deterministic gate (cheap) + one new NN input feature (expensive but higher ceiling). Neither is guaranteed — a shortlist that all fails Phase 3 means the acoustic chain is eating every signal and we need to work on that chain instead.

**Phase 4 exit gate — strip losing candidates from firmware.** Before any Phase 4 work merges, every candidate feature that did **not** make the shortlist must be deleted from the firmware (computation + flag + stream field), not just flag-defaulted-off. Run `grep -r 'SignalRegistry' blinky-things/` to confirm only the shipped signals remain. Investigation-only code that lingers in deployment firmware is how flash budget leaks — see the `v21`–`v26` cleanup in b132 for a recent cautionary tale.

## Open questions / risks

1. **Parity cost.** Each firmware feature needs a Python reference and a parity test. For 6 signals that's ~6 days of careful work. Reuse `ml-training/verify_mel_pipeline.py` as a template to keep per-signal cost to ~1 day.

2. **Phase history RAM.** Complex SD and WPD both need per-bin phase memory (128 floats × 2 = 1 KB RAM each). Acceptable on nRF52840 (256 KB total, ~20 KB globals + arena currently), but only if the buffer is shared across signals that need it.

3. **Label noise from unlabeled onsets.** GT labels are kick/snare-weighted. An NN or feature firing on a hi-hat counts as "non-onset" under current labels but is actually a real onset. Mitigations: (a) use drum-stem-labeled consensus onsets (already in use — `onsets_consensus.json`), (b) include synthetic tonals as a clean "all firings are wrong" corpus, (c) spot-check the strongest apparent disagreements manually.

4. **Investigation ≠ deployment.** The test-time firmware has feature flags and compute paths that deployed firmware shouldn't carry. When Phase 4 picks winners, strip unused candidates — see Phase 4 exit gate above.

5. **Scope creep.** The catalog could grow indefinitely (50+ onset-detection features in the literature). Start with the 10 listed; only expand if Phase 3 scores are disappointingly low across the entire shortlist.

## References

- `docs/AUDIO_ARCHITECTURE.md` — current v27-hybrid architecture and the 32-input feature set.
- `docs/ML_IMPROVEMENT_PLAN.md` — NN training roadmap.
- `blinky-server/blinky_server/testing/scoring.py` `HybridMetrics` — prototype scoring shipped in b132 for `flat`/`rflux`. Generalize in Phase 2.
- `ml-training/verify_mel_pipeline.py` — parity template for Phase 2a.
- Bello et al., *A Tutorial on Onset Detection in Music Signals*, IEEE TSAP 2005 — most of the candidate catalog derives from Section III.

---

## Phase 1 results — 2026-04-20

**Corpus:** 18 EDM tracks (`blinky-test-player/music/edm/`), 143,289 frames at 62.5 Hz, `onsets_consensus.json` as ground truth.
**Labeling:** frame is "onset" if within ±50 ms of any GT onset, "non-onset" otherwise. No NN involved.
**Code:** `ml-training/analysis/features.py` + `ml-training/analysis/run_catalog.py`.
Regenerate with `./venv/bin/python -m analysis.run_catalog --corpus ../blinky-test-player/music/edm --out outputs/feature_catalog`.

**Corpus-weighted ranking** (|d| is mean per-track-weighted |Cohen's d| between onset and non-onset frames; signed d sign convention: positive = feature is larger on onset frames; `pos tracks` = fraction of tracks with d > 0, i.e. how often the sign is correct):

| rank | feature | \|d\| | d (signed) | pos tracks | AUC | KS |
|-----:|---------|-----:|-----------:|-----------:|----:|----:|
| 1 | complex_sd | 0.217 | +0.201 | 17/18 | 0.549 | 0.092 |
| 2 | hfc | 0.210 | +0.201 | 16/18 | 0.556 | 0.107 |
| 3 | raw_flux | 0.155 | +0.152 | 17/18 | 0.554 | 0.091 |
| 4 | flatness | 0.142 | +0.106 | 13/18 | 0.531 | 0.082 |
| 5 | centroid | 0.130 | +0.086 | 12/18 | 0.528 | 0.084 |
| 6 | wpd | 0.103 | +0.095 | 16/18 | 0.528 | 0.060 |
| 7 | rolloff | 0.101 | +0.040 | 10/18 | 0.524 | 0.076 |
| 8 | kick_ratio | 0.095 | −0.012 | 10/18 | 0.481 | 0.071 |
| 9 | crest | 0.076 | −0.049 | 5/18 | 0.480 | 0.063 |
| 10 | renyi | 0.069 | +0.005 | 10/18 | 0.523 | 0.069 |

**Findings.**

1. **All effect sizes are weak offline** (|d| < 0.5 everywhere, top signal at 0.22). This is the expected offline baseline: the features *do* separate onsets from non-onsets on clean audio, but only modestly. The interesting question is whether on-device measurement preserves or erodes this separation, which is what Phase 3 answers.

2. **Four features are reliably positive discriminators across the corpus:** `complex_sd` (17/18 tracks), `raw_flux` (17/18), `hfc` (16/18), `wpd` (16/18). These have both non-trivial |d| and a consistent sign — the signal exists and points the same way on nearly every track.

3. **Three candidates show inconsistent sign and low |d|:** `kick_ratio` (10/18), `crest` (5/18), `renyi` (10/18). Their means wobble around zero across tracks. Drop these from Phase 2 — they don't pass the "does the signal exist?" bar.

4. **`flatness` and `centroid` are weaker** than the top four but still positive on a majority of tracks. Keep for Phase 2 as secondary candidates; on-device behavior may differ.

**Phase 2 shortlist** (6 features):

1. `complex_sd` — phase-aware transient, not currently in NN input. **Primary.**
2. `hfc` — broadband HF content, not currently in NN input. **Primary.**
3. `raw_flux` — already in NN input; parity reference and baseline for Phase 3 scaling.
4. `flatness` — already in NN input; parity reference and baseline.
5. `wpd` — weaker |d| but 16/18 sign-consistent; only other phase-based candidate.
6. `centroid` — cheapest compute (~0.2 ms), weak but consistent-ish (12/18); include for cost/benefit breadth.

**Dropped:** `kick_ratio`, `rolloff`, `crest`, `renyi`.

**Phase 2 implication.** Firmware-parity tests and `SignalRegistry` scaffolding target these 6. `raw_flux` and `flatness` already have firmware implementations in `SharedSpectralAnalysis` — parity tests there become first deliverables. The other four need Python reference + firmware implementation + parity in Phase 2a before any on-device measurement in Phase 3.
