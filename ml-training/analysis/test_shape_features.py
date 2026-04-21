"""Targeted unit tests for bin-indexed shape features.

Regression guard for PR 130 gap (b): centroid and HFC weight each bin by its
bin index (k). The Python reference in features.py uses `k = np.arange(1,
N+1)` to match firmware's 1-based bin indexing (firmware index 1 → first
positive-frequency bin, not index 0). An off-by-one here silently skews
every downstream gate-check result by ~1 bin, so verify the known-bin
behaviour directly with synthetic impulses.

Run as a script (exits non-zero on failure):
    ./venv/bin/python -m analysis.test_shape_features

Runs in well under a second — no audio I/O, no disk.
"""

from __future__ import annotations

import sys

import numpy as np

from .features import (
    NUM_BINS,
    high_frequency_content,
    spectral_centroid,
    spectral_flatness,
    spectral_rolloff,
)


def _one_hot(bin_idx: int, n_bins: int = NUM_BINS) -> np.ndarray:
    """Single-frame magnitude spectrum with energy only at `bin_idx`.

    `bin_idx` is firmware 1-based (so bin_idx=1 is the first positive-
    frequency bin and maps to column 0 of the NUM_BINS-wide array).
    """
    assert 1 <= bin_idx <= n_bins, f"bin_idx {bin_idx} outside [1,{n_bins}]"
    m = np.zeros((1, n_bins), dtype=np.float32)
    m[0, bin_idx - 1] = 1.0
    return m


def _assert_close(actual: float, expected: float, tol: float, label: str) -> None:
    if not np.isfinite(actual) or abs(actual - expected) > tol:
        raise AssertionError(
            f"{label}: got {actual}, expected {expected} ± {tol}"
        )


def test_centroid_one_hot() -> None:
    """Centroid of an impulse at bin k must equal k (1-based)."""
    for k in (1, 10, 50, NUM_BINS):
        mags = _one_hot(k)
        c = float(spectral_centroid(mags)[0])
        _assert_close(c, float(k), 1e-4, f"centroid @ bin {k}")


def test_hfc_one_hot() -> None:
    """HFC = Σ k·|X|². Impulse with unit magnitude at bin k ⇒ HFC = k."""
    for k in (1, 7, 32, 127):
        if k > NUM_BINS:
            continue
        mags = _one_hot(k)
        hfc = float(high_frequency_content(mags)[0])
        _assert_close(hfc, float(k), 1e-4, f"HFC @ bin {k}")


def test_centroid_two_bin_mean() -> None:
    """Equal-magnitude impulses at bins a and b ⇒ centroid = (a+b)/2."""
    a, b = 3, 11
    mags = np.zeros((1, NUM_BINS), dtype=np.float32)
    mags[0, a - 1] = 1.0
    mags[0, b - 1] = 1.0
    c = float(spectral_centroid(mags)[0])
    _assert_close(c, (a + b) / 2.0, 1e-4, "two-bin centroid mean")


def test_hfc_two_bin_sum() -> None:
    """HFC is additive: impulses at bins a and b ⇒ HFC = a² + b² (|X|²=1)."""
    a, b = 4, 17
    mags = np.zeros((1, NUM_BINS), dtype=np.float32)
    mags[0, a - 1] = 1.0
    mags[0, b - 1] = 1.0
    # Unit magnitudes, so |X|² = 1 per bin → HFC = a·1 + b·1 = a + b.
    hfc = float(high_frequency_content(mags)[0])
    _assert_close(hfc, float(a + b), 1e-4, "two-bin HFC sum")


def test_rolloff_one_hot() -> None:
    """Rolloff of an impulse at bin k lands at that bin (zero-based after DC drop)."""
    for k in (1, 5, 42, 100):
        if k > NUM_BINS:
            continue
        mags = _one_hot(k)
        r = float(spectral_rolloff(mags)[0])
        _assert_close(r, float(k - 1), 1e-4, f"rolloff @ bin {k}")


def test_flatness_flat_spectrum_is_one() -> None:
    """A uniform magnitude spectrum should have flatness ≈ 1 (geo = arith)."""
    mags = np.ones((1, NUM_BINS), dtype=np.float32)
    f = float(spectral_flatness(mags)[0])
    _assert_close(f, 1.0, 1e-4, "flatness uniform")


def test_flatness_single_bin_is_low() -> None:
    """A single-bin impulse should have very low flatness (pure tone)."""
    mags = _one_hot(10)
    f = float(spectral_flatness(mags)[0])
    if not (0.0 <= f < 0.01):
        raise AssertionError(f"flatness pure tone: got {f}, expected ~0 < 0.01")


def test_resolve_hybrid_features_explicit_list() -> None:
    """`features.hybrid_features: [...]` returns canonical-ordered subset."""
    from scripts.audio import resolve_hybrid_features

    cfg = {"features": {"hybrid_features": ["hfc", "flatness"]}}
    # Regardless of input order, output follows HYBRID_FEATURE_NAMES order.
    assert resolve_hybrid_features(cfg) == ["flatness", "hfc"]


def test_resolve_hybrid_features_legacy_bool() -> None:
    """Legacy `use_hybrid: true` maps to the v27 [flatness, raw_flux] pair."""
    from scripts.audio import resolve_hybrid_features

    cfg = {"features": {"use_hybrid": True}}
    assert resolve_hybrid_features(cfg) == ["flatness", "raw_flux"]


def test_resolve_hybrid_features_empty() -> None:
    """Neither new list nor legacy flag set → empty list."""
    from scripts.audio import resolve_hybrid_features

    assert resolve_hybrid_features({}) == []
    assert resolve_hybrid_features({"features": {}}) == []
    assert resolve_hybrid_features({"features": {"use_hybrid": False}}) == []


def test_resolve_hybrid_features_unknown_raises() -> None:
    """Unknown name in `hybrid_features` list raises with a helpful message."""
    from scripts.audio import resolve_hybrid_features

    cfg = {"features": {"hybrid_features": ["flatness", "bogus"]}}
    try:
        resolve_hybrid_features(cfg)
    except ValueError as e:
        assert "bogus" in str(e)
        return
    raise AssertionError("expected ValueError for unknown hybrid feature name")


def main() -> int:
    tests = [
        test_centroid_one_hot,
        test_hfc_one_hot,
        test_centroid_two_bin_mean,
        test_hfc_two_bin_sum,
        test_rolloff_one_hot,
        test_flatness_flat_spectrum_is_one,
        test_flatness_single_bin_is_low,
        test_resolve_hybrid_features_explicit_list,
        test_resolve_hybrid_features_legacy_bool,
        test_resolve_hybrid_features_empty,
        test_resolve_hybrid_features_unknown_raises,
    ]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
            failed += 1
    if failed:
        print(f"\n{failed}/{len(tests)} test(s) failed")
        return 1
    print(f"\n{len(tests)}/{len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
