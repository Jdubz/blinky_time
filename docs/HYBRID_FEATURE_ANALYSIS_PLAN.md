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

**Percussion corpus:** 18 EDM tracks, 9,763 GT onsets total.
**Tonal corpus:** 9 synthetic tracks (`generate_tonal_corpus.py`), 720 impulses — sine/saw/FM stabs, detuned leads, harmonic stacks, bass note attacks, vocal-formant bursts, pad+stab mix. Pure tonal content only, known impulse times.
**Labeling:** per-onset peak — one sample per GT onset = max feature value within ±50 ms. Non-onset frames = at least 100 ms from every GT onset. No NN.
**Code:** `ml-training/analysis/features.py` + `generate_tonal_corpus.py` + `run_catalog.py`.
Regenerate with

```
./venv/bin/python -m analysis.generate_tonal_corpus --out ../blinky-test-player/music/synthetic_tonals
./venv/bin/python -m analysis.run_catalog --perc-corpus ../blinky-test-player/music/edm --tonal-corpus ../blinky-test-player/music/synthetic_tonals --out outputs/feature_catalog
```

### Primary metric: percussion-peak vs tonal-impulse-peak (cross-corpus)

Positive signed d ⇒ the feature is *larger* on percussion peaks than on tonal-impulse peaks (the direction we want for FP reduction). AUC < 0.5 ⇒ the ordering is reversed — tonal impulses score HIGHER than drums on this feature, so it fails as a discriminator even if it's a strong onset detector.

| rank | feature | d (perc − tonal) | AUC | perc_mean | tonal_mean | verdict |
|-----:|---------|----------------:|----:|---------:|---------:|---------|
| 1 | renyi | −2.05 | 0.132 | 2.10 | 16.27 | reversed, skip |
| 2 | centroid | **+1.37** | **0.831** | 37.12 | 19.66 | **strong discriminator** |
| 3 | flatness | **+1.23** | **0.813** | 0.490 | 0.260 | **strong discriminator** (already in NN) |
| 4 | raw_flux | −1.15 | 0.184 | 0.143 | 0.385 | reversed, skip (and currently in NN) |
| 5 | kick_ratio | **+1.13** | **0.810** | 0.887 | 0.547 | **strong discriminator** |
| 6 | hfc | −1.13 | 0.143 | 64.7 | 313.4 | reversed, skip |
| 7 | crest | **+1.00** | **0.765** | 9.03 | 7.43 | **moderate discriminator** |
| 8 | complex_sd | −0.89 | 0.248 | 13.4 | 22.6 | reversed, skip |
| 9 | rolloff | +0.80 | 0.625 | 36.98 | 16.96 | weak discriminator |
| 10 | wpd | −0.29 | 0.430 | 1.58 | 1.67 | near-tie, skip |

**Within-corpus sanity checks** (per-onset peak vs non-onset frame) confirm every feature fires at real onsets — percussion-corpus |d| ranges 0.59–1.21 with AUC 0.71–0.86 across the full catalog. The sanity check is not the discriminator.

### Findings

1. **The real drum-vs-tonal discriminators are shape metrics**, not transient detectors. `centroid`, `flatness`, `kick_ratio`, `crest` all have positive cross-corpus d > 1.0 and AUC > 0.75. They describe *what the spectrum looks like* at an attack, and drums genuinely look different from tonal impulses in that respect.

2. **Four "best onset detectors" are reversed cross-corpus.** `raw_flux`, `hfc`, `complex_sd`, `renyi` all score higher on tonal impulses than on drums on this corpus — they detect *any* sharp attack, percussion or not. They cannot distinguish drums from tonal impulses by themselves.

3. **`raw_flux` being reversed is the most consequential result.** It's already an NN input feature (deployed at b132). If the NN had been relying on raw_flux alone to reject tonals, this data says it couldn't. The model presumably uses mel+flatness+flux jointly, and flatness does discriminate — but adding raw_flux as an input may actually *teach* the NN that any broadband attack is drum-like, which fits the observed precision plateau at 0.50.

4. **Synthetic corpus caveat — likely an upper bound on difficulty.** The generated tonal tracks have near-instant attacks and silence between impulses; real tonal content sits inside continuous music with slower attacks and ongoing background energy. A feature that fails against this synthetic corpus will likely still fail against real FP-inducing events; a feature that passes should be considered a lower bound on real-world performance until Phase 3 confirms.

### Revised Phase 2 shortlist (4 features, priorities re-scored)

| # | feature | cross-corpus d | AUC | currently in NN? | firmware cost |
|--:|---------|-------------:|----:|:----------------:|--------------:|
| 1 | centroid | +1.37 | 0.83 | no | ~0.2 ms |
| 2 | flatness | +1.23 | 0.81 | yes | shipping |
| 3 | kick_ratio | +1.13 | 0.81 | partially (bass-gate) | ~0.1 ms |
| 4 | crest | +1.00 | 0.77 | no | ~0.3 ms |

**Dropped from previous shortlist:** `raw_flux` (reversed), `hfc` (reversed), `complex_sd` (reversed), `wpd` (near-zero), plus `rolloff` and `renyi` from the original dropped set.

**Phase 2 implications.**

- **Firmware parity tests** (2a) now target `centroid`, `kick_ratio`, `crest` as *new* implementations + Python references (3 days instead of 6). `flatness` already shipped, parity-test it too to confirm the existing implementation matches the Python reference we just validated.
- **`raw_flux` as NN input is suspect.** Worth running the NN with `raw_flux` ablated (feed zero into that input channel) on a validation set to see if F1 holds or improves. If removing it doesn't hurt, that's one less ms of compute and one fewer FP-inducing input. Add this as a parallel Phase 2d task.
- **`kick_ratio` generalizes the existing bass-gate.** The on-device bass-gate is currently a binary threshold on bass-band ratio used only to gate pulse detection. Using `kick_ratio` as an NN input, or as a continuous gate, may recover some of the precision the bass-gate alone misses.

**Open question.** Every feature reversed here has one thing in common: it's summed across the spectrum. The discriminators (centroid, flatness, kick_ratio, crest) are all *shape* metrics (ratios or normalized statistics). Is there a principled reason why absolute magnitudes will always get fooled by sharp-attack tonal content? If so, future feature exploration should stay in the shape-metric family.
