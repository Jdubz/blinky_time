# Onset Detection Literature — Reference for the Blinky System

> **Purpose.** Ground design decisions for the Blinky on-device onset detector in published research rather than internal extrapolation. Compiled 2026-05-02 after v36-fmax closed as a negative result and we needed an honest reckoning with where the literature actually places us.
>
> **Audience.** Future-me / future-engineers picking up this work. Quote a specific paper before claiming "the literature says X."
>
> **Maintenance.** Add a new entry whenever a paper materially updates a planning decision. Don't paraphrase — quote numbers and architectures.

## TL;DR for impatient readers

| Question | Answer |
|---|---|
| What F1 do published onset systems achieve? | 0.88–0.91 on Böck/MIREX-style hand-curated GT; 0.85–0.88 for classical superflux baselines |
| At what parameter count? | 25K–100K is typical for small-CNN onset detectors. **No published onset detector at ≤10K INT8 params exists.** |
| What input do they use? | 80+ mel × 16 kHz fmax × ~10 ms hop is the canonical Schlüter '14 spec |
| What labels do they use? | Hand-curated (Böck, ENST, MDB-Drums), rhythm-game crowdsourced (ADTOF), or MIDI-aligned (Callender 2020). Consensus-of-detectors as training labels is rare and known-noisier |
| Is "multi-instrument heads + masked loss" published? | The two ingredients separately yes (Vogl 2017, Fonseca 2020). The exact combination for onset detection is novel — no precedent |
| Is our eval set GT trustworthy? | Inter-annotator F1 caps at 0.92–0.95 on clean drums, **lower on dense EDM**. Our consensus-derived GT plausibly caps practical F1 at 0.75–0.80 |

## Section 1 — Onset detection state of the art

### Schlüter & Böck (2014) "Improved Musical Onset Detection with Convolutional Neural Networks" — ICASSP

- Canonical small-CNN onset detector. **The reference architecture for this problem.**
- Input: 3 stacked log-mel spectrograms (80 mel, ~10 ms hop, fmax up to 16 kHz)
- Architecture: 2 conv layers (10 + 20 feature maps) + 1 dense layer (~256 units), 7-frame context patches
- **~25K–100K params** depending on variant
- **F-measure 0.903 mean on Böck onset corpus** (~26K hand-annotated onsets)
- Ablations: input width (mel resolution × fmax) is the dominant lever; capacity beyond a few-conv-layer 25-50K-param net has sharply diminishing returns
- URL: https://www.ofai.at/~jan.schlueter/pubs/2014_icassp.pdf

### Böck, Arzt, Krebs, Schedl (2012) "Online Real-time Onset Detection with RNNs" — DAFx

- The pre-CNN canonical onset detector
- F1 ~0.89 online, ~0.89-0.90 offline on Böck corpus
- LSTM/BLSTM variants

### Stahl & Sturm (2023) "Supervised Contrastive Learning For Musical Onset Detection" — Audio Mostly

- F1 0.888 on Böck corpus with cross-entropy; supervised contrastive ≈ BCE within noise
- **Implication for us:** loss-function tweaking has not produced ≥0.01 F1 lift in any published comparison. Our internal "9 loss variants tried, none beat BCE" finding is consistent with literature.

### MIREX onset detection task

- Classical superflux + adaptive peak picking: 0.85–0.88 F1
- Modern CNN/RNN systems push 0.88–0.91
- All against hand-curated GT

## Section 2 — Drum transcription (related task, multi-output)

### Vogl, Dorfer, Widmer, Knees (2017) "Drum Transcription via Joint Beat and Drum Modeling using CRNNs" — ISMIR

- **Closest published architecture to our proposed #135**
- Joint multi-output: kick / snare / hihat + beat + downbeat heads
- Per-instrument F1 0.74–0.87 on ENST/MDB-Drums/SMT/RBMA13
- **No fallback head, no missing-label masking**
- URL: https://www.cp.jku.at/research/papers/Vogl_etal_ISMIR_2016.pdf

### Vogl (2018) "Towards Multi-Instrument Drum Transcription" — DAFx
- Extension of 2017 work to more instruments. Same multi-output pattern.

### Wu et al. (2018) "A Review of Automatic Drum Transcription"
- The reference review of the ADT field. Per-instrument F1 0.74–0.87 typical.
- URL: https://www.open-access.bcu.ac.uk/6180/1/Wu-et-al.-2018-A-review-of-automatic-drum-transcription.pdf

### Cartwright & Bello (2018, 2019)
- Large-vocabulary ADT trained on synthesized drum kits with multi-task heads
- Specific to drum classification, not directly onset detection

### Choi & Cho (2019) "DrummerNet: Deep Unsupervised Drum Transcription" — ISMIR

- Unsupervised drum transcription via signal reconstruction loss
- F1 0.869 on SMT after 249 hours of unlabeled drum tracks
- **Unique:** zero hand-labeled training data
- URL: https://www.researchgate.net/publication/333678830_Deep_Unsupervised_Drum_Transcription

### Callender et al. (2020) "Improving ADT Using Large-Scale Audio-to-MIDI Aligned Data"
- MIDI-aligned training data; large-scale
- URL: https://www.researchgate.net/publication/352171605

