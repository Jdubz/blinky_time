"""Hybrid feature analysis (Phase 1 of HYBRID_FEATURE_ANALYSIS_PLAN.md).

Computes candidate percussion-vs-tonal discriminators per frame on GT audio,
labels each frame as TP/FP/TN using the deployed v27 NN, and ranks signals
by their ability to separate TP from FP. Output feeds Phase 2 / 3 decisions.

Offline Python only — no firmware changes, no device round-trips.
"""
