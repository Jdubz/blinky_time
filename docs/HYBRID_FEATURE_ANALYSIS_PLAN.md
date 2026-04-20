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

**Scope: EDM only.** The fleet's target deployment is EDM-driven installations (techno, trance, breakbeat, dnb, dubstep, garage, amapiano, reggaeton, etc. — everything already in the corpus). This investigation does not need to generalize to rock, jazz, classical, or sparse percussion. Feature rankings and sign-stability results are taken as authoritative for EDM without further cross-genre validation.

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

### Primary metric: |d| between percussion and tonal-impulse peaks

|d| and the AUC distance from 0.5 measure **how separable the two classes are**. Sign is metadata — "higher on tonals" is exactly as discriminative as "higher on drums", you just flip the comparison direction. For an NN input, the model learns direction automatically; for a deterministic gate, you invert the threshold. Sign only disqualifies a feature if it's **unstable across different tonal contexts** (a threshold tuned on one corpus breaks on another). That's what Phase 2d below tests.

| rank | feature | \|d\| | AUC distance from 0.5 | sign (perc − tonal) | perc_mean | tonal_mean |
|-----:|---------|-----:|-----:|:----:|---------:|---------:|
| 1 | renyi | 2.05 | 0.368 | − | 2.10 | 16.27 |
| 2 | centroid | 1.37 | 0.331 | + | 37.12 | 19.66 |
| 3 | flatness | 1.23 | 0.313 | + | 0.490 | 0.260 |
| 4 | raw_flux | 1.15 | 0.316 | − | 0.143 | 0.385 |
| 5 | kick_ratio | 1.13 | 0.310 | + | 0.887 | 0.547 |
| 6 | hfc | 1.13 | 0.357 | − | 64.7 | 313.4 |
| 7 | crest | 1.00 | 0.265 | + | 9.03 | 7.43 |
| 8 | complex_sd | 0.89 | 0.252 | − | 13.4 | 22.6 |
| 9 | rolloff | 0.80 | 0.125 | + | 36.98 | 16.96 |
| 10 | wpd | 0.29 | 0.070 | − | 1.58 | 1.67 |

Nine features have |d| ≥ 0.8 and AUC distance ≥ 0.13. All nine are usable for discrimination; they differ in *how* they can be used and *how stable* their direction is.

**Within-corpus sanity checks** (per-onset peak vs non-onset frame) confirm every feature fires at real onsets — percussion-corpus |d| ranges 0.59–1.21 with AUC 0.71–0.86 across the full catalog. That's a sanity check that the feature implementation is doing something, not a discrimination score.

### Findings

1. **Nine features with useful |d|, in two groups by sign.** Positive-sign (`centroid`, `flatness`, `kick_ratio`, `crest`, `rolloff`): drums score higher than tonals. Negative-sign (`renyi`, `raw_flux`, `hfc`, `complex_sd`): tonals score higher than drums on this corpus. All measure something real.

2. **The sign split has a mechanical explanation.** The negative-sign features all measure *total spectral activity* — flux sums energy changes, HFC sums bin-weighted energy, complex-SD sums residuals, Rényi on power distribution. The synthetic corpus has silence between impulses and sharp digital attacks with rich harmonic stacks, so a tonal impulse produces an enormous total-energy jump from zero. Real music has continuous baseline energy and softer attacks, so the relative jump is smaller. The positive-sign features (centroid, flatness, kick_ratio, crest) are spectral *shape* metrics — normalized ratios that describe what the spectrum looks like at the peak, independent of total energy. Shape signs are likely more stable across corpora; activity signs probably aren't.

3. **`raw_flux` is both an NN input AND context-dependent.** Already shipping (b132). The NN can and presumably does learn some useful direction from it, but if the sign direction differs between "synthetic silence-interrupted tonals" and "real continuous music with tonal stabs", a raw_flux-driven decision boundary inside the model is fragile. Worth checking by ablation (Phase 2d) whether raw_flux actually helps model F1 or whether it's neutral/hurtful — especially since it's one of just two non-mel input channels.

### Phase 2 shortlists by use case