### Zehren et al. (2021) "ADTOF: A Large Dataset of Non-Synthetic Music for ADT" — ISMIR
- Rhythm-game-derived training labels at large scale
- URL: https://archives.ismir.net/ismir2021/paper/000102.pdf

## Section 3 — Multi-task / missing-label techniques

### Fonseca et al. (2020) "Addressing Missing Labels in Large-Scale Sound Event Recognition with a Teacher-Student Framework with Loss Masking" — ICASSP

- **The exact masked-loss mechanism we propose for #135**
- Applied to AudioSet sound events, not music onsets
- Mask the loss for missing labels per-example; train multi-task heads otherwise normally
- URL: https://arxiv.org/abs/2005.00878

### DCASE 2024 Task 4 "Sound Event Detection with Heterogeneous Training Dataset and Potentially Missing Labels"
- Entire DCASE challenge built around the masked-loss mechanism
- Confirms this is a well-explored pattern in SED literature
- URL: https://dcase.community/challenge2024/task-sound-event-detection-with-heterogeneous-training-dataset-and-potentially-missing-labels

## Section 4 — TinyML / small-parameter audio

**No published onset detector at ≤10K INT8 params exists.** Our scale is uncharted in the onset-detection literature. The closest reference points:

- Keyword-spotting / simple AED at Cortex-M scale: F1 0.85–0.91 with quantized CNN/MFCC pipelines (different task)
- Schlüter '14's smallest variant: ~25K params at FP32

**Implication.** Our 10K INT8 budget is **2–10× smaller than any published onset detector**. We cannot generalize upward from internal "wider doesn't help" ablations because we tested entirely below the published range. **The architecture-capacity question remains open.**

## Section 5 — Eval-set / GT-quality literature

### Inter-annotator agreement on drum/onset GT

- Cartwright (2018), Gillick et al. (2019): inter-annotator F1 caps at 0.92–0.95 on clean drum recordings
- **Lower on dense electronic / EDM content** — annotators disagree on ghost notes, hi-hat rolls, ambient onset edges
- Practical implication: even hand-curated EDM GT plateaus around F1 0.92

### Consensus-of-detectors GT

- Not a standard practice in MIREX or major drum-transcription benchmarks
- Used as a *weak supervision* signal, not as a *headline metric ground truth*
- The consensus filter we use (≥3 of 5 systems agree within ±70 ms) is internal to this project; no published baseline uses an equivalent GT scheme for headline F1

**Practical ceiling against consensus GT** (from literature on annotator agreement + consensus-vs-hand-curated agreement studies, applied with the project's specific 5-detector mix): **plausibly 0.75–0.80**, not the 0.88–0.91 of hand-curated benchmarks.

## Section 6 — Things explicitly NOT in the literature for our scale

These are levers we have used or considered. They do not have published support at ≤10K params on onset detection — flagged so we don't claim "the literature says X" when it doesn't.

- "Wider Conv1D doesn't help at fixed param count" — tested only at our scale; not published
- target_rms_db calibration — not a parameter in published onset literature
- Hybrid spectral features (centroid, flatness, HFC) as auxiliary NN inputs — not standard in published onset CNNs (the inputs *are* mel; non-mel features are used as gates downstream of detection, e.g., superflux)
- min_systems threshold tightening on consensus labels — internal to this project
- max-OR pooling of multi-instrument heads to produce any-onset score — engineering, not literature
- INT8 export of onset CNNs — TinyML literature exists but not specific to onset detection

## Section 7 — Recommended priors for the next experiment

Given the literature, the realistic prior on what an experiment can move:

| Lever | Literature-supported lift | Cost | Confidence |
|---|---|---|---|
| Hand-curate GT (replaces consensus-derived) | +0.05–0.10 measured F1, no model change | 4–8 hours | high |
| Per-instrument heads + masked-loss fallback (#135) | +0.05–0.10 if coverage is binding | 1 week | medium (novel combination) |
| KD from BS-RoFormer / madmom RNN | +0.03–0.08 | 1 week | medium |
| Capacity bump (10K → 25K params) | +0.05–0.15 | requires firmware budget rev | unknown at 10K start |
| HPSS preprocessing | ±0.03 | 2 days | low |
| Loss function tweaks | ≤0.01 | trivial | high (ceiling) |
| target_rms_db / augmentation tweaks | tested exhaustively, ≤0.01 | trivial | high (ceiling) |

**Recommended experiment ordering** is in `ML_IMPROVEMENT_PLAN.md` 2026-05-02 entry. Hand-curate GT first (cheapest, most diagnostic). Per-class F1 breakdown second. #135 only after the first two confirm a label-coverage gap exists.

## Open questions the literature does NOT answer

1. What F1 is achievable at ≤10K INT8 params on onset detection? (no precedent)
2. Does max-OR pooling of multi-instrument heads beat a single onsets_consensus head trained directly? (not a published comparison)
3. What's the expected practical ceiling against our specific 5-detector consensus GT? (project-specific; need to measure inter-detector agreement)
4. Does our INT8 quantization step disproportionately hurt small-band-count vs many-band-count models? (project-specific; v30/v31/v32 collapse hints yes but isn't published)
