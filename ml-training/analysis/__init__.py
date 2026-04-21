"""Hybrid feature analysis (Phases 1-3 of HYBRID_FEATURE_ANALYSIS_PLAN.md).

Computes candidate percussion-vs-tonal discriminators per frame on GT audio,
labels onsets from ground truth (not NN inference — the NN is tested offline
as a separate step), and ranks features by their ability to separate
onset-peak samples from non-onset frames, plus percussion vs synthetic tonal
impulses. Output feeds gate (b)/(c)/(d)/(e) decisions in Phase 3.

Offline Python with a native C++ parity harness (`tests/parity/`) that links
the firmware's `SharedSpectralAnalysis` directly for bit-for-bit verification
of every feature.
"""
