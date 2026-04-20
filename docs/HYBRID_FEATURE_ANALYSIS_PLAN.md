# Hybrid Feature Analysis Plan

**Status:** Planned (Apr 2026)
**Owner:** Audio/ML
**Supersedes:** ad-hoc `flat`/`rflux` streaming (b132)

## Goal

The v27-hybrid NN onset detector plateaus at on-device F1 ≈ 0.63 with precision ≈ 0.50. The model fires reliably on kicks/snares but also on tonal impulses (chords, synth stabs, vocals). We need to identify **deterministic signals** that can:

1. Discriminate true-positive (percussion) onsets from false-positive (tonal) impulses.
2. Behave consistently offline (clean audio) and on-device (speaker → room → mic → INT8), so the signal is not a sim-to-real artifact.
3. Survive cross-device variation (mic unit-to-unit differences, placement).

Qualifying signals feed two parallel downstream paths:
- **Deterministic FP gates** on-device (like the existing bass-gate) — cheapest and fastest to iterate.
- **New NN input features** — retrain with the winners to raise the F1 ceiling.

## Design decisions

### D1: Selective feature computation + streaming (not ring buffer)

An earlier version of this plan proposed a fixed ring buffer of all candidate features, dumped to the server post-run. **Rejected** because it requires always-computing every candidate, poisoning the test baseline with CPU cost that isn't present in deployment.

Instead, each candidate feature has an independent runtime flag that gates **both computation and streaming**:

```
stream signal flatness on    # compute AND stream
stream signal centroid off   # compute skipped, field absent from m object
```

- **Deployed firmware cost:** zero (all flags default off, branch-predicted out)
- **Test-time cost:** proportional to what's enabled
- **Bandwidth:** only enabled features consume stream bytes; irrelevant when selective

### D2: FP label source — both GT tracks and synthetic tonals

- **GT tracks (`.beats.json`):** FP = NN fires without a matching GT onset within ±50 ms. Covers the real-world distribution of FPs in music. Limitation: GT labels are kick/snare-weighted; hi-hats and other non-labeled percussive events can appear as phantom FPs.
- **Synthetic tonal tracks:** pure piano chords, held synth pads, sustained vocals — any NN firing is an FP by construction. Clean labels but limited ecological validity.

Using both surfaces different failure modes: GT tracks show which signals discriminate in-context; synthetics isolate the tonal-impulse response without drum masking.

### D3: Same analysis pipeline offline and on-device

To compute the sim-to-real gap per signal, the **Python offline analysis** and the **firmware computation** must match bit-for-bit at the feature level (within INT8 quantization error). Reuse the existing mel-pipeline parity validation (MAE = 0.0017 INT8 levels, see `docs/AUDIO_ARCHITECTURE.md`). Each candidate feature needs a parity test before Phase 3 conclusions are trustworthy.

## Candidate signal catalog

Ten candidates grouped by family. Each has offline Python reference + firmware implementation + parity test as acceptance criteria.

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

Total if all enabled: ~3.5 ms/frame. At 62.5 Hz that's 22% CPU — unacceptable for deployment but fine when testing a subset.

## Phase plan

### Phase 1 — Offline catalog (Python-only)

**Input:** EDM track library with `.beats.json` ground truth + curated synthetic tonal clips.
**Compute:** all 10 candidate features per frame on every track.
**Label each frame** as TP / FP / TN / FN using:
- TP: GT onset within ±50 ms AND NN activation above threshold.
- FP: NN activation above threshold, no GT onset within ±50 ms.
- TN: neither.
- FN: GT onset, no NN firing.

NN activations come from the existing offline evaluator (runs the trained model on clean audio).

**Per-signal outputs:**
- Distribution (histogram) for each class.
- Cohen's d effect size between TP and FP.
- ROC/AUC of the signal alone as a TP-vs-FP classifier.
- Rank signals by effect size; keep top 6 for Phase 2.

**Deliverable:** `ml-training/analysis/hybrid_feature_catalog.ipynb` + ranked shortlist committed to this doc as an addendum.

### Phase 2 — Selective streaming infrastructure

Firmware and server changes needed so Phase 3 can capture on-device data for arbitrary subsets of the shortlist.

**Firmware:**
- Add a `SignalRegistry` with one entry per candidate (name, compute_fn, flag).
- Extend `SerialConsole` with `stream signal <name> on|off` commands.
- In the stream tick, iterate the registry and emit only enabled signals with a shared JSON schema (`"signals":{"flat":0.34, "hfc":0.81, ...}`).
- Compute functions gated by flag — zero cost when disabled.