**NN input candidates (Path B) — all 9.** The model learns direction. All nine features with |d| ≥ 0.8 are on the table; the 4 not yet in the NN (centroid, kick_ratio, crest, plus any of the reversed-sign candidates that survive Phase 2d) are the concrete additions.

**Deterministic gate candidates (Path A) — 4 sign-stable ones.** Until Phase 2d proves otherwise, only the shape-metric features get a gate: `centroid`, `flatness`, `kick_ratio`, `crest`. All are low-cost (~0.1–0.3 ms) and need no phase history. A gate with an unstable direction across corpora is a bug waiting to happen.

**Firmware parity tests (Phase 2a) priority order:**
  1. `flatness` — already shipping, parity-test first to confirm the firmware implementation matches the Python reference (fast, high-confidence win).
  2. `kick_ratio` — cheap and directly generalizes the existing bass-gate.
  3. `centroid` — cheap, unlocked new NN input channel.
  4. `crest` — cheap, transient-character signal missing from current NN inputs.
  5–9. Reversed-sign features — only after Phase 2d confirms sign stability.

### Phase 2d — Sign stability test

Purpose: confirm that the sign of each feature's cross-corpus d is stable when the tonal corpus changes. A feature whose sign flips between different tonal contexts cannot be used as a single-threshold gate — the threshold direction is context-dependent, which breaks under any deployment that doesn't match the tuning corpus.

Concrete test:
1. Regenerate the tonal corpus with a second variant: `generate_tonal_corpus.py --variant embedded` — adds a continuous tonal bed under every impulse and multiplies attack times by 4×, simulating tonal content inside continuous music rather than silence-interrupted stabs.
2. Run `run_catalog.py` against each variant separately.
3. For each feature, record sign and |d| on both corpora. **Stable** = signs agree; **unstable** = signs differ.

### Phase 2d results — 2026-04-20

| feature | d (clean) | d (embedded) | AUC (clean) | AUC (embedded) | verdict |
|---------|----------:|------------:|------------:|--------------:|---------|
| centroid | +1.37 | +0.81 | 0.831 | 0.714 | **stable +** |
| flatness | +1.23 | +0.53 | 0.813 | 0.672 | **stable +** |
| crest | +1.00 | +0.66 | 0.765 | 0.709 | **stable +** |
| rolloff | +0.80 | +0.99 | 0.625 | 0.679 | **stable +** |
| hfc | −1.13 | −1.01 | 0.143 | 0.189 | **stable −** |
| raw_flux | −1.15 | −0.53 | 0.184 | 0.298 | **stable −** |
| complex_sd | −0.89 | −0.50 | 0.248 | 0.346 | **stable −** |
| kick_ratio | +1.13 | −0.66 | 0.810 | 0.333 | **UNSTABLE** (+ → −) |
| renyi | −2.05 | +0.32 | 0.132 | 0.587 | **UNSTABLE** (− → +) |
| wpd | −0.29 | +0.29 | 0.430 | 0.601 | weak (|d|<0.3 both) |

**Two genuine surprises.**

1. **`kick_ratio` is sign-unstable.** On silence-separated synthetic tonals, drums have a higher bass/total ratio than tonals (kicks dominate the spectrum). But when the tonal corpus has a low-frequency bed (my embedded variant uses 180 Hz + 270 Hz sustained voices), the bass/total ratio is permanently high regardless of what impulses fire — so the sign flips. This invalidates the "generalized bass-gate" hypothesis: the on-device bass gate works well against silence backgrounds but would fail against bass-heavy musical context. A kick_ratio gate tuned on one corpus would fire the wrong way on the other.

2. **`rolloff` is the cheapest sign-stable discriminator that wasn't on any prior shortlist.** Clean |d|=0.80 was modest; embedded |d|=0.99 actually *improved*. Rolloff cost is ~0.3 ms, same family as the other shape metrics. Promote it to the gate shortlist.

**Unexpected stability.** The three "reversed" features (`raw_flux`, `hfc`, `complex_sd`) all kept their sign when the context changed — tonal impulses consistently produced a larger total spectral activity change than drum hits on both corpora. Magnitude |d| dropped in the embedded variant (context-energy reduces the relative delta) but the direction held. That makes the reversed sign a robust signal, not a synthetic-corpus artifact.