**Server (`blinky-server`):**
- `test_session.py`: ingest arbitrary signal fields under `signals` into a generic `SignalFrame` list (replaces the hardcoded `NNFrame`).
- `scoring.py`: parameterize `HybridMetrics` to compute TP/FP/TN gaps for every signal present in the capture.
- `test_runner.py`: accept a `signals: [str]` parameter in validate/sweep requests; issue `stream signal X on` per test.

**Acceptance:** a validate request with `signals=["flatness","hfc"]` produces a capture with both fields, and a request with `signals=[]` produces a capture with none (confirming zero-cost baseline).

### Phase 3 — Sim-to-real gap measurement

**Procedure** for each of the top 6 signals from Phase 1:
1. Enable the signal via `stream signal X on` on all 4 devices.
2. Play the same GT tracks offline (Phase 1) and on-device (validation harness).
3. Align frames by timestamp (within 1 hop = 16 ms).
4. Compute:
   - **Pearson correlation** between offline and on-device values (environmental robustness).
   - **KS statistic** on the two distributions (distribution-level similarity).
   - **Cross-device std** across the 4 devices (mic invariance).
   - **On-device TP-vs-FP Cohen's d** (discriminative power in real conditions).

**Deployability score** per signal:
```
score = |cohen_d_ondevice| × corr_offline_ondevice × (1 / (cross_device_std_normalized + ε))
```

with `ε = 0.01` to prevent a perfectly consistent signal from scoring infinitely. Equivalent formulation: `score = |d| × r / max(ε, cross_device_std_normalized)`. High score = discriminative AND robust AND consistent. Bottom third of the shortlist gets dropped.

**Deliverable:** a decision matrix appended to this doc listing per-signal offline_d, on-device_d, offline↔device correlation, cross-device std, final score.

### Phase 4 — Act on the shortlist

Two paths run in parallel; whichever lands first wins.

**Path A: deterministic FP gates.** For each top-ranked signal, add a threshold-based gate in the onset detection code (extending the current bass-gate pattern). Sweep thresholds via `run_param_sweep`. Cheap to try, cheap to revert.

**Path B: retrain NN with new features.** Add the top 2-3 signals as additional NN input channels (bumps 32 → 34-35). Retrain v28, evaluate, deploy if F1 improves by ≥0.03.

Expected outcome: one new deterministic gate (cheap) + one new NN input feature (expensive but higher ceiling). Neither is guaranteed.

**Phase 4 exit gate — strip losing candidates from firmware.** Before any Phase 4 work merges to master, every candidate feature that did **not** make the shortlist must be deleted from the firmware (computation + flag + stream field), not just flag-defaulted-off. Run `grep -r 'SignalRegistry' blinky-things/` to confirm only the shipped signals remain. Investigation-only code that lingers in deployment firmware is how flash budget leaks accumulate — see the `v21`–`v26` cleanup in b132 for an example we had to do retroactively.

## Open questions / risks

1. **Parity cost.** Each firmware feature needs a Python reference and a parity test. For 6 signals that's ~6 days of careful work. Can we batch parity validation by sharing a test harness? Yes — reuse `ml-training/verify_mel_pipeline.py` as a template.

2. **Phase history RAM cost.** Complex SD and WPD both need per-bin phase memory (128 floats × 2 = 1 KB RAM each). Acceptable on nRF52840 (256 KB total, 3.4 KB arena + 16 KB globals currently), but only if we share the buffer across signals that need it.

3. **FP label noise.** GT tracks only label kicks and snares. An NN firing on a hi-hat counts as an FP under the current labeling, but it's actually a real (just unlabeled) onset. Mitigations: (a) use drum-stem-labeled tracks only for FP analysis, (b) include synthetic tonals as the "cleanest" FP corpus, (c) manually spot-check the highest-confidence FPs to estimate label noise.

4. **Investigation ≠ deployment.** The test-time firmware has feature flags and computation paths that the deployed firmware shouldn't carry (wasted flash). When Phase 4 picks winners, strip unused candidates from the code, not just flag them off.

5. **Scope creep.** The catalog could grow indefinitely (50+ onset detection features in the literature). Start with the 10 listed; only expand if Phase 3 scores are disappointingly low across the board.

## References

- `docs/AUDIO_ARCHITECTURE.md` — current v27-hybrid architecture and the 32-input feature set.
- `docs/ML_IMPROVEMENT_PLAN.md` — NN training roadmap.
- `blinky-server/blinky_server/testing/scoring.py` `HybridMetrics` — prototype scoring shipped in b132 for `flat`/`rflux`. Generalize in Phase 2.
- Bello et al., *A Tutorial on Onset Detection in Music Signals*, IEEE TSAP 2005 — most of the candidate catalog derives from Section III.