**Three feature groups for Phase 2:**

| Group | Features | Use |
|-------|----------|-----|
| **Stable +** (drum > tonal) | centroid, flatness, crest, rolloff | Deterministic gates (all directions intuitive). NN inputs. |
| **Stable −** (tonal > drum) | raw_flux, hfc, complex_sd | NN inputs. Gates with inverted threshold (fire if LOW). |
| **Unstable** | kick_ratio, renyi | NN-only. Do NOT use as single-threshold gates. |

**Dropped for low |d| on both corpora:** `wpd` (|d| 0.29 / 0.29, mixed sign).

### Revised Phase 2 shortlist

**Phase 2a firmware parity tests (priority order):**
1. `flatness` — already shipping, validate existing implementation against Python reference.
2. `centroid` — cheap (~0.2 ms), no phase history, new NN input candidate.
3. `crest` — cheap (~0.3 ms), new NN input candidate.
4. `rolloff` — cheap (~0.3 ms), surprise winner from Phase 2d.
5. `hfc` — cheap (~0.2 ms), stable − so usable as inverted gate. NN input candidate once sign direction is confirmed in training.

Seven parity tests total (the four stable-+ plus raw_flux, hfc, complex_sd). Three implementations already exist in firmware (flatness, raw_flux, plus the bass-energy math used by kick_ratio and the existing gate). Four new implementations: centroid, crest, rolloff, hfc, complex_sd.

**Phase 2b selective streaming** then covers these seven. `kick_ratio` and `renyi` get flagged as NN-only and are excluded from gate-infrastructure work.

**Phase 2d ablation of raw_flux in the NN** remains a parallel task. The feature is stable-negative (useful to the model if it learns the inverted direction), but we don't yet know whether the current v27-hybrid model is learning that or is just noise-fitting on it.

### Held-out stability check — 2026-04-20

Concern: we'd iterated methodology on the same 18-track corpus three times (per-onset peak vs frame-mean, GT-only vs NN-oracle labels, clean vs embedded tonal variants). Each round could be tightening results to this specific corpus rather than finding generalizable discriminators.

Test: random 13/5 split (seed=42) of the percussion corpus, recompute cross-corpus d against both tonal variants for each half independently. Sign and magnitude should be stable across splits if the ranking isn't corpus-overfit.

Result — every feature's sign verdict is identical across full / kept-13 / held-out-5, and |d| values agree within ~0.15 on every feature. `renyi`, `kick_ratio`, and `wpd` remain classified UNSTABLE (sign flips between variants) on every split; the other seven remain STABLE. **The shortlist is not an overfit artifact** of which 18 EDM tracks we picked. EDM-only is the target scope (see Goal), so this is sufficient stability evidence to proceed.

### Training-set contamination — 2026-04-20 — **ACTION REQUIRED**

While setting up the held-out check, discovered: the 18 EDM tracks in `blinky-test-player/music/edm/` are byte-identical with files inside the v27-hybrid training corpus at `/mnt/storage/blinky-ml-data/audio/combined/`, with the same filenames.

Per `/mnt/storage/blinky-ml-data/processed_v27/.prep_splits.json`:

- **14 of 18** are in **train**: afrobeat-feelgood-groove, amapiano-vibez, breakbeat-background, breakbeat-drive, dnb-energetic-breakbeat, edm-trap-electro, garage-uk-2step, techno-deep-ambience, techno-dub-groove, techno-machine-drum, techno-minimal-01, techno-minimal-emotion, trance-goa-mantra, trance-party
- **4 of 18** are in **val** (used for model selection): dnb-liquid-jungle, dubstep-edm-halftime, reggaeton-fuego-lento, trance-infected-vibes
- **0** are in a held-out test split. The v27 pipeline split `train` + `val` and ran no blind-test evaluation.

**What this affects.**

- **Phase 1 feature ranking (this document):** unaffected. No NN is involved in feature computation or labeling, so training exposure does not contaminate the onset-vs-tonal Cohen's d.
- **On-device F1 = 0.63 reported for v27-hybrid (`AUDIO_ARCHITECTURE.md`):** *optimistic*. The reported number was measured (directly or indirectly) on tracks the model trained on or was selected against. Expect lower F1 on truly unseen music.
- **Phase 2d raw_flux ablation:** would be contaminated if run on these 18 tracks. Needs a held-out corpus (see below).
- **Any future Path B retrain evaluation:** must use an explicit held-out test split, or the F1 improvement threshold loses meaning.

**Action items.**

1. **Carve a true test split for future NN evaluations (EDM only).** Source EDM tracks that were not in `processed_v27/.prep_splits.json` and generate `onsets_consensus.json` GT for them. Re-split with an explicit `test` bucket in the next data-prep run. The v27 training should be considered "train + val only — no blind test"; any F1 number from it is an upper bound.
2. **Before any Phase 2d NN ablation**, obtain a held-out EDM set. Candidates inside `/mnt/storage/blinky-ml-data/`: `labels/edm-test/` (already has `.beats.json` GT — verify none are in v27 splits), `labels/fma/` EDM-tagged tracks (filter out any already in train/val).
3. **Update `AUDIO_ARCHITECTURE.md` and `IMPROVEMENT_PLAN.md`** to reflect that v27-hybrid F1 numbers are on train+val, not a blind test. Current numbers should not be compared against published benchmarks that use held-out test sets.
4. **Phase 3 feature-level offline-vs-device comparison is still valid** on these 18 tracks — features are computed from audio directly, not through the model — so on-device feature capture work doesn't need to wait.

This investigation's feature rankings stand. The NN evaluation methodology around them does not.

### Held-out EDM validation — 2026-04-20

Located 25 GiantSteps tracks inside the local ML-data storage that are **not** in `processed_v27/.prep_splits.json` (neither train nor val). Each has `.beats.json` GT already generated. Materialized as symlinks under `blinky-test-player/music/edm_holdout/` via `analysis/setup_holdout_corpus.py`.

Re-ran Phase 1 cross-corpus Cohen's d against both tonal variants on this truly-unseen corpus:

| feature | seen d_clean | seen d_emb | **holdout d_clean** | **holdout d_emb** | verdict |
|---------|-----:|-----:|-----:|-----:|---------|
| renyi | −2.05 | +0.32 | −2.02 | +0.60 | UNSTABLE (confirmed) |
| centroid | +1.37 | +0.81 | **+1.62** | **+1.07** | STABLE + (stronger on holdout) |
| flatness | +1.23 | +0.53 | **+1.43** | **+0.81** | STABLE + (stronger on holdout) |
| raw_flux | −1.15 | −0.53 | −1.16 | −0.50 | STABLE − |
| kick_ratio | +1.13 | −0.66 | +0.87 | −0.89 | UNSTABLE (confirmed) |
| hfc | −1.13 | −1.01 | −1.02 | −0.86 | STABLE − |
| crest | +1.00 | +0.66 | +0.77 | +0.38 | STABLE + (weaker on holdout) |
| complex_sd | −0.89 | −0.50 | −0.56 | **−0.12** | STABLE − but near-zero on holdout+embedded |
| rolloff | +0.80 | +0.99 | **+1.13** | **+1.34** | STABLE + (stronger on holdout) |
| wpd | −0.29 | +0.29 | −0.01 | +0.64 | UNSTABLE (confirmed) |

**Every STABLE/UNSTABLE classification holds** on the held-out corpus. `centroid`, `flatness`, and `rolloff` are *stronger* on unseen data than on the seen corpus — the Phase 1 shortlist isn't an artifact of models or tracks the training pipeline saw.

**One borderline:** `complex_sd` weakens substantially on held-out + embedded (d = −0.115, nearly zero). It's still sign-stable — just barely. Keep it as an NN input candidate but demote it from the deterministic-gate shortlist until the sign magnitude is confirmed against a larger held-out corpus. Cheap to re-test once we expand the held-out pool.

**Phase 2a parity-test priority is unchanged.** `flatness`, `centroid`, `crest`, `rolloff` as stable-+ gates; `hfc`, `raw_flux` as stable-− NN inputs and inverted-threshold gate candidates; `complex_sd` NN-only pending larger held-out corpus; `kick_ratio`, `renyi` NN-only (unstable); `wpd` dropped.

### Phase 3 first measurement — 2026-04-20

b133 deployed to all 4 serial devices with centroid/crest/rolloff/hfc added to the debug stream. Full 18-track validation run against `blinky-test-player/music/edm/` produced 71 per-device-per-track observations (18 tracks × 4 devices, one run per combination). The server's new per-signal `SignalGapStats` computes a Cohen's d for each.

**On-device vs offline Cohen's d (mean across devices and tracks):**

| signal | offline d | on-device d (μ ± σ) | on-device \|d\| μ | signs agree across devices |
|--------|----------:|--------------------:|----------------:|---------------------------:|
| centroid | +1.367 | +0.012 ± 0.237 | 0.161 | 3 / 18 tracks |
| flatness | +1.233 | +0.008 ± 0.230 | 0.159 | 3 / 18 tracks |
| raw_flux | −1.147 | −0.038 ± 0.154 | 0.124 | 3 / 18 tracks |
| hfc | −1.128 | −0.027 ± 0.214 | 0.156 | 5 / 18 tracks |
| crest | +1.000 | −0.003 ± 0.159 | 0.125 | 7 / 18 tracks |
| rolloff | +0.795 | +0.007 ± 0.259 | 0.139 | 6 / 18 tracks |

**Two severe problems.**

1. **|d| is 8-10× smaller on-device.** Offline all six signals had |d| > 0.8; on-device |d| mean is 0.12-0.16. The acoustic chain (speaker → room → 4 different mics → INT8 front-end) is eating most of the discrimination.
2. **Cross-device sign disagreement.** Only 17-39% of tracks have all 4 devices agreeing on even the sign of d. On the remaining 11-15 tracks, individual devices report +d while others report −d on the same signal on the same track. This is not acoustic-chain attenuation — this is the feature responding to different things on different devices.

**Hypotheses, in order of likelihood.**

- **a) Firmware frames vs GT alignment is noisy.** Signal frames are timestamped with server wall-clock (`time.time()`), not firmware `millis()`. Transients use firmware timestamps via `clock_offset`. These live in the same coordinate after conversion but signal frames carry USB jitter (~2–10 ms). The existing `audio_latency_ms` estimate comes from *transients*, which are firmware-timestamped. Applying a transient-derived offset to server-timestamped frames is the wrong coordinate system. With a 50 ms onset window, even 10 ms of systematic misalignment puts a noticeable fraction of frames on the wrong side of the classification. **Cheap fix**: add a `ts` field to the music stream in firmware and use firmware millis for signal frames too.
- **b) Frame-average vs per-onset peak.** Offline analysis used per-onset peak (single sample = max in ±50 ms around each GT onset). On-device scoring uses frame-average over every frame in the onset window. For transient-sharp features (centroid, hfc, rolloff) the peak is 1-2 frames; the ±50 ms window includes many non-peak frames that dilute the onset mean toward the non-onset mean. **Cheap fix**: mirror offline methodology on-device — per-GT-onset-peak.
- **c) Unverified firmware implementations.** Without a parity test (Phase 2a harness still pending), we can't rule out that the new C++ shape features disagree with their Python reference beyond acoustic-chain effects. The existing `flatness` uses compressed magnitudes in firmware while my Python reference uses raw STFT — already a known mismatch. The new ones are on pre-compressor magnitudes and should match, but "should" isn't "verified".
- **d) Real acoustic-chain degradation.** Even if (a)-(c) are all addressed, some portion of the gap is likely genuine — MEMS mics attenuate highs, the room adds reflections, INT8 quantization on widely-varying levels clips dynamic range. Some features might just not survive.

**Next priority:**

1. **Fix frame timestamp coordinate system** (hypothesis a). Add firmware `millis()` to the music stream, thread through `test_session.py`, retest. Cheapest first.
2. **Implement per-onset peak on-device** (hypothesis b). Mirrors offline Phase 1 methodology exactly. Should show whether features are merely diluted or actually lost.
3. **Standalone parity harness** (hypothesis c). Still worth building — rules out implementation-vs-reference bugs and becomes permanent CI regression protection.

Only after all three are in place can we confidently say (d) is the residual. Until then, Phase 4 gate / retrain decisions have no reliable signal.
